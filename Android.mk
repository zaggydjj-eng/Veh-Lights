LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := VehicleLights
LOCAL_SRC_FILES := vlights.cpp
LOCAL_CPPFLAGS += -std=c++17 -O2
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
