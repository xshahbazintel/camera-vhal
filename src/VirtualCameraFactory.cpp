/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2019-2022 Intel Corporation
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Contains implementation of a class VirtualCameraFactory that manages cameras
 * available for emulation.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "VirtualCamera_Factory"

#include "VirtualBuffer.h"
#include "VirtualCameraFactory.h"
#include "VirtualFakeCamera3.h"
#include "ClientCommunicator.h"
#include "onevpl-video-decode/MfxDecoder.h"

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

void VirtualCameraFactory::readSystemProperties() {
    char prop_val[PROPERTY_VALUE_MAX] = {'\0'};

    property_get("ro.vendor.camera.in_frame_format.h264", prop_val, "false");
    gIsInFrameH264 = !strcmp(prop_val, "true");

    property_get("ro.vendor.camera.in_frame_format.i420", prop_val, "false");
    gIsInFrameI420 = !strcmp(prop_val, "true");

    mNumClients = 1;
    if (property_get("ro.concurrent.user.num", prop_val, "") > 0){
        int num = atoi(prop_val);
        if (num > 1 && num <= MAX_CONCURRENT_USER_NUM){
            mNumClients = num;
            ALOGI("%s: Support %d concurrent multi users", __FUNCTION__, num);
        } else if (num == 1) {
            ALOGI("%s: Support only single user", __FUNCTION__);
        } else {
            ALOGE("%s: Unsupported number of multi-user request(%d), please check it again",
                  __FUNCTION__, num);
        }
    }
    ALOGI("%s - gIsInFrameH264: %d, gIsInFrameI420: %d, mNumClients: %d",
          __func__, gIsInFrameH264, gIsInFrameI420, mNumClients);
}

VirtualCameraFactory::VirtualCameraFactory()
    : mConstructedOK(false),
      mCallbacks(nullptr),
      mNumClients(1) {
    mVirtualCameras.clear();

    readSystemProperties();

    // Create socket listener which accepts client connections.
    createSocketListener();
    ALOGV("%s socket listener created: ", __func__);

    mClientCameras.resize(mNumClients);
    mClientThreads.resize(mNumClients);
    // Create cameras based on the client request.
    for(int id = 0; id < mNumClients; id++)
    {
        // NV12 Decoder
        std::shared_ptr<MfxDecoder> decoder;
        if (gIsInFrameH264) {
            // create decoder
            ALOGV("%s Creating decoder.", __func__);
            decoder = std::make_shared<MfxDecoder>();
        }
        auto client_thread = std::make_shared<ClientCommunicator>(mSocketListener, decoder, id);
        mClientThreads[id] = client_thread;
    }

    ALOGI("%s: Cameras will be initialized dynamically when client connects", __FUNCTION__);

    mConstructedOK = true;
}

bool VirtualCameraFactory::createSocketListener() {
    ALOGV("%s: E", __FUNCTION__);

    char id[PROPERTY_VALUE_MAX] = {0};
    // This property is used by gtest, set this before running gtest
    if (property_get("ro.boot.container.testid", id, "") > 0) {
        mSocketListener = std::make_shared<ConnectionsListener>(id);
        mSocketListener->run("ConnectionsListener");
    } else if (property_get("ro.boot.container.id", id, "") > 0) {
        mSocketListener = std::make_shared<ConnectionsListener>(id);
        mSocketListener->run("ConnectionsListener");
    } else
        ALOGE("%s: FATAL: container id is not set!!", __func__);

    ALOGV("%s: X", __FUNCTION__);
    // TODO need to return false if error.
    return true;
}

VirtualCameraFactory::~VirtualCameraFactory() {
    for (auto it = mVirtualCameras.begin(); it != mVirtualCameras.end(); it++) {
        delete it->second;
        it->second = nullptr;
    }
    mVirtualCameras.clear();

    if (mSocketListener) {
        mSocketListener->requestExit();
        mSocketListener->join();
    }
}

bool VirtualCameraFactory::IsClientCapabilityValid(int clientId) {
    return mClientThreads[clientId]->IsValidClientCapInfo();
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

void VirtualCameraFactory::clearCameraInfo(int clientId){
    for (int cameraId : mClientCameras[clientId]) {
        if (mVirtualCameras[cameraId] != nullptr) {
            mCallbacks->camera_device_status_change(mCallbacks, cameraId, CAMERA_DEVICE_STATUS_NOT_PRESENT);
            delete mVirtualCameras[cameraId];
            //Entry for CameraId is not removed in order to keep the number of cameras count intact such that we never hit the condition of
            //cameraId being graterthan or equalto number of cameras. It is made to point to nullptr.
            mVirtualCameras[cameraId] = nullptr;
        }
    }
    mClientCameras[clientId].clear();
}

/********************************************************************************
 * Internal API
 *******************************************************************************/

bool VirtualCameraFactory::createVirtualRemoteCamera(
    std::shared_ptr<MfxDecoder> decoder,
    int clientId,
    android::socket::camera_info_t cameraInfo) {
    ALOGV("%s: E", __FUNCTION__);
    int cameraId = mVirtualCameras.size();
    std::shared_ptr<ClientCommunicator> client_thread = mClientThreads[clientId];
    for (int id = 0; id < (int)mVirtualCameras.size(); id++) {
        if (mVirtualCameras[id] == nullptr) {
            ALOGI("%s:CameraId is set to %d", __FUNCTION__, id);
            cameraId = id;
            break;
        }
    }

    mVirtualCameras[cameraId] = new VirtualFakeCamera3(cameraId, &HAL_MODULE_INFO_SYM.common, client_thread, decoder, cameraInfo);
    if (mVirtualCameras[cameraId] == nullptr) {
        ALOGE("%s: Unable to instantiate fake camera class", __FUNCTION__);
    } else {
        mVirtualCameras[cameraId]->setUserId(clientId);
        for (int id : mClientCameras[clientId]) {
            mVirtualCameras[cameraId]->setConflictingCameras(id);
            mVirtualCameras[id]->setConflictingCameras(cameraId);
        }
        mClientCameras[clientId].push_back(cameraId);
        status_t res = mVirtualCameras[cameraId]->Initialize();
        if (res == NO_ERROR) {
            ALOGI("%s: Initialization for Camera ID: %d completed successfully..", __FUNCTION__,
                  cameraId);
            // Camera creation and initialization was successful.
            mCallbacks->camera_device_status_change(mCallbacks, cameraId, CAMERA_DEVICE_STATUS_PRESENT);
            return true;
        } else {
            ALOGE("%s: Unable to initialize camera %d: %s (%d)", __FUNCTION__,
                  cameraId, strerror(-res), res);
            delete mVirtualCameras[cameraId];
            mVirtualCameras[cameraId] = nullptr;
        }
    }
    return false;
}

/********************************************************************************
 * Initializer for the static member structure.
 *******************************************************************************/

// Entry point for camera HAL API.
struct hw_module_methods_t VirtualCameraFactory::mCameraModuleMethods = {
    .open = VirtualCameraFactory::device_open};

};  // end of namespace android
