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
#define LOG_TAG "ClientCommunicator: "
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
#include "ClientCommunicator.h"
#include "VirtualCameraFactory.h"
#include <mutex>

namespace android {

Mutex ClientCommunicator::sMutex;

using namespace socket;
ClientCommunicator::ClientCommunicator(std::shared_ptr<ConnectionsListener> listener,
                                                   std::shared_ptr<CGVideoDecoder> decoder,
                                                   int client_id)
    : mRunning{true},
      mClientId(client_id),
      mClientFd(-1),
      mListener{listener},
      mVideoDecoder{decoder} {
    ALOGD("%s: Created Connection Thread for client %d", __FUNCTION__, mClientId);
    mNumOfCamerasRequested = 0;
    mCameraSessionState = socket::CameraSessionState::kNone;
    mThread = std::async(&ClientCommunicator::threadLooper, this);
}

ClientCommunicator::~ClientCommunicator() {
    mRunning = false;
    if (mThread.valid()) { mThread.wait(); }
    Mutex::Autolock al(mMutex);
    if (mClientFd > 0) {
        shutdown(mClientFd, SHUT_RDWR);
        close(mClientFd);
        mClientFd = -1;
    }
}

int ClientCommunicator::getClientId() {
    return mClientId;
}

status_t ClientCommunicator::sendCommandToClient(camera_packet_t *config_cmd_packet, size_t config_cmd_packet_size) {
    Mutex::Autolock al(mMutex);
    if (mClientFd < 0) {
        ALOGE("%s: We're not connected to client yet!", __FUNCTION__);
        return false;
    }
    if (send(mClientFd, config_cmd_packet, config_cmd_packet_size, 0) < 0) {
        ALOGE(LOG_TAG "%s: Failed to send command to client, err %s ", __FUNCTION__, strerror(errno));
        return false;
    }
    return true;
}

bool ClientCommunicator::configureCapabilities() {
    ALOGVV("(%d) %s Enter", mClientId, __FUNCTION__);

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

    Mutex::Autolock al(mMutex);
    if ((recv_size = recv(mClientFd, (char *)&header, sizeof(camera_header_t), MSG_WAITALL)) < 0) {
        ALOGE("(%d) %s: Failed to receive header, err: %s ", mClientId, __FUNCTION__, strerror(errno));
        goto out;
    }

    if (header.type != REQUEST_CAPABILITY) {
        ALOGE("(%d) %s: Invalid packet type\n", mClientId, __FUNCTION__);
        goto out;
    }
    ALOGI("(%d) %s: Received REQUEST_CAPABILITY header from client", mClientId, __FUNCTION__);

    cap_packet = (camera_packet_t *)malloc(cap_packet_size);
    if (cap_packet == NULL) {
        ALOGE("(%d) %s: cap camera_packet_t allocation failed: %d ", mClientId, __FUNCTION__, __LINE__);
        return false;
    }

    cap_packet->header.type = CAPABILITY;
    cap_packet->header.size = sizeof(camera_capability_t);
    capability.codec_type = (uint32_t)VideoCodecType::kAll;
    capability.resolution = (uint32_t)FrameResolution::k1080p;
    capability.maxNumberOfCameras = MAX_NUMBER_OF_SUPPORTED_CAMERAS;

    memcpy(cap_packet->payload, &capability, sizeof(camera_capability_t));
    if (send(mClientFd, cap_packet, cap_packet_size, 0) < 0) {
        ALOGE("(%d) %s: Failed to send camera capabilities, err: %s ", mClientId, __FUNCTION__,
              strerror(errno));
        goto out;
    }
    ALOGI("(%d) %s: Sent CAPABILITY packet to client", mClientId, __FUNCTION__);

    if ((recv_size = recv(mClientFd, (char *)&header, sizeof(camera_header_t), MSG_WAITALL)) < 0) {
        ALOGE("(%d) %s: Failed to receive header, err: %s ", mClientId, __FUNCTION__, strerror(errno));
        goto out;
    }

    if (header.type != CAMERA_INFO) {
        ALOGE("(%d) %s: invalid camera_packet_type: %s", mClientId, __FUNCTION__,
              camera_type_to_str(header.type));
        goto out;
    }

    // Get the number of cameras requested to support from client.
    for (int i = 1; i <= MAX_NUMBER_OF_SUPPORTED_CAMERAS; i++) {
        if (header.size == i * sizeof(camera_info_t)) {
            mNumOfCamerasRequested = i;
            break;
        } else if (mNumOfCamerasRequested == 0 && i == MAX_NUMBER_OF_SUPPORTED_CAMERAS) {
            ALOGE("(%d) %s: Failed to support number of cameras requested by client "
                  "which is higher than the max number of cameras supported in the HAL",
                  mClientId, __FUNCTION__);
            goto out;
        }
    }

    if (mNumOfCamerasRequested == 0) {
        ALOGE("(%d) %s: invalid header size received, size = %zu", mClientId, __FUNCTION__, recv_size);
        goto out;
    }

    if ((recv_size = recv(mClientFd, (char *)&camera_info,
                          mNumOfCamerasRequested * sizeof(camera_info_t), MSG_WAITALL)) < 0) {
        ALOGE("(%d) %s: Failed to receive camera info, err: %s ", mClientId, __FUNCTION__, strerror(errno));
        goto out;
    }

    ALOGI("(%d) %s: Received CAMERA_INFO packet from client with recv_size: %zd ", mClientId, __FUNCTION__,
          recv_size);
    ALOGI("(%d) %s: Number of cameras requested = %d", mClientId, __FUNCTION__, mNumOfCamerasRequested);

    // validate capability info received from the client.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        expctd_cam_id = i;
        if (expctd_cam_id == (int)camera_info[i].cameraId)
            ALOGVV("(%d) %s: Camera Id number %u received from client is matching with expected Id",
                   mClientId, __FUNCTION__, camera_info[i].cameraId);
        else
            ALOGI("(%d) %s: [Warning] Camera Id number %u received from client is not matching with "
                  "expected Id %d",
                  mClientId, __FUNCTION__, camera_info[i].cameraId, expctd_cam_id);

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
        camera_id = i;
        ALOGI("(%d) %s - Client requested for codec_type: %s, resolution: %s, orientation: %u, and "
              "facing: %u for camera Id %d",
              mClientId, __FUNCTION__, codec_type_to_str(camera_info[i].codec_type),
              resolution_to_str(camera_info[i].resolution), camera_info[i].sensorOrientation,
              camera_info[i].facing, camera_id);

        if (!val_client_cap[i].validResolution) {
            // Set default resolution if receive invalid capability info from client.
            // Default resolution would be 480p.
            camera_info[i].resolution = (uint32_t)FrameResolution::k480p;
            ALOGE("(%d) %s: Not received valid resolution, "
                  "hence selected 480p as default",
                  mClientId, __FUNCTION__);
        }

        if (!val_client_cap[i].validCodecType) {
            // Set default codec type if receive invalid capability info from client.
            // Default codec type would be H264.
            camera_info[i].codec_type = (uint32_t)VideoCodecType::kH264;
            ALOGE("(%d) %s: Not received valid codec type, hence selected H264 as default",
                  mClientId, __FUNCTION__);
        }

        if (!val_client_cap[i].validOrientation) {
            // Set default camera sensor orientation if received invalid orientation data from
            // client. Default sensor orientation would be zero deg and consider as landscape
            // display.
            camera_info[i].sensorOrientation = (uint32_t)SensorOrientation::ORIENTATION_0;
            ALOGE("(%d) %s: Not received valid sensor orientation, "
                  "hence selected ORIENTATION_0 as default",
                  mClientId, __FUNCTION__);
        }

        if (!val_client_cap[i].validCameraFacing) {
            // Set default camera facing info if received invalid facing info from client.
            // Default would be back for camera Id '0' and front for camera Id '1'.
            if (camera_id == 1)
                camera_info[i].facing = (uint32_t)CameraFacing::FRONT_FACING;
            else
                camera_info[i].facing = (uint32_t)CameraFacing::BACK_FACING;
            ALOGE("(%d) %s: Not received valid camera facing info, "
                  "hence selected default",
                  mClientId, __FUNCTION__);
        }

        // Wait till complete the metadata update for a camera.
        {
            Mutex::Autolock al(sMutex);
            gVirtualCameraFactory.createVirtualRemoteCamera(mVideoDecoder, mClientId, camera_info[i]);
        }
    }

    ack_packet = (camera_packet_t *)malloc(ack_packet_size);
    if (ack_packet == NULL) {
        ALOGE("(%d) %s: ack camera_packet_t allocation failed: %d ", mClientId, __FUNCTION__, __LINE__);
        goto out;
    }
    ack_payload = (valid_client_cap_info) ? ACK_CONFIG : NACK_CONFIG;

    ack_packet->header.type = ACK;
    ack_packet->header.size = sizeof(camera_ack_t);

    memcpy(ack_packet->payload, &ack_payload, sizeof(camera_ack_t));
    if (send(mClientFd, ack_packet, ack_packet_size, 0) < 0) {
        ALOGE("(%d) %s: Failed to send camera capabilities, err: %s ", mClientId, __FUNCTION__,
              strerror(errno));
        goto out;
    }
    ALOGI("(%d) %s: Sent ACK packet to client with ack_size: %zu ", mClientId, __FUNCTION__,
          ack_packet_size);

    status = true;
out:
    free(ack_packet);
    free(cap_packet);
    ALOGVV("(%d) %s: Exit", mClientId, __FUNCTION__);
    return status;
}

bool ClientCommunicator::threadLooper() {
    while (mRunning) {
        if (!clientThread()) {
            ALOGI("(%d) %s : clientThread returned flase, Exit", mClientId, __FUNCTION__);
            return false;
        } else {
            ALOGI("(%d) %s : Re-spawn clientThread", mClientId, __FUNCTION__);
        }
    }
    return true;
}

bool ClientCommunicator::clientThread() {
    ALOGVV("(%d) %s Enter", mClientId, __FUNCTION__);
    bool status = false;
    {
        Mutex::Autolock al(mMutex);
        mClientFd = mListener->getClientFd(mClientId);
        ALOGI("(%d) %s: Received fd %d", mClientId, __FUNCTION__, mClientFd);
    }
    status = configureCapabilities();
    if (status) {
        ALOGI("(%d) %s: Capability negotiation and metadata update "
              "for %d camera(s) completed successfully..",
              mClientId, __FUNCTION__, mNumOfCamerasRequested);
    } else {
        if (mNumOfCamerasRequested == 0) {
            ALOGE(LOG_TAG " %s: Camera info received, but no camera device to support, "
                  "hence no need to continue the process", __FUNCTION__);
            return false;
        } else {
            ALOGE(LOG_TAG "%s: Capability negotiation failed..",  __FUNCTION__);
        }
    }

    char *fbuffer;
    if (gIsInFrameI420) {
        fbuffer = new char[460800];
    }

    struct pollfd fd;
    int event;

    fd.fd = mClientFd;  // your socket handler
    fd.events = POLLIN | POLLHUP;

    while (mRunning) {
        // check if there are any events on fd.
        int ret = poll(&fd, 1, 3000);  // 3 seconds for timeout

        event = fd.revents;  // returned events

        if (event & POLLHUP) {
            // connnection disconnected => socket is closed at the other end => close the
            // socket.
            ALOGE("(%d) %s: POLLHUP: Close camera socket connection", mClientId, __FUNCTION__);
            break;
        } else if (status && (event & POLLIN)) {  // preview / record
            Mutex::Autolock al(mMutex);
            // data is available in socket => read data
            if (gIsInFrameI420) {
                ssize_t size = 0;

                if ((size = recv(mClientFd, (char *)fbuffer, 460800, MSG_WAITALL)) > 0) {
                    if (mCameraBuffer) {
                        mCameraBuffer->clientRevCount++;
                        memcpy(mCameraBuffer->clientBuf.buffer, &fbuffer, 460800);
                        ALOGVV("(%d) [I420] %s: Pocket rev %d and "
                            "size %zd",
                            mClientId, __FUNCTION__, mCameraBuffer->clientRevCount, size);
                    } else {
                        ALOGE("(%d) ClientVideoBuffer not ready", mClientId);
                    }
                }
            } else if (gIsInFrameH264) {  // default H264
                ssize_t size = 0;
                camera_header_t header = {};
                if ((size = recv(mClientFd, (char *)&header, sizeof(camera_header_t),
                                 MSG_WAITALL)) > 0) {
                    ALOGVV("%s: Received Header %zd bytes. Payload size: %u", __FUNCTION__,
                           size, header.size);
                    if (header.type == REQUEST_CAPABILITY) {
                        ALOGI("(%d) %s: [Warning] Capability negotiation was already "
                              "done for %d camera(s); Can't do re-negotiation again!!!",
                              mClientId, __FUNCTION__, mNumOfCamerasRequested);
                        continue;
                    } else if (header.type != CAMERA_DATA) {
                        ALOGE("(%d) %s: invalid camera_packet_type: %s", mClientId, __FUNCTION__,
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
                                if (mCameraBuffer == NULL) break;
                                mCameraBuffer->clientRevCount++;
                                ALOGVV("%s: Received Payload #%d %zd/%u bytes", __func__,
                                       mCameraBuffer->clientRevCount, size, header.size);
                                mVideoDecoder->decode(mSocketBuffer.data(), mSocketBufferSize);
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
        } else if (event != 0) {
            ALOGE("(%d) %s: Event(%d), continue polling..", mClientId, __FUNCTION__, event);
        }
    }
    ALOGE(" %s: Quit ClientCommunicator... fd(%d)", __FUNCTION__, mClientFd);
    if (gIsInFrameI420) {
        delete[] fbuffer;
    }
    mListener->clearClientFd(mClientId);
    shutdown(mClientFd, SHUT_RDWR);
    close(mClientFd);
    mClientFd = -1;
    ALOGVV("(%d) %s: Exit", mClientId, __FUNCTION__);
    return true;
}

}  // namespace android
