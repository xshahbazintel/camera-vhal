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

//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "sensor"

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include "fake-pipeline2/Sensor.h"
#include "CGCodec.h"
#include <libyuv.h>
#include <log/log.h>
#include <cmath>
#include <future>
#include <mutex>
#include <cstdlib>
#include <cutils/properties.h>
#include <functional>
#include <chrono>
#include <thread>
#include <string>
#include "system/camera_metadata.h"
#include "GrallocModule.h"

#define BPP_RGB32 4

using namespace std::string_literals;

namespace android {
std::mutex client_buf_mutex;

// const nsecs_t Sensor::kExposureTimeRange[2] =
//    {1000L, 30000000000L} ; // 1 us - 30 sec
// const nsecs_t Sensor::kFrameDurationRange[2] =
//    {33331760L, 30000000000L}; // ~1/30 s - 30 sec
const nsecs_t Sensor::kExposureTimeRange[2] = {1000L, 300000000L};       // 1 us - 0.3 sec
const nsecs_t Sensor::kFrameDurationRange[2] = {33331760L, 300000000L};  // ~1/30 s - 0.3 sec

const nsecs_t Sensor::kMinVerticalBlank = 10000L;

const uint8_t Sensor::kColorFilterArrangement = ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;

// Output image data characteristics
const uint32_t Sensor::kMaxRawValue = 4000;
const uint32_t Sensor::kBlackLevel = 1000;

// Sensor sensitivity
const float Sensor::kSaturationVoltage = 0.520f;
const uint32_t Sensor::kSaturationElectrons = 2000;
const float Sensor::kVoltsPerLuxSecond = 0.100f;

const float Sensor::kElectronsPerLuxSecond =
    Sensor::kSaturationElectrons / Sensor::kSaturationVoltage * Sensor::kVoltsPerLuxSecond;

const float Sensor::kBaseGainFactor = (float)Sensor::kMaxRawValue / Sensor::kSaturationElectrons;

const float Sensor::kReadNoiseStddevBeforeGain = 1.177;  // in electrons
const float Sensor::kReadNoiseStddevAfterGain = 2.100;   // in digital counts
const float Sensor::kReadNoiseVarBeforeGain =
    Sensor::kReadNoiseStddevBeforeGain * Sensor::kReadNoiseStddevBeforeGain;
const float Sensor::kReadNoiseVarAfterGain =
    Sensor::kReadNoiseStddevAfterGain * Sensor::kReadNoiseStddevAfterGain;

const int32_t Sensor::kSensitivityRange[2] = {100, 1600};
const uint32_t Sensor::kDefaultSensitivity = 100;

/** A few utility functions for math, normal distributions */

// Take advantage of IEEE floating-point format to calculate an approximate
// square root. Accurate to within +-3.6%
float sqrtf_approx(float r) {
    // Modifier is based on IEEE floating-point representation; the
    // manipulations boil down to finding approximate log2, dividing by two,
    // and then inverting the log2. A bias is added to make the relative
    // error symmetric about the real answer.
    const int32_t modifier = 0x1FBB4000;

    int32_t r_i = *(int32_t *)(&r);
    r_i = (r_i >> 1) + modifier;

    return *(float *)(&r_i);
}

Sensor::Sensor(uint32_t camera_id, uint32_t width, uint32_t height, 
               std::shared_ptr<CGVideoDecoder> decoder,
               std::shared_ptr<ClientVideoBuffer> cameraBuffer)
    : Thread(false),
      mResolution{width, height},
      mActiveArray{0, 0, width, height},
      mRowReadoutTime(kFrameDurationRange[0] / height),
      mExposureTime(kFrameDurationRange[0] - kMinVerticalBlank),
      mFrameDuration(kFrameDurationRange[0]),
      mScene(width, height, kElectronsPerLuxSecond),
      mCameraId(camera_id),
      mDecoder{decoder},
      mCameraBuffer(cameraBuffer) {
    // Max supported resolution of the camera sensor.
    // It is based on client camera capability info.
    mSrcWidth = width;
    mSrcHeight = height;
    mSrcFrameSize = mSrcWidth * mSrcHeight * BPP_NV12;
    mDumpEnabled = 0;
}

Sensor::~Sensor() { shutDown(); }

status_t Sensor::startUp() {
    ALOGI(LOG_TAG "%s: E", __FUNCTION__);

    int res;
    mCapturedBuffers = nullptr;

    const hw_module_t *module = nullptr;
    int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
    if (ret) {
        ALOGE(LOG_TAG "%s: Failed to get gralloc module: %d", __FUNCTION__, ret);
    }
    m_major_version = (module->module_api_version >> 8) & 0xff;
    ALOGI(LOG_TAG " m_major_version[%d]", m_major_version);

    res = run("Sensor", ANDROID_PRIORITY_URGENT_DISPLAY);

    if (res != OK) {
        ALOGE("Unable to start up sensor capture thread: %d", res);
    }
    return res;
}

status_t Sensor::shutDown() {
    ALOGVV("%s: E", __FUNCTION__);

    int res;
    res = requestExitAndWait();
    if (res != OK) {
        ALOGE("Unable to shut down sensor capture thread: %d", res);
    }
    return res;
}

Scene &Sensor::getScene() { return mScene; }

void Sensor::setExposureTime(uint64_t ns) {
    Mutex::Autolock lock(mControlMutex);
    ALOGVV("Exposure set to %f", ns / 1000000.f);
    mExposureTime = ns;
}

void Sensor::setFrameDuration(uint64_t ns) {
    Mutex::Autolock lock(mControlMutex);
    ALOGVV("Frame duration set to %f", ns / 1000000.f);
    mFrameDuration = ns;
}

void Sensor::setSensitivity(uint32_t gain) {
    Mutex::Autolock lock(mControlMutex);
    ALOGVV("Gain set to %d", gain);
    mGainFactor = gain;
}

void Sensor::setDestinationBuffers(Buffers *buffers) {
    Mutex::Autolock lock(mControlMutex);
    mNextBuffers = buffers;
}

void Sensor::setFrameNumber(uint32_t frameNumber) {
    Mutex::Autolock lock(mControlMutex);
    mFrameNumber = frameNumber;
}

bool Sensor::waitForVSync(nsecs_t reltime) {
    int res;
    Mutex::Autolock lock(mControlMutex);

    mGotVSync = false;
    res = mVSync.waitRelative(mControlMutex, reltime);
    if (res != OK && res != TIMED_OUT) {
        ALOGE("%s: Error waiting for VSync signal: %d", __FUNCTION__, res);
        return false;
    }
    return mGotVSync;
}

bool Sensor::waitForNewFrame(nsecs_t reltime, nsecs_t *captureTime) {
    Mutex::Autolock lock(mReadoutMutex);
    if (mCapturedBuffers == nullptr) {
        int res;
        res = mReadoutAvailable.waitRelative(mReadoutMutex, reltime);
        if (res == TIMED_OUT) {
            return false;
        } else if (res != OK || mCapturedBuffers == nullptr) {
            ALOGE("Error waiting for sensor readout signal: %d", res);
            return false;
        }
    }
    mReadoutComplete.signal();

    *captureTime = mCaptureTime;
    mCapturedBuffers = nullptr;
    return true;
}

Sensor::SensorListener::~SensorListener() {}

void Sensor::setSensorListener(SensorListener *listener) {
    Mutex::Autolock lock(mControlMutex);
    mListener = listener;
}

status_t Sensor::readyToRun() {
    ALOGV("Starting up sensor thread");
    // mStartupTime = systemTime();
    mNextCaptureTime = 0;
    mNextCapturedBuffers = nullptr;
    return OK;
}

bool Sensor::threadLoop() {
    /**
     * Sensor capture operation main loop.
     *
     * Stages are out-of-order relative to a single frame's processing, but
     * in-order in time.
     */

    /**
     * Stage 1: Read in latest control parameters
     */
    uint64_t exposureDuration;
    uint64_t frameDuration;
    uint32_t gain;
    Buffers *nextBuffers;
    uint32_t frameNumber;
    char value[PROPERTY_VALUE_MAX];
    ALOGVV("Sensor Thread stage E :1");
    SensorListener *listener = nullptr;
    {
        Mutex::Autolock lock(mControlMutex);
        exposureDuration = mExposureTime;
        frameDuration = mFrameDuration;
        gain = mGainFactor;
        nextBuffers = mNextBuffers;
        frameNumber = mFrameNumber;
        listener = mListener;
        // Don't reuse a buffer set
        mNextBuffers = nullptr;

        // Signal VSync for start of readout
        ALOGVV("Sensor VSync");
        mGotVSync = true;
        mVSync.signal();
    }
    ALOGVV("Sensor Thread stage X :1");

    /**
     * Stage 3: Read out latest captured image
     */
    ALOGVV("Sensor Thread stage E :2");

    Buffers *capturedBuffers = nullptr;
    nsecs_t captureTime = 0;

    nsecs_t startRealTime = systemTime();
    // Stagefright cares about system time for timestamps, so base simulated
    // time on that.
    nsecs_t simulatedTime = startRealTime;
    nsecs_t frameEndRealTime = startRealTime + frameDuration;

    if (mNextCapturedBuffers != nullptr) {
        ALOGVV("Sensor starting readout");
        // Pretend we're doing readout now; will signal once enough time
        // has elapsed
        capturedBuffers = mNextCapturedBuffers;
        captureTime = mNextCaptureTime;
    }
    simulatedTime += mRowReadoutTime + kMinVerticalBlank;

    // TODO: Move this signal to another thread to simulate readout
    // time properly
    if (capturedBuffers != nullptr) {
        ALOGVV("Sensor readout complete");
        Mutex::Autolock lock(mReadoutMutex);
        if (mCapturedBuffers != nullptr) {
            ALOGV("Waiting for readout thread to catch up!");
            mReadoutComplete.wait(mReadoutMutex);
        }

        mCapturedBuffers = capturedBuffers;
        mCaptureTime = captureTime;
        mReadoutAvailable.signal();
        capturedBuffers = nullptr;
    }
    ALOGVV("Sensor Thread stage X :2");

    /**
     * Stage 2: Capture new image
     */
    ALOGVV("Sensor Thread stage E :3");
    mNextCaptureTime = simulatedTime;
    mNextCapturedBuffers = nextBuffers;

    if (mNextCapturedBuffers != nullptr) {
        if (listener != nullptr) {
            listener->onSensorEvent(frameNumber, SensorListener::EXPOSURE_START, mNextCaptureTime);
        }
        ALOGVV("Starting next capture: Exposure: %f ms, gain: %d", (float)exposureDuration / 1e6,
               gain);
        mScene.setExposureDuration((float)exposureDuration / 1e9);
        mScene.calculateScene(mNextCaptureTime);

        mCameraBuffer->clientBuf.decoded = false;

        // Check the vendor specific property to dump the raw frames
        // if it is set to '1'
        property_get("vendor.camera.dump.uncompressed", value, 0);
        mDumpEnabled = atoi(value);

        // Might be adding more buffers, so size isn't constant
        for (size_t i = 0; i < mNextCapturedBuffers->size(); i++) {
            const StreamBuffer &b = (*mNextCapturedBuffers)[i];
            ALOGVV(
                "Sensor capturing buffer %zu: stream %d,"
                " %d x %d, format %x, stride %d, buf %p, img %p",
                i, b.streamId, b.width, b.height, b.format, b.stride, b.buffer, b.img);
            switch (b.format) {
                case HAL_PIXEL_FORMAT_RAW16:
                    captureRaw(b.img, gain, b.stride);
                    break;
                case HAL_PIXEL_FORMAT_RGB_888:
                    captureRGB(b.img, gain, b.width, b.height);
                    break;
                case HAL_PIXEL_FORMAT_RGBA_8888:
                    captureRGBA(b.img, gain, b.width, b.height);
                    break;
                case HAL_PIXEL_FORMAT_BLOB:
                    if (b.dataSpace != HAL_DATASPACE_DEPTH) {
                        // Add auxillary buffer of the right
                        // size Assumes only one BLOB (JPEG)
                        // buffer in mNextCapturedBuffers
                        StreamBuffer bAux = {};
                        bAux.streamId = 0;
                        bAux.width = b.width;
                        bAux.height = b.height;
                        bAux.format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
                        bAux.stride = b.width;
                        bAux.buffer = nullptr;
                        bAux.img = new uint8_t[b.width * b.height * 3];
                        mNextCapturedBuffers->push_back(bAux);
                    } else {
                        captureDepthCloud(b.img);
                    }
                    break;
                case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                    captureNV21(b.img, gain, b.width, b.height);
                    break;
                case HAL_PIXEL_FORMAT_YCbCr_420_888:
                    captureNV12(b.img, gain, b.width, b.height);
                    break;
                case HAL_PIXEL_FORMAT_YV12:
                    // TODO:
                    ALOGE("%s: Format %x is TODO", __FUNCTION__, b.format);
                    break;
                case HAL_PIXEL_FORMAT_Y16:
                    captureDepth(b.img, gain, b.width, b.height);
                    break;
                default:
                    ALOGE("%s: Unknown format %x, no output", __FUNCTION__, b.format);
                    break;
            }
        }
    }

    ALOGVV("Sensor Thread stage X :3");

    ALOGVV("Sensor Thread stage E :4");
    ALOGVV("Sensor vertical blanking interval");
    nsecs_t workDoneRealTime = systemTime();
    const nsecs_t timeAccuracy =
        3e6;  // 3 ms of imprecision is ok. TODO: This imprecision needs to be revisted.
    if (workDoneRealTime < frameEndRealTime - timeAccuracy) {
        timespec t;
        t.tv_sec = (frameEndRealTime - timeAccuracy - workDoneRealTime) / 1000000000L;
        t.tv_nsec = (frameEndRealTime - timeAccuracy - workDoneRealTime) % 1000000000L;

        int ret;
        do {
            ret = nanosleep(&t, &t);
        } while (ret != 0);
    }

    ALOGVV("Sensor Thread stage X :4");
    ALOGVV("Frame No: %d took %d ms, target %d ms", frameNumber,
           (int)(workDoneRealTime - startRealTime) / 1000000, (int)(frameDuration / 1000000));
    return true;
};

void Sensor::captureRaw(uint8_t *img, uint32_t gain, uint32_t stride) {
    ALOGVV("%s", __FUNCTION__);
    float totalGain = gain / 100.0 * kBaseGainFactor;
    float noiseVarGain = totalGain * totalGain;
    float readNoiseVar = kReadNoiseVarBeforeGain * noiseVarGain + kReadNoiseVarAfterGain;

    int bayerSelect[4] = {Scene::R, Scene::Gr, Scene::Gb, Scene::B};  // RGGB
    mScene.setReadoutPixel(0, 0);
    for (unsigned int y = 0; y < mResolution[1]; y++) {
        int *bayerRow = bayerSelect + (y & 0x1) * 2;
        uint16_t *px = (uint16_t *)img + y * stride;
        for (unsigned int x = 0; x < mResolution[0]; x++) {
            uint32_t electronCount;
            electronCount = mScene.getPixelElectrons()[bayerRow[x & 0x1]];

            // TODO: Better pixel saturation curve?
            electronCount =
                (electronCount < kSaturationElectrons) ? electronCount : kSaturationElectrons;

            // TODO: Better A/D saturation curve?
            uint16_t rawCount = electronCount * totalGain;
            rawCount = (rawCount < kMaxRawValue) ? rawCount : kMaxRawValue;

            // Calculate noise value
            // TODO: Use more-correct Gaussian instead of uniform
            // noise
            float photonNoiseVar = electronCount * noiseVarGain;
            float noiseStddev = sqrtf_approx(readNoiseVar + photonNoiseVar);
            // Scaled to roughly match gaussian/uniform noise stddev
            float noiseSample = std::rand() * (2.5 / (1.0 + RAND_MAX)) - 1.25;

            rawCount += kBlackLevel;
            rawCount += noiseStddev * noiseSample;

            *px++ = rawCount;
        }
    }
    ALOGVV("Raw sensor image captured");
}

void Sensor::dumpFrame(uint8_t *frame_addr, size_t frame_size, uint32_t camera_id,
                       const char *frame_type, uint32_t resolution, size_t frame_count) {
    char filename[64] = {'\0'};
    snprintf(filename, sizeof(filename), "/ipc/DUMP_vHAL_CAM%u_%s_%up_%zu",
             camera_id, frame_type, resolution, frame_count);
    FILE *fp = fopen(filename, "w+");
    if (fp) {
        fwrite(frame_addr, frame_size, 1, fp);
        fclose(fp);
    } else {
        ALOGE("%s: file open failed!!!", __func__);
    }
}

bool Sensor::getNV12Frames(uint8_t *input_buf, int *camera_input_size,
                           std::chrono::milliseconds timeout_ms /* default 5ms */) {
    auto cg_video_frame = std::make_shared<CGVideoFrame>();
    size_t retry_count = 0;
    size_t maxRetryCount;

    if (!gUseVaapi) {  // SW decoding
        timeout_ms = 10ms;
        maxRetryCount = 10;
    } else {
        maxRetryCount = 5;
    }

    do {
        int ret = mDecoder->get_decoded_frame(cg_video_frame);
        if (ret == 0) {  // success
            ALOGVV("%s frames are decoded", __func__);
            break;
        } else if (retry_count++ <= maxRetryCount) {  // decoded frames are not ready
            ALOGVV("%s retry #%zu get_decoded_frame() not ready, lets wait for %zums", __func__,
                   retry_count, size_t(timeout_ms.count()));
            std::this_thread::sleep_for(timeout_ms);
            continue;
        } else if (retry_count > maxRetryCount) {
            ALOGE(
                "%s Failed to get decoded frames even after retrying %zu times with total timeout "
                "of %zums",
                __func__, (retry_count - 1), (retry_count - 1) * size_t(timeout_ms.count()));
            return false;
        }
    } while (true);

    cg_video_frame->copy_to_buffer(input_buf, camera_input_size);
    ALOGVV("%s converted to format: %s size: %d \n", __FUNCTION__,
           cg_video_frame->format() == NV12 ? "NV12" : "I420", *camera_input_size);
    ALOGVV("%s decoded buffers are copied", __func__);

    return true;
}

void Sensor::captureRGBA(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height) {
    ALOGVV("%s: E", __FUNCTION__);

    uint8_t *bufData = mCameraBuffer->clientBuf.buffer;
    int cameraInputDataSize;
    size_t frameSizeRGB32 = mSrcWidth * mSrcHeight * BPP_RGB32;
    static size_t frameCount = 0;

    if (!gIsInFrameI420 && !gIsInFrameH264) {
        ALOGE("%s Exit - only H264, H265, I420 input frames supported", __FUNCTION__);
        return;
    }

    // Initialize the input data size based on client camera resolution.
    cameraInputDataSize = mSrcFrameSize;

    if (gIsInFrameH264) {
        if (mCameraBuffer->clientBuf.decoded) {
            // Note: bufData already assigned in the function start
            ALOGVV("%s - Already Decoded Camera Input Frame..", __FUNCTION__);
        } else {  // This is the default condition in all apps.
                  // To get the decoded frame.
            getNV12Frames(bufData, &cameraInputDataSize);
            mCameraBuffer->clientBuf.decoded = true;
            std::unique_lock<std::mutex> ulock(client_buf_mutex);
            mCameraBuffer->decodedFrameNo++;
            ALOGVV("%s Decoded Camera Input Frame No: %zd with size of %d", __FUNCTION__,
                   mCameraBuffer->decodedFrameNo, cameraInputDataSize);
            ulock.unlock();
        }
    }

    int src_size = mSrcWidth * mSrcHeight;
    int dstFrameSize = width * height;

    // For Max supported Resolution.
    if (width == (uint32_t)mSrcWidth && height == (uint32_t)mSrcHeight) {
        if (gIsInFrameI420) {
            ALOGVV(LOG_TAG " %s: I420, scaling not required: Size = %dx%d", __FUNCTION__, width,
                   height);
            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            uint8_t *dst_abgr = img;
            int dst_stride_abgr = width * 4;

            if (int ret =
                    libyuv::I420ToABGR(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                       src_stride_v, dst_abgr, dst_stride_abgr, width, height)) {
            }
        } else {
            ALOGVV(LOG_TAG " %s: NV12, scaling not required: Size = %dx%d", __FUNCTION__, width,
                   height);
            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_uv = bufData + src_size;
            int src_stride_uv = width;
            uint8_t *dst_abgr = img;
            int dst_stride_abgr = width * 4;

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV12 input [%zu] for preview/video",__FUNCTION__, frameCount);
                dumpFrame(bufData, mSrcFrameSize, mCameraId, "NV12", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }

            if (int ret = libyuv::NV12ToABGR(src_y, src_stride_y, src_uv, src_stride_uv, dst_abgr,
                                             dst_stride_abgr, width, height)) {
            }

	    if (mDumpEnabled) {
                ALOGI("%s: Dump RGB32 output [%zu] for preview/video",__FUNCTION__, frameCount);
                dumpFrame(img, frameSizeRGB32, mCameraId, "RGB32", height, frameCount);
            }
        }
        // For upscaling and downscaling all other resolutions below max supported resolution.
    } else {
        if (gIsInFrameI420) {
            ALOGVV(LOG_TAG " %s: I420, need to scale: Size = %dx%d", __FUNCTION__, width, height);
            int destFrameSize = width * height;

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;
            uint8_t *dst_y = mDstPrevBuf.data();
            int dst_stride_y = width;
            uint8_t *dst_u = mDstPrevBuf.data() + destFrameSize;
            int dst_stride_u = width >> 1;
            uint8_t *dst_v = mDstPrevBuf.data() + destFrameSize + destFrameSize / 4;
            int dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }
            ALOGVV("%s: I420, Scaling done!", __FUNCTION__);

            src_y = mDstPrevBuf.data();
            src_stride_y = width;
            src_u = mDstPrevBuf.data() + destFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstPrevBuf.data() + destFrameSize + destFrameSize / 4;
            src_stride_v = width >> 1;
            uint8_t *dst_abgr = img;
            int dst_stride_abgr = width * 4;

            if (int ret =
                    libyuv::I420ToABGR(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                       src_stride_v, dst_abgr, dst_stride_abgr, width, height)) {
            }
        } else {
            ALOGVV(LOG_TAG " %s: NV12 scaling required: Size = %dx%d", __FUNCTION__, width, height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_uv = bufData + src_size;
            int src_stride_uv = mSrcWidth;
            uint8_t *dst_y = mDstTempPrevBuf.data();
            int dst_stride_y = mSrcWidth;
            uint8_t *dst_u = mDstTempPrevBuf.data() + src_size;
            int dst_stride_u = mSrcWidth >> 1;
            uint8_t *dst_v = mDstTempPrevBuf.data() + src_size + src_size / 4;
            int dst_stride_v = mSrcWidth >> 1;

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV12 input [%zu] for preview/video",__FUNCTION__, frameCount);
                dumpFrame(bufData, mSrcFrameSize, mCameraId, "NV12", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }

            if (int ret = libyuv::NV12ToI420(src_y, src_stride_y, src_uv, src_stride_uv, dst_y,
                                             dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                             mSrcWidth, mSrcHeight)) {
            }

            src_y = mDstTempPrevBuf.data();
            src_stride_y = mSrcWidth;
            const uint8_t *src_u = mDstTempPrevBuf.data() + src_size;
            int src_stride_u = mSrcWidth >> 1;
            const uint8_t *src_v = mDstTempPrevBuf.data() + src_size + src_size / 4;
            int src_stride_v = mSrcWidth >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;

            dst_y = mDstPrevBuf.data();
            dst_stride_y = width;
            dst_u = mDstPrevBuf.data() + dstFrameSize;
            dst_stride_u = width >> 1;
            dst_v = mDstPrevBuf.data() + dstFrameSize + dstFrameSize / 4;
            dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }

            src_y = mDstPrevBuf.data();
            src_stride_y = width;
            src_u = mDstPrevBuf.data() + dstFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstPrevBuf.data() + dstFrameSize + dstFrameSize / 4;
            src_stride_v = width >> 1;

            uint8_t *dst_abgr = img;
            int dst_stride_abgr = width * 4;

            if (int ret =
                    libyuv::I420ToABGR(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                       src_stride_v, dst_abgr, dst_stride_abgr, width, height)) {
            }

	    if (mDumpEnabled) {
                ALOGI("%s: Dump RGB32 output [%zu] for preview/video",__FUNCTION__, frameCount);
                dumpFrame(img, frameSizeRGB32, mCameraId, "RGB32", height, frameCount);
            }
        }
    }
    ALOGVV(" %s: Captured RGB32 image successfully..", __FUNCTION__);
}

void Sensor::captureRGB(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / kMaxRawValue;
    unsigned int DivH = (float)mResolution[1] / height * (0x1 << 10);
    unsigned int DivW = (float)mResolution[0] / width * (0x1 << 10);

    for (unsigned int outY = 0; outY < height; outY++) {
        unsigned int y = outY * DivH >> 10;
        uint8_t *px = img + outY * width * 3;
        mScene.setReadoutPixel(0, y);
        unsigned int lastX = 0;
        const uint32_t *pixel = mScene.getPixelElectrons();
        for (unsigned int outX = 0; outX < width; outX++) {
            uint32_t rCount, gCount, bCount;
            unsigned int x = outX * DivW >> 10;
            if (x - lastX > 0) {
                for (unsigned int k = 0; k < (x - lastX); k++) {
                    pixel = mScene.getPixelElectrons();
                }
            }
            lastX = x;
            // TODO: Perfect demosaicing is a cheat
            rCount = (pixel[Scene::R] + (outX + outY) % 64) * scale64x;
            gCount = (pixel[Scene::Gr] + (outX + outY) % 64) * scale64x;
            bCount = (pixel[Scene::B] + (outX + outY) % 64) * scale64x;

            *px++ = rCount < 255 * 64 ? rCount / 64 : 255;
            *px++ = gCount < 255 * 64 ? gCount / 64 : 255;
            *px++ = bCount < 255 * 64 ? bCount / 64 : 255;
        }
    }
    ALOGVV("RGB sensor image captured");
}

void Sensor::captureNV12(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height) {
    ALOGVV(LOG_TAG "%s: E", __FUNCTION__);

    uint8_t *bufData = mCameraBuffer->clientBuf.buffer;
    int cameraInputDataSize;
    static size_t frameCount = 0;

    ALOGVV(LOG_TAG " %s: bufData[%p] img[%p] resolution[%d:%d]", __func__, bufData, img, width,
           height);

    if (!gIsInFrameI420 && !gIsInFrameH264) {
        ALOGE("%s Exit - only H264, I420 input frames supported", __FUNCTION__);
        return;
    }

    // Initialize the input data size based on client camera resolution.
    cameraInputDataSize = mSrcFrameSize;

    if (gIsInFrameH264) {
        if (mCameraBuffer->clientBuf.decoded) {
            // Already decoded camera input as part of preview frame.
            // This is the default condition in most of the apps.
            ALOGVV("%s - Already Decoded Camera Input frame..", __FUNCTION__);
        } else {
            // To get the decoded frame for the apps which doesn't have RGBA preview.
            getNV12Frames(bufData, &cameraInputDataSize);
            mCameraBuffer->clientBuf.decoded = true;
            std::unique_lock<std::mutex> ulock(client_buf_mutex);
            mCameraBuffer->decodedFrameNo++;
            ALOGVV("%s Decoded Camera Input Frame No: %zd with size of %d", __FUNCTION__,
                   mCameraBuffer->decodedFrameNo, cameraInputDataSize);
            ulock.unlock();
        }
    }

    int src_size = mSrcWidth * mSrcHeight;
    int dstFrameSize = width * height;

    // For Max supported Resolution.
    if (width == (uint32_t)mSrcWidth && height == (uint32_t)mSrcHeight) {
        if (gIsInFrameI420) {
            // For I420 input support
            ALOGVV(LOG_TAG " %s: I420 no scaling required Size = %dx%d", __FUNCTION__, width,
                   height);
            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = mSrcWidth >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = mSrcWidth >> 1;
            uint8_t *dst_y = img;
            int dst_stride_y = width;
            uint8_t *dst_uv = dst_y + src_size;
            int dst_stride_uv = width;
            if (m_major_version == 1) {
                ALOGVV(LOG_TAG " %s: [SG1] convert I420 to NV12!", __FUNCTION__);
                if (int ret = libyuv::I420ToNV12(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                                 src_stride_v, dst_y, dst_stride_y, dst_uv,
                                                 dst_stride_uv, width, height)) {
                }
            } else {
                ALOGVV(LOG_TAG " %s: [NON-SG1] convert I420 to NV21!", __FUNCTION__);
                if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                                 src_stride_v, dst_y, dst_stride_y, dst_uv,
                                                 dst_stride_uv, width, height)) {
                }
            }
        } else {
            // For NV12 Input support. No Color conversion
            ALOGVV(LOG_TAG " %s: NV12 frame without scaling and color conversion: Size = %dx%d",
                   __FUNCTION__, width, height);

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV12 input [%zu] for capture",__FUNCTION__, frameCount);
                dumpFrame(bufData, mSrcFrameSize, mCameraId, "NV12_CAP", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }
            memcpy(img, bufData, cameraInputDataSize);
        }
        // For upscaling and downscaling all other resolutions below max supported resolution.
    } else {
        if (gIsInFrameI420) {
            // For I420 input support
            ALOGVV(LOG_TAG " %s: I420 with scaling: Size = %dx%d", __FUNCTION__, width, height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;
            uint8_t *dst_y = mDstBuf.data();
            int dst_stride_y = width;
            uint8_t *dst_u = mDstBuf.data() + dstFrameSize;
            int dst_stride_u = width >> 1;
            uint8_t *dst_v = mDstBuf.data() + dstFrameSize + dstFrameSize / 4;
            int dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }

            ALOGVV("%s: I420, Scaling done!", __FUNCTION__);

            src_y = mDstBuf.data();
            src_stride_y = width;
            src_u = mDstBuf.data() + dstFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstBuf.data() + dstFrameSize + dstFrameSize / 4;
            src_stride_v = width >> 1;
            dst_y = img;
            dst_stride_y = width;

            uint8_t *dst_uv = dst_y + width * height;
            int dst_stride_uv = width;

            if (m_major_version == 1) {
                ALOGVV(LOG_TAG " %s: [SG1] convert I420 to NV12!", __FUNCTION__);
                if (int ret = libyuv::I420ToNV12(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                                 src_stride_v, dst_y, dst_stride_y, dst_uv,
                                                 dst_stride_uv, width, height)) {
                }
            } else {
                ALOGVV(LOG_TAG " %s: [NON-SG1] convert I420 to NV21!", __FUNCTION__);
                if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                                 src_stride_v, dst_y, dst_stride_y, dst_uv,
                                                 dst_stride_uv, width, height)) {
                }
            }
        } else {
            // For NV12 Input support
            ALOGVV(LOG_TAG " %s: NV12 frame with scaling to Size = %dx%d", __FUNCTION__, width,
                   height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_uv = bufData + src_size;
            int src_stride_uv = mSrcWidth;
            uint8_t *dst_y = mDstTempBuf.data();
            int dst_stride_y = mSrcWidth;
            uint8_t *dst_u = mDstTempBuf.data() + src_size;
            int dst_stride_u = mSrcWidth >> 1;
            uint8_t *dst_v = mDstTempBuf.data() + src_size + src_size / 4;
            int dst_stride_v = mSrcWidth >> 1;

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV12 input [%zu] for capture",__FUNCTION__, frameCount);
                dumpFrame(bufData, mSrcFrameSize, mCameraId, "NV12_CAP", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }

            if (int ret = libyuv::NV12ToI420(src_y, src_stride_y, src_uv, src_stride_uv, dst_y,
                                             dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                             mSrcWidth, mSrcHeight)) {
            }

            src_y = mDstTempBuf.data();
            src_stride_y = mSrcWidth;

            uint8_t *src_u = mDstTempBuf.data() + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = mDstTempBuf.data() + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;

            dst_y = mDstBuf.data();
            dst_stride_y = width;
            dst_u = mDstBuf.data() + dstFrameSize;
            dst_stride_u = width >> 1;
            dst_v = mDstBuf.data() + dstFrameSize + dstFrameSize / 4;
            dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }
            src_y = mDstBuf.data();
            src_stride_y = width;
            src_u = mDstBuf.data() + dstFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstBuf.data() + dstFrameSize + dstFrameSize / 4;
            src_stride_v = width >> 1;
            dst_y = img;
            dst_stride_y = width;

            uint8_t *dst_uv = dst_y + dstFrameSize;
            int dst_stride_uv = width;
            if (int ret = libyuv::I420ToNV12(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                             src_stride_v, dst_y, dst_stride_y, dst_uv,
                                             dst_stride_uv, width, height)) {
            }
        }
    }
    ALOGVV(LOG_TAG " %s: Captured NV12 image successfully..", __FUNCTION__);
}

void Sensor::captureNV21(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height) {
    ALOGVV("%s: E", __FUNCTION__);

    uint8_t *bufData = mCameraBuffer->clientBuf.buffer;

    int src_size = mSrcWidth * mSrcHeight;
    int dstFrameSize = width * height;
    int cameraInputDataSize;
    static size_t frameCount = 0;

    if (!gIsInFrameI420 && !gIsInFrameH264) {
        ALOGE("%s Exit - only H264, H265, I420 input frames supported", __FUNCTION__);
        return;
    }

    // Initialize the input data size based on client camera resolution.
    cameraInputDataSize = mSrcFrameSize;

    if (gIsInFrameH264) {
        if (mCameraBuffer->clientBuf.decoded) {
            // If already decoded camera input frame.
            ALOGVV("%s - Already Decoded Camera Input frame", __FUNCTION__);
        } else {
            // To get the decoded frame.
            getNV12Frames(bufData, &cameraInputDataSize);
            mCameraBuffer->clientBuf.decoded = true;
            std::unique_lock<std::mutex> ulock(client_buf_mutex);
            mCameraBuffer->decodedFrameNo++;
            ALOGVV("%s Decoded Camera Input Frame No: %zd with size of %d", __FUNCTION__,
                   mCameraBuffer->decodedFrameNo, cameraInputDataSize);
            ulock.unlock();
        }
    }
    // For Max supported Resolution.
    if (width == (uint32_t)mSrcWidth && height == (uint32_t)mSrcHeight) {
        // For I420 input
        if (gIsInFrameI420) {
            ALOGVV(LOG_TAG "%s: I420 to NV21 conversion without scaling: Size = %dx%d",
                   __FUNCTION__, width, height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = mSrcWidth >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = mSrcWidth >> 1;

            uint8_t *dst_y = img;
            int dst_stride_y = width;
            uint8_t *dst_vu = dst_y + src_size;
            int dst_stride_vu = width;

            if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                             src_stride_v, dst_y, dst_stride_y, dst_vu,
                                             dst_stride_vu, width, height)) {
            }
            // For NV12 input
        } else {
            ALOGVV(LOG_TAG "%s: NV12 to NV21 conversion without scaling: Size = %dx%d",
                   __FUNCTION__, width, height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_uv = bufData + src_size;
            int src_stride_uv = mSrcWidth;

            uint8_t *dst_y = mDstJpegBuf.data();
            int dst_stride_y = mSrcWidth;
            uint8_t *dst_u = mDstJpegBuf.data() + src_size;
            int dst_stride_u = mSrcWidth >> 1;
            uint8_t *dst_v = mDstJpegBuf.data() + src_size + src_size / 4;
            int dst_stride_v = mSrcWidth >> 1;

            if (int ret = libyuv::NV12ToI420(src_y, src_stride_y, src_uv, src_stride_uv, dst_y,
                                             dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                             mSrcWidth, mSrcHeight)) {
            }

            src_y = mDstJpegBuf.data();
            src_stride_y = mSrcWidth;
            uint8_t *src_u = mDstJpegBuf.data() + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = mDstJpegBuf.data() + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;

            dst_y = img;
            dst_stride_y = width;
            uint8_t *dst_vu = dst_y + dstFrameSize;
            int dst_stride_vu = width;

            if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                             src_stride_v, dst_y, dst_stride_y, dst_vu,
                                             dst_stride_vu, width, height)) {
            }

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV21 output [%zu] for capture",__FUNCTION__, frameCount);
                dumpFrame(img, mSrcFrameSize, mCameraId, "NV21_CAP", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }
        }
        // For upscaling and downscaling all other resolutions below max supported resolution.
    } else {
        // For I420 input
        if (gIsInFrameI420) {
            ALOGVV(LOG_TAG "%s: I420 to NV21 with scaling: Size = %dx%d", __FUNCTION__, width,
                   height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_u = bufData + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = bufData + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;

            uint8_t *dst_y = mDstJpegBuf.data();
            int dst_stride_y = width;
            uint8_t *dst_u = mDstJpegBuf.data() + dstFrameSize;
            int dst_stride_u = width >> 1;
            uint8_t *dst_v = mDstJpegBuf.data() + dstFrameSize + dstFrameSize / 4;
            int dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }

            src_y = mDstJpegBuf.data();
            src_stride_y = width;
            src_u = mDstJpegBuf.data() + dstFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstJpegBuf.data() + dstFrameSize + dstFrameSize / 4;
            src_stride_v = width >> 1;
            dst_y = img;
            dst_stride_y = width;

            uint8_t *dst_vu = dst_y + width * height;
            int dst_stride_vu = width;

            if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                             src_stride_v, dst_y, dst_stride_y, dst_vu,
                                             dst_stride_vu, width, height)) {
            }
            // For NV12 input
        } else {
            ALOGVV(LOG_TAG "%s: NV12 to NV21 conversion with scaling: Size = %dx%d", __FUNCTION__,
                   width, height);

            const uint8_t *src_y = bufData;
            int src_stride_y = mSrcWidth;
            const uint8_t *src_uv = bufData + src_size;
            int src_stride_uv = mSrcWidth;

            uint8_t *dst_y = mDstJpegTempBuf.data();
            int dst_stride_y = mSrcWidth;
            uint8_t *dst_u = mDstJpegTempBuf.data() + src_size;
            int dst_stride_u = mSrcWidth >> 1;
            uint8_t *dst_v = mDstJpegTempBuf.data() + src_size + src_size / 4;
            int dst_stride_v = mSrcWidth >> 1;

            if (int ret = libyuv::NV12ToI420(src_y, src_stride_y, src_uv, src_stride_uv, dst_y,
                                             dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                             mSrcWidth, mSrcHeight)) {
            }

            src_y = mDstJpegTempBuf.data();
            src_stride_y = mSrcWidth;
            uint8_t *src_u = mDstJpegTempBuf.data() + src_size;
            int src_stride_u = src_stride_y >> 1;
            const uint8_t *src_v = mDstJpegTempBuf.data() + src_size + src_size / 4;
            int src_stride_v = src_stride_y >> 1;
            int src_width = mSrcWidth;
            int src_height = mSrcHeight;

            dst_y = mDstJpegBuf.data();
            dst_stride_y = width;
            dst_u = mDstJpegBuf.data() + dstFrameSize;
            dst_stride_u = width >> 1;
            dst_v = mDstJpegBuf.data() + dstFrameSize + dstFrameSize / 4;
            dst_stride_v = width >> 1;
            int dst_width = width;
            int dst_height = height;
            auto filtering = libyuv::kFilterNone;

            if (int ret = libyuv::I420Scale(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                            src_stride_v, src_width, src_height, dst_y,
                                            dst_stride_y, dst_u, dst_stride_u, dst_v, dst_stride_v,
                                            dst_width, dst_height, filtering)) {
            }

            src_y = mDstJpegBuf.data();
            src_stride_y = width;
            src_u = mDstJpegBuf.data() + dstFrameSize;
            src_stride_u = width >> 1;
            src_v = mDstJpegBuf.data() + dstFrameSize + dstFrameSize / 4;
            src_stride_v = width >> 1;

            dst_y = img;
            dst_stride_y = width;
            uint8_t *dst_vu = dst_y + dstFrameSize;
            int dst_stride_vu = width;

            if (int ret = libyuv::I420ToNV21(src_y, src_stride_y, src_u, src_stride_u, src_v,
                                             src_stride_v, dst_y, dst_stride_y, dst_vu,
                                             dst_stride_vu, width, height)) {
            }

	    if (mDumpEnabled) {
                ALOGI("%s: Dump NV21 output [%zu] for capture",__FUNCTION__, frameCount);
                dumpFrame(img, mSrcFrameSize, mCameraId, "NV21_CAP", height, frameCount);
                frameCount++;
            } else {
                frameCount = 0;
            }
        }
    }
    ALOGVV("%s: Captured NV21 image successfully..", __FUNCTION__);
}

void Sensor::captureDepth(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height) {
    ALOGVV("%s", __FUNCTION__);

    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate scaling factor to 13bpp millimeters
    int scale64x = 64 * totalGain * 8191 / kMaxRawValue;
    unsigned int DivH = (float)mResolution[1] / height * (0x1 << 10);
    unsigned int DivW = (float)mResolution[0] / width * (0x1 << 10);

    for (unsigned int outY = 0; outY < height; outY++) {
        unsigned int y = outY * DivH >> 10;
        uint16_t *px = ((uint16_t *)img) + outY * width;
        mScene.setReadoutPixel(0, y);
        unsigned int lastX = 0;
        const uint32_t *pixel = mScene.getPixelElectrons();
        for (unsigned int outX = 0; outX < width; outX++) {
            uint32_t depthCount;
            unsigned int x = outX * DivW >> 10;
            if (x - lastX > 0) {
                for (unsigned int k = 0; k < (x - lastX); k++) {
                    pixel = mScene.getPixelElectrons();
                }
            }
            lastX = x;
            depthCount = pixel[Scene::Gr] * scale64x;
            *px++ = depthCount < 8191 * 64 ? depthCount / 64 : 0;
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("Depth sensor image captured");
}

void Sensor::captureDepthCloud(uint8_t *img) {
    ALOGVV("%s", __FUNCTION__);

    android_depth_points *cloud = reinterpret_cast<android_depth_points *>(img);

    cloud->num_points = 16;

    // TODO: Create point cloud values that match RGB scene
    const int FLOATS_PER_POINT = 4;
    const float JITTER_STDDEV = 0.1f;
    for (size_t y = 0, i = 0; y < 4; y++) {
        for (size_t x = 0; x < 4; x++, i++) {
            float randSampleX = std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
            randSampleX *= JITTER_STDDEV;

            float randSampleY = std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
            randSampleY *= JITTER_STDDEV;

            float randSampleZ = std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
            randSampleZ *= JITTER_STDDEV;

            cloud->xyzc_points[i * FLOATS_PER_POINT + 0] = x - 1.5f + randSampleX;
            cloud->xyzc_points[i * FLOATS_PER_POINT + 1] = y - 1.5f + randSampleY;
            cloud->xyzc_points[i * FLOATS_PER_POINT + 2] = 3.f + randSampleZ;
            cloud->xyzc_points[i * FLOATS_PER_POINT + 3] = 0.8f;
        }
    }

    ALOGVV("Depth point cloud captured");
}

}  // namespace android
