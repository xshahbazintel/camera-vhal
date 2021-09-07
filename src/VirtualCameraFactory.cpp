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
      mVirtualCameraNum(0),
      mFakeCameraNum(0),
      mConstructedOK(false),
      mCallbacks(nullptr) {
    /*
     * Figure out how many cameras need to be created, so we can allocate the
     * array of virtual cameras before populating it.
     */
    int virtualCamerasSize = 0;

    mCameraSessionState = socket::CameraSessionState::kNone;

    waitForRemoteSfFakeCameraPropertyAvailable();
    // Fake Cameras
    if (isFakeCameraEmulationOn(/* backCamera */ true)) {
        mFakeCameraNum++;
    }
    if (isFakeCameraEmulationOn(/* backCamera */ false)) {
        mFakeCameraNum++;
    }
    virtualCamerasSize += mFakeCameraNum;

    /*
     * We have the number of cameras we need to create, now allocate space for
     * them.
     */
    mVirtualCameras = new VirtualBaseCamera *[virtualCamerasSize];
    if (mVirtualCameras == nullptr) {
        ALOGE("%s: Unable to allocate virtual camera array for %d entries", __FUNCTION__,
              mVirtualCameraNum);
        return;
    }
    if (mVirtualCameras != nullptr) {
        for (int n = 0; n < virtualCamerasSize; n++) {
            mVirtualCameras[n] = nullptr;
        }
    }

    readSystemProperties();

    if (gIsInFrameH264) {
        // create decoder
        ALOGV("%s Creating decoder.", __func__);
        mDecoder = std::make_shared<CGVideoDecoder>();
    }

    // create socket server who push packets to decoder
    createSocketServer(mDecoder);
    ALOGV("%s socket server created: ", __func__);

    // Create fake cameras, if enabled.
    if (isFakeCameraEmulationOn(/* backCamera */ true)) {
        createFakeCamera(mSocketServer, mDecoder, /* backCamera */ true);
    }
    if (isFakeCameraEmulationOn(/* backCamera */ false)) {
        createFakeCamera(mSocketServer, mDecoder, /* backCamera */ false);
    }

    ALOGI("%d cameras are being virtual. %d of them are fake cameras.", mVirtualCameraNum,
          mFakeCameraNum);

    mConstructedOK = true;
}

bool VirtualCameraFactory::createSocketServer(std::shared_ptr<CGVideoDecoder> decoder) {
    ALOGV("%s: E", __FUNCTION__);

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
        for (int n = 0; n < mVirtualCameraNum; n++) {
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

void VirtualCameraFactory::createFakeCamera(std::shared_ptr<CameraSocketServerThread> socket_server,
                                            std::shared_ptr<CGVideoDecoder> decoder,
                                            bool backCamera) {
    int halVersion = getCameraHalVersion(backCamera);

    /*
     * Create and initialize the fake camera, using the index into
     * mVirtualCameras as the camera ID.
     */
    switch (halVersion) {
        case 1:
        case 2:
            ALOGE("%s: Unuspported Camera HAL version. Only HAL version 3 is supported.", __func__);
            break;
        case 3: {
            mVirtualCameras[mVirtualCameraNum] =
                new VirtualFakeCamera3(mVirtualCameraNum, backCamera, &HAL_MODULE_INFO_SYM.common,
                                       socket_server, decoder, std::ref(mCameraSessionState));
        } break;
        default:
            ALOGE("%s: Unknown %s camera hal version requested: %d", __FUNCTION__,
                  backCamera ? "back" : "front", halVersion);
    }

    if (mVirtualCameras[mVirtualCameraNum] == nullptr) {
        ALOGE("%s: Unable to instantiate fake camera class", __FUNCTION__);
    } else {
        ALOGV("%s: %s camera device version is %d", __FUNCTION__, backCamera ? "Back" : "Front",
              halVersion);
        status_t res = mVirtualCameras[mVirtualCameraNum]->Initialize(nullptr, nullptr, nullptr);
        if (res == NO_ERROR) {
            ALOGI("%s: Initialization for %s Camera ID: %d completed sucessfully..", __FUNCTION__,
                  backCamera ? "Back" : "Front", mVirtualCameraNum);
            // Camera creation and initialization was successful.
            mVirtualCameraNum++;
        } else {
            ALOGE("%s: Unable to initialize %s camera %d: %s (%d)", __FUNCTION__,
                  backCamera ? "back" : "front", mVirtualCameraNum, strerror(-res), res);
            delete mVirtualCameras[mVirtualCameraNum];
        }
    }
}

void VirtualCameraFactory::waitForRemoteSfFakeCameraPropertyAvailable() {
    /*
     * Camera service may start running before remote-props sets
     * remote.sf.fake_camera to any of the follwing four values:
     * "none,front,back,both"; so we need to wait.
     *
     * android/camera/camera-service.c
     * bug: 30768229
     */
    int numAttempts = 100;
    char prop[PROPERTY_VALUE_MAX];
    bool timeout = true;
    for (int i = 0; i < numAttempts; ++i) {
        if (property_get("remote.sf.fake_camera", prop, nullptr) != 0) {
            timeout = false;
            break;
        }
        usleep(5000);
    }
    if (timeout) {
        ALOGE("timeout (%dms) waiting for property remote.sf.fake_camera to be set\n",
              5 * numAttempts);
    }
}

bool VirtualCameraFactory::isFakeCameraEmulationOn(bool backCamera) {
    /*
     * Defined by 'remote.sf.fake_camera' boot property. If the property exists,
     * and if it's set to 'both', then fake cameras are used to emulate both
     * sides. If it's set to 'back' or 'front', then a fake camera is used only
     * to emulate the back or front camera, respectively.
     */
    char prop[PROPERTY_VALUE_MAX];
    if ((property_get("remote.sf.fake_camera", prop, nullptr) > 0) &&
        (!strcmp(prop, "both") || !strcmp(prop, backCamera ? "back" : "front"))) {
        return true;
    } else {
        return false;
    }
}

int VirtualCameraFactory::getCameraHalVersion(bool backCamera) {
    /*
     * Defined by 'remote.sf.front_camera_hal_version' and
     * 'remote.sf.back_camera_hal_version' boot properties. If the property
     * doesn't exist, it is assumed we are working with HAL v1.
     */
    char prop[PROPERTY_VALUE_MAX];
    const char *propQuery = backCamera ? "remote.sf.back_camera_hal" : "remote.sf.front_camera_hal";
    if (property_get(propQuery, prop, nullptr) > 0) {
        char *propEnd = prop;
        int val = strtol(prop, &propEnd, 10);
        if (*propEnd == '\0') {
            return val;
        }
        // Badly formatted property. It should just be a number.
        ALOGE("remote.sf.back_camera_hal is not a number: %s", prop);
    }
    return 3;
}

/********************************************************************************
 * Initializer for the static member structure.
 *******************************************************************************/

// Entry point for camera HAL API.
struct hw_module_methods_t VirtualCameraFactory::mCameraModuleMethods = {
    .open = VirtualCameraFactory::device_open};

};  // end of namespace android
