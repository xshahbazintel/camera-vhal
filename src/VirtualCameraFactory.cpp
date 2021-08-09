/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Contains implementation of a class VirtualCameraFactory that manages cameras
 * available for emulation.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "VirtualCamera_Factory"

#include "VirtualCameraFactory.h"
#include "VirtualFakeCamera3.h"
#include "CameraSocketServerThread.h"
#include "CGCodec.h"

#include <log/log.h>
#include <cutils/properties.h>

extern camera_module_t HAL_MODULE_INFO_SYM;

/*
 * A global instance of VirtualCameraFactory is statically instantiated and
 * initialized when camera emulation HAL is loaded.
 */
android::VirtualCameraFactory gVirtualCameraFactory;

namespace android {

bool gIsInFrameI420;
bool gIsInFrameH264;
bool gUseVaapi;

void VirtualCameraFactory::readSystemProperties() {
    char prop_val[PROPERTY_VALUE_MAX] = {'\0'};

    property_get("ro.vendor.camera.in_frame_format.h264", prop_val, "false");
    gIsInFrameH264 = !strcmp(prop_val, "true");

    property_get("ro.vendor.camera.in_frame_format.i420", prop_val, "false");
    gIsInFrameI420 = !strcmp(prop_val, "true");

    property_get("ro.vendor.camera.decode.vaapi", prop_val, "false");
    gUseVaapi = !strcmp(prop_val, "true");

    ALOGI("%s - gIsInFrameH264: %d, gIsInFrameI420: %d, gUseVaapi: %d", __func__, gIsInFrameH264,
          gIsInFrameI420, gUseVaapi);
}

VirtualCameraFactory::VirtualCameraFactory()
    : mVirtualCameras(nullptr),
      mNumOfCamerasSupported(0),
      mConstructedOK(false),
      mCallbacks(nullptr) {
    readSystemProperties();

    if (gIsInFrameH264) {
        // Create decoder to decode H264/H265 input frames.
        ALOGV("%s Creating decoder.", __func__);
        mDecoder = std::make_shared<CGVideoDecoder>();
    }

    // Create socket server which is used to communicate with client device.
    createSocketServer(mDecoder);
    ALOGV("%s socket server created: ", __func__);

    // Check whether capability info is received or not and
    // wait until receive capability info from client HW.
    // Number of supported cameras and its corresponding
    // metadata info would be updated always based on this
    // capability info from the remote client HW.
    while (!gCapabilityInfoReceived) {
        ALOGV("%s: waiting for the capability info...", __func__);
        // 1ms sleep for this thread.
        std::this_thread::sleep_for(1ms);
    }

    ALOGV("%s: Received capability info from remote client device", __FUNCTION__);

    // Update number of cameras requested from remote client HW.
    mNumOfCamerasSupported = gMaxNumOfCamerasSupported;

    // Allocate space for each cameras requested.
    mVirtualCameras = new VirtualBaseCamera *[mNumOfCamerasSupported];
    if (mVirtualCameras == nullptr) {
        ALOGE("%s: Unable to allocate virtual camera array", __FUNCTION__);
        return;
    } else {
        for (int n = 0; n < mNumOfCamerasSupported; n++) {
            mVirtualCameras[n] = nullptr;
        }
    }

    // Create cameras based on the client request.
    for (int cameraId = 0; cameraId < mNumOfCamerasSupported; cameraId++) {
        // Wait until start updating metadata for each camera.
        while (!gStartMetadataUpdate) {
            ALOGV("%s: wait until start updating metadata for a single camera", __func__);
            // 200us sleep for this thread.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        createVirtualRemoteCamera(mSocketServer, mDecoder, cameraId);
        // Created a camera successfully hence update the status.
        gDoneMetadataUpdate = true;
        gStartMetadataUpdate = false;
    }

    ALOGI("%s: Total number of cameras supported: %d", __FUNCTION__, mNumOfCamerasSupported);

    mConstructedOK = true;
}

bool VirtualCameraFactory::createSocketServer(std::shared_ptr<CGVideoDecoder> decoder) {
    ALOGV("%s: E", __FUNCTION__);

    mCameraSessionState = socket::CameraSessionState::kNone;
    char id[PROPERTY_VALUE_MAX] = {0};
    if (property_get("ro.boot.container.id", id, "") > 0) {
        mSocketServer =
            std::make_shared<CameraSocketServerThread>(id, decoder, std::ref(mCameraSessionState));

        mSocketServer->run("FrontBackCameraSocketServerThread");
    } else
        ALOGE("%s: FATAL: container id is not set!!", __func__);

    ALOGV("%s: X", __FUNCTION__);
    // TODO need to return false if error.
    return true;
}

VirtualCameraFactory::~VirtualCameraFactory() {
    if (mVirtualCameras != nullptr) {
        for (int n = 0; n < mNumOfCamerasSupported; n++) {
            if (mVirtualCameras[n] != nullptr) {
                delete mVirtualCameras[n];
            }
        }
        delete[] mVirtualCameras;
    }

    if (mSocketServer) {
        mSocketServer->requestExit();
        mSocketServer->join();
    }
}

/******************************************************************************
 * Camera HAL API handlers.
 *
 * Each handler simply verifies existence of an appropriate VirtualBaseCamera
 * instance, and dispatches the call to that instance.
 *
 *****************************************************************************/

int VirtualCameraFactory::cameraDeviceOpen(int cameraId, hw_device_t **device) {
    ALOGI("%s: id = %d", __FUNCTION__, cameraId);

    *device = nullptr;

    if (!isConstructedOK()) {
        ALOGE("%s: VirtualCameraFactory has failed to initialize", __FUNCTION__);
        return -EINVAL;
    }

    if (cameraId < 0 || cameraId >= getVirtualCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, cameraId,
              getVirtualCameraNum());
        return -ENODEV;
    }

    return mVirtualCameras[cameraId]->openCamera(device);
}

int VirtualCameraFactory::getCameraInfo(int cameraId, struct camera_info *info) {
    ALOGI("%s: id = %d", __FUNCTION__, cameraId);

    if (!isConstructedOK()) {
        ALOGE("%s: VirtualCameraFactory has failed to initialize", __FUNCTION__);
        return -EINVAL;
    }

    if (cameraId < 0 || cameraId >= getVirtualCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, cameraId,
              getVirtualCameraNum());
        return -ENODEV;
    }

    return mVirtualCameras[cameraId]->getCameraInfo(info);
}

int VirtualCameraFactory::setCallbacks(const camera_module_callbacks_t *callbacks) {
    ALOGV("%s: callbacks = %p", __FUNCTION__, callbacks);

    mCallbacks = callbacks;

    return OK;
}

void VirtualCameraFactory::getVendorTagOps(vendor_tag_ops_t *ops) {
    ALOGV("%s: ops = %p", __FUNCTION__, ops);
    // No vendor tags defined for emulator yet, so not touching ops.
}

/****************************************************************************
 * Camera HAL API callbacks.
 ***************************************************************************/

int VirtualCameraFactory::device_open(const hw_module_t *module, const char *name,
                                      hw_device_t **device) {
    /*
     * Simply verify the parameters, and dispatch the call inside the
     * VirtualCameraFactory instance.
     */

    if (module != &HAL_MODULE_INFO_SYM.common) {
        ALOGE("%s: Invalid module %p expected %p", __FUNCTION__, module,
              &HAL_MODULE_INFO_SYM.common);
        return -EINVAL;
    }
    if (name == nullptr) {
        ALOGE("%s: NULL name is not expected here", __FUNCTION__);
        return -EINVAL;
    }

    return gVirtualCameraFactory.cameraDeviceOpen(atoi(name), device);
}

int VirtualCameraFactory::get_number_of_cameras() {
    return gVirtualCameraFactory.getVirtualCameraNum();
}

int VirtualCameraFactory::get_camera_info(int camera_id, struct camera_info *info) {
    return gVirtualCameraFactory.getCameraInfo(camera_id, info);
}

int VirtualCameraFactory::set_callbacks(const camera_module_callbacks_t *callbacks) {
    return gVirtualCameraFactory.setCallbacks(callbacks);
}

void VirtualCameraFactory::get_vendor_tag_ops(vendor_tag_ops_t *ops) {
    gVirtualCameraFactory.getVendorTagOps(ops);
}

int VirtualCameraFactory::open_legacy(const struct hw_module_t *module, const char *id,
                                      uint32_t halVersion, struct hw_device_t **device) {
    // Not supporting legacy open.
    return -ENOSYS;
}

/********************************************************************************
 * Internal API
 *******************************************************************************/

void VirtualCameraFactory::createVirtualRemoteCamera(
    std::shared_ptr<CameraSocketServerThread> socket_server,
    std::shared_ptr<CGVideoDecoder> decoder, int cameraId) {
    ALOGV("%s: E", __FUNCTION__);
    mVirtualCameras[cameraId] =
        new VirtualFakeCamera3(cameraId, &HAL_MODULE_INFO_SYM.common, socket_server, decoder,
                               std::ref(mCameraSessionState));

    if (mVirtualCameras[cameraId] == nullptr) {
        ALOGE("%s: Unable to instantiate fake camera class", __FUNCTION__);
    } else {
        status_t res = mVirtualCameras[cameraId]->Initialize();
        if (res == NO_ERROR) {
            ALOGI("%s: Initialization for %s Camera ID: %d completed successfully..", __FUNCTION__,
                  gCameraFacingBack ? "Back" : "Front", cameraId);
            // Camera creation and initialization was successful.
        } else {
            ALOGE("%s: Unable to initialize %s camera %d: %s (%d)", __FUNCTION__,
                  gCameraFacingBack ? "back" : "front", cameraId, strerror(-res), res);
            delete mVirtualCameras[cameraId];
        }
    }
}

/********************************************************************************
 * Initializer for the static member structure.
 *******************************************************************************/

// Entry point for camera HAL API.
struct hw_module_methods_t VirtualCameraFactory::mCameraModuleMethods = {
    .open = VirtualCameraFactory::device_open};

};  // end of namespace android
