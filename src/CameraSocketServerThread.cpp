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
//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "CameraSocketServerThread: "
#include <log/log.h>

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <array>
#include <atomic>

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include "CameraSocketServerThread.h"
#include "VirtualBuffer.h"
#include "VirtualCameraFactory.h"
#include <mutex>

android::ClientVideoBuffer *android::ClientVideoBuffer::ic_instance = 0;

namespace android {

uint32_t gMaxNumOfCamerasSupported;

int32_t gMaxSupportedWidth;
int32_t gMaxSupportedHeight;

int32_t gCameraMaxWidth;
int32_t gCameraMaxHeight;

uint32_t gCodecType;
uint32_t gCameraSensorOrientation;
bool gCameraFacingBack;

bool gCapabilityInfoReceived;
bool gStartMetadataUpdate;
bool gDoneMetadataUpdate;

using namespace socket;
CameraSocketServerThread::CameraSocketServerThread(std::string suffix,
                                                   std::shared_ptr<CGVideoDecoder> decoder,
                                                   std::atomic<CameraSessionState> &state)
    : Thread(/*canCallJava*/ false),
      mRunning{true},
      mSocketServerFd{-1},
      mVideoDecoder{decoder},
      mCameraSessionState{state} {
    std::string sock_path = "/ipc/camera-socket" + suffix;
    char *k8s_env_value = getenv("K8S_ENV");
    mSocketPath = (k8s_env_value != NULL && !strcmp(k8s_env_value, "true")) ? "/conn/camera-socket"
                                                                            : sock_path.c_str();
    ALOGI("%s camera socket server path is %s", __FUNCTION__, mSocketPath.c_str());
    mNumOfCamerasRequested = 0;
}

CameraSocketServerThread::~CameraSocketServerThread() {
    if (mClientFd > 0) {
        shutdown(mClientFd, SHUT_RDWR);
        close(mClientFd);
        mClientFd = -1;
    }
    if (mSocketServerFd > 0) {
        close(mSocketServerFd);
        mSocketServerFd = -1;
    }
}

status_t CameraSocketServerThread::requestExitAndWait() {
    ALOGE("%s: Not implemented. Use requestExit + join instead", __FUNCTION__);
    return INVALID_OPERATION;
}

int CameraSocketServerThread::getClientFd() {
    Mutex::Autolock al(mMutex);
    return mClientFd;
}

void CameraSocketServerThread::requestExit() {
    Mutex::Autolock al(mMutex);

    ALOGV("%s: Requesting thread exit", __FUNCTION__);
    mRunning = false;
    ALOGV("%s: Request exit complete.", __FUNCTION__);
}

status_t CameraSocketServerThread::readyToRun() {
    Mutex::Autolock al(mMutex);

    return OK;
}

void CameraSocketServerThread::setCameraMaxSupportedResolution(int32_t width, int32_t height) {
    ALOGVV(LOG_TAG "%s: E", __FUNCTION__);

    if (gMaxSupportedWidth < width && gMaxSupportedHeight < height) {
        gMaxSupportedWidth = width;
        gMaxSupportedHeight = height;
        ALOGI(LOG_TAG "%s: Set Camera Max supported resolution: %dx%d", __FUNCTION__,
              gMaxSupportedWidth, gMaxSupportedHeight);
    }
}

void CameraSocketServerThread::setCameraResolution(uint32_t resolution) {
    ALOGVV(LOG_TAG "%s: E", __FUNCTION__);

    switch (resolution) {
        case uint32_t(FrameResolution::k480p):
            gCameraMaxWidth = 640;
            gCameraMaxHeight = 480;
            break;
        case uint32_t(FrameResolution::k720p):
            gCameraMaxWidth = 1280;
            gCameraMaxHeight = 720;
            break;
        case uint32_t(FrameResolution::k1080p):
            gCameraMaxWidth = 1920;
            gCameraMaxHeight = 1080;
            break;
        default:
            break;
    }
    ALOGI(LOG_TAG "%s: Set Camera resolution: %dx%d", __FUNCTION__, gCameraMaxWidth,
          gCameraMaxHeight);

    setCameraMaxSupportedResolution(gCameraMaxWidth, gCameraMaxHeight);
}

bool CameraSocketServerThread::configureCapabilities() {
    ALOGVV(LOG_TAG " %s Enter", __FUNCTION__);

    bool status = false;
    bool valid_client_cap_info = false;
    int camera_id, expctd_cam_id;
    struct ValidateClientCapability val_client_cap[MAX_NUMBER_OF_SUPPORTED_CAMERAS];
    size_t ack_packet_size = sizeof(camera_header_t) + sizeof(camera_ack_t);
    size_t cap_packet_size = sizeof(camera_header_t) + sizeof(camera_capability_t);
    ssize_t recv_size = 0;
    camera_ack_t ack_payload = ACK_CONFIG;

    camera_info_t camera_info[MAX_NUMBER_OF_SUPPORTED_CAMERAS] = {};
    camera_capability_t capability = {};

    camera_packet_t *cap_packet = NULL;
    camera_packet_t *ack_packet = NULL;
    camera_header_t header = {};

    if ((recv_size = recv(mClientFd, (char *)&header, sizeof(camera_header_t), MSG_WAITALL)) < 0) {
        ALOGE(LOG_TAG "%s: Failed to receive header, err: %s ", __FUNCTION__, strerror(errno));
        goto out;
    }

    if (header.type != REQUEST_CAPABILITY) {
        ALOGE(LOG_TAG "%s: Invalid packet type\n", __FUNCTION__);
        goto out;
    }
    ALOGI(LOG_TAG "%s: Received REQUEST_CAPABILITY header from client", __FUNCTION__);

    cap_packet = (camera_packet_t *)malloc(cap_packet_size);
    if (cap_packet == NULL) {
        ALOGE(LOG_TAG "%s: cap camera_packet_t allocation failed: %d ", __FUNCTION__, __LINE__);
        return false;
    }

    cap_packet->header.type = CAPABILITY;
    cap_packet->header.size = sizeof(camera_capability_t);
    capability.codec_type = (uint32_t)VideoCodecType::kAll;
    capability.resolution = (uint32_t)FrameResolution::kAll;
    capability.maxNumberOfCameras = MAX_NUMBER_OF_SUPPORTED_CAMERAS;

    memcpy(cap_packet->payload, &capability, sizeof(camera_capability_t));
    if (send(mClientFd, cap_packet, cap_packet_size, 0) < 0) {
        ALOGE(LOG_TAG "%s: Failed to send camera capabilities, err: %s ", __FUNCTION__,
              strerror(errno));
        goto out;
    }
    ALOGI(LOG_TAG "%s: Sent CAPABILITY packet to client", __FUNCTION__);

    if ((recv_size = recv(mClientFd, (char *)&header, sizeof(camera_header_t), MSG_WAITALL)) < 0) {
        ALOGE(LOG_TAG "%s: Failed to receive header, err: %s ", __FUNCTION__, strerror(errno));
        goto out;
    }

    if (header.type != CAMERA_INFO) {
        ALOGE(LOG_TAG "%s: invalid camera_packet_type: %s", __FUNCTION__,
              camera_type_to_str(header.type));
        goto out;
    }

    // Get the number fo cameras requested to support from client.
    for (int i = 1; i <= MAX_NUMBER_OF_SUPPORTED_CAMERAS; i++) {
        if (header.size == i * sizeof(camera_info_t)) {
            mNumOfCamerasRequested = i;
            break;
        } else if (mNumOfCamerasRequested == 0 && i == MAX_NUMBER_OF_SUPPORTED_CAMERAS) {
            ALOGE(LOG_TAG
                  "%s: Failed to support number of cameras requested by client "
                  "which is higher than the max number of cameras supported in the HAL",
                  __FUNCTION__);
            goto out;
        }
    }

    if (mNumOfCamerasRequested == 0) {
        ALOGE(LOG_TAG "%s: invalid header size received, size = %zu", __FUNCTION__, recv_size);
        goto out;
    } else {
        // Update the number of cameras globally to create camera pipeline.
        gMaxNumOfCamerasSupported = mNumOfCamerasRequested;
    }

    if ((recv_size = recv(mClientFd, (char *)&camera_info,
                          mNumOfCamerasRequested * sizeof(camera_info_t), MSG_WAITALL)) < 0) {
        ALOGE(LOG_TAG "%s: Failed to receive camera info, err: %s ", __FUNCTION__, strerror(errno));
        goto out;
    }

    ALOGI(LOG_TAG "%s: Received CAMERA_INFO packet from client with recv_size: %zd ", __FUNCTION__,
          recv_size);
    ALOGI(LOG_TAG "%s: Number of cameras requested = %d", __FUNCTION__, mNumOfCamerasRequested);

    // Update status globally after received successful capability info.
    gCapabilityInfoReceived = true;

    // validate capability info received from the client.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        expctd_cam_id = i;
        if (expctd_cam_id == (int)camera_info[i].cameraId)
            ALOGVV(LOG_TAG
                   "%s: Camera Id number %u received from client is matching with expected Id",
                   __FUNCTION__, camera_info[i].cameraId);
        else
            ALOGI(LOG_TAG
                  "%s: [Warning] Camera Id number %u received from client is not matching with "
                  "expected Id %d",
                  __FUNCTION__, camera_info[i].cameraId, expctd_cam_id);

        switch (camera_info[i].codec_type) {
            case uint32_t(VideoCodecType::kH264):
            case uint32_t(VideoCodecType::kH265):
                val_client_cap[i].validCodecType = true;
                break;
            default:
                val_client_cap[i].validCodecType = false;
                break;
        }

        switch (camera_info[i].resolution) {
            case uint32_t(FrameResolution::k480p):
            case uint32_t(FrameResolution::k720p):
            case uint32_t(FrameResolution::k1080p):
                val_client_cap[i].validResolution = true;
                break;
            default:
                val_client_cap[i].validResolution = false;
                break;
        }

        switch (camera_info[i].sensorOrientation) {
            case uint32_t(SensorOrientation::ORIENTATION_0):
            case uint32_t(SensorOrientation::ORIENTATION_90):
            case uint32_t(SensorOrientation::ORIENTATION_180):
            case uint32_t(SensorOrientation::ORIENTATION_270):
                val_client_cap[i].validOrientation = true;
                break;
            default:
                val_client_cap[i].validOrientation = false;
                break;
        }

        switch (camera_info[i].facing) {
            case uint32_t(CameraFacing::BACK_FACING):
            case uint32_t(CameraFacing::FRONT_FACING):
                val_client_cap[i].validCameraFacing = true;
                break;
            default:
                val_client_cap[i].validCameraFacing = false;
                break;
        }
    }

    // Check whether recceived any invalid capability info or not.
    // ACK packet to client would be updated based on this verification.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        if (!val_client_cap[i].validCodecType || !val_client_cap[i].validResolution ||
            !val_client_cap[i].validOrientation || !val_client_cap[i].validCameraFacing) {
            valid_client_cap_info = false;
            ALOGE("%s: capability info received from client is not completely correct and expected",
                  __FUNCTION__);
            break;
        } else {
            ALOGVV("%s: capability info received from client is correct and expected",
                   __FUNCTION__);
            valid_client_cap_info = true;
        }
    }

    // Updating metadata for each camera seperately with its capability info received.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        // Going to update metadata for each camera, so update the status.
        gStartMetadataUpdate = false;
        gDoneMetadataUpdate = false;
        camera_id = i;
        ALOGI(LOG_TAG
              "%s - Client requested for codec_type: %s, resolution: %s, orientation: %u, and "
              "facing: %u for camera Id %d",
              __FUNCTION__, codec_type_to_str(camera_info[i].codec_type),
              resolution_to_str(camera_info[i].resolution), camera_info[i].sensorOrientation,
              camera_info[i].facing, camera_id);

        if (val_client_cap[i].validResolution) {
            // Set Camera capable resolution based on remote client capability info.
            setCameraResolution(camera_info[i].resolution);
        } else {
            // Set default resolution if receive invalid capability info from client.
            // Default resolution would be 480p.
            setCameraResolution((uint32_t)FrameResolution::k480p);
            ALOGE(LOG_TAG
                  "%s: Not received valid resolution, "
                  "hence selected 480p as default",
                  __FUNCTION__);
        }

        if (val_client_cap[i].validCodecType) {
            // Set codec type based on remote client capability info.
            gCodecType = camera_info[i].codec_type;
        } else {
            // Set default codec type if receive invalid capability info from client.
            // Default codec type would be H264.
            gCodecType = (uint32_t)VideoCodecType::kH264;
            ALOGE(LOG_TAG "%s: Not received valid codec type, hence selected H264 as default",
                  __FUNCTION__);
        }

        if (val_client_cap[i].validOrientation) {
            // Set Camera sensor orientation based on remote client camera orientation.
            gCameraSensorOrientation = camera_info[i].sensorOrientation;
        } else {
            // Set default camera sensor orientation if received invalid orientation data from
            // client. Default sensor orientation would be zero deg and consider as landscape
            // display.
            gCameraSensorOrientation = (uint32_t)SensorOrientation::ORIENTATION_0;
            ALOGE(LOG_TAG
                  "%s: Not received valid sensor orientation, "
                  "hence selected ORIENTATION_0 as default",
                  __FUNCTION__);
        }

        if (val_client_cap[i].validCameraFacing) {
            // Set camera facing based on client request.
            if (camera_info[i].facing == (uint32_t)CameraFacing::BACK_FACING)
                gCameraFacingBack = true;
            else
                gCameraFacingBack = false;
        } else {
            // Set default camera facing info if received invalid facing info from client.
            // Default would be back for camera Id '0' and front for camera Id '1'.
            if (camera_id == 1)
                gCameraFacingBack = false;
            else
                gCameraFacingBack = true;
            ALOGE(LOG_TAG
                  "%s: Not received valid camera facing info, "
                  "hence selected default",
                  __FUNCTION__);
        }

        // Start updating metadata for one camera, so update the status.
        gStartMetadataUpdate = true;

        // Wait till complete the metadata update for a camera.
        while (!gDoneMetadataUpdate) {
            ALOGVV("%s: wait till complete the metadata update for a camera", __FUNCTION__);
            // 200us sleep for this thread.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    ack_packet = (camera_packet_t *)malloc(ack_packet_size);
    if (ack_packet == NULL) {
        ALOGE(LOG_TAG "%s: ack camera_packet_t allocation failed: %d ", __FUNCTION__, __LINE__);
        goto out;
    }
    ack_payload = (valid_client_cap_info) ? ACK_CONFIG : NACK_CONFIG;

    ack_packet->header.type = ACK;
    ack_packet->header.size = sizeof(camera_ack_t);

    memcpy(ack_packet->payload, &ack_payload, sizeof(camera_ack_t));
    if (send(mClientFd, ack_packet, ack_packet_size, 0) < 0) {
        ALOGE(LOG_TAG "%s: Failed to send camera capabilities, err: %s ", __FUNCTION__,
              strerror(errno));
        goto out;
    }
    ALOGI(LOG_TAG "%s: Sent ACK packet to client with ack_size: %zu ", __FUNCTION__,
          ack_packet_size);

    status = true;
out:
    free(ack_packet);
    free(cap_packet);
    ALOGVV(LOG_TAG " %s: Exit", __FUNCTION__);
    return status;
}

bool CameraSocketServerThread::threadLoop() {
    mSocketServerFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (mSocketServerFd < 0) {
        ALOGE("%s:%d Fail to construct camera socket with error: %s", __FUNCTION__, __LINE__,
              strerror(errno));
        return false;
    }

    struct sockaddr_un addr_un;
    memset(&addr_un, 0, sizeof(addr_un));
    addr_un.sun_family = AF_UNIX;
    strncpy(&addr_un.sun_path[0], mSocketPath.c_str(), strlen(mSocketPath.c_str()));

    int ret = 0;
    if ((access(mSocketPath.c_str(), F_OK)) != -1) {
        ALOGI(" %s camera socket server file is %s", __FUNCTION__, mSocketPath.c_str());
        ret = unlink(mSocketPath.c_str());
        if (ret < 0) {
            ALOGE(LOG_TAG " %s Failed to unlink %s address %d, %s", __FUNCTION__,
                  mSocketPath.c_str(), ret, strerror(errno));
            return false;
        }
    } else {
        ALOGV(LOG_TAG " %s camera socket server file %s will created. ", __FUNCTION__,
              mSocketPath.c_str());
    }

    ret = ::bind(mSocketServerFd, (struct sockaddr *)&addr_un,
                 sizeof(sa_family_t) + strlen(mSocketPath.c_str()) + 1);
    if (ret < 0) {
        ALOGE(LOG_TAG " %s Failed to bind %s address %d, %s", __FUNCTION__, mSocketPath.c_str(),
              ret, strerror(errno));
        return false;
    }

    struct stat st;
    __mode_t mod = S_IRWXU | S_IRWXG | S_IRWXO;
    if (fstat(mSocketServerFd, &st) == 0) {
        mod |= st.st_mode;
    }
    chmod(mSocketPath.c_str(), mod);
    stat(mSocketPath.c_str(), &st);

    ret = listen(mSocketServerFd, 5);
    if (ret < 0) {
        ALOGE("%s Failed to listen on %s", __FUNCTION__, mSocketPath.c_str());
        return false;
    }

    while (mRunning) {
        ALOGI(LOG_TAG " %s: Wait for camera client to connect. . .", __FUNCTION__);

        socklen_t alen = sizeof(struct sockaddr_un);

        int new_client_fd = ::accept(mSocketServerFd, (struct sockaddr *)&addr_un, &alen);
        ALOGI(LOG_TAG " %s: Accepted client: [%d]", __FUNCTION__, new_client_fd);
        if (new_client_fd < 0) {
            ALOGE(LOG_TAG " %s: Fail to accept client. Error: [%s]", __FUNCTION__, strerror(errno));
            continue;
        }
        mClientFd = new_client_fd;

        bool status = false;
        status = configureCapabilities();
        if (status) {
            ALOGI(LOG_TAG
                  "%s: Capability negotiation and metadata update"
                  "for %d camera(s) completed successfully..",
                  __FUNCTION__, mNumOfCamerasRequested);
        }

        ClientVideoBuffer *handle = ClientVideoBuffer::getClientInstance();
        char *fbuffer = (char *)handle->clientBuf[handle->clientRevCount % 1].buffer;
        // Reset and clear the input buffer before receiving the frames.
        handle->reset();

        struct pollfd fd;
        int event;

        fd.fd = mClientFd;  // your socket handler
        fd.events = POLLIN | POLLHUP;

        while (true) {
            // check if there are any events on fd.
            int ret = poll(&fd, 1, 3000);  // 3 seconds for timeout

            event = fd.revents;  // returned events

            if (event & POLLHUP) {
                // connnection disconnected => socket is closed at the other end => close the
                // socket.
                ALOGE(LOG_TAG " %s: POLLHUP: Close camera socket connection", __FUNCTION__);
                shutdown(mClientFd, SHUT_RDWR);
                close(mClientFd);
                mClientFd = -1;
                handle->reset();
                break;
            } else if (event & POLLIN) {  // preview / record
                // data is available in socket => read data
                if (gIsInFrameI420) {
                    ssize_t size = 0;

                    if ((size = recv(mClientFd, (char *)fbuffer, 460800, MSG_WAITALL)) > 0) {
                        handle->clientRevCount++;
                        ALOGVV(LOG_TAG
                               "[I420] %s: Pocket rev %d and "
                               "size %zd",
                               __FUNCTION__, handle->clientRevCount, size);
                    }
                } else if (gIsInFrameH264) {  // default H264
                    ssize_t size = 0;
                    camera_header_t header = {};
                    if ((size = recv(mClientFd, (char *)&header, sizeof(camera_header_t),
                                     MSG_WAITALL)) > 0) {
                        ALOGVV("%s: Received Header %zd bytes. Payload size: %u", __FUNCTION__,
                               size, header.size);
                        if (header.type == REQUEST_CAPABILITY) {
                            ALOGI(LOG_TAG
                                  "%s: [Warning] Capability negotiation was already "
                                  "done for %d camera(s); Can't do re-negotiation again!!!",
                                  __FUNCTION__, mNumOfCamerasRequested);
                            continue;
                        } else if (header.type != CAMERA_DATA) {
                            ALOGE(LOG_TAG "%s: invalid camera_packet_type: %s", __FUNCTION__,
                                  camera_type_to_str(header.type));
                            continue;
                        }

                        if (header.size > mSocketBuffer.size()) {
                            // maximum size of a H264 packet in any aggregation packet is 65535
                            // bytes. Source: https://tools.ietf.org/html/rfc6184#page-13
                            ALOGE(
                                "%s Fatal: Unusual encoded packet size detected: %u! Max is %zu, "
                                "...",
                                __func__, header.size, mSocketBuffer.size());
                            continue;
                        }

                        // recv frame
                        if ((size = recv(mClientFd, (char *)mSocketBuffer.data(), header.size,
                                         MSG_WAITALL)) > 0) {
                            if (size < header.size) {
                                ALOGW("%s : Incomplete data read %zd/%u bytes", __func__, size,
                                      header.size);
                                size_t remaining_size = header.size;
                                remaining_size -= size;
                                while (remaining_size > 0) {
                                    if ((size = recv(mClientFd, (char *)mSocketBuffer.data() + size,
                                                     remaining_size, MSG_WAITALL)) > 0) {
                                        remaining_size -= size;
                                        ALOGI("%s : Read-%zd after Incomplete data, remaining-%lu",
                                              __func__, size, remaining_size);
                                    }
                                }
                                size = header.size;
                            }

                            mSocketBufferSize = header.size;
                            ALOGVV("%s: Camera session state: %s", __func__,
                                   kCameraSessionStateNames.at(mCameraSessionState).c_str());
                            switch (mCameraSessionState) {
                                case CameraSessionState::kCameraOpened:
                                    mCameraSessionState = CameraSessionState::kDecodingStarted;
                                    ALOGVV("%s: Decoding started now.", __func__);
                                case CameraSessionState::kDecodingStarted:
                                    mVideoDecoder->decode(mSocketBuffer.data(), mSocketBufferSize);
                                    handle->clientRevCount++;
                                    ALOGVV("%s: Received Payload #%d %zd/%u bytes", __func__,
                                           handle->clientRevCount, size, header.size);
                                    mSocketBuffer.fill(0);
                                    break;
                                case CameraSessionState::kCameraClosed:
                                    ALOGI("%s: Decoding stopping and flushing decoder.", __func__);
                                    mCameraSessionState = CameraSessionState::kDecodingStopped;
                                    ALOGI("%s: Decoding stopped now.", __func__);
                                    break;
                                case CameraSessionState::kDecodingStopped:
                                    ALOGVV("%s: Decoding is already stopped, skip the packets",
                                           __func__);
                                    mSocketBuffer.fill(0);
                                    break;
                                default:
                                    ALOGE("%s: Invalid Camera session state!", __func__);
                                    break;
                            }
                        }
                    }
                } else {
                    ALOGE(
                        "%s: Only H264, H265, I420 Input frames are supported. Check Input format",
                        __FUNCTION__);
                }
            } else {
                //    ALOGE("%s: continue polling..", __FUNCTION__);
            }
        }
    }
    ALOGE(" %s: Quit CameraSocketServerThread... %s(%d)", __FUNCTION__, mSocketPath.c_str(),
          mClientFd);
    shutdown(mClientFd, SHUT_RDWR);
    close(mClientFd);
    mClientFd = -1;
    close(mSocketServerFd);
    mSocketServerFd = -1;
    return true;
}

}  // namespace android
