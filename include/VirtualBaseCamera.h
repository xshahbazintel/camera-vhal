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

#ifndef HW_EMULATOR_CAMERA_VIRTUALD_BASE_CAMERA_H
#define HW_EMULATOR_CAMERA_VIRTUALD_BASE_CAMERA_H

#include <hardware/camera_common.h>
#include <utils/Errors.h>

namespace android {

/*
 * Contains declaration of a class VirtualBaseCamera that encapsulates
 * functionality common to all virtual camera device versions ("fake",
 * "webcam", "video file", etc.).  Instances of this class (for each virtual
 * camera) are created during the construction of the VirtualCameraFactory
 * instance.  This class serves as an entry point for all camera API calls that
 * are common across all versions of the camera_device_t/camera_module_t
 * structures.
 */

class VirtualBaseCamera {
public:
    VirtualBaseCamera(int cameraId, uint32_t cameraVersion, struct hw_device_t *device,
                      struct hw_module_t *module);

    virtual ~VirtualBaseCamera();

    /****************************************************************************
     * Public API
     ***************************************************************************/

public:
    /* Initializes VirtualCamera instance.
     * Return:
     *  NO_ERROR on success, or an appropriate error status on failure.
     */
    virtual status_t Initialize() = 0;

    /****************************************************************************
     * Camera API implementation
     ***************************************************************************/

public:
    /* Creates connection to the virtual camera device.
     * This method is called in response to hw_module_methods_t::open callback.
     * NOTE: When this method is called the object is locked.
     * Note that failures in this method are reported as negative EXXX statuses.
     */
    virtual status_t openCamera(hw_device_t **device) = 0;

    /* Closes connection to the virtual camera.
     * This method is called in response to camera_device::close callback.
     * NOTE: When this method is called the object is locked.
     * Note that failures in this method are reported as negative EXXX statuses.
     */
    virtual status_t closeCamera() = 0;

    /* Gets camera information.
     * This method is called in response to camera_module_t::get_camera_info
     * callback.
     * NOTE: When this method is called the object is locked.
     * Note that failures in this method are reported as negative EXXX statuses.
     */
    virtual status_t getCameraInfo(struct camera_info *info) = 0;

    /****************************************************************************
     * Data members
     ***************************************************************************/

    virtual status_t setCameraFD(int socketFd);
    virtual status_t cleanCameraFD(int socketFd);

protected:
    /* Fixed camera information for camera2 devices. Must be valid to access if
     * mCameraDeviceVersion is >= HARDWARE_DEVICE_API_VERSION(2,0)  */
    camera_metadata_t *mCameraInfo = nullptr;

    /* Zero-based ID assigned to this camera. */
    int mCameraID;
    int mCameraSocketFD = -1;

private:
    /* Version of the camera device HAL implemented by this camera */
    int mCameraDeviceVersion;
};

} /* namespace android */

#endif /* HW_EMULATOR_CAMERA_VIRTUALD_BASE_CAMERA_H */
