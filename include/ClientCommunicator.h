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

#ifndef CLIENT_COMMUNICATOR_H
#define CLIENT_COMMUNICATOR_H

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <utils/Thread.h>
#include <utils/Vector.h>
#include <string>
#include <memory>
#include <atomic>
#include <array>
#include <chrono>
#include <thread>
#include "CGCodec.h"
#include "CameraSocketCommand.h"
#include "ConnectionsListener.h"
#include "VirtualBuffer.h"

namespace android {

class VirtualCameraFactory;
class ClientCommunicator : public Thread {
public:
    ClientCommunicator(std::shared_ptr<ConnectionsListener> listener,
                             std::shared_ptr<CGVideoDecoder> decoder,
                             int client_id);
    ~ClientCommunicator();

    virtual void requestExit();
    virtual status_t requestExitAndWait();
    int getClientId();
    status_t sendCommandToClient(socket::camera_packet_t *config_cmd_packet, size_t config_cmd_packet_size);
    std::atomic<socket::CameraSessionState> mCameraSessionState;
    std::shared_ptr<ClientVideoBuffer> mCameraBuffer;

private:
    virtual status_t readyToRun();
    virtual bool threadLoop() override;

    bool configureCapabilities();

    Mutex mMutex;
    static Mutex sMutex; //Synchronize across threads
    bool mRunning;  // guarding only when it's important
    int mClientId;
    int mClientFd = -1;
    int mNumOfCamerasRequested;  // Number of cameras requested to support by client.

    std::shared_ptr<ConnectionsListener> mListener;
    std::shared_ptr<CGVideoDecoder> mVideoDecoder;

    // maximum size of a H264 packet in any aggregation packet is 65535 bytes.
    // Source: https://tools.ietf.org/html/rfc6184#page-13
    std::array<uint8_t, 200 * 1024> mSocketBuffer = {};
    size_t mSocketBufferSize = 0;

    struct ValidateClientCapability {
        bool validCodecType = false;
        bool validResolution = false;
        bool validOrientation = false;
        bool validCameraFacing = false;
    };
};
}  // namespace android

#endif
