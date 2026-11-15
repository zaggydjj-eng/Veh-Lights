// VehicleLights - AML mod for GTA SA Android (ARM32)
// Headlights, brake lights, reverse lights and turn signals for ANY vehicle model.
// Light positions come from each model's collision bounding box, so they
// automatically fit cars, bikes and custom models.
//
// UNTESTED: offsets and mangled symbols are from memory (v2.00, ARM32).
// Check the log (tag "VLights") and the constants below if something fails.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

MYMOD(net.example.vlights, VehicleLights, 1.0, You)

// ---------------- offsets / constants (verify for your libGTASA) ----------------
#define OFF_MATRIX       0x14   // CPlaceable::m_matrix (pointer)
#define OFF_MODEL        0x26   // CEntity::m_nModelIndex (uint16)
#define OFF_SPEED        0x44   // CPhysical::m_vecMoveSpeed
#define OFF_MI_COLMODEL  0x14   // CBaseModelInfo::m_pColModel
#define CORONA_TYPE      1      // corona texture index; try 0..8 if the glow looks wrong
#define MAX_MODELS       20000
#define MAX_STATES       128

#define HAZARD_FILE      "/sdcard/vlights_hazard.txt"  // exists = hazards on (player car)
#define HEADLIGHT_FILE   "/sdcard/vlights_head.txt"    // exists = headlights forced on

// ---------------- game symbols ----------------
typedef void (*RegisterCorona_t)(uint32_t id, void* attachTo,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a, const float* pos,
    float radius, float farClip, uint32_t coronaType, uint8_t flareType,
    bool reflection, bool checkObstacles, int unused, float angle,
    bool longDistance, float nearClip, uint8_t fadeState, float fadeSpeed,
    bool onlyFromBelow, bool reflectionDelay);

static RegisterCorona_t RegisterCorona;
static void* (*FindPlayerVehicle)(int, bool);
static void** g_modelInfoPtrs;
static uint32_t* g_timeMs;
static uint8_t*  g_hour;

static void (*Auto_PreRender)(void*);
static void (*Bike_PreRender)(void*);

// ---------------- safe memory check (avoids crashes on bad offsets) ----------------
static int g_pipe[2] = { -1, -1 };

static bool Readable(const void* p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000 || g_pipe[1] < 0 || n > 64) return false;
    ssize_t w = write(g_pipe[1], p, n);
    if (w < 0) return false;
    char tmp[64];
    while (w > 0) {
        ssize_t r = read(g_pipe[0], tmp, w);
        if (r <= 0) break;
        w -= r;
    }
    return true;
}

// ---------------- per-model light layout ----------------
struct Profile {
    bool  ready;
    float halfW, yFront, yRear, zLow, zHigh;
};
static Profile g_prof[MAX_MODELS];

static void BuildProfile(int id, Profile* p)
{
    // defaults for a mid-size car
    p->halfW = 0.9f; p->yFront = 2.3f; p->yRear = -2.3f; p->zLow = -0.6f; p->zHigh = 0.8f;
    p->ready = true;

    if (!g_modelInfoPtrs) return;
    char* mi = (char*)g_modelInfoPtrs[id];
    if (!Readable(mi + OFF_MI_COLMODEL, 4)) return;
    char* col = *(char**)(mi + OFF_MI_COLMODEL);
    if (!Readable(col, 0x30)) return;

    // The bounding box position inside CColModel differs between builds,
    // so try a few offsets and accept the first that looks like a vehicle box.
    static const int cand[] = { 0x00, 0x10, 0x14, 0x18, 0x1C };
    for (int i = 0; i < 5; i++) {
        float* b = (float*)(col + cand[i]);
        if (!Readable(b, 24)) continue;
        float mnx = b[0], mny = b[1], mnz = b[2], mxx = b[3], mxy = b[4], mxz = b[5];
        if (mnx < -0.2f && mxx > 0.2f && mnx > -4.f && mxx < 4.f &&
            mny < -0.8f && mxy > 0.8f && mny > -10.f && mxy < 10.f &&
            mxz > mnz && mxz - mnz < 6.f) {
            p->halfW = (mxx - mnx) * 0.5f;
            p->yFront = mxy; p->yRear = mny;
            p->zLow = mnz;   p->zHigh = mxz;
            return;
        }
    }
}

static Profile* GetProfile(int id)
{
    if (id < 0 || id >= MAX_MODELS) return nullptr;
    Profile* p = &g_prof[id];
    if (!p->ready) BuildProfile(id, p);
    return p;
}

// ---------------- per-vehicle state ----------------
struct State {
    void*    veh;
    uint32_t lastT;
    float    lastF, yaw;
    uint32_t leftUntil, rightUntil;
};
static State g_state[MAX_STATES];

static State* GetState(void* v)
{
    State* free_ = nullptr;
    for (int i = 0; i < MAX_STATES; i++) {
        if (g_state[i].veh == v) return &g_state[i];
        if (!free_ && !g_state[i].veh) free_ = &g_state[i];
    }
    if (!free_) { free_ = &g_state[(((uintptr_t)v) >> 4) % MAX_STATES]; }
    memset(free_, 0, sizeof(State));
    free_->veh = v;
    free_->lastT = 0xFFFFFFFF;
    return free_;
}

// ---------------- helpers ----------------
static void Lamp(char* v, const float* m, int slot, float lx, float ly, float lz,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a, float size)
{
    float pos[3] = {
        m[12] + m[0] * lx + m[4] * ly + m[8]  * lz,
        m[13] + m[1] * lx + m[5] * ly + m[9]  * lz,
        m[14] + m[2] * lx + m[6] * ly + m[10] * lz
    };
    RegisterCorona((uint32_t)(uintptr_t)v + 0x100 + slot, v, r, g, b, a, pos,
                   size, 120.0f, CORONA_TYPE, 0, true, false, 0, 0.0f,
                   false, 0.5f, 0, 15.0f, false, false);
}

static bool g_hazard = false, g_forceHead = false;
static uint32_t g_lastCheck = 0;

static void DoLights(void* veh)
{
    if (!veh || !RegisterCorona || !g_timeMs) return;
    char* v = (char*)veh;

    if (!Readable(v + OFF_MATRIX, 4)) return;
    float* m = *(float**)(v + OFF_MATRIX);
    if (!Readable(m, 0x40)) return;

    Profile* p = GetProfile(*(uint16_t*)(v + OFF_MODEL));
    if (!p) return;

    uint32_t t = *g_timeMs;
    if (t - g_lastCheck > 500) {
        g_lastCheck = t;
        g_hazard    = access(HAZARD_FILE, F_OK) == 0;
        g_forceHead = access(HEADLIGHT_FILE, F_OK) == 0;
    }

    bool isPlayer = FindPlayerVehicle && FindPlayerVehicle(-1, false) == veh;

    // --- motion analysis ---
    float* s = (float*)(v + OFF_SPEED);
    float f = s[0] * m[4] + s[1] * m[5] + s[2] * m[6];   // speed along vehicle forward
    float yaw = atan2f(-m[4], m[5]);

    State* st = GetState(veh);
    if (st->lastT != t) {
        if (st->lastT != 0xFFFFFFFF) {
            float dy = yaw - st->yaw;
            while (dy >  3.14159f) dy -= 6.28318f;
            while (dy < -3.14159f) dy += 6.28318f;
            if (fabsf(f) > 0.03f) {
                if (dy >  0.010f) st->leftUntil  = t + 1200;
                if (dy < -0.010f) st->rightUntil = t + 1200;
            }
        }
        st->lastT = t; st->yaw = yaw;
    }
    bool braking = (fabsf(f) < fabsf(st->lastF) - 0.004f) || (isPlayer && fabsf(f) < 0.01f);
    bool reversing = f < -0.015f;
    st->lastF = f;

    bool night = g_hour && (*g_hour >= 20 || *g_hour < 6);
    bool headOn = night || (isPlayer && g_forceHead);
    bool blinkPhase = ((t / 350) & 1) != 0;
    bool left  = (t < st->leftUntil)  || (isPlayer && g_hazard);
    bool right = (t < st->rightUntil) || (isPlayer && g_hazard);

    // --- layout from model shape ---
    float hw = p->halfW;
    bool bike = hw < 0.6f;
    float h  = p->zHigh - p->zLow;
    float zF = p->zLow + h * 0.40f;
    float zR = p->zLow + h * 0.45f;
    float yF = p->yFront - 0.08f;
    float yR = p->yRear  + 0.08f;
    int sides = bike ? 1 : 2;

    for (int i = 0; i < sides; i++) {
        float sg = (i == 0) ? -1.f : 1.f;
        float hx = bike ? 0.f : sg * hw * 0.62f;   // headlight
        float tx = bike ? 0.f : sg * hw * 0.66f;   // tail / brake
        float rx = bike ? 0.f : sg * hw * 0.40f;   // reverse (inner)

        if (headOn)
            Lamp(v, m, 0 + i, hx, yF, zF, 255, 245, 220, 255, 0.34f);

        if (braking)
            Lamp(v, m, 4 + i, tx, yR, zR, 255, 0, 0, 255, 0.30f);
        else if (headOn)
            Lamp(v, m, 2 + i, tx, yR, zR, 150, 0, 0, 200, 0.16f);

        if (reversing)
            Lamp(v, m, 6 + i, rx, yR, zR - 0.05f, 255, 255, 255, 255, 0.22f);
    }

    // --- turn signals (corner of the body, so they follow the shape) ---
    float bx = hw * 0.88f;
    if (blinkPhase) {
        if (left) {
            Lamp(v, m, 8,  -bx, yF - 0.05f, zF - 0.06f, 255, 150, 0, 255, 0.16f);
            Lamp(v, m, 10, -bx, yR + 0.05f, zR + 0.02f, 255, 150, 0, 255, 0.16f);
        }
        if (right) {
            Lamp(v, m, 9,  bx, yF - 0.05f, zF - 0.06f, 255, 150, 0, 255, 0.16f);
            Lamp(v, m, 11, bx, yR + 0.05f, zR + 0.02f, 255, 150, 0, 255, 0.16f);
        }
    }
}

// ---------------- hooks ----------------
static void Auto_Hook(void* self) { Auto_PreRender(self); DoLights(self); }
static void Bike_Hook(void* self) { Bike_PreRender(self); DoLights(self); }

static void* FindSym(void* h, const char* a, const char* b = nullptr)
{
    void* r = aml->GetSym(h, a);
    if (!r && b) r = aml->GetSym(h, b);
    return r;
}

extern "C" void OnModLoad()
{
    logger->SetTag("VLights");
    void* h = aml->GetLibHandle("libGTASA.so");

    if (pipe(g_pipe) == 0) {
        fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
    }

    // CCoronas::RegisterCorona - signature differs between builds, try both
    SET_TO(RegisterCorona, FindSym(h,
        "_ZN8CCoronas14RegisterCoronaEjP7CEntityhhhhRK7CVectorffjhbbifbfhfbb",
        "_ZN8CCoronas14RegisterCoronaEjP7CEntityhhhhRK7CVectorffP9RwTexturehbbifbfhfbb"));
    SET_TO(FindPlayerVehicle, aml->GetSym(h, "_Z17FindPlayerVehicleib"));
    g_modelInfoPtrs = (void**)aml->GetSym(h, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    g_timeMs = (uint32_t*)aml->GetSym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    g_hour   = (uint8_t*)aml->GetSym(h, "_ZN6CClock17ms_nGameClockHoursE");

    if (!RegisterCorona) logger->Error("RegisterCorona symbol not found - check nm -D libGTASA.so");
    if (!g_modelInfoPtrs) logger->Error("ms_modelInfoPtrs not found - default light layout will be used");

    void* ap = aml->GetSym(h, "_ZN11CAutomobile9PreRenderEv");
    void* bp = aml->GetSym(h, "_ZN5CBike9PreRenderEv");
    if (ap) aml->Hook(ap, (void*)Auto_Hook, (void**)&Auto_PreRender);
    if (bp) aml->Hook(bp, (void*)Bike_Hook, (void**)&Bike_PreRender);
}
