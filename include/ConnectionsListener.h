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

#ifndef CONNECTIONS_LISTENER_H
#define CONNECTIONS_LISTENER_H

#include <utils/Mutex.h>
#include <utils/Thread.h>
#include <future>
#include <vector>

#define MAX_CONCURRENT_USER_NUM  8

namespace android {

class ConnectionsListener : public Thread {
public:
    ConnectionsListener(std::string suffix);
    virtual void requestExit();
    virtual status_t requestExitAndWait();
    int getClientFd(int clientId);
    void clearClientFd(int clientId);

private:
    virtual status_t readyToRun();
    virtual bool threadLoop() override;
    Mutex mMutex;
    bool mRunning;
    int mSocketServerFd = -1;
    std::string mSocketPath;
    uint32_t mNumConcurrentUsers = 0;
    std::vector<std::promise<int>> mClientFdPromises;
    std::vector<bool> mClientsConnected;
};
}  // namespace android

#endif
