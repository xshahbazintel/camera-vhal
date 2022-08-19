# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#ifeq ($(TARGET_USE_CAMERA_VHAL), true)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

ifneq ($(TARGET_BOARD_PLATFORM), celadon)
####### Build FFmpeg modules from prebuilt libs #########

FFMPEG_PREBUILD := prebuilts/ffmpeg-4.2.2/android-x86_64
FFMPEG_LIB_PATH := ${FFMPEG_PREBUILD}/lib

include $(CLEAR_VARS)
LOCAL_MODULE				:= libavcodec
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB 				:= 64
LOCAL_SRC_FILES 			:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libswresample
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libavutil
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB 				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libavdevice
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libavfilter
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libavformat
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE				:= libswscale
LOCAL_CHECK_ELF_FILES                   := false
LOCAL_MULTILIB				:= 64
LOCAL_SRC_FILES				:= $(FFMPEG_LIB_PATH)/$(LOCAL_MODULE).so
LOCAL_PROPRIETARY_MODULE	:= true
LOCAL_MODULE_SUFFIX			:= .so
LOCAL_MODULE_CLASS			:= SHARED_LIBRARIES
include $(BUILD_PREBUILT)
##########################################################
endif

include $(CLEAR_VARS)

##################### Build camera-vhal #######################

ifeq ($(TARGET_BOARD_PLATFORM), celadon)
LOCAL_MODULE		:= camera.$(TARGET_BOARD_PLATFORM)
else
LOCAL_MODULE		:= camera.$(TARGET_PRODUCT)
endif

LOCAL_MULTILIB 		:= 64
LOCAL_VENDOR_MODULE := true

camera_vhal_src := \
	src/VirtualCameraHal.cpp \
	src/VirtualCameraFactory.cpp \
	src/VirtualBaseCamera.cpp \
	src/Converters.cpp \
	src/NV21JpegCompressor.cpp \
	src/fake-pipeline2/Scene.cpp \
	src/fake-pipeline2/Sensor.cpp \
	src/fake-pipeline2/JpegCompressor.cpp \
	src/VirtualCamera3.cpp \
	src/VirtualFakeCamera3.cpp \
	src/Exif.cpp \
	src/Thumbnail.cpp \
	src/CameraSocketServerThread.cpp \
	src/CameraSocketCommand.cpp
ifneq ($(TARGET_BOARD_PLATFORM), celadon)
camera_vhal_src += src/CGCodec.cpp
endif
camera_vhal_c_includes := external/libjpeg-turbo \
	external/libexif \
	external/libyuv/files/include \
	frameworks/native/include/media/hardware \
	hardware/libhardware/modules/gralloc \
	$(LOCAL_PATH)/include \
	$(LOCAL_PATH)/$(FFMPEG_PREBUILD)/include \
	$(call include-path-for, camera)

camera_vhal_shared_libraries := \
    libexif \
    liblog \
    libutils \
    libcutils \
    libui \
    libdl \
    libjpeg \
    libcamera_metadata \
    libhardware \
    libsync 

ifneq ($(TARGET_BOARD_PLATFORM), celadon)
camera_vhal_shared_libraries +=     libavcodec    \
    libavdevice   \
    libavfilter   \
    libavformat   \
    libavutil     \
    libswresample \
    libswscale
endif

camera_vhal_static_libraries := \
	android.hardware.camera.common@1.0-helper \
	libyuv_static

camera_vhal_module_relative_path := hw
camera_vhal_cflags				 := -fno-short-enums -DREMOTE_HARDWARE
camera_vhal_cflags				 += -Wno-unused-parameter -Wno-missing-field-initializers
camera_vhal_clang_flags			 := -Wno-c++11-narrowing -Werror -Wno-unknown-pragmas

ifeq ($(BOARD_USES_GRALLOC1), true)
camera_vhal_cflags += -DUSE_GRALLOC1
endif

ifeq ($(TARGET_BOARD_PLATFORM), celadon)
camera_vhal_cflags += -DGRALLOC_MAPPER4
else
camera_vhal_cflags += -DENABLE_FFMPEG
endif

LOCAL_MODULE_RELATIVE_PATH	:= ${camera_vhal_module_relative_path}
LOCAL_CFLAGS				:= ${camera_vhal_cflags}
LOCAL_CPPFLAGS 				+= -std=c++17
LOCAL_CLANG_CFLAGS			+= ${camera_vhal_clang_flags}

LOCAL_C_INCLUDES			+= ${camera_vhal_c_includes}
LOCAL_SRC_FILES 			:= ${camera_vhal_src}
LOCAL_SHARED_LIBRARIES 		:= ${camera_vhal_shared_libraries}
LOCAL_STATIC_LIBRARIES 		:= ${camera_vhal_static_libraries}

include $(BUILD_SHARED_LIBRARY)

#####################################################

include $(CLEAR_VARS)

################ Build JPEG Library #################

LOCAL_VENDOR_MODULE := true
LOCAL_MULTILIB := 64

jpeg_module_relative_path := hw
jpeg_cflags := -fno-short-enums -DREMOTE_HARDWARE
jpeg_cflags += -Wno-unused-parameter
LOCAL_CPPFLAGS += -std=c++17
jpeg_clang_flags += -Wno-c++11-narrowing
jpeg_shared_libraries := \
    libcutils \
    libexif \
    libjpeg \
    liblog \

jpeg_c_includes := external/libjpeg-turbo \
                   external/libexif \
                   frameworks/native/include \
	           $(LOCAL_PATH)/include \
	           $(LOCAL_PATH)/include/jpeg-stub \

jpeg_src := \
    src/jpeg-stub/Compressor.cpp \
    src/jpeg-stub/JpegStub.cpp \


LOCAL_MODULE_RELATIVE_PATH := ${jpeg_module_relative_path}
LOCAL_CFLAGS += ${jpeg_cflags}
LOCAL_CLANG_CFLAGS += ${jpeg_clangflags}


LOCAL_SHARED_LIBRARIES := ${jpeg_shared_libraries}
LOCAL_C_INCLUDES += ${jpeg_c_includes}
LOCAL_SRC_FILES := ${jpeg_src}

ifeq ($(TARGET_BOARD_PLATFORM), celadon)
LOCAL_MODULE		:= camera.$(TARGET_BOARD_PLATFORM).jpeg
else
LOCAL_MODULE := camera.$(TARGET_PRODUCT).jpeg
endif

include $(BUILD_SHARED_LIBRARY)

######################################################

#endif # TARGET_USE_CAMERA_VHAL
