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

#ifndef CLIENT_COMMUNICATOR_H
#define CLIENT_COMMUNICATOR_H

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <utils/Vector.h>
#include <string>
#include <memory>
#include <atomic>
#include <future>
#include <array>
#include <chrono>
#include "onevpl-video-decode/MfxDecoder.h"
#include "CameraSocketCommand.h"
#include "ConnectionsListener.h"
#include "CapabilitiesHelper.h"
#include "VirtualBuffer.h"

namespace android {

class VirtualCameraFactory;
class ClientCommunicator {
public:
    ClientCommunicator(std::shared_ptr<ConnectionsListener> listener,
                       std::shared_ptr<MfxDecoder> decoder,
                       int client_id);
    ~ClientCommunicator();

    int getClientId();
    status_t sendCommandToClient(socket::camera_packet_t *config_cmd_packet,
                                 size_t config_cmd_packet_size);
    bool IsValidClientCapInfo();
    void requestExit();
    std::atomic<socket::CameraSessionState> mCameraSessionState;
    std::shared_ptr<ClientVideoBuffer> mCameraBuffer;

private:
    bool clientThread();
    bool threadLooper();

    void sendCameraCapabilities();
    void handleCameraInfo(uint32_t header_size);
    void sendAck();
    void handleIncomingFrames(uint32_t header_size);

    Mutex mMutex;
    static Mutex sMutex; //Synchronize across threads
    bool mRunning;  // guarding only when it's important
    int mClientId;
    int mClientFd = -1;
    int mNumOfCamerasRequested;  // Number of cameras requested to support by client.
    bool mIsConfigurationDone = false;
    bool mValidClientCapInfo = false;

    std::shared_ptr<ConnectionsListener> mListener;
    std::shared_ptr<MfxDecoder> mVideoDecoder;
    CapabilitiesHelper mCapabilitiesHelper;

    // maximum size of a H264 packet in any aggregation packet is 65535 bytes.
    // Source: https://tools.ietf.org/html/rfc6184#page-13
    std::array<uint8_t, 200 * 1024> mSocketBuffer = {};
    size_t mSocketBufferSize = 0;

    std::future<bool> mThread;

};
}  // namespace android

#endif
