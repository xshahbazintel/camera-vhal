/*
 * Copyright (C) 2012 The Android Open Source Project
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
 * Contains implementation of a class VirtualBaseCamera that encapsulates
 * functionality common to all virtual camera device versions ("fake",
 * "webcam", "video file", "cam2.0" etc.).  Instances of this class (for each
 * virtual camera) are created during the construction of the
 * VirtualCameraFactory instance.  This class serves as an entry point for all
 * camera API calls that are common across all versions of the
 * camera_device_t/camera_module_t structures.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "VirtualCamera_BaseCamera"
#include <log/log.h>

#include "VirtualBaseCamera.h"

namespace android {

VirtualBaseCamera::VirtualBaseCamera(int cameraId, uint32_t cameraVersion,
                                     struct hw_device_t *device, struct hw_module_t *module)
    : mCameraID(cameraId), mCameraDeviceVersion(cameraVersion) {
    /*
     * Initialize camera_device descriptor for this object.
     */

    /* Common header */
    device->tag = HARDWARE_DEVICE_TAG;
    device->version = cameraVersion;
    device->module = module;
    device->close = NULL;  // Must be filled in by child implementation
}

VirtualBaseCamera::~VirtualBaseCamera() {}

status_t VirtualBaseCamera::getCameraInfo(struct camera_info *info) {
    ALOGV("%s", __FUNCTION__);

    info->device_version = mCameraDeviceVersion;
    if (mCameraDeviceVersion >= HARDWARE_DEVICE_API_VERSION(2, 0)) {
        info->static_camera_characteristics = mCameraInfo;
    } else {
        info->static_camera_characteristics = (camera_metadata_t *)0xcafef00d;
    }

    return NO_ERROR;
}

status_t  VirtualBaseCamera::setTorchMode(const char* camera_id, bool enable){
    ALOGV("%s", __FUNCTION__);

    return OK;
}

status_t VirtualBaseCamera::setCameraFD(int socketFd) {
    mCameraSocketFD = socketFd;
    ALOGV("%s mCameraSocketFD = %d", __FUNCTION__, mCameraSocketFD);
    return NO_ERROR;
}

status_t VirtualBaseCamera::cleanCameraFD(int socketFd) {
    mCameraSocketFD = -1;
    ALOGV("%s Clean mCameraSocketFD. Now it is %d", __func__, mCameraSocketFD);
    return NO_ERROR;
}

} /* namespace android */
