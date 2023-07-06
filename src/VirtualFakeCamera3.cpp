/*
 * Copyright (C) 2013 The Android Open Source Project
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
 * Contains implementation of a class VirtualFakeCamera3 that encapsulates
 * functionality of an advanced fake camera.
 */

#include <inttypes.h>

//#define LOG_NNDEBUG 0
#define LOG_TAG "VirtualFakeCamera3: "
#include <cutils/properties.h>
#include <log/log.h>

#include <ui/Fence.h>
#include "GrallocModule.h"
#include "VirtualCameraFactory.h"
#include "VirtualFakeCamera3.h"

#include <cmath>
#include "fake-pipeline2/JpegCompressor.h"
#include "fake-pipeline2/Sensor.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>
#include "VirtualBuffer.h"

#if defined(LOG_NNDEBUG) && LOG_NNDEBUG == 0
#define ALOGVV ALOGV
#else
#define ALOGVV(...) ((void)0)
#endif

using namespace std;
using namespace chrono;
using namespace chrono_literals;

namespace android {

using namespace socket;
/**
 * Constants for camera capabilities
 */

const int64_t USEC = 1000LL;
const int64_t MSEC = USEC * 1000LL;

const int32_t VirtualFakeCamera3::kHalSupportedFormats[] = {
    HAL_PIXEL_FORMAT_BLOB,
    HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,  // defined as RGB32
    HAL_PIXEL_FORMAT_RGBA_8888,               // RGB32
    HAL_PIXEL_FORMAT_YCbCr_420_888,           // NV12
    HAL_PIXEL_FORMAT_YCrCb_420_SP,            // NV21
    // HAL_PIXEL_FORMAT_YV12 /* Not supporting now*/
};

/**
 * 3A constants
 */

// Default exposure and gain targets for different scenarios
const nsecs_t VirtualFakeCamera3::kNormalExposureTime = 10 * MSEC;
const nsecs_t VirtualFakeCamera3::kFacePriorityExposureTime = 30 * MSEC;
const int VirtualFakeCamera3::kNormalSensitivity = 100;
const int VirtualFakeCamera3::kFacePrioritySensitivity = 400;
// CTS requires 8 frames timeout in waitForAeStable
const float VirtualFakeCamera3::kExposureTrackRate = 0.2;
const int VirtualFakeCamera3::kPrecaptureMinFrames = 10;
const int VirtualFakeCamera3::kStableAeMaxFrames = 100;
const float VirtualFakeCamera3::kExposureWanderMin = -2;
const float VirtualFakeCamera3::kExposureWanderMax = 1;

/**
 * Camera device lifecycle methods
 */

VirtualFakeCamera3::VirtualFakeCamera3(int cameraId, struct hw_module_t *module,
                                       std::shared_ptr<ClientCommunicator> client_thread,
                                       std::shared_ptr<MfxDecoder> decoder,
                                       android::socket::camera_info_t clientCameraInfo)
    : VirtualCamera3(cameraId, module),
      mClientThread(client_thread),
      mDecoder(decoder),
      mClientCameraInfo(clientCameraInfo) {
    ALOGI("Constructing virtual fake camera 3: for ID %d", mCameraID);

    mControlMode = ANDROID_CONTROL_MODE_AUTO;
    mFacePriority = false;
    mAeMode = ANDROID_CONTROL_AE_MODE_ON;
    mAfMode = ANDROID_CONTROL_AF_MODE_AUTO;
    mAwbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    mAeState = ANDROID_CONTROL_AE_STATE_INACTIVE;
    mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
    mAwbState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
    mAeCounter = 0;
    mAeTargetExposureTime = kNormalExposureTime;
    mAeCurrentExposureTime = kNormalExposureTime;
    mAeCurrentSensitivity = kNormalSensitivity;
    mSensorWidth = 0;
    mSensorHeight = 0;
    mSrcWidth = 0;
    mSrcHeight = 0;
    mCodecType = 0;
    mDecoderResolution = 0;
    mFacingBack = false;
    mDecoderInitDone = false;
    mInputStream = NULL;
    mSensor = NULL;
    mReadoutThread = NULL;
    mJpegCompressor = NULL;
}

VirtualFakeCamera3::~VirtualFakeCamera3() {
    for (size_t i = 0; i < CAMERA3_TEMPLATE_COUNT; i++) {
        if (mDefaultTemplates[i] != NULL) {
            free_camera_metadata(mDefaultTemplates[i]);
        }
    }
}

status_t VirtualFakeCamera3::Initialize() {
    ALOGVV("%s: E", __FUNCTION__);
    status_t res;

    if (mStatus != STATUS_ERROR) {
        ALOGE("%s: Already initialized!", __FUNCTION__);
        return INVALID_OPERATION;
    }

    res = getCameraCapabilities();
    if (res != OK) {
        ALOGE("%s: Unable to get camera capabilities: %s (%d)", __FUNCTION__, strerror(-res), res);
        return res;
    }

    res = constructStaticInfo();
    if (res != OK) {
        ALOGE("%s: Unable to allocate static info: %s (%d)", __FUNCTION__, strerror(-res), res);
        return res;
    }

    return VirtualCamera3::Initialize();
}

status_t VirtualFakeCamera3::openCamera(hw_device_t **device) {
    ALOGI(LOG_TAG "%s: E", __FUNCTION__);
    Mutex::Autolock l(mLock);

    return VirtualCamera3::openCamera(device);
}

uint32_t VirtualFakeCamera3::setDecoderResolution(uint32_t resolution) {
    ALOGVV(LOG_TAG "%s: E", __FUNCTION__);
    uint32_t res = 0;
    switch (resolution) {
        case DECODER_SUPPORTED_RESOLUTION_480P:
            res = (uint32_t)FrameResolution::k480p;
            break;
        case DECODER_SUPPORTED_RESOLUTION_720P:
            res = (uint32_t)FrameResolution::k720p;
            break;
        case DECODER_SUPPORTED_RESOLUTION_1080P:
            res = (uint32_t)FrameResolution::k1080p;
            break;
        default:
            ALOGI("%s: Selected default 480p resolution!!!", __func__);
            res = (uint32_t)FrameResolution::k480p;
            break;
    }

    ALOGI("%s: Resolution selected for decoder init is %s", __func__, resolution_to_str(res));
    return res;
}
status_t VirtualFakeCamera3::sendCommandToClient(camera_cmd_t cmd) {
    ALOGI("%s E", __func__);

    status_t status = INVALID_OPERATION;
    size_t config_cmd_packet_size = sizeof(camera_header_t) + sizeof(camera_config_cmd_t);
    camera_config_cmd_t config_cmd = {};
    config_cmd.version = CAMERA_VHAL_VERSION_2;
    config_cmd.cmd = cmd;
    config_cmd.config.cameraId = mClientCameraInfo.cameraId;
    config_cmd.config.codec_type = mCodecType;
    config_cmd.config.resolution = mDecoderResolution;

    camera_packet_t *config_cmd_packet = NULL;

    int client_id = mClientThread->getClientId();

    config_cmd_packet = (camera_packet_t *)malloc(config_cmd_packet_size);
    if (config_cmd_packet == NULL) {
        ALOGE(LOG_TAG "%s: config camera_packet_t allocation failed: %d ", __FUNCTION__, __LINE__);
        goto out;
    }

    config_cmd_packet->header.type = CAMERA_CONFIG;
    config_cmd_packet->header.size = sizeof(camera_config_cmd_t);
    memcpy(config_cmd_packet->payload, &config_cmd, sizeof(camera_config_cmd_t));

    ALOGI("%s: Client(%d) Sending cmd %s cameraId(%d)", __FUNCTION__, client_id,
          (cmd == camera_cmd_t::CMD_CLOSE) ? "CloseCamera" : "OpenCamera", config_cmd.config.cameraId);
    if (!mClientThread->sendCommandToClient(config_cmd_packet, config_cmd_packet_size)) {
        goto out;
    }

    ALOGI("%s: Sent cmd %s to client Id %d!", __FUNCTION__,
          (cmd == camera_cmd_t::CMD_CLOSE) ? "CloseCamera" : "OpenCamera", client_id);
    status = OK;
out:
    free(config_cmd_packet);
    return status;
}

status_t VirtualFakeCamera3::connectCamera() {
    ALOGI(LOG_TAG "%s: E", __FUNCTION__);

    if (gIsInFrameH264) {
        mDecoderResolution = setDecoderResolution(mSrcHeight);
        // initialize decoder
        mfxStatus ret = MFX_ERR_NONE;
        ret = mDecoder->Init(mCodecType, mSrcWidth, mSrcHeight);
        if (ret == MFX_ERR_NONE) {
            mDecoderInitDone = true;
            ALOGI("%s Video mfx init success!!!", __func__);
        } else {
            ALOGE("%s Video mfx init failed", __func__);
        }
    }

    ALOGI("%s Calling sendCommandToClient", __func__);
    status_t ret;
    if ((ret = sendCommandToClient(camera_cmd_t::CMD_OPEN)) != OK) {
        ALOGE("%s sendCommandToClient failed", __func__);
        return ret;
    }
    ALOGI("%s Called sendCommandToClient", __func__);
    mClientThread->mCameraSessionState = CameraSessionState::kCameraOpened;


    // Fill the input buffer with black frame to avoid green frame
    // while changing the resolution in each request.
    mCameraBuffer = std::make_shared<ClientVideoBuffer>(mSrcWidth, mSrcHeight);
    mCameraBuffer->clearBuffer();
    mClientThread->mCameraBuffer = mCameraBuffer;
    ALOGI("%s: Camera input buffers are cleared", __func__);
    // create sensor who gets decoded frames and forwards them to framework
    mSensor = new Sensor(mCameraID, mSrcWidth, mSrcHeight, mDecoder, mCameraBuffer);
    mSensor->setSensorListener(this);

    status_t res = mSensor->startUp();
    if (res != NO_ERROR) return res;

    mReadoutThread = new ReadoutThread(this);
    mJpegCompressor = new JpegCompressor();

    res = mReadoutThread->run("EmuCam3::readoutThread");
    if (res != NO_ERROR) return res;

    // Initialize fake 3A

    mControlMode = ANDROID_CONTROL_MODE_AUTO;
    mFacePriority = false;
    mAeMode = ANDROID_CONTROL_AE_MODE_ON;
    mAfMode = ANDROID_CONTROL_AF_MODE_AUTO;
    mAwbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    mAeState = ANDROID_CONTROL_AE_STATE_INACTIVE;
    mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
    mAwbState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
    mAeCounter = 0;
    mAeTargetExposureTime = kNormalExposureTime;
    mAeCurrentExposureTime = kNormalExposureTime;
    mAeCurrentSensitivity = kNormalSensitivity;

    return OK;
}

/**
 * @brief  Closes Camera session.
 * 1. Shutdown Sensor thread and purge all framework buffers.
 * 2. Set Camera Session state to Close and wait for ClientCommunicator to stop decoding.
 * 3. Release ownershipt from this (VirtualFakeCamera3) object.
 * 4. Send Close command to Client for stopping camera feed.
 *
 * @return status_t::OK if successfull, error otherwise.
 */
status_t VirtualFakeCamera3::closeCamera() {
    ALOGI(LOG_TAG " %s: E ", __FUNCTION__);

    if (mSensor == NULL) {
        return VirtualCamera3::closeCamera();
    }

    {
        Mutex::Autolock l(mLock);
        if (mStatus == STATUS_CLOSED) return OK;

        auto ret = mSensor->shutDown();
        if (ret != NO_ERROR) {
            ALOGE("%s: Unable to shut down sensor: %d", __FUNCTION__, ret);
        }
        mSensor.clear();

        mReadoutThread->requestExit();
    }

    if (gIsInFrameH264) {
        // Set state to CameraClosed, so that ClientCommunicator stops decoding.
        mClientThread->mCameraSessionState = socket::CameraSessionState::kCameraClosed;
        mDecoder->Release();
        ALOGI("%s Decoding is stopped, now send CLOSE command to client", __func__);
    }

    mReadoutThread->join();

    {
        Mutex::Autolock l(mLock);
        // Clear out private stream information
        for (StreamIterator s = mStreams.begin(); s != mStreams.end(); s++) {
            PrivateStreamInfo *privStream = static_cast<PrivateStreamInfo *>((*s)->priv);
            delete privStream;
            (*s)->priv = NULL;
        }
        mStreams.clear();
        mReadoutThread.clear();
        mCameraBuffer.reset();
        mClientThread->mCameraBuffer = nullptr;
    }

    // Send close command to client
    status_t ret = sendCommandToClient(camera_cmd_t::CMD_CLOSE);
    if (ret != OK) {
        ALOGE("%s sendCommandToClient failed", __func__);
    }

    // Set NULL or Zero to some local members which would be updated in the
    // next configure_streams call to support Dynamic multi-resolution.
    mSrcWidth = 0;
    mSrcHeight = 0;
    mDecoderResolution = 0;
    mDecoderInitDone = false;
    mSensor = NULL;
    mReadoutThread = NULL;
    mJpegCompressor = NULL;

    return VirtualCamera3::closeCamera();
}

status_t VirtualFakeCamera3::getCameraInfo(struct camera_info *info) {
    return VirtualCamera3::getCameraInfo(info);
}

/**
 * Camera3 interface methods
 */

status_t VirtualFakeCamera3::configureStreams(camera3_stream_configuration *streamList) {
    Mutex::Autolock l(mLock);
    status_t res;

    if (mStatus != STATUS_OPEN && mStatus != STATUS_READY) {
        ALOGE("%s: Cannot configure streams in state %d", __FUNCTION__, mStatus);
        return NO_INIT;
    }

    /**
     * Sanity-check input list.
     */
    if (streamList == NULL) {
        ALOGE("%s: NULL stream configuration", __FUNCTION__);
        return BAD_VALUE;
    }

    ALOGI("%s: %d streams", __FUNCTION__, streamList->num_streams);
    if (streamList->streams == NULL) {
        ALOGE("%s: NULL stream list", __FUNCTION__);
        return BAD_VALUE;
    }

    ALOGI("%s: %d streams", __FUNCTION__, streamList->num_streams);
    if (streamList->num_streams < 1) {
        ALOGE("%s: Bad number of streams requested: %d", __FUNCTION__, streamList->num_streams);
        return BAD_VALUE;
    }

    camera3_stream_t *inputStream = NULL;
    for (size_t i = 0; i < streamList->num_streams; i++) {
        camera3_stream_t *newStream = streamList->streams[i];

        if (newStream == NULL) {
            ALOGE("%s: Stream index %zu was NULL", __FUNCTION__, i);
            return BAD_VALUE;
        }

        ALOGI(
            " %s: Stream %p (id %zu), type %d, usage 0x%x, format 0x%x "
            "width %d, height %d, rotation %d",
            __FUNCTION__, newStream, i, newStream->stream_type, newStream->usage, newStream->format,
            newStream->width, newStream->height, newStream->rotation);

        if (newStream->stream_type == CAMERA3_STREAM_INPUT ||
            newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL) {
            if (inputStream != NULL) {
                ALOGE("%s: Multiple input streams requested!", __FUNCTION__);
                return BAD_VALUE;
            }
            inputStream = newStream;
        }

        if (newStream->stream_type != CAMERA3_STREAM_INPUT) {
            if (newStream->rotation < CAMERA3_STREAM_ROTATION_0 ||
                newStream->rotation > CAMERA3_STREAM_ROTATION_270) {
                ALOGE("%s: Unsupported stream rotation 0x%x requested", __FUNCTION__,
                      newStream->rotation);
                return BAD_VALUE;
            }
        }

        if (newStream->width == 0 || newStream->height == 0 ||
            newStream->width > (uint32_t)mSensorWidth ||
            newStream->height > (uint32_t)mSensorHeight) {
            ALOGE("%s: Unsupported stream width 0x%x height 0x%x", __FUNCTION__, newStream->width,
                  newStream->height);
            return BAD_VALUE;
        }

        bool validFormat = false;
        for (size_t f = 0; f < sizeof(kHalSupportedFormats) / sizeof(kHalSupportedFormats[0]);
             f++) {
            if (newStream->format == kHalSupportedFormats[f]) {
                validFormat = true;
                break;
            }
        }
        if (!validFormat) {
            ALOGE("%s: Unsupported stream format 0x%x requested", __FUNCTION__, newStream->format);
            return BAD_VALUE;
        }

        if (mSrcWidth < newStream->width && mSrcHeight < newStream->height) {
            // Update app's res request to local variable.
            mSrcWidth = newStream->width;
            mSrcHeight = newStream->height;
        }
    }
    mInputStream = inputStream;

    ALOGI("%s: Camera current input resolution is %dx%d", __FUNCTION__, mSrcWidth, mSrcHeight);

    /**
     * Initially mark all existing streams as not alive
     */
    for (StreamIterator s = mStreams.begin(); s != mStreams.end(); ++s) {
        PrivateStreamInfo *privStream = static_cast<PrivateStreamInfo *>((*s)->priv);
        privStream->alive = false;
    }

    /**
     * Find new streams and mark still-alive ones
     */
    for (size_t i = 0; i < streamList->num_streams; i++) {
        camera3_stream_t *newStream = streamList->streams[i];
        if (newStream->priv == NULL) {
            // New stream, construct info
            PrivateStreamInfo *privStream = new PrivateStreamInfo();
            privStream->alive = true;

            newStream->max_buffers = kMaxBufferCount;
            newStream->priv = privStream;
            mStreams.push_back(newStream);
        } else {
            // Existing stream, mark as still alive.
            PrivateStreamInfo *privStream = static_cast<PrivateStreamInfo *>(newStream->priv);
            privStream->alive = true;
        }
        // Always update usage and max buffers
        newStream->max_buffers = kMaxBufferCount;
        switch (newStream->stream_type) {
            case CAMERA3_STREAM_OUTPUT:
                // Workarroud: SG1:  HAL_PIXEL_FORMAT_RGBA_8888 &&
                // GRALLOC_USAGE_HW_CAMERA_WRITE combination doesn't supported by minigbm
                newStream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                break;
            case CAMERA3_STREAM_INPUT:
                newStream->usage |= GRALLOC_USAGE_HW_CAMERA_READ;
                break;
            case CAMERA3_STREAM_BIDIRECTIONAL:
                newStream->usage |= (GRALLOC_USAGE_HW_CAMERA_READ | GRALLOC_USAGE_HW_CAMERA_WRITE);
                break;
        }
        // Set the buffer format, inline with gralloc implementation
        if (newStream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            if ((newStream->usage & GRALLOC_USAGE_HW_TEXTURE) ||
                    (newStream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)) {
                // Both preview and video capture output format would
                // be RGB32 always if it is HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED.
                newStream->format = HAL_PIXEL_FORMAT_RGBA_8888;
            } else {
                newStream->format = HAL_PIXEL_FORMAT_RGB_888;
            }
        }
    }

    /**
     * Reap the dead streams
     */
    for (StreamIterator s = mStreams.begin(); s != mStreams.end();) {
        PrivateStreamInfo *privStream = static_cast<PrivateStreamInfo *>((*s)->priv);
        if (!privStream->alive) {
            (*s)->priv = NULL;
            delete privStream;
            s = mStreams.erase(s);
        } else {
            ++s;
        }
    }

    /**
     * Can't reuse settings across configure call
     */
    mPrevSettings.clear();

    /**
     * Initialize Camera sensor and Input decoder based on app's res request.
     */
    if (!mDecoderInitDone) {
        ALOGI("%s: Initializing decoder and sensor for new resolution request!!!", __func__);
        res = connectCamera();
        if (res != OK) {
            return res;
        }
    }

    return OK;
}

status_t VirtualFakeCamera3::registerStreamBuffers(const camera3_stream_buffer_set *bufferSet) {
    ALOGVV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mLock);

    // Should not be called in HAL versions >= 3.2

    ALOGE("%s: Should not be invoked on new HALs!", __FUNCTION__);
    return NO_INIT;
}

const camera_metadata_t *VirtualFakeCamera3::constructDefaultRequestSettings(int type) {
    ALOGVV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mLock);

    if (type < 0 || type >= CAMERA3_TEMPLATE_COUNT) {
        ALOGE("%s: Unknown request settings template: %d", __FUNCTION__, type);
        return NULL;
    }

    if (!hasCapability(BACKWARD_COMPATIBLE) && type != CAMERA3_TEMPLATE_PREVIEW) {
        ALOGE("%s: Template %d not supported w/o BACKWARD_COMPATIBLE capability", __FUNCTION__,
              type);
        return NULL;
    }

    /**
     * Cache is not just an optimization - pointer returned has to live at
     * least as long as the camera device instance does.
     */
    if (mDefaultTemplates[type] != NULL) {
        return mDefaultTemplates[type];
    }

    CameraMetadata settings;

    /** android.request */

    static const uint8_t metadataMode = ANDROID_REQUEST_METADATA_MODE_FULL;
    settings.update(ANDROID_REQUEST_METADATA_MODE, &metadataMode, 1);

    static const int32_t requestId = 0;
    settings.update(ANDROID_REQUEST_ID, &requestId, 1);

    static const int32_t frameCount = 0;
    settings.update(ANDROID_REQUEST_FRAME_COUNT, &frameCount, 1);

    /** android.lens */

    static const float focalLength = 5.0f;
    settings.update(ANDROID_LENS_FOCAL_LENGTH, &focalLength, 1);

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const float focusDistance = 0;
        settings.update(ANDROID_LENS_FOCUS_DISTANCE, &focusDistance, 1);

        static const float aperture = 2.8f;
        settings.update(ANDROID_LENS_APERTURE, &aperture, 1);

        static const float filterDensity = 0;
        settings.update(ANDROID_LENS_FILTER_DENSITY, &filterDensity, 1);

        static const uint8_t opticalStabilizationMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        settings.update(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &opticalStabilizationMode, 1);

        // FOCUS_RANGE set only in frame
    }

    /** android.sensor */

    if (hasCapability(MANUAL_SENSOR)) {
        const int64_t exposureTime = 10 * MSEC;
        settings.update(ANDROID_SENSOR_EXPOSURE_TIME, &exposureTime, 1);

        const int64_t frameDuration = 33333333L;  // 1/30 s
        settings.update(ANDROID_SENSOR_FRAME_DURATION, &frameDuration, 1);

        const int32_t sensitivity = 100;
        settings.update(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    }

    // TIMESTAMP set only in frame

    /** android.flash */

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
        settings.update(ANDROID_FLASH_MODE, &flashMode, 1);

        static const uint8_t flashPower = 10;
        settings.update(ANDROID_FLASH_FIRING_POWER, &flashPower, 1);

        static const int64_t firingTime = 0;
        settings.update(ANDROID_FLASH_FIRING_TIME, &firingTime, 1);
    }

    /** Processing block modes */
    if (hasCapability(MANUAL_POST_PROCESSING)) {
        uint8_t hotPixelMode = 0;
        uint8_t demosaicMode = 0;
        uint8_t noiseMode = 0;
        uint8_t shadingMode = 0;
        uint8_t colorMode = 0;
        uint8_t tonemapMode = 0;
        uint8_t edgeMode = 0;
        switch (type) {
            case CAMERA3_TEMPLATE_STILL_CAPTURE:
                // fall-through
            case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
                // fall-through
            case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
                hotPixelMode = ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY;
                demosaicMode = ANDROID_DEMOSAIC_MODE_HIGH_QUALITY;
                noiseMode = ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY;
                shadingMode = ANDROID_SHADING_MODE_HIGH_QUALITY;
                colorMode = ANDROID_COLOR_CORRECTION_MODE_HIGH_QUALITY;
                tonemapMode = ANDROID_TONEMAP_MODE_HIGH_QUALITY;
                edgeMode = ANDROID_EDGE_MODE_HIGH_QUALITY;
                break;
            case CAMERA3_TEMPLATE_PREVIEW:
                // fall-through
            case CAMERA3_TEMPLATE_VIDEO_RECORD:
                // fall-through
            default:
                hotPixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
                demosaicMode = ANDROID_DEMOSAIC_MODE_FAST;
                noiseMode = ANDROID_NOISE_REDUCTION_MODE_FAST;
                shadingMode = ANDROID_SHADING_MODE_FAST;
                colorMode = ANDROID_COLOR_CORRECTION_MODE_FAST;
                tonemapMode = ANDROID_TONEMAP_MODE_FAST;
                edgeMode = ANDROID_EDGE_MODE_FAST;
                break;
        }
        settings.update(ANDROID_HOT_PIXEL_MODE, &hotPixelMode, 1);
        settings.update(ANDROID_DEMOSAIC_MODE, &demosaicMode, 1);
        settings.update(ANDROID_NOISE_REDUCTION_MODE, &noiseMode, 1);
        settings.update(ANDROID_SHADING_MODE, &shadingMode, 1);
        settings.update(ANDROID_COLOR_CORRECTION_MODE, &colorMode, 1);
        settings.update(ANDROID_TONEMAP_MODE, &tonemapMode, 1);
        settings.update(ANDROID_EDGE_MODE, &edgeMode, 1);
    }

    /** android.colorCorrection */

    if (hasCapability(MANUAL_POST_PROCESSING)) {
        static const camera_metadata_rational colorTransform[9] = {
            {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}};
        settings.update(ANDROID_COLOR_CORRECTION_TRANSFORM, colorTransform, 9);

        static const float colorGains[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        settings.update(ANDROID_COLOR_CORRECTION_GAINS, colorGains, 4);
    }

    /** android.tonemap */

    if (hasCapability(MANUAL_POST_PROCESSING)) {
        static const float tonemapCurve[4] = {0.f, 0.f, 1.f, 1.f};
        settings.update(ANDROID_TONEMAP_CURVE_RED, tonemapCurve, 4);
        settings.update(ANDROID_TONEMAP_CURVE_GREEN, tonemapCurve, 4);
        settings.update(ANDROID_TONEMAP_CURVE_BLUE, tonemapCurve, 4);
    }

    /** android.scaler */
    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const int32_t cropRegion[4] = {0, 0, mSensorWidth, mSensorHeight};
        settings.update(ANDROID_SCALER_CROP_REGION, cropRegion, 4);
    }

    /** android.jpeg */
    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t jpegQuality = 95;
        settings.update(ANDROID_JPEG_QUALITY, &jpegQuality, 1);

        static const int32_t thumbnailSize[2] = {320, 240};
        settings.update(ANDROID_JPEG_THUMBNAIL_SIZE, thumbnailSize, 2);

        static const uint8_t thumbnailQuality = 95;
        settings.update(ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbnailQuality, 1);

        static const double gpsCoordinates[3] = {0, 0, 0};
        settings.update(ANDROID_JPEG_GPS_COORDINATES, gpsCoordinates, 3);

        static const uint8_t gpsProcessingMethod[32] = "None";
        settings.update(ANDROID_JPEG_GPS_PROCESSING_METHOD, gpsProcessingMethod, 32);

        static const int64_t gpsTimestamp = 0;
        settings.update(ANDROID_JPEG_GPS_TIMESTAMP, &gpsTimestamp, 1);

        static const int32_t jpegOrientation = 0;
        settings.update(ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);
    }

    /** android.stats */

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
        settings.update(ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetectMode, 1);

        static const uint8_t hotPixelMapMode = ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
        settings.update(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, &hotPixelMapMode, 1);
    }

    // faceRectangles, faceScores, faceLandmarks, faceIds, histogram,
    // sharpnessMap only in frames

    /** android.control */

    uint8_t controlIntent = 0;
    switch (type) {
        case CAMERA3_TEMPLATE_PREVIEW:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case CAMERA3_TEMPLATE_STILL_CAPTURE:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            break;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            break;
        case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
            break;
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
            break;
        case CAMERA3_TEMPLATE_MANUAL:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
            break;
        default:
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
            break;
    }
    settings.update(ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);

    const uint8_t controlMode =
        (type == CAMERA3_TEMPLATE_MANUAL) ? ANDROID_CONTROL_MODE_OFF : ANDROID_CONTROL_MODE_AUTO;
    settings.update(ANDROID_CONTROL_MODE, &controlMode, 1);

    int32_t aeTargetFpsRange[2] = {15, 30};
    if (type == CAMERA3_TEMPLATE_VIDEO_RECORD || type == CAMERA3_TEMPLATE_VIDEO_SNAPSHOT) {
        aeTargetFpsRange[0] = 30;
    }
    settings.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, aeTargetFpsRange, 2);

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
        settings.update(ANDROID_CONTROL_EFFECT_MODE, &effectMode, 1);

        const uint8_t sceneMode = ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY;
        settings.update(ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);

        const uint8_t aeMode = (type == CAMERA3_TEMPLATE_MANUAL) ? ANDROID_CONTROL_AE_MODE_OFF
                                                                 : ANDROID_CONTROL_AE_MODE_ON;
        settings.update(ANDROID_CONTROL_AE_MODE, &aeMode, 1);

        static const uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
        settings.update(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);

        static const int32_t controlRegions[5] = {0, 0, 0, 0, 0};
        settings.update(ANDROID_CONTROL_AE_REGIONS, controlRegions, 5);

        static const int32_t aeExpCompensation = 0;
        settings.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &aeExpCompensation, 1);

        static const uint8_t aeAntibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
        settings.update(ANDROID_CONTROL_AE_ANTIBANDING_MODE, &aeAntibandingMode, 1);

        static const uint8_t aePrecaptureTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
        settings.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aePrecaptureTrigger, 1);

        const uint8_t awbMode = (type == CAMERA3_TEMPLATE_MANUAL) ? ANDROID_CONTROL_AWB_MODE_OFF
                                                                  : ANDROID_CONTROL_AWB_MODE_AUTO;
        settings.update(ANDROID_CONTROL_AWB_MODE, &awbMode, 1);

        static const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
        settings.update(ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);

        uint8_t afMode = 0;

        if (mFacingBack) {
            switch (type) {
                case CAMERA3_TEMPLATE_PREVIEW:
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                    break;
                case CAMERA3_TEMPLATE_STILL_CAPTURE:
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                    break;
                case CAMERA3_TEMPLATE_VIDEO_RECORD:
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
                    break;
                case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
                    break;
                case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                    break;
                case CAMERA3_TEMPLATE_MANUAL:
                    afMode = ANDROID_CONTROL_AF_MODE_OFF;
                    break;
                default:
                    afMode = ANDROID_CONTROL_AF_MODE_AUTO;
                    break;
            }
        } else {
            afMode = ANDROID_CONTROL_AF_MODE_OFF;
        }
        settings.update(ANDROID_CONTROL_AF_MODE, &afMode, 1);

        settings.update(ANDROID_CONTROL_AF_REGIONS, controlRegions, 5);

        const uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
        settings.update(ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);

        static const uint8_t vstabMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
        settings.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &vstabMode, 1);

        static const uint8_t blackLevelLock = ANDROID_BLACK_LEVEL_LOCK_OFF;
        settings.update(ANDROID_BLACK_LEVEL_LOCK, &blackLevelLock, 1);

        static const uint8_t lensShadingMapMode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
        settings.update(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMapMode, 1);

        uint8_t aberrationMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
        if (type == CAMERA3_TEMPLATE_STILL_CAPTURE) {
            aberrationMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY;
        }
        settings.update(ANDROID_COLOR_CORRECTION_ABERRATION_MODE, &aberrationMode, 1);

        static const int32_t testPatternMode = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
        settings.update(ANDROID_SENSOR_TEST_PATTERN_MODE, &testPatternMode, 1);
    }

    mDefaultTemplates[type] = settings.release();

    return mDefaultTemplates[type];
}

status_t VirtualFakeCamera3::processCaptureRequest(camera3_capture_request *request) {
    ALOGVV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mLock);
    status_t res;
    /** Validation */

    if (mStatus < STATUS_READY) {
        ALOGE("%s: Can't submit capture requests in state %d", __FUNCTION__, mStatus);
        return INVALID_OPERATION;
    }

    if (request == NULL) {
        ALOGE("%s: NULL request!", __FUNCTION__);
        return BAD_VALUE;
    }

    ALOGVV("%s: Number of requested buffers = %u, Frame no: %u", __FUNCTION__,
           request->num_output_buffers, request->frame_number);

    uint32_t frameNumber = request->frame_number;

    if (request->settings == NULL && mPrevSettings.isEmpty()) {
        ALOGE(
            "%s: Request %d: NULL settings for first request after"
            "configureStreams()",
            __FUNCTION__, frameNumber);
        return BAD_VALUE;
    }

    if (request->input_buffer != NULL && request->input_buffer->stream != mInputStream) {
        ALOGE("%s: Request %d: Input buffer not from input stream!", __FUNCTION__, frameNumber);
        ALOGV("%s: Bad stream %p, expected: %p", __FUNCTION__, request->input_buffer->stream,
              mInputStream);
        ALOGV("%s: Bad stream type %d, expected stream type %d", __FUNCTION__,
              request->input_buffer->stream->stream_type,
              mInputStream ? mInputStream->stream_type : -1);

        return BAD_VALUE;
    }

    if (request->num_output_buffers < 1 || request->output_buffers == NULL) {
        ALOGE("%s: Request %d: No output buffers provided!", __FUNCTION__, frameNumber);
        return BAD_VALUE;
    }

    // Validate all buffers, starting with input buffer if it's given

    ssize_t idx;
    const camera3_stream_buffer_t *b;
    if (request->input_buffer != NULL) {
        idx = -1;
        b = request->input_buffer;
    } else {
        idx = 0;
        b = request->output_buffers;
    }
    do {
        PrivateStreamInfo *priv = static_cast<PrivateStreamInfo *>(b->stream->priv);
        if (priv == NULL) {
            ALOGE("%s: Request %d: Buffer %zu: Unconfigured stream!", __FUNCTION__, frameNumber,
                  idx);
            return BAD_VALUE;
        }
        if (!priv->alive) {
            ALOGE("%s: Request %d: Buffer %zu: Dead stream!", __FUNCTION__, frameNumber, idx);
            return BAD_VALUE;
        }
        if (b->status != CAMERA3_BUFFER_STATUS_OK) {
            ALOGE("%s: Request %d: Buffer %zu: Status not OK!", __FUNCTION__, frameNumber, idx);
            return BAD_VALUE;
        }
        if (b->release_fence != -1) {
            ALOGE("%s: Request %d: Buffer %zu: Has a release fence!", __FUNCTION__, frameNumber,
                  idx);
            return BAD_VALUE;
        }
        if (b->buffer == NULL) {
            ALOGE("%s: Request %d: Buffer %zu: NULL buffer handle!", __FUNCTION__, frameNumber,
                  idx);
            return BAD_VALUE;
        }
        idx++;
        b = &(request->output_buffers[idx]);
    } while (idx < (ssize_t)request->num_output_buffers);

    // TODO: Validate settings parameters

    /**
     * Start processing this request
     */

    mStatus = STATUS_ACTIVE;

    CameraMetadata settings;

    if (request->settings == NULL) {
        settings.acquire(mPrevSettings);
    } else {
        settings = request->settings;
    }

    res = process3A(settings);
    if (res != OK) {
        return res;
    }

    // TODO: Handle reprocessing

    /**
     * Get ready for sensor config
     */

    int syncTimeoutCount = 0;
    ReadoutThread::Request r;
    nsecs_t exposureTime;
    nsecs_t frameDuration;
    uint32_t sensitivity;
    bool needJpeg = false;
    camera_metadata_entry_t entry;
    entry = settings.find(ANDROID_SENSOR_EXPOSURE_TIME);
    exposureTime = (entry.count > 0) ? entry.data.i64[0] : Sensor::kExposureTimeRange[0];
    entry = settings.find(ANDROID_SENSOR_FRAME_DURATION);
    frameDuration = (entry.count > 0) ? entry.data.i64[0] : Sensor::kFrameDurationRange[0];
    entry = settings.find(ANDROID_SENSOR_SENSITIVITY);
    sensitivity = (entry.count > 0) ? entry.data.i32[0] : Sensor::kSensitivityRange[0];

    Buffers *sensorBuffers = new Buffers();
    HalBufferVector *buffers = new HalBufferVector();

    sensorBuffers->setCapacity(request->num_output_buffers);
    buffers->setCapacity(request->num_output_buffers);

    // Process all the buffers we got for output, constructing internal buffer
    // structures for them, and lock them for writing.
    for (size_t i = 0; i < request->num_output_buffers; i++) {
        const camera3_stream_buffer &srcBuf = request->output_buffers[i];
        StreamBuffer destBuf;
        bool bImported = false;

        destBuf.streamId = kGenericStreamId;
        destBuf.width = srcBuf.stream->width;
        destBuf.height = srcBuf.stream->height;
        destBuf.stride = srcBuf.stream->width;
        destBuf.dataSpace = srcBuf.stream->data_space;
        destBuf.buffer = srcBuf.buffer;
        // Set this first to get rid of klocwork warnings.
        // It would be overwritten again if it is HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
        destBuf.format = (srcBuf.stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
                             ? HAL_PIXEL_FORMAT_RGBA_8888
                             : srcBuf.stream->format;

        if (srcBuf.stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            if ((srcBuf.stream->usage & GRALLOC_USAGE_HW_TEXTURE) ||
                (srcBuf.stream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)) {
                // Both preview and video capture output format would
                // be RGB32 always if it is HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED.
                destBuf.format = HAL_PIXEL_FORMAT_RGBA_8888;
            } else if ((srcBuf.stream->usage & GRALLOC_USAGE_HW_CAMERA_MASK) ==
                       GRALLOC_USAGE_HW_CAMERA_ZSL) {
                // Note: Currently no support for ZSL mode
                destBuf.format = HAL_PIXEL_FORMAT_RGB_888;
            }
        }

        if (destBuf.format == HAL_PIXEL_FORMAT_BLOB) {
            needJpeg = true;
        }

        // Wait on fence
        sp<Fence> bufferAcquireFence = new Fence(srcBuf.acquire_fence);
        res = bufferAcquireFence->wait(kFenceTimeoutMs);
        if (res == TIMED_OUT) {
            ALOGE("%s: Request %d: Buffer %zu: Fence timed out after %d ms", __FUNCTION__,
                  frameNumber, i, kFenceTimeoutMs);
        }
        if (res == OK) {
            // Lock buffer for writing
            res = GrallocModule::getInstance().import(*(destBuf.buffer), &destBuf.importedHandle);
            if (res == OK) {
                bImported = true;
                /**
                 * For HAL_PIXEL_FORMAT_BLOB format, framework allocates the gralloc buffer
                 * with height 1. If lock is done with destination buffer height instead of
                 * height=1 for BLOB format, lock will fail with "Invalid region: height
                 * greater than buffer height". Inorder to avoid the lock failure, call lock with
                 * height=1 for BLOB format.
                 */
                res = GrallocModule::getInstance().lock(destBuf.importedHandle,
                                     GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, destBuf.width,
                                     (destBuf.format == HAL_PIXEL_FORMAT_BLOB) ? 1 : destBuf.height,
                                     (void **)&(destBuf.img));
                if (res != OK) {
                    ALOGE("%s: Request %d: Buffer %zu: Unable to lock buffer format: 0x%x",
                          __FUNCTION__, frameNumber, i, destBuf.format);
                } else {
                    ALOGVV(" %s, stream format 0x%x width %d height %d buffer 0x%p img 0x%p",
                           __FUNCTION__, destBuf.format, destBuf.width, destBuf.height, destBuf.buffer,
                           destBuf.img);
                }
            }
        }

        if (res != OK) {
            ALOGE("%s: Request %d: Buffer %zu: Gralloc failure with buffer format: 0x%x",
                  __FUNCTION__, frameNumber, i, destBuf.format);
            // Either waiting or locking failed. Unlock locked buffers from
            // previous iterations of i
            for (const auto& buf : *sensorBuffers) {
                GrallocModule::getInstance().unlock(buf.importedHandle);
                GrallocModule::getInstance().release(buf.importedHandle);
            }
            // Release if import successful in this iteration of i.
            // Unlock not required, as lock not successful/called in this iteration.
            if (bImported)
                GrallocModule::getInstance().release(destBuf.importedHandle);
            delete sensorBuffers;
            delete buffers;
            return NO_INIT;
        }

        sensorBuffers->push_back(destBuf);
        buffers->push_back(srcBuf);
    }

    /**
     * Wait for JPEG compressor to not be busy, if needed
     */
    if (needJpeg) {
        bool ready = mJpegCompressor->waitForDone(kJpegTimeoutNs);
        if (!ready) {
            ALOGE("%s: Timeout waiting for JPEG compression to complete!", __FUNCTION__);
            res = NO_INIT;
            goto out;
        }
        res = mJpegCompressor->reserve();
        if (res != OK) {
            ALOGE("%s: Error managing JPEG compressor resources, can't reserve it!", __FUNCTION__);
            res = NO_INIT;
            goto out;
        }
    }

    /**
     * Wait until the in-flight queue has room
     */
    res = mReadoutThread->waitForReadout();
    if (res != OK) {
        ALOGE("%s: Timeout waiting for previous requests to complete!", __FUNCTION__);
        res = NO_INIT;
        goto out;
    }

    /**
     * Wait until sensor's ready. This waits for lengthy amounts of time with
     * mLock held, but the interface spec is that no other calls may by done to
     * the HAL by the framework while process_capture_request is happening.
     */
    while (!mSensor->waitForVSync(kSyncWaitTimeout)) {
        if (mStatus == STATUS_ERROR) {
            res = NO_INIT;
            goto out;
        }
        if (syncTimeoutCount == kMaxSyncTimeoutCount) {
            ALOGE("%s: Request %d: Sensor sync timed out after %" PRId64 " ms", __FUNCTION__,
                  frameNumber, kSyncWaitTimeout * kMaxSyncTimeoutCount / 1000000);
            res = NO_INIT;
            goto out;
        }
        syncTimeoutCount++;
    }

    /**
     * Configure sensor and queue up the request to the readout thread
     */
    mSensor->setExposureTime(exposureTime);
    mSensor->setFrameDuration(frameDuration);
    mSensor->setSensitivity(sensitivity);
    mSensor->setDestinationBuffers(sensorBuffers);
    mSensor->setFrameNumber(request->frame_number);

    r.frameNumber = request->frame_number;
    r.settings = settings;
    r.sensorBuffers = sensorBuffers;
    r.buffers = buffers;

    mReadoutThread->queueCaptureRequest(r);
    ALOGVV("%s: Queued frame %d", __FUNCTION__, request->frame_number);

    // Cache the settings for next time
    mPrevSettings.acquire(settings);

    return OK;
out:
    delete sensorBuffers;
    delete buffers;
    return res;
}

status_t VirtualFakeCamera3::flush() {
    ALOGVV("%s: Not implemented; ignored", __FUNCTION__);
    return OK;
}

/** Debug methods */

void VirtualFakeCamera3::dump(int fd) {}

/**
 * Private methods
 */

status_t VirtualFakeCamera3::getCameraCapabilities() {
    char prop[PROPERTY_VALUE_MAX];

    if (property_get("ro.vendor.camera.hw_capability.config", prop, NULL) > 0 &&
            !strcasecmp(prop, "FULL")) {
        ALOGI("%s: Selected FULL HW level", __func__);
        mCapabilities.add(FULL_LEVEL);
    } else {
        // Default would be LIMITED for remote cameras.
        mCapabilities.add(LIMITED_LEVEL);
        ALOGI("%s: Selected LIMITED HW level", __func__);
    }

    // Add capability features based on HW LEVELs
    if (hasCapability(FULL_LEVEL)) {
        mCapabilities.add(BACKWARD_COMPATIBLE);
        mCapabilities.add(BURST_CAPTURE);
        mCapabilities.add(MANUAL_SENSOR);
        mCapabilities.add(MANUAL_POST_PROCESSING);
    } else if (hasCapability(LIMITED_LEVEL)) {
        mCapabilities.add(BACKWARD_COMPATIBLE);
        mCapabilities.add(BURST_CAPTURE);
    }

    ALOGI("%s: Supported HW capabilities for Camera%d", __func__, mCameraID);
    for (size_t i = 0; i < mCapabilities.size(); i++) {
        ALOGI("  %s", sAvailableCapabilitiesStrings[mCapabilities[i]]);
    }

    return OK;
}

bool VirtualFakeCamera3::hasCapability(AvailableCapabilities cap) {
    ssize_t idx = mCapabilities.indexOf(cap);
    return idx >= 0;
}

void VirtualFakeCamera3::setCameraFacingInfo() {
    // Updating facing info based on client request.
    mFacingBack = (mClientCameraInfo.facing  == (uint32_t)CameraFacing::BACK_FACING);
    ALOGI("%s: Camera ID %d is set as %s facing", __func__, mCameraID,
          mFacingBack ? "Back" : "Front");
}

void VirtualFakeCamera3::setInputCodecType() {
    mCodecType = mClientCameraInfo.codec_type;
    ALOGI("%s: Selected %s Codec_type for Camera %d", __func__, codec_type_to_str(mCodecType),
          mCameraID);
}

void VirtualFakeCamera3::setMaxSupportedResolution() {
    // Updating max sensor supported resolution based on client camera.
    // This would be used in sensor related operations and metadata info.
    switch (mClientCameraInfo.resolution) {
        case uint32_t(FrameResolution::k480p):
            mSensorWidth = 640;
            mSensorHeight = 480;
            break;
        case uint32_t(FrameResolution::k720p):
            mSensorWidth = 1280;
            mSensorHeight = 720;
            break;
        case uint32_t(FrameResolution::k1080p):
            mSensorWidth = 1920;
            mSensorHeight = 1080;
            break;
        default:
            break;
    }
    ALOGI("%s: Maximum supported Resolution of Camera %d: %dx%d", __func__, mCameraID, mSensorWidth,
          mSensorHeight);
}

status_t VirtualFakeCamera3::constructStaticInfo() {
    CameraMetadata info;
    Vector<int32_t> availableCharacteristicsKeys;
    status_t res;
    int32_t width = 0, height = 0;

    ALOGVV("%s: Updating metadata for Camera %d", __func__, mCameraID);

    // Setting the max supported Camera resolution.
    setMaxSupportedResolution();
    // set codec type of the input frame.
    setInputCodecType();
    // Set camera facing info.
    setCameraFacingInfo();

    // Updating width and height based on capability info.
    width = mSensorWidth;
    height = mSensorHeight;

#define ADD_STATIC_ENTRY(name, varptr, count) \
    availableCharacteristicsKeys.add(name);   \
    res = info.update(name, varptr, count);   \
    if (res != OK) return res

    // android.sensor

    if (hasCapability(MANUAL_SENSOR)) {
        ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, Sensor::kExposureTimeRange, 2);

        ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &Sensor::kFrameDurationRange[1],
                         1);

        ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, Sensor::kSensitivityRange,
                         sizeof(Sensor::kSensitivityRange) / sizeof(int32_t));

        ADD_STATIC_ENTRY(ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY, &Sensor::kSensitivityRange[1], 1);
    }

    static const uint8_t sensorColorFilterArrangement =
        ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;
    ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &sensorColorFilterArrangement,
                     1);

    static const float sensorPhysicalSize[2] = {3.20f, 2.40f};  // mm
    ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, sensorPhysicalSize, 2);

    int32_t pixelArray[] = {mSensorWidth, mSensorHeight};
    ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArray, 2);
    int32_t activeArray[] = {0, 0, mSensorWidth, mSensorHeight};
    ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);

    int32_t orientation = mClientCameraInfo.sensorOrientation;
    ADD_STATIC_ENTRY(ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    static const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);

    if (hasCapability(MANUAL_SENSOR)) {
        ADD_STATIC_ENTRY(ANDROID_SENSOR_INFO_WHITE_LEVEL, (int32_t *)&Sensor::kMaxRawValue, 1);

        static const int32_t blackLevelPattern[4] = {
            (int32_t)Sensor::kBlackLevel, (int32_t)Sensor::kBlackLevel,
            (int32_t)Sensor::kBlackLevel, (int32_t)Sensor::kBlackLevel};
        ADD_STATIC_ENTRY(ANDROID_SENSOR_BLACK_LEVEL_PATTERN, blackLevelPattern,
                         sizeof(blackLevelPattern) / sizeof(int32_t));
    }

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const int32_t availableTestPatternModes[] = {ANDROID_SENSOR_TEST_PATTERN_MODE_OFF};
        ADD_STATIC_ENTRY(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, availableTestPatternModes,
                         sizeof(availableTestPatternModes) / sizeof(int32_t));
    }

    // android.lens
    static const float focalLengths = 5.0f;
    ADD_STATIC_ENTRY(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalLengths, 1);

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        // 5 cm min focus distance for back camera, infinity (fixed focus) for front
        float minFocusDistance = mFacingBack ? 1.0 / 0.05 : 0.0;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &minFocusDistance, 1);

        // 5 m hyperfocal distance for back camera, infinity (fixed focus) for front
        float hyperFocalDistance = mFacingBack ? 1.0 / 5.0 : 0.0;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperFocalDistance, 1);

        static const float apertures = 2.8f;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_AVAILABLE_APERTURES, &apertures, 1);
        static const float filterDensities = 0;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, &filterDensities, 1);
        static const uint8_t availableOpticalStabilization =
            ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
                         &availableOpticalStabilization, 1);

        static const int32_t lensShadingMapSize[] = {1, 1};
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_SHADING_MAP_SIZE, lensShadingMapSize,
                         sizeof(lensShadingMapSize) / sizeof(int32_t));

        static const uint8_t lensFocusCalibration =
            ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE;
        ADD_STATIC_ENTRY(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &lensFocusCalibration, 1);
    }

    uint8_t lensFacing = mFacingBack ? ANDROID_LENS_FACING_BACK : ANDROID_LENS_FACING_FRONT;
    ADD_STATIC_ENTRY(ANDROID_LENS_FACING, &lensFacing, 1);

    // android.flash

    static const uint8_t flashAvailable = 0;
    ADD_STATIC_ENTRY(ANDROID_FLASH_INFO_AVAILABLE, &flashAvailable, 1);

    // android.hotPixel

    if (hasCapability(MANUAL_POST_PROCESSING)) {
        static const uint8_t availableHotPixelModes[] = {ANDROID_HOT_PIXEL_MODE_FAST,
                                                         ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, availableHotPixelModes,
                         sizeof(availableHotPixelModes));
    }

    // android.tonemap

    if (hasCapability(MANUAL_POST_PROCESSING)) {
        static const int32_t tonemapCurvePoints = 128;
        ADD_STATIC_ENTRY(ANDROID_TONEMAP_MAX_CURVE_POINTS, &tonemapCurvePoints, 1);

        static const uint8_t availableToneMapModes[] = {ANDROID_TONEMAP_MODE_CONTRAST_CURVE,
                                                        ANDROID_TONEMAP_MODE_FAST,
                                                        ANDROID_TONEMAP_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, availableToneMapModes,
                         sizeof(availableToneMapModes));
    }

    // android.scaler

    const std::vector<int32_t> availableStreamConfigurationsDefault = {
        HAL_PIXEL_FORMAT_BLOB,
        width,
        height,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };

    const std::vector<int32_t> availableStreamConfigurations1080p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        1280,
        720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        1280,
        720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        1280,
        720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,
        1280,
        720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };

    const std::vector<int32_t> availableStreamConfigurations720p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        640,
        480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        640,
        480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        640,
        480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,
        640,
        480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };

    const std::vector<int32_t> availableStreamConfigurations480p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        320,
        240,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        320,
        240,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        320,
        240,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,
        320,
        240,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };

    const std::vector<int32_t> availableStreamConfigurationsBurst = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        width,
        height,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        width,
        height,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_RGBA_8888,
        width,
        height,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };

    std::vector<int32_t> availableStreamConfigurations;

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        if (width == 1920 && height == 1080) {
            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurationsDefault.begin(),
                                                 availableStreamConfigurationsDefault.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations1080p.begin(),
                                                 availableStreamConfigurations1080p.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations720p.begin(),
                                                 availableStreamConfigurations720p.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations480p.begin(),
                                                 availableStreamConfigurations480p.end());

        } else if (width == 1280 && height == 720) {
            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurationsDefault.begin(),
                                                 availableStreamConfigurationsDefault.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations720p.begin(),
                                                 availableStreamConfigurations720p.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations480p.begin(),
                                                 availableStreamConfigurations480p.end());
        } else {  // For 480p
            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurationsDefault.begin(),
                                                 availableStreamConfigurationsDefault.end());

            availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                                 availableStreamConfigurations480p.begin(),
                                                 availableStreamConfigurations480p.end());
        }
    }

    if (hasCapability(BURST_CAPTURE)) {
        availableStreamConfigurations.insert(availableStreamConfigurations.end(),
                                             availableStreamConfigurationsBurst.begin(),
                                             availableStreamConfigurationsBurst.end());
    }

    if (availableStreamConfigurations.size() > 0) {
        ADD_STATIC_ENTRY(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                         &availableStreamConfigurations[0], availableStreamConfigurations.size());
    }

    const std::vector<int64_t> availableMinFrameDurationsDefault = {
        HAL_PIXEL_FORMAT_BLOB,
        width,
        height,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableMinFrameDurations1080p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        1280,
        720,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        1280,
        720,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        1280,
        720,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_BLOB,
        1280,
        720,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableMinFrameDurations720p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        640,
        480,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        640,
        480,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        640,
        480,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_BLOB,
        640,
        480,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableMinFrameDurations480p = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        320,
        240,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCrCb_420_SP,
        320,
        240,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        320,
        240,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_BLOB,
        320,
        240,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableMinFrameDurationsBurst = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        width,
        height,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        width,
        height,
        Sensor::kFrameDurationRange[0],
        HAL_PIXEL_FORMAT_RGBA_8888,
        width,
        height,
        Sensor::kFrameDurationRange[0],
    };

    std::vector<int64_t> availableMinFrameDurations;

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        if (width == 1920 && height == 1080) {
            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurationsDefault.begin(),
                                              availableMinFrameDurationsDefault.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations1080p.begin(),
                                              availableMinFrameDurations1080p.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations720p.begin(),
                                              availableMinFrameDurations720p.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations480p.begin(),
                                              availableMinFrameDurations480p.end());
        } else if (width == 1280 && height == 720) {
            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurationsDefault.begin(),
                                              availableMinFrameDurationsDefault.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations720p.begin(),
                                              availableMinFrameDurations720p.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations480p.begin(),
                                              availableMinFrameDurations480p.end());
        } else {  // For 480p
            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurationsDefault.begin(),
                                              availableMinFrameDurationsDefault.end());

            availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                              availableMinFrameDurations480p.begin(),
                                              availableMinFrameDurations480p.end());
        }
    }

    if (hasCapability(BURST_CAPTURE)) {
        availableMinFrameDurations.insert(availableMinFrameDurations.end(),
                                          availableMinFrameDurationsBurst.begin(),
                                          availableMinFrameDurationsBurst.end());
    }

    if (availableMinFrameDurations.size() > 0) {
        ADD_STATIC_ENTRY(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                         &availableMinFrameDurations[0], availableMinFrameDurations.size());
    }

    const std::vector<int64_t> availableStallDurationsDefault = {
        HAL_PIXEL_FORMAT_BLOB,
        width,
        height,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableStallDurations1080p = {
        HAL_PIXEL_FORMAT_BLOB,
        1280,
        720,
        Sensor::kFrameDurationRange[0],
    };
    const std::vector<int64_t> availableStallDurations720p = {
        HAL_PIXEL_FORMAT_BLOB,
        640,
        480,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableStallDurations480p = {
        HAL_PIXEL_FORMAT_BLOB,
        320,
        240,
        Sensor::kFrameDurationRange[0],
    };

    const std::vector<int64_t> availableStallDurationsBurst = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        640,
        480,
        0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        640,
        480,
        0,
        HAL_PIXEL_FORMAT_RGBA_8888,
        640,
        480,
        0,
    };

    std::vector<int64_t> availableStallDurations;

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        if (width == 1920 && height == 1080) {
            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurationsDefault.begin(),
                                           availableStallDurationsDefault.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations1080p.begin(),
                                           availableStallDurations1080p.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations720p.begin(),
                                           availableStallDurations720p.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations480p.begin(),
                                           availableStallDurations480p.end());
        } else if (width == 1280 && height == 720) {
            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurationsDefault.begin(),
                                           availableStallDurationsDefault.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations720p.begin(),
                                           availableStallDurations720p.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations480p.begin(),
                                           availableStallDurations480p.end());
        } else {  // For 480p
            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurationsDefault.begin(),
                                           availableStallDurationsDefault.end());

            availableStallDurations.insert(availableStallDurations.end(),
                                           availableStallDurations480p.begin(),
                                           availableStallDurations480p.end());
        }
    }

    if (hasCapability(BURST_CAPTURE)) {
        availableStallDurations.insert(availableStallDurations.end(),
                                       availableStallDurationsBurst.begin(),
                                       availableStallDurationsBurst.end());
    }

    if (availableStallDurations.size() > 0) {
        ADD_STATIC_ENTRY(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, &availableStallDurations[0],
                         availableStallDurations.size());
    }

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_FREEFORM;
        ADD_STATIC_ENTRY(ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

        static const float maxZoom = 10;
        ADD_STATIC_ENTRY(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);
    }

    // android.jpeg

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const int32_t jpegThumbnailSizes[] = {0, 0, 160, 120, 320, 180, 320, 240};
        ADD_STATIC_ENTRY(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpegThumbnailSizes,
                         sizeof(jpegThumbnailSizes) / sizeof(int32_t));

        static const int32_t jpegMaxSize = JpegCompressor::kMaxJpegSize;
        ADD_STATIC_ENTRY(ANDROID_JPEG_MAX_SIZE, &jpegMaxSize, 1);
    }

    // android.stats

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t availableFaceDetectModes[] = {
            ANDROID_STATISTICS_FACE_DETECT_MODE_OFF, ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE,
            ANDROID_STATISTICS_FACE_DETECT_MODE_FULL};
        ADD_STATIC_ENTRY(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
                         availableFaceDetectModes, sizeof(availableFaceDetectModes));

        static const int32_t maxFaceCount = 8;
        ADD_STATIC_ENTRY(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaceCount, 1);

        static const uint8_t availableShadingMapModes[] = {
            ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF};
        ADD_STATIC_ENTRY(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
                         availableShadingMapModes, sizeof(availableShadingMapModes));
    }

    // android.sync

    static const int32_t maxLatency =
        hasCapability(FULL_LEVEL) ? ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL : 3;
    ADD_STATIC_ENTRY(ANDROID_SYNC_MAX_LATENCY, &maxLatency, 1);

    // android.control

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const uint8_t availableControlModes[] = {ANDROID_CONTROL_MODE_OFF,
                                                 ANDROID_CONTROL_MODE_AUTO,
                                                 ANDROID_CONTROL_MODE_USE_SCENE_MODE};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AVAILABLE_MODES, availableControlModes,
                         sizeof(availableControlModes));
    } else {
        const uint8_t availableControlModes[] = {ANDROID_CONTROL_MODE_AUTO};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AVAILABLE_MODES, availableControlModes,
                         sizeof(availableControlModes));
    }

    const uint8_t availableSceneModes[] = {hasCapability(BACKWARD_COMPATIBLE)
                                               ? ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY
                                               : ANDROID_CONTROL_SCENE_MODE_DISABLED};
    ADD_STATIC_ENTRY(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, availableSceneModes,
                     sizeof(availableSceneModes));

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t availableEffects[] = {ANDROID_CONTROL_EFFECT_MODE_OFF};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AVAILABLE_EFFECTS, availableEffects,
                         sizeof(availableEffects));
    }

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const int32_t max3aRegions[] = {/*AE*/ 1, /*AWB*/ 0, /*AF*/ 1};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_MAX_REGIONS, max3aRegions,
                         sizeof(max3aRegions) / sizeof(max3aRegions[0]));

        static const uint8_t availableAeModes[] = {ANDROID_CONTROL_AE_MODE_OFF,
                                                   ANDROID_CONTROL_AE_MODE_ON};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_AVAILABLE_MODES, availableAeModes,
                         sizeof(availableAeModes));

        static const camera_metadata_rational exposureCompensationStep = {0, 3};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_COMPENSATION_STEP, &exposureCompensationStep, 1);

        static const int32_t exposureCompensationRange[] = {0, 0};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_COMPENSATION_RANGE, exposureCompensationRange,
                         sizeof(exposureCompensationRange) / sizeof(int32_t));
    }

    static const int32_t availableTargetFpsRanges[] = {15, 30, 30, 30};
    ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, availableTargetFpsRanges,
                     sizeof(availableTargetFpsRanges) / sizeof(int32_t));

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t availableAntibandingModes[] = {
            ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF, ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, availableAntibandingModes,
                         sizeof(availableAntibandingModes));
    }

    const uint8_t aeLockAvailable = hasCapability(BACKWARD_COMPATIBLE)
                                        ? ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE
                                        : ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE;

    ADD_STATIC_ENTRY(ANDROID_CONTROL_AE_LOCK_AVAILABLE, &aeLockAvailable, 1);

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t availableAwbModes[] = {
            ANDROID_CONTROL_AWB_MODE_OFF,          ANDROID_CONTROL_AWB_MODE_AUTO,
            ANDROID_CONTROL_AWB_MODE_INCANDESCENT, ANDROID_CONTROL_AWB_MODE_FLUORESCENT,
            ANDROID_CONTROL_AWB_MODE_DAYLIGHT,     ANDROID_CONTROL_AWB_MODE_SHADE};
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AWB_AVAILABLE_MODES, availableAwbModes,
                         sizeof(availableAwbModes));
    }

    const uint8_t awbLockAvailable = hasCapability(BACKWARD_COMPATIBLE)
                                         ? ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE
                                         : ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE;

    ADD_STATIC_ENTRY(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awbLockAvailable, 1);

    static const uint8_t availableAfModesBack[] = {
        ANDROID_CONTROL_AF_MODE_OFF, ANDROID_CONTROL_AF_MODE_AUTO, ANDROID_CONTROL_AF_MODE_MACRO,
        ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO, ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE};

    static const uint8_t availableAfModesFront[] = {ANDROID_CONTROL_AF_MODE_OFF};

    if (mFacingBack && hasCapability(BACKWARD_COMPATIBLE)) {
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AF_AVAILABLE_MODES, availableAfModesBack,
                         sizeof(availableAfModesBack));
    } else {
        ADD_STATIC_ENTRY(ANDROID_CONTROL_AF_AVAILABLE_MODES, availableAfModesFront,
                         sizeof(availableAfModesFront));
    }

    static const uint8_t availableVstabModes[] = {ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF};
    ADD_STATIC_ENTRY(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, availableVstabModes,
                     sizeof(availableVstabModes));

    // android.colorCorrection

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const uint8_t availableAberrationModes[] = {
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
                         availableAberrationModes, sizeof(availableAberrationModes));
    } else {
        const uint8_t availableAberrationModes[] = {
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
        };
        ADD_STATIC_ENTRY(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
                         availableAberrationModes, sizeof(availableAberrationModes));
    }
    // android.edge

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const uint8_t availableEdgeModes[] = {ANDROID_EDGE_MODE_OFF, ANDROID_EDGE_MODE_FAST,
                                              ANDROID_EDGE_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_EDGE_AVAILABLE_EDGE_MODES, availableEdgeModes,
                         sizeof(availableEdgeModes));
    } else {
        const uint8_t availableEdgeModes[] = {ANDROID_EDGE_MODE_OFF};
        ADD_STATIC_ENTRY(ANDROID_EDGE_AVAILABLE_EDGE_MODES, availableEdgeModes,
                         sizeof(availableEdgeModes));
    }

    // android.info

    const uint8_t supportedHardwareLevel = hasCapability(FULL_LEVEL)
                                               ? ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL
                                               : ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
    ADD_STATIC_ENTRY(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &supportedHardwareLevel,
                     /*count*/ 1);

    // android.noiseReduction

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const uint8_t availableNoiseReductionModes[] = {ANDROID_NOISE_REDUCTION_MODE_OFF,
                                                        ANDROID_NOISE_REDUCTION_MODE_FAST,
                                                        ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                         availableNoiseReductionModes, sizeof(availableNoiseReductionModes));
    } else {
        const uint8_t availableNoiseReductionModes[] = {
            ANDROID_NOISE_REDUCTION_MODE_OFF,
        };
        ADD_STATIC_ENTRY(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                         availableNoiseReductionModes, sizeof(availableNoiseReductionModes));
    }

    // android.shading

    if (hasCapability(BACKWARD_COMPATIBLE)) {
        const uint8_t availableShadingModes[] = {
            ANDROID_SHADING_MODE_OFF, ANDROID_SHADING_MODE_FAST, ANDROID_SHADING_MODE_HIGH_QUALITY};
        ADD_STATIC_ENTRY(ANDROID_SHADING_AVAILABLE_MODES, availableShadingModes,
                         sizeof(availableShadingModes));
    } else {
        const uint8_t availableShadingModes[] = {ANDROID_SHADING_MODE_OFF};
        ADD_STATIC_ENTRY(ANDROID_SHADING_AVAILABLE_MODES, availableShadingModes,
                         sizeof(availableShadingModes));
    }

    // android.request

    static const int32_t maxNumOutputStreams[] = {kMaxRawStreamCount, kMaxProcessedStreamCount,
                                                  kMaxJpegStreamCount};
    ADD_STATIC_ENTRY(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxNumOutputStreams, 3);

    static const uint8_t maxPipelineDepth = kMaxBufferCount;
    ADD_STATIC_ENTRY(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &maxPipelineDepth, 1);

    static const int32_t partialResultCount = 1;
    ADD_STATIC_ENTRY(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialResultCount,
                     /*count*/ 1);

    SortedVector<uint8_t> caps;
    for (size_t i = 0; i < mCapabilities.size(); i++) {
        switch (mCapabilities[i]) {
            case BACKWARD_COMPATIBLE:
                caps.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
                break;
            case MANUAL_SENSOR:
                caps.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
                break;
            case MANUAL_POST_PROCESSING:
                caps.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
                break;
            case BURST_CAPTURE:
                caps.add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE);
                break;
            default:
                // Ignore LEVELs
                break;
        }
    }
    ADD_STATIC_ENTRY(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, caps.array(), caps.size());

    // Scan a default request template for included request keys
    Vector<int32_t> availableRequestKeys;
    const camera_metadata_t *previewRequest =
        constructDefaultRequestSettings(CAMERA3_TEMPLATE_PREVIEW);
    for (size_t i = 0; i < get_camera_metadata_entry_count(previewRequest); i++) {
        camera_metadata_ro_entry_t entry;
        get_camera_metadata_ro_entry(previewRequest, i, &entry);
        availableRequestKeys.add(entry.tag);
    }
    ADD_STATIC_ENTRY(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, availableRequestKeys.array(),
                     availableRequestKeys.size());

    // Add a few more result keys. Must be kept up to date with the various places
    // that add these

    Vector<int32_t> availableResultKeys(availableRequestKeys);
    if (hasCapability(BACKWARD_COMPATIBLE)) {
        availableResultKeys.add(ANDROID_CONTROL_AE_STATE);
        availableResultKeys.add(ANDROID_CONTROL_AF_STATE);
        availableResultKeys.add(ANDROID_CONTROL_AWB_STATE);
        availableResultKeys.add(ANDROID_FLASH_STATE);
        availableResultKeys.add(ANDROID_LENS_STATE);
        availableResultKeys.add(ANDROID_LENS_FOCUS_RANGE);
        availableResultKeys.add(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW);
        availableResultKeys.add(ANDROID_STATISTICS_SCENE_FLICKER);
    }

    availableResultKeys.add(ANDROID_REQUEST_PIPELINE_DEPTH);
    availableResultKeys.add(ANDROID_SENSOR_TIMESTAMP);

    ADD_STATIC_ENTRY(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, availableResultKeys.array(),
                     availableResultKeys.size());

    // Needs to be last, to collect all the keys set

    availableCharacteristicsKeys.add(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    info.update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, availableCharacteristicsKeys);

    mCameraInfo = info.release();

#undef ADD_STATIC_ENTRY
    return OK;
}

status_t VirtualFakeCamera3::process3A(CameraMetadata &settings) {
    /**
     * Extract top-level 3A controls
     */
    status_t res;

    camera_metadata_entry e;

    e = settings.find(ANDROID_CONTROL_MODE);
    if (e.count == 0) {
        ALOGE("%s: No control mode entry!", __FUNCTION__);
        return BAD_VALUE;
    }
    uint8_t controlMode = e.data.u8[0];

    if (controlMode == ANDROID_CONTROL_MODE_OFF) {
        mAeMode = ANDROID_CONTROL_AE_MODE_OFF;
        mAfMode = ANDROID_CONTROL_AF_MODE_OFF;
        mAwbMode = ANDROID_CONTROL_AWB_MODE_OFF;
        mAeState = ANDROID_CONTROL_AE_STATE_INACTIVE;
        mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
        mAwbState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
        update3A(settings);
        return OK;
    } else if (controlMode == ANDROID_CONTROL_MODE_USE_SCENE_MODE) {
        if (!hasCapability(BACKWARD_COMPATIBLE)) {
            ALOGE("%s: Can't use scene mode when BACKWARD_COMPATIBLE not supported!", __FUNCTION__);
            return BAD_VALUE;
        }

        e = settings.find(ANDROID_CONTROL_SCENE_MODE);
        if (e.count == 0) {
            ALOGE("%s: No scene mode entry!", __FUNCTION__);
            return BAD_VALUE;
        }
        uint8_t sceneMode = e.data.u8[0];

        switch (sceneMode) {
            case ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY:
                mFacePriority = true;
                break;
            default:
                ALOGE("%s: Emulator doesn't support scene mode %d", __FUNCTION__, sceneMode);
                return BAD_VALUE;
        }
    } else {
        mFacePriority = false;
    }

    // controlMode == AUTO or sceneMode = FACE_PRIORITY
    // Process individual 3A controls

    res = doFakeAE(settings);
    if (res != OK) return res;

    res = doFakeAF(settings);
    if (res != OK) return res;

    res = doFakeAWB(settings);
    if (res != OK) return res;

    update3A(settings);
    return OK;
}

status_t VirtualFakeCamera3::doFakeAE(CameraMetadata &settings) {
    camera_metadata_entry e;
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_real_distribution<double> dist {1.0, 100000.0};

    e = settings.find(ANDROID_CONTROL_AE_MODE);
    if (e.count == 0 && hasCapability(BACKWARD_COMPATIBLE)) {
        ALOGE("%s: No AE mode entry!", __FUNCTION__);
        return BAD_VALUE;
    }
    uint8_t aeMode = (e.count > 0) ? e.data.u8[0] : (uint8_t)ANDROID_CONTROL_AE_MODE_ON;
    mAeMode = aeMode;

    switch (aeMode) {
        case ANDROID_CONTROL_AE_MODE_OFF:
            // AE is OFF
            mAeState = ANDROID_CONTROL_AE_STATE_INACTIVE;
            return OK;
        case ANDROID_CONTROL_AE_MODE_ON:
            // OK for AUTO modes
            break;
        default:
            // Mostly silently ignore unsupported modes
            ALOGV("%s: Emulator doesn't support AE mode %d, assuming ON", __FUNCTION__, aeMode);
            break;
    }

    e = settings.find(ANDROID_CONTROL_AE_LOCK);
    bool aeLocked = (e.count > 0) ? (e.data.u8[0] == ANDROID_CONTROL_AE_LOCK_ON) : false;

    e = settings.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER);
    bool precaptureTrigger = false;
    if (e.count != 0) {
        precaptureTrigger = (e.data.u8[0] == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START);
    }

    if (precaptureTrigger) {
        //            ALOGV("%s: Pre capture trigger = %d", __FUNCTION__,
        //            precaptureTrigger);
    } else if (e.count > 0) {
        //          ALOGV("%s: Pre capture trigger was present? %zu",
        //              __FUNCTION__,
        //            e.count);
    }

    if (precaptureTrigger || mAeState == ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
        // Run precapture sequence
        if (mAeState != ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
            mAeCounter = 0;
        }

        if (mFacePriority) {
            mAeTargetExposureTime = kFacePriorityExposureTime;
        } else {
            mAeTargetExposureTime = kNormalExposureTime;
        }

        if (mAeCounter > kPrecaptureMinFrames &&
            (mAeTargetExposureTime - mAeCurrentExposureTime) < mAeTargetExposureTime / 10) {
            // Done with precapture
            mAeCounter = 0;
            mAeState =
                aeLocked ? ANDROID_CONTROL_AE_STATE_LOCKED : ANDROID_CONTROL_AE_STATE_CONVERGED;
        } else {
            // Converge some more
            mAeCurrentExposureTime +=
                (mAeTargetExposureTime - mAeCurrentExposureTime) * kExposureTrackRate;
            mAeCounter++;
            mAeState = ANDROID_CONTROL_AE_STATE_PRECAPTURE;
        }
    } else if (!aeLocked) {
        // Run standard occasional AE scan
        switch (mAeState) {
            case ANDROID_CONTROL_AE_STATE_INACTIVE:
                mAeState = ANDROID_CONTROL_AE_STATE_SEARCHING;
                break;
            case ANDROID_CONTROL_AE_STATE_CONVERGED:
                mAeCounter++;
                if (mAeCounter > kStableAeMaxFrames) {
                    mAeTargetExposureTime =
                        mFacePriority ? kFacePriorityExposureTime : kNormalExposureTime;
                    float exposureStep =
                        (dist(mt) / RAND_MAX) * (kExposureWanderMax - kExposureWanderMin) +
                        kExposureWanderMin;
                    mAeTargetExposureTime *= std::pow(2, exposureStep);
                    mAeState = ANDROID_CONTROL_AE_STATE_SEARCHING;
                }
                break;
            case ANDROID_CONTROL_AE_STATE_SEARCHING:
                mAeCurrentExposureTime +=
                    (mAeTargetExposureTime - mAeCurrentExposureTime) * kExposureTrackRate;
                if (abs(mAeTargetExposureTime - mAeCurrentExposureTime) <
                    mAeTargetExposureTime / 10) {
                    // Close enough
                    mAeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
                    mAeCounter = 0;
                }
                break;
            case ANDROID_CONTROL_AE_STATE_LOCKED:
                mAeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
                mAeCounter = 0;
                break;
            default:
                ALOGE("%s: Emulator in unexpected AE state %d", __FUNCTION__, mAeState);
                return INVALID_OPERATION;
        }
    } else {
        // AE is locked
        mAeState = ANDROID_CONTROL_AE_STATE_LOCKED;
    }

    return OK;
}

status_t VirtualFakeCamera3::doFakeAF(CameraMetadata &settings) {
    camera_metadata_entry e;
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<int> dist {1,};

    e = settings.find(ANDROID_CONTROL_AF_MODE);
    if (e.count == 0 && hasCapability(BACKWARD_COMPATIBLE)) {
        ALOGE("%s: No AF mode entry!", __FUNCTION__);
        return BAD_VALUE;
    }
    uint8_t afMode = (e.count > 0) ? e.data.u8[0] : (uint8_t)ANDROID_CONTROL_AF_MODE_OFF;

    e = settings.find(ANDROID_CONTROL_AF_TRIGGER);
    typedef camera_metadata_enum_android_control_af_trigger af_trigger_t;
    af_trigger_t afTrigger;
    if (e.count != 0) {
        afTrigger = static_cast<af_trigger_t>(e.data.u8[0]);

        // ALOGV("%s: AF trigger set to 0x%x", __FUNCTION__, afTrigger);
        // ALOGV("%s: AF mode is 0x%x", __FUNCTION__, afMode);
    } else {
        afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    }

    switch (afMode) {
        case ANDROID_CONTROL_AF_MODE_OFF:
            mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
            return OK;
        case ANDROID_CONTROL_AF_MODE_AUTO:
        case ANDROID_CONTROL_AF_MODE_MACRO:
        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
            if (!mFacingBack) {
                // Always report INACTIVE for front Virtual Camera
                mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
                return OK;
            }
            break;
        default:
            ALOGE("%s: Emulator doesn't support AF mode %d", __FUNCTION__, afMode);
            return BAD_VALUE;
    }

    bool afModeChanged = mAfMode != afMode;
    mAfMode = afMode;

    /**
     * Simulate AF triggers. Transition at most 1 state per frame.
     * - Focusing always succeeds (goes into locked, or PASSIVE_SCAN).
     */

    bool afTriggerStart = false;
    bool afTriggerCancel = false;
    switch (afTrigger) {
        case ANDROID_CONTROL_AF_TRIGGER_IDLE:
            break;
        case ANDROID_CONTROL_AF_TRIGGER_START:
            afTriggerStart = true;
            break;
        case ANDROID_CONTROL_AF_TRIGGER_CANCEL:
            afTriggerCancel = true;
            // Cancel trigger always transitions into INACTIVE
            mAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;

            ALOGVV("%s: AF State transition to STATE_INACTIVE", __FUNCTION__);

            // Stay in 'inactive' until at least next frame
            return OK;
        default:
            ALOGE("%s: Unknown af trigger value %d", __FUNCTION__, afTrigger);
            return BAD_VALUE;
    }

    // If we get down here, we're either in an autofocus mode
    //  or in a continuous focus mode (and no other modes)

    int oldAfState = mAfState;
    switch (mAfState) {
        case ANDROID_CONTROL_AF_STATE_INACTIVE:
            if (afTriggerStart) {
                switch (afMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAfState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        mAfState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                        break;
                }
            } else {
                // At least one frame stays in INACTIVE
                if (!afModeChanged) {
                    switch (afMode) {
                        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                            // fall-through
                        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                            mAfState = ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN;
                            break;
                    }
                }
            }
            break;
        case ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN:
            /**
             * When the AF trigger is activated, the algorithm should finish
             * its PASSIVE_SCAN if active, and then transition into AF_FOCUSED
             * or AF_NOT_FOCUSED as appropriate
             */
            if (afTriggerStart) {
                // Randomly transition to focused or not focused
                if (dist(mt) % 3) {
                    mAfState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
                } else {
                    mAfState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                }
            }
            /**
             * When the AF trigger is not involved, the AF algorithm should
             * start in INACTIVE state, and then transition into PASSIVE_SCAN
             * and PASSIVE_FOCUSED states
             */
            else if (!afTriggerCancel) {
                // Randomly transition to passive focus
                if (dist(mt) % 3 == 0) {
                    mAfState = ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED;
                }
            }

            break;
        case ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED:
            if (afTriggerStart) {
                // Randomly transition to focused or not focused
                if (dist(mt) % 3) {
                    mAfState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
                } else {
                    mAfState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                }
            }
            // TODO: initiate passive scan (PASSIVE_SCAN)
            break;
        case ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN:
            // Simulate AF sweep completing instantaneously

            // Randomly transition to focused or not focused
            if (dist(mt) % 3) {
                mAfState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
            } else {
                mAfState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
            }
            break;
        case ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED:
            if (afTriggerStart) {
                switch (afMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAfState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        // continuous autofocus => trigger start has no effect
                        break;
                }
            }
            break;
        case ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
            if (afTriggerStart) {
                switch (afMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAfState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        // continuous autofocus => trigger start has no effect
                        break;
                }
            }
            break;
        default:
            ALOGE("%s: Bad af state %d", __FUNCTION__, mAfState);
    }

    {
        char afStateString[100] = {
            0,
        };
        camera_metadata_enum_snprint(ANDROID_CONTROL_AF_STATE, oldAfState, afStateString,
                                     sizeof(afStateString));

        char afNewStateString[100] = {
            0,
        };
        camera_metadata_enum_snprint(ANDROID_CONTROL_AF_STATE, mAfState, afNewStateString,
                                     sizeof(afNewStateString));
        ALOGVV("%s: AF state transitioned from %s to %s", __FUNCTION__, afStateString,
               afNewStateString);
    }

    return OK;
}

status_t VirtualFakeCamera3::doFakeAWB(CameraMetadata &settings) {
    camera_metadata_entry e;

    e = settings.find(ANDROID_CONTROL_AWB_MODE);
    if (e.count == 0 && hasCapability(BACKWARD_COMPATIBLE)) {
        ALOGE("%s: No AWB mode entry!", __FUNCTION__);
        return BAD_VALUE;
    }
    uint8_t awbMode = (e.count > 0) ? e.data.u8[0] : (uint8_t)ANDROID_CONTROL_AWB_MODE_AUTO;

    // TODO: Add white balance simulation

    e = settings.find(ANDROID_CONTROL_AWB_LOCK);
    bool awbLocked = (e.count > 0) ? (e.data.u8[0] == ANDROID_CONTROL_AWB_LOCK_ON) : false;

    switch (awbMode) {
        case ANDROID_CONTROL_AWB_MODE_OFF:
            mAwbState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
            break;
        case ANDROID_CONTROL_AWB_MODE_AUTO:
        case ANDROID_CONTROL_AWB_MODE_INCANDESCENT:
        case ANDROID_CONTROL_AWB_MODE_FLUORESCENT:
        case ANDROID_CONTROL_AWB_MODE_DAYLIGHT:
        case ANDROID_CONTROL_AWB_MODE_SHADE:
            // Always magically right, or locked
            mAwbState =
                awbLocked ? ANDROID_CONTROL_AWB_STATE_LOCKED : ANDROID_CONTROL_AWB_STATE_CONVERGED;
            break;
        default:
            ALOGE("%s: Emulator doesn't support AWB mode %d", __FUNCTION__, awbMode);
            return BAD_VALUE;
    }

    return OK;
}

// Update the 3A Region by calculating the intersection of AE/AF/AWB and CROP
// regions
static void update3ARegion(uint32_t tag, CameraMetadata &settings) {
    if (tag != ANDROID_CONTROL_AE_REGIONS && tag != ANDROID_CONTROL_AF_REGIONS &&
        tag != ANDROID_CONTROL_AWB_REGIONS) {
        return;
    }
    camera_metadata_entry_t entry;
    entry = settings.find(ANDROID_SCALER_CROP_REGION);
    if (entry.count > 0) {
        int32_t cropRegion[4];
        cropRegion[0] = entry.data.i32[0];
        cropRegion[1] = entry.data.i32[1];
        cropRegion[2] = entry.data.i32[2] + cropRegion[0];
        cropRegion[3] = entry.data.i32[3] + cropRegion[1];
        entry = settings.find(tag);
        if (entry.count > 0) {
            int32_t *ARegion = entry.data.i32;
            // calculate the intersection of AE/AF/AWB and CROP regions
            if (ARegion[0] < cropRegion[2] && cropRegion[0] < ARegion[2] &&
                ARegion[1] < cropRegion[3] && cropRegion[1] < ARegion[3]) {
                int32_t interSect[5];
                interSect[0] = std::max(ARegion[0], cropRegion[0]);
                interSect[1] = std::max(ARegion[1], cropRegion[1]);
                interSect[2] = std::min(ARegion[2], cropRegion[2]);
                interSect[3] = std::min(ARegion[3], cropRegion[3]);
                interSect[4] = ARegion[4];
                settings.update(tag, &interSect[0], 5);
            }
        }
    }
}

void VirtualFakeCamera3::update3A(CameraMetadata &settings) {
    if (mAeMode != ANDROID_CONTROL_AE_MODE_OFF) {
        settings.update(ANDROID_SENSOR_EXPOSURE_TIME, &mAeCurrentExposureTime, 1);
        settings.update(ANDROID_SENSOR_SENSITIVITY, &mAeCurrentSensitivity, 1);
    }

    settings.update(ANDROID_CONTROL_AE_STATE, &mAeState, 1);
    settings.update(ANDROID_CONTROL_AF_STATE, &mAfState, 1);
    settings.update(ANDROID_CONTROL_AWB_STATE, &mAwbState, 1);

    uint8_t lensState;
    switch (mAfState) {
        case ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN:
        case ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN:
            lensState = ANDROID_LENS_STATE_MOVING;
            break;
        case ANDROID_CONTROL_AF_STATE_INACTIVE:
        case ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED:
        case ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED:
        case ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
        case ANDROID_CONTROL_AF_STATE_PASSIVE_UNFOCUSED:
        default:
            lensState = ANDROID_LENS_STATE_STATIONARY;
            break;
    }
    settings.update(ANDROID_LENS_STATE, &lensState, 1);
    update3ARegion(ANDROID_CONTROL_AE_REGIONS, settings);
    update3ARegion(ANDROID_CONTROL_AF_REGIONS, settings);
    update3ARegion(ANDROID_CONTROL_AWB_REGIONS, settings);
}

void VirtualFakeCamera3::signalReadoutIdle() {
    Mutex::Autolock l(mLock);
    // Need to chek isIdle again because waiting on mLock may have allowed
    // something to be placed in the in-flight queue.
    if (mStatus == STATUS_ACTIVE && mReadoutThread->isIdle()) {
        ALOGV("Now idle");
        mStatus = STATUS_READY;
    }
}

void VirtualFakeCamera3::onSensorEvent(uint32_t frameNumber, Event e, nsecs_t timestamp) {
    switch (e) {
        case Sensor::SensorListener::EXPOSURE_START: {
            //          ALOGVV("%s: Frame %d: Sensor started exposure at %lld",
            //               __FUNCTION__, frameNumber, timestamp);
            // Trigger shutter notify to framework
            camera3_notify_msg_t msg;
            msg.type = CAMERA3_MSG_SHUTTER;
            msg.message.shutter.frame_number = frameNumber;
            msg.message.shutter.timestamp = timestamp;
            sendNotify(&msg);
            break;
        }
        default:
            ALOGW("%s: Unexpected sensor event %d at %" PRId64, __FUNCTION__, e, timestamp);
            break;
    }
}

VirtualFakeCamera3::ReadoutThread::ReadoutThread(VirtualFakeCamera3 *parent)
    : mParent(parent), mJpegWaiting(false) {
    mThreadActive = false;
    mJpegFrameNumber = 0;
    mJpegHalBuffer.acquire_fence = -1;
    mJpegHalBuffer.release_fence = -1;
    mJpegHalBuffer.buffer = NULL;
    mJpegHalBuffer.status = CAMERA3_BUFFER_STATUS_ERROR;
    mJpegHalBuffer.stream = NULL;
}

VirtualFakeCamera3::ReadoutThread::~ReadoutThread() {
    for (List<Request>::iterator i = mInFlightQueue.begin(); i != mInFlightQueue.end(); i++) {
        delete i->buffers;
        delete i->sensorBuffers;
    }
}

void VirtualFakeCamera3::ReadoutThread::queueCaptureRequest(const Request &r) {
    Mutex::Autolock l(mLock);

    mInFlightQueue.push_back(r);
    mInFlightSignal.signal();
}

bool VirtualFakeCamera3::ReadoutThread::isIdle() {
    Mutex::Autolock l(mLock);
    return mInFlightQueue.empty() && !mThreadActive;
}

status_t VirtualFakeCamera3::ReadoutThread::waitForReadout() {
    status_t res;
    Mutex::Autolock l(mLock);
    int loopCount = 0;
    while (mInFlightQueue.size() >= kMaxQueueSize) {
        res = mInFlightSignal.waitRelative(mLock, kWaitPerLoop);
        if (res != OK && res != TIMED_OUT) {
            ALOGE("%s: Error waiting for in-flight queue to shrink", __FUNCTION__);
            return INVALID_OPERATION;
        }
        if (loopCount == kMaxWaitLoops) {
            ALOGE("%s: Timed out waiting for in-flight queue to shrink", __FUNCTION__);
            return TIMED_OUT;
        }
        loopCount++;
    }
    return OK;
}

bool VirtualFakeCamera3::ReadoutThread::threadLoop() {
    status_t res = NO_ERROR;

    ALOGVV("%s: ReadoutThread waiting for request", __FUNCTION__);

    // First wait for a request from the in-flight queue

    if (mCurrentRequest.settings.isEmpty()) {
        Mutex::Autolock l(mLock);
        if (mInFlightQueue.empty()) {
            res = mInFlightSignal.waitRelative(mLock, kWaitPerLoop);
            if (res == TIMED_OUT) {
                ALOGVV("%s: ReadoutThread: Timed out waiting for request", __FUNCTION__);
                return true;
            } else if (res != NO_ERROR) {
                ALOGE("%s: Error waiting for capture requests: %d", __FUNCTION__, res);
                return false;
            }
        }
        mCurrentRequest.frameNumber = mInFlightQueue.begin()->frameNumber;
        mCurrentRequest.settings.acquire(mInFlightQueue.begin()->settings);
        mCurrentRequest.buffers = mInFlightQueue.begin()->buffers;
        mCurrentRequest.sensorBuffers = mInFlightQueue.begin()->sensorBuffers;
        mInFlightQueue.erase(mInFlightQueue.begin());
        mInFlightSignal.signal();
        mThreadActive = true;
        ALOGVV("%s: Beginning readout of frame %d", __FUNCTION__, mCurrentRequest.frameNumber);
    }

    // Then wait for it to be delivered from the sensor
    ALOGVV("%s: ReadoutThread: Wait for frame to be delivered from sensor", __FUNCTION__);

    nsecs_t captureTime;
    bool gotFrame = mParent->mSensor->waitForNewFrame(kWaitPerLoop, &captureTime);
    if (!gotFrame) {
        ALOGVV("%s: ReadoutThread: Timed out waiting for sensor frame", __FUNCTION__);
        return true;
    }

    //     ALOGVV("Sensor done with readout for frame %d, captured at %lld ",
    //          mCurrentRequest.frameNumber, captureTime);

    // Check if we need to JPEG encode a buffer, and send it for async
    // compression if so. Otherwise prepare the buffer for return.
    bool needJpeg = false;
    HalBufferVector::iterator buf = mCurrentRequest.buffers->begin();
    while (buf != mCurrentRequest.buffers->end()) {
        bool goodBuffer = true;
        if (buf->stream->format == HAL_PIXEL_FORMAT_BLOB &&
            buf->stream->data_space != HAL_DATASPACE_DEPTH) {
            Mutex::Autolock jl(mJpegLock);
            if (mJpegWaiting) {
                // This shouldn't happen, because processCaptureRequest should
                // be stalling until JPEG compressor is free.
                ALOGI("%s: Already processing a JPEG!", __FUNCTION__);
                goodBuffer = false;
            }
            if (goodBuffer) {
                // Compressor takes ownership of sensorBuffers here
                res = mParent->mJpegCompressor->start(mCurrentRequest.sensorBuffers, this,
                                                      &(mCurrentRequest.settings));
                goodBuffer = (res == OK);
            }
            if (goodBuffer) {
                needJpeg = true;
                ALOGVV("Sensor done with readout for frame %d, needJpeg = %d",
                       mCurrentRequest.frameNumber, needJpeg);

                mJpegHalBuffer = *buf;
                mJpegFrameNumber = mCurrentRequest.frameNumber;
                mJpegWaiting = true;

                mCurrentRequest.sensorBuffers = NULL;
                buf = mCurrentRequest.buffers->erase(buf);

                continue;
            }
            ALOGE("%s: Error compressing output buffer: %s (%d)", __FUNCTION__, strerror(-res),
                  res);
            // fallthrough for cleanup
        }

        buf->status = goodBuffer ? CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
        buf->acquire_fence = -1;
        buf->release_fence = -1;

        ++buf;
    }  // end while

    // Construct result for all completed buffers and results

    camera3_capture_result result = {};

    if (mParent->hasCapability(BACKWARD_COMPATIBLE)) {
        static const uint8_t sceneFlicker = ANDROID_STATISTICS_SCENE_FLICKER_NONE;
        mCurrentRequest.settings.update(ANDROID_STATISTICS_SCENE_FLICKER, &sceneFlicker, 1);

        static const uint8_t flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
        mCurrentRequest.settings.update(ANDROID_FLASH_STATE, &flashState, 1);

        nsecs_t rollingShutterSkew = Sensor::kFrameDurationRange[0];
        mCurrentRequest.settings.update(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW, &rollingShutterSkew,
                                        1);

        float focusRange[] = {1.0f / 5.0f, 0};  // 5 m to infinity in focus
        mCurrentRequest.settings.update(ANDROID_LENS_FOCUS_RANGE, focusRange,
                                        sizeof(focusRange) / sizeof(float));
    }

    mCurrentRequest.settings.update(ANDROID_SENSOR_TIMESTAMP, &captureTime, 1);

    // JPEGs take a stage longer
    const uint8_t pipelineDepth = needJpeg ? kMaxBufferCount : kMaxBufferCount - 1;
    mCurrentRequest.settings.update(ANDROID_REQUEST_PIPELINE_DEPTH, &pipelineDepth, 1);

    result.frame_number = mCurrentRequest.frameNumber;
    result.result = mCurrentRequest.settings.getAndLock();
    result.num_output_buffers = mCurrentRequest.buffers->size();
    result.output_buffers = mCurrentRequest.buffers->array();
    result.input_buffer = nullptr;
    result.partial_result = 1;

    // Go idle if queue is empty, before sending result
    bool signalIdle = false;
    {
        Mutex::Autolock l(mLock);
        if (mInFlightQueue.empty()) {
            mThreadActive = false;
            signalIdle = true;
        }
    }
    if (signalIdle) mParent->signalReadoutIdle();

    if (mCurrentRequest.sensorBuffers != NULL) {
        for (int i = 0; i < mCurrentRequest.sensorBuffers->size(); i++) {
            const StreamBuffer &b = (*mCurrentRequest.sensorBuffers)[i];
            GrallocModule::getInstance().unlock(b.importedHandle);
            GrallocModule::getInstance().release(b.importedHandle);
        }
    }

    // Send it off to the framework
    ALOGVV("%s: ReadoutThread: Send result to framework", __FUNCTION__);
    mParent->sendCaptureResult(&result);

    // Clean up
    mCurrentRequest.settings.unlock(result.result);

    delete mCurrentRequest.buffers;
    mCurrentRequest.buffers = NULL;
    if (!needJpeg) {
        delete mCurrentRequest.sensorBuffers;
        mCurrentRequest.sensorBuffers = NULL;
    }
    mCurrentRequest.settings.clear();

    return true;
}

void VirtualFakeCamera3::ReadoutThread::onJpegDone(const StreamBuffer &jpegBuffer, bool success) {
    Mutex::Autolock jl(mJpegLock);

    mJpegHalBuffer.status = success ? CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
    mJpegHalBuffer.acquire_fence = -1;
    mJpegHalBuffer.release_fence = -1;
    mJpegWaiting = false;

    camera3_capture_result result = {};

    result.frame_number = mJpegFrameNumber;
    result.result = NULL;
    result.num_output_buffers = 1;
    result.output_buffers = &mJpegHalBuffer;
    result.input_buffer = nullptr;
    result.partial_result = 0;

    if (!success) {
        ALOGE(
            "%s: Compression failure, returning error state buffer to"
            " framework",
            __FUNCTION__);
    } else {
        ALOGV("%s: Compression complete, returning buffer to framework", __FUNCTION__);
    }

    GrallocModule::getInstance().unlock(jpegBuffer.importedHandle);
    GrallocModule::getInstance().release(jpegBuffer.importedHandle);
    mParent->sendCaptureResult(&result);
}

void VirtualFakeCamera3::ReadoutThread::onJpegInputDone(const StreamBuffer &inputBuffer) {
    // Should never get here, since the input buffer has to be returned
    // by end of processCaptureRequest
    ALOGE("%s: Unexpected input buffer from JPEG compressor!", __FUNCTION__);
}

};  // namespace android
