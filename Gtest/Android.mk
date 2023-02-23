LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_LDLIBS += -landroid -llog
LOCAL_CFLAGS += -fexceptions
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES := main.cpp CameraFixture.cpp CameraClient.cpp
LOCAL_SHARED_LIBRARIES += liblog libcutils libutils camera.$(TARGET_PRODUCT) libvhal-client
LOCAL_MULTILIB := 64
LOCAL_VENDOR_MODULE := true
LOCAL_STATIC_LIBRARIES += libgtest_main libgtest libgmock \
			  android.hardware.camera.common@1.0-helper android.hardware.graphics.mapper@2.0


LOCAL_CPPFLAGS := $(LOG_FLAGS) $(WARNING_LEVEL) $(DEBUG_FLAGS) $(VERSION_FLAGS)

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH) \
    vendor/intel/external/project-celadon/libvhal-client \
    vendor/intel/external/project-celadon/camera-vhal/include \
    external/libjpeg-turbo \
    external/libexif \
    external/libyuv/files/include \
    frameworks/native/include/media/hardware \
    hardware/intel/libva \
    hardware/intel/onevpl/api/vpl \
    hardware/libhardware/modules/gralloc \
    $(LOCAL_PATH)/include \
    $(call include-path-for, camera) \
    external/gtest/include \
    external/gtest \
    bionic

LOCAL_MODULE:= CameraFixtureTest

include $(BUILD_EXECUTABLE)
