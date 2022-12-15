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

//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "ClientCommunicator"
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
                                       std::shared_ptr<MfxDecoder> decoder,
                                       int client_id)
    : mRunning{true},
      mClientId(client_id),
      mClientFd(-1),
      mListener{listener},
      mVideoDecoder{decoder} {
    ALOGD("%s(%d): Created Connection Thread", __FUNCTION__, mClientId);
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
        ALOGE("%s(%d): We're not connected to client yet!", __FUNCTION__, mClientId);
        return false;
    }
    if (send(mClientFd, config_cmd_packet, config_cmd_packet_size, 0) < 0) {
        ALOGE("%s(%d): Failed to send command to client, err %s ", __FUNCTION__, mClientId, strerror(errno));
        return false;
    }
    return true;
}

void ClientCommunicator::configureCapabilities() {
    ALOGVV("%s(%d) Enter", __FUNCTION__, mClientId);

    bool valid_client_cap_info = false;
    int camera_id, expctd_cam_id;
    struct ValidateClientCapability val_client_cap[MAX_NUMBER_OF_SUPPORTED_CAMERAS];
    size_t ack_packet_size = sizeof(camera_header_t) + sizeof(camera_ack_t);
    size_t cap_packet_size = sizeof(camera_header_t) + sizeof(camera_capability_t);
    ssize_t recv_size = 0;
    mNumOfCamerasRequested = 0;
    camera_ack_t ack_payload = ACK_CONFIG;

    camera_capability_t capability = {};

    camera_packet_t *cap_packet = NULL;
    camera_packet_t *ack_packet = NULL;
    camera_header_t header = {};

    Mutex::Autolock al(mMutex);
    cap_packet = (camera_packet_t *)malloc(cap_packet_size);
    if (cap_packet == NULL) {
        ALOGE("%s(%d): cap camera_packet_t allocation failed: %d ", __FUNCTION__, mClientId, __LINE__);
        return;
    }

    cap_packet->header.type = CAPABILITY;
    cap_packet->header.size = sizeof(camera_capability_t);
    capability.codec_type = (uint32_t)VideoCodecType::kAll;
    capability.resolution = (uint32_t)FrameResolution::k1080p;
    capability.maxNumberOfCameras = MAX_NUMBER_OF_SUPPORTED_CAMERAS;

    memcpy(cap_packet->payload, &capability, sizeof(camera_capability_t));
    if (send(mClientFd, cap_packet, cap_packet_size, 0) < 0) {
        ALOGE("%s(%d): Failed to send camera capabilities, err: %s ", __FUNCTION__, mClientId,
              strerror(errno));
        goto out;
    }
    ALOGI("%s(%d): Sent CAPABILITY packet to client", __FUNCTION__, mClientId);

    if ((recv_size = recv(mClientFd, &header, sizeof(camera_header_t), MSG_WAITALL)) < 0) {
        ALOGE("%s(%d): Failed to receive header, err: %s ", __FUNCTION__, mClientId, strerror(errno));
        goto out;
    }

    if (header.type != CAMERA_INFO) {
        ALOGE("%s(%d): invalid camera_packet_type: %s", __FUNCTION__, mClientId,
              camera_type_to_str(header.type));
        goto out;
    }

    if (header.size < sizeof(camera_info_t)) {
        ALOGE("%s(%d): No camera device to support, header size received, size = %zu",
              __FUNCTION__, mClientId, recv_size);
        goto send_ack;
    } else {
        // Get the number of cameras requested to support from client.
        mNumOfCamerasRequested = (header.size) / sizeof(camera_info_t);
    }

    camera_info_t camera_info[mNumOfCamerasRequested];
    memset(camera_info, 0, sizeof(camera_info));

    if ((recv_size = recv(mClientFd, camera_info,
                          header.size, MSG_WAITALL)) < 0) {
        ALOGE("%s(%d): Failed to receive camera info, err: %s ", __FUNCTION__, mClientId, strerror(errno));
        goto out;
    }

    ALOGI("%s(%d): Received CAMERA_INFO packet from client with recv_size: %zd ", __FUNCTION__, mClientId,
          recv_size);
    ALOGI("%s(%d): Number of cameras requested = %d", __FUNCTION__, mClientId, mNumOfCamerasRequested);

    if (mNumOfCamerasRequested > MAX_NUMBER_OF_SUPPORTED_CAMERAS) {
        ALOGW("%s(%d):[warning] Number of cameras requested by client is higher "
              "than the max number of cameras supported in the HAL"
              "We can only support max number of cameras that is supported in "
              "the HAL instead of number of cameras requested by client",
              __FUNCTION__, mClientId);
        mNumOfCamerasRequested = MAX_NUMBER_OF_SUPPORTED_CAMERAS;
    }

    // validate capability info received from the client.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        expctd_cam_id = i;
        if (expctd_cam_id == (int)camera_info[i].cameraId)
            ALOGVV("%s(%d): Camera Id number %u received from client is matching with expected Id",
                   __FUNCTION__, mClientId, camera_info[i].cameraId);
        else
            ALOGI("%s(%d): [Warning] Camera Id number %u received from client is not matching with "
                  "expected Id %d",
                  __FUNCTION__, mClientId, camera_info[i].cameraId, expctd_cam_id);

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
            ALOGE("%s(%d): capability info received from client is not completely correct and expected",
                  __FUNCTION__, mClientId);
            break;
        } else {
            ALOGVV("%s(%d): capability info received from client is correct and expected",
                   __FUNCTION__, mClientId);
            valid_client_cap_info = true;
        }
    }

    // Updating metadata for each camera seperately with its capability info received.
    for (int i = 0; i < mNumOfCamerasRequested; i++) {
        camera_id = i;
        ALOGI("%s(%d) - Client requested for codec_type: %s, resolution: %s, orientation: %u, and "
              "facing: %u for camera Id %d",
              __FUNCTION__, mClientId, codec_type_to_str(camera_info[i].codec_type),
              resolution_to_str(camera_info[i].resolution), camera_info[i].sensorOrientation,
              camera_info[i].facing, camera_id);

        if (!val_client_cap[i].validResolution) {
            // Set default resolution if receive invalid capability info from client.
            // Default resolution would be 480p.
            camera_info[i].resolution = (uint32_t)FrameResolution::k480p;
            ALOGE("%s(%d): Not received valid resolution, "
                  "hence selected 480p as default",
                  __FUNCTION__, mClientId);
        }

        if (!val_client_cap[i].validCodecType) {
            // Set default codec type if receive invalid capability info from client.
            // Default codec type would be H264.
            camera_info[i].codec_type = (uint32_t)VideoCodecType::kH264;
            ALOGE("%s(%d): Not received valid codec type, hence selected H264 as default",
                  __FUNCTION__, mClientId);
        }

        if (!val_client_cap[i].validOrientation) {
            // Set default camera sensor orientation if received invalid orientation data from
            // client. Default sensor orientation would be zero deg and consider as landscape
            // display.
            camera_info[i].sensorOrientation = (uint32_t)SensorOrientation::ORIENTATION_0;
            ALOGE("%s(%d): Not received valid sensor orientation, "
                  "hence selected ORIENTATION_0 as default",
                  __FUNCTION__, mClientId);
        }

        if (!val_client_cap[i].validCameraFacing) {
            // Set default camera facing info if received invalid facing info from client.
            // Default would be back for camera Id '0' and front for camera Id '1'.
            if (camera_id == 1)
                camera_info[i].facing = (uint32_t)CameraFacing::FRONT_FACING;
            else
                camera_info[i].facing = (uint32_t)CameraFacing::BACK_FACING;
            ALOGE("%s(%d): Not received valid camera facing info, "
                  "hence selected default",
                  __FUNCTION__, mClientId);
        }

        // Wait till complete the metadata update for a camera.
        {
            Mutex::Autolock al(sMutex);
            // Dynamic client configuration is not supported when camera
            // session is active
            if ((mCameraSessionState != CameraSessionState::kCameraOpened)
                  && (mCameraSessionState != CameraSessionState::kDecodingStarted)) {
                gVirtualCameraFactory.createVirtualRemoteCamera(mVideoDecoder, mClientId, camera_info[i]);
            }
        }
    }

send_ack:
    ack_packet = (camera_packet_t *)malloc(ack_packet_size);
    if (ack_packet == NULL) {
        ALOGE("%s(%d): ack camera_packet_t allocation failed: %d ", __FUNCTION__, mClientId, __LINE__);
        goto out;
    }
    ack_payload = (valid_client_cap_info) ? ACK_CONFIG : NACK_CONFIG;

    ack_packet->header.type = ACK;
    ack_packet->header.size = sizeof(camera_ack_t);

    memcpy(ack_packet->payload, &ack_payload, sizeof(camera_ack_t));
    if (send(mClientFd, ack_packet, ack_packet_size, 0) < 0) {
        ALOGE("%s(%d): Failed to send camera capabilities, err: %s ", __FUNCTION__, mClientId,
              strerror(errno));
        goto out;
    }
    ALOGI("%s(%d): Sent ACK packet to client with ack_size: %zu ", __FUNCTION__, mClientId,
          ack_packet_size);
    ALOGI("%s(%d): Capability negotiation and metadata update for %d camera(s) completed successfully..",
          __FUNCTION__, mClientId, mNumOfCamerasRequested);
    mIsConfigurationDone = true;
out:
    free(ack_packet);
    free(cap_packet);
    ALOGVV("%s(%d): Exit", __FUNCTION__, mClientId);
}

bool ClientCommunicator::threadLooper() {
    while (mRunning) {
        if (!clientThread()) {
            ALOGI("%s(%d) : clientThread returned flase, Exit", __FUNCTION__, mClientId);
            return false;
        } else {
            ALOGI("%s(%d) : Re-spawn clientThread", __FUNCTION__, mClientId);
        }
    }
    return true;
}

bool ClientCommunicator::clientThread() {
    ALOGVV("%s(%d) Enter", __FUNCTION__, mClientId);
    {
        Mutex::Autolock al(mMutex);
        mClientFd = mListener->getClientFd(mClientId);
        ALOGI("%s(%d): Received fd %d", __FUNCTION__, mClientId, mClientFd);
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
            ALOGE("%s(%d): POLLHUP: Close camera socket connection", __FUNCTION__, mClientId);
            break;
        } else if (event & POLLIN) {  // preview / record
            // data is available in socket => read data
            if (gIsInFrameI420) {
                ssize_t size = 0;

                if ((size = recv(mClientFd, fbuffer, 460800, MSG_WAITALL)) > 0) {
                    if (mCameraBuffer) {
                        mCameraBuffer->clientRevCount++;
                        memcpy(mCameraBuffer->clientBuf.buffer, fbuffer, 460800);
                        ALOGVV("%s(%d): [I420] Packet rev %d and "
                            "size %zd",
                            __FUNCTION__, mClientId, mCameraBuffer->clientRevCount, size);
                    } else {
                        ALOGE("%s(%d) ClientVideoBuffer not ready", __FUNCTION__, mClientId);
                    }
                }
            } else if (gIsInFrameH264) {  // default H264
                ssize_t size = 0;
                camera_header_t header = {};
                if ((size = recv(mClientFd, &header, sizeof(camera_header_t),
                                 MSG_WAITALL)) > 0) {
                    ALOGVV("%s(%d): Received Header %zd bytes. Payload size: %u",
                           __FUNCTION__, mClientId, size, header.size);
                    if (header.type == REQUEST_CAPABILITY && header.size == 0) {
                        ALOGI("%s(%d): Configure capability", __FUNCTION__, mClientId);
                        // Dynamic client configuration is not supported when camera
                        // session is active
                        if ((mCameraSessionState != CameraSessionState::kCameraOpened)
                              && (mCameraSessionState != CameraSessionState::kDecodingStarted)) {
                            gVirtualCameraFactory.clearCameraInfo(mClientId);
                        }
                        configureCapabilities();
                        continue;
                    }
                    if (!mIsConfigurationDone || header.type != CAMERA_DATA) {
                        ALOGE("%s(%d): invalid camera_packet_type: %s", __FUNCTION__, mClientId,
                              camera_type_to_str(header.type));
                        continue;
                    }

                    if (header.size > mSocketBuffer.size()) {
                        // maximum size of a H264 packet in any aggregation packet is 65535
                        // bytes. Source: https://tools.ietf.org/html/rfc6184#page-13
                        ALOGE(
                            "%s(%d) Fatal: Unusual encoded packet size detected: %u! Max is %zu, "
                            "...",
                            __func__, mClientId, header.size, mSocketBuffer.size());
                        continue;
                    }

                    // recv frame
                    if ((size = recv(mClientFd, mSocketBuffer.data(), header.size,
                                     MSG_WAITALL)) > 0) {
                        if (size < header.size) {
                            ALOGW("%s(%d) : Incomplete data read %zd/%u bytes", __func__, mClientId,
                                  size, header.size);
                            size_t bytes_read = size;
                            while (bytes_read < header.size) {
                                if ((size = recv(mClientFd, mSocketBuffer.data() + bytes_read,
                                                 header.size - bytes_read, MSG_WAITALL)) > 0) {
                                    bytes_read += size;
                                    ALOGI("%s(%d) : Read-%zd after Incomplete data, remaining-%lu",
                                          __func__, mClientId, size, header.size - bytes_read);
                                }
                            }
                            size = header.size;
                        }

                        ALOGV("%s : Received encoded frame from client", __func__);
                        mSocketBufferSize = header.size;

                        ALOGVV("%s(%d): Camera session state: %s", __func__, mClientId,
                               kCameraSessionStateNames.at(mCameraSessionState).c_str());
                        switch (mCameraSessionState) {
                            case CameraSessionState::kCameraOpened:
                                mCameraSessionState = CameraSessionState::kDecodingStarted;
                                ALOGVV("%s(%d): Decoding started now.", __func__, mClientId);
                            case CameraSessionState::kDecodingStarted: {
                                if (mCameraBuffer == NULL) break;
                                mCameraBuffer->clientRevCount++;
                                ALOGVV("%s(%d): Received Payload #%d %zd/%u bytes", __func__, mClientId,
                                       mCameraBuffer->clientRevCount, size, header.size);

                                mfxStatus ret = MFX_ERR_NONE;
                                // Start decoding received frames.
                                ret = mVideoDecoder->DecodeFrame(mSocketBuffer.data(),
                                                                 mSocketBufferSize);
                                if (ret == MFX_ERR_NONE) {
                                    ALOGV("%s(%d): Decoding success! Now need to get the output",
                                          __func__, mClientId);
                                } else {
                                    ALOGE("%s(%d): Decoding failed. ret = %d", __func__,
                                          mClientId, ret);
                                }

                                mSocketBuffer.fill(0);
                                break;
                            }
                            case CameraSessionState::kCameraClosed:
                                ALOGI("%s(%d): Closing and releasing the decoder", __func__, mClientId);
                                mCameraSessionState = CameraSessionState::kDecodingStopped;
                                break;
                            case CameraSessionState::kDecodingStopped:
                                ALOGVV("%s(%d): Decoder is already released, hence skip the client input",
                                       __func__, mClientId);
                                mSocketBuffer.fill(0);
                                break;
                            default:
                                ALOGE("%s(%d): Invalid Camera session state!", __func__, mClientId);
                                break;
                        }
                    }
                }
            } else {
                ALOGE(
                    "%s(%d): Only H264, H265, I420 Input frames are supported. Check Input format",
                    __FUNCTION__, mClientId);
            }
        } else if (event != 0) {
            ALOGE("%s(%d): Event(%d), continue polling..", __FUNCTION__, mClientId, event);
        }
    }
    ALOGE("%s(%d): Quit ClientCommunicator... fd(%d)", __FUNCTION__, mClientId, mClientFd);
    if (gIsInFrameI420) {
        delete[] fbuffer;
    }
    mListener->clearClientFd(mClientId);
    shutdown(mClientFd, SHUT_RDWR);
    close(mClientFd);
    mClientFd = -1;
    ALOGVV("%s(%d): Exit", __FUNCTION__, mClientId);
    return true;
}

}  // namespace android
