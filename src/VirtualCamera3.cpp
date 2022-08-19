/*
 * Copyright (C) 2013 The Android Open Source Project
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

/**
 * Contains implementation of a class VirtualCamera that encapsulates
 * functionality common to all version 3.0 virtual camera devices.  Instances
 * of this class (for each virtual camera) are created during the construction
 * of the VirtualCameraFactory instance.  This class serves as an entry point
 * for all camera API calls that defined by camera3_device_ops_t API.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "VirtualCamera3_Camera"
#include <log/log.h>

#include "VirtualCamera3.h"
#include "system/camera_metadata.h"

namespace android {

/**
 * Constructs VirtualCamera3 instance.
 * Param:
 *  cameraId - Zero based camera identifier, which is an index of the camera
 *      instance in camera factory's array.
 *  module - Virtual camera HAL module descriptor.
 */
VirtualCamera3::VirtualCamera3(int cameraId, struct hw_module_t *module)
    : VirtualBaseCamera(cameraId, CAMERA_DEVICE_API_VERSION_3_3, &common, module),
      mStatus(STATUS_ERROR) {
    common.close = VirtualCamera3::close;
    ops = &sDeviceOps;

    mCallbackOps = NULL;
}

/* Destructs VirtualCamera3 instance. */
VirtualCamera3::~VirtualCamera3() {}

/****************************************************************************
 * Abstract API
 ***************************************************************************/

/****************************************************************************
 * Public API
 ***************************************************************************/

status_t VirtualCamera3::Initialize() {
    ALOGV("%s", __FUNCTION__);

    mStatus = STATUS_CLOSED;
    return NO_ERROR;
}

/****************************************************************************
 * Camera API implementation
 ***************************************************************************/

status_t VirtualCamera3::openCamera(hw_device_t **device) {
    ALOGV("%s: E", __FUNCTION__);
    if (device == NULL) return BAD_VALUE;

    if (mStatus != STATUS_CLOSED) {
        ALOGE("%s: Trying to open a camera in state %d!", __FUNCTION__, mStatus);
        return INVALID_OPERATION;
    }

    *device = &common;
    mStatus = STATUS_OPEN;
    ALOGI("%s : Camera %d opened successfully..", __FUNCTION__, mCameraID);
    return NO_ERROR;
}

status_t VirtualCamera3::closeCamera() {
    mStatus = STATUS_CLOSED;
    ALOGI("%s : Camera %d closed successfully..", __FUNCTION__, mCameraID);
    return NO_ERROR;
}

status_t VirtualCamera3::getCameraInfo(struct camera_info *info) {
    return VirtualBaseCamera::getCameraInfo(info);
}

status_t VirtualCamera3::setTorchMode(const char* camera_id, bool enable) {
    return VirtualBaseCamera::setTorchMode(camera_id,enable);
}

/****************************************************************************
 * Camera Device API implementation.
 * These methods are called from the camera API callback routines.
 ***************************************************************************/

status_t VirtualCamera3::initializeDevice(const camera3_callback_ops *callbackOps) {
    if (callbackOps == NULL) {
        ALOGE("%s: NULL callback ops provided to HAL!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mStatus != STATUS_OPEN) {
        ALOGE("%s: Trying to initialize a camera in state %d!", __FUNCTION__, mStatus);
        return INVALID_OPERATION;
    }

    mCallbackOps = callbackOps;
    mStatus = STATUS_READY;

    return NO_ERROR;
}

status_t VirtualCamera3::configureStreams(camera3_stream_configuration *streamList) {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t VirtualCamera3::registerStreamBuffers(const camera3_stream_buffer_set *bufferSet) {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

const camera_metadata_t *VirtualCamera3::constructDefaultRequestSettings(int type) {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return NULL;
}

status_t VirtualCamera3::processCaptureRequest(camera3_capture_request *request) {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t VirtualCamera3::flush() {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

/** Debug methods */

void VirtualCamera3::dump(int fd) {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return;
}

/****************************************************************************
 * Protected API. Callbacks to the framework.
 ***************************************************************************/

void VirtualCamera3::sendCaptureResult(camera3_capture_result_t *result) {
    mCallbackOps->process_capture_result(mCallbackOps, result);
}

void VirtualCamera3::sendNotify(camera3_notify_msg_t *msg) {
    mCallbackOps->notify(mCallbackOps, msg);
}

/****************************************************************************
 * Private API.
 ***************************************************************************/

/****************************************************************************
 * Camera API callbacks as defined by camera3_device_ops structure.  See
 * hardware/libhardware/include/hardware/camera3.h for information on each
 * of these callbacks. Implemented in this class, these callbacks simply
 * dispatch the call into an instance of VirtualCamera3 class defined by the
 * 'camera_device3' parameter, or set a member value in the same.
 ***************************************************************************/

VirtualCamera3 *getInstance(const camera3_device_t *d) {
    const VirtualCamera3 *cec = static_cast<const VirtualCamera3 *>(d);
    return const_cast<VirtualCamera3 *>(cec);
}

int VirtualCamera3::initialize(const struct camera3_device *d,
                               const camera3_callback_ops_t *callback_ops) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->initializeDevice(callback_ops);
}

int VirtualCamera3::configure_streams(const struct camera3_device *d,
                                      camera3_stream_configuration_t *stream_list) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->configureStreams(stream_list);
}

int VirtualCamera3::register_stream_buffers(const struct camera3_device *d,
                                            const camera3_stream_buffer_set_t *buffer_set) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->registerStreamBuffers(buffer_set);
}

int VirtualCamera3::process_capture_request(const struct camera3_device *d,
                                            camera3_capture_request_t *request) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->processCaptureRequest(request);
}

const camera_metadata_t *VirtualCamera3::construct_default_request_settings(
    const camera3_device_t *d, int type) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->constructDefaultRequestSettings(type);
}

void VirtualCamera3::dump(const camera3_device_t *d, int fd) {
    VirtualCamera3 *ec = getInstance(d);
    ec->dump(fd);
}

int VirtualCamera3::flush(const camera3_device_t *d) {
    VirtualCamera3 *ec = getInstance(d);
    return ec->flush();
}

int VirtualCamera3::close(struct hw_device_t *device) {
    VirtualCamera3 *ec =
        static_cast<VirtualCamera3 *>(reinterpret_cast<camera3_device_t *>(device));
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera3 device", __FUNCTION__);
        return BAD_VALUE;
    }
    return ec->closeCamera();
}

camera3_device_ops_t VirtualCamera3::sDeviceOps = {
    VirtualCamera3::initialize,
    VirtualCamera3::configure_streams,
    /* DEPRECATED: register_stream_buffers */ nullptr,
    VirtualCamera3::construct_default_request_settings,
    VirtualCamera3::process_capture_request,
    /* DEPRECATED: get_metadata_vendor_tag_ops */ nullptr,
    VirtualCamera3::dump,
    VirtualCamera3::flush};

const char *VirtualCamera3::sAvailableCapabilitiesStrings[NUM_CAPABILITIES] = {
    "BACKWARD_COMPATIBLE",
    "MANUAL_SENSOR",
    "MANUAL_POST_PROCESSING",
    "RAW",
    "PRIVATE_REPROCESSING",
    "READ_SENSOR_SETTINGS",
    "BURST_CAPTURE",
    "YUV_REPROCESSING",
    "DEPTH_OUTPUT",
    "CONSTRAINED_HIGH_SPEED_VIDEO",
    "FULL_LEVEL"};

}; /* namespace android */
