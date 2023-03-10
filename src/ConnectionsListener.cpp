/*
 * Copyright (C) 2019-2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ConnectionsListener.h"

#define LOG_TAG "ConnectionsListener"
#include <log/log.h>

#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <cutils/properties.h>
#include "CameraSocketCommand.h"

namespace android {

ConnectionsListener::ConnectionsListener(std::string suffix)
    : mRunning{true},
      mSocketServerFd{-1} {
    std::string sock_path = "/ipc/camera-socket" + suffix;
    char *k8s_env_value = getenv("K8S_ENV");
    mSocketPath = (k8s_env_value != NULL && !strcmp(k8s_env_value, "true")) ? "/conn/camera-socket"
                                                                            : sock_path.c_str();
    ALOGI("%s camera socket server path is %s", __FUNCTION__, mSocketPath.c_str());
    char buf[PROPERTY_VALUE_MAX] = {
        '\0',
    };
    uint32_t num_clients = 1;
    if (property_get("ro.concurrent.user.num", buf, "") > 0){
        uint32_t num = atoi(buf);
        if (num > 1 && num <= MAX_CONCURRENT_USER_NUM){
            mNumConcurrentUsers = num_clients = num;
            ALOGI("%s Support %u concurrent multi users", __FUNCTION__, num);
        } else if (num == 1) {
            ALOGI("%s Support only single user", __FUNCTION__);
        } else {
            ALOGE("%s: Unsupported number of multi-user request(%u), please check it again",
                  __FUNCTION__, num);
        }
    }
    mClientFdPromises.resize(num_clients);
    mClientsConnected.resize(num_clients, false);
    mSocketListenerThread = std::make_unique<std::thread>(&ConnectionsListener::socketListenerThreadProc,this);
}

void ConnectionsListener::requestJoin () {
    if(mSocketListenerThread->joinable())
        mSocketListenerThread->join();
}

int ConnectionsListener::getClientFd(int clientId) {
    return mClientFdPromises[clientId].get_future().get();
}

void ConnectionsListener::clearClientFd(int clientId) {
    mClientFdPromises[clientId] = std::promise<int>();
    mClientsConnected[clientId] = false;
}

void ConnectionsListener::requestExit() {
    Mutex::Autolock al(mMutex);
    mRunning = false;
}

bool ConnectionsListener::socketListenerThreadProc() {
    mSocketServerFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (mSocketServerFd < 0) {
        ALOGE("%s:%d Fail to construct camera socket with error: %s", __FUNCTION__, __LINE__,
              strerror(errno));
        return false;
    }

    struct pollfd fd;
    struct sockaddr_un addr_un;
    memset(&addr_un, 0, sizeof(addr_un));
    addr_un.sun_family = AF_UNIX;
    strncpy(&addr_un.sun_path[0], mSocketPath.c_str(), strlen(mSocketPath.c_str()));

    int ret = 0;
    if ((access(mSocketPath.c_str(), F_OK)) != -1) {
        ALOGI(" %s camera socket server file is %s", __FUNCTION__, mSocketPath.c_str());
        ret = unlink(mSocketPath.c_str());
        if (ret < 0) {
            ALOGE(" %s Failed to unlink %s address %d, %s", __FUNCTION__,
                  mSocketPath.c_str(), ret, strerror(errno));
            return false;
        }
    } else {
        ALOGI(" %s camera socket server file %s will created. ", __FUNCTION__,
              mSocketPath.c_str());
    }

    ret = ::bind(mSocketServerFd, (struct sockaddr *)&addr_un,
                 sizeof(sa_family_t) + strlen(mSocketPath.c_str()) + 1);
    if (ret < 0) {
        ALOGE(" %s Failed to bind %s address %d, %s", __FUNCTION__, mSocketPath.c_str(),
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
    fd.fd = mSocketServerFd;
    fd.events = POLLIN;

    while (mRunning) {
        uint32_t client_id = 0;
        if (!mClientsConnected[client_id])
            ALOGI(" %s: Wait for camera client to connect. . .", __FUNCTION__);
        ret = poll(&fd, 1, 3000);
        if (ret == 0)
        {
            ALOGV("%s: Poll() timedout", __FUNCTION__);
            continue;
        }
        else if (ret < 0) {
            ALOGE("%s: Poll() failed with err = %d", __FUNCTION__,ret);
            continue;
        }
        else if (fd.revents & POLLIN) {
            socklen_t alen = sizeof(struct sockaddr_un);

            int new_client_fd = ::accept(mSocketServerFd, (struct sockaddr *)&addr_un, &alen);
            if (new_client_fd < 0) {
                ALOGE(" %s: Fail to accept client. Error: [%s]", __FUNCTION__, strerror(errno));
                continue;
            } else {
                ALOGI(" %s: Accepted client: [%d]", __FUNCTION__, new_client_fd);
            }
            if (mNumConcurrentUsers > 0) {
                size_t packet_size = sizeof(android::socket::camera_header_t) + sizeof(client_id);
                bool status = true;
                android::socket::camera_packet_t * user_id_packet = (android::socket::camera_packet_t *)malloc(packet_size);
                if (user_id_packet == NULL) {
                    ALOGE("%s: user_id_packet allocation failed: %d ", __FUNCTION__, __LINE__);
                    continue;
                }
                if (recv(new_client_fd, (char *)user_id_packet, packet_size, MSG_WAITALL) < 0) {
                    ALOGE("%s: Failed to receive user_id header, err: %s ", __FUNCTION__, strerror(errno));
                    status = false;
                }
                if (user_id_packet->header.type != android::socket::CAMERA_USER_ID) {
                    ALOGE("%s: Invalid packet type %d\n", __FUNCTION__, user_id_packet->header.type);
                    status = false;
                }
                if (user_id_packet->header.size != sizeof(client_id)) {
                    ALOGE("%s: Invalid packet size %u\n", __FUNCTION__, user_id_packet->header.size);
                    status = false;
                }
                if (!status) {
                    free(user_id_packet);
                    continue;
                }
                memcpy(&client_id, user_id_packet->payload, sizeof(client_id));
                free(user_id_packet);
                if (client_id < 0 || client_id >= mNumConcurrentUsers) {
                    ALOGE("%s: client_id = %u is not valid", __FUNCTION__, client_id);
                    continue;
                }
            }
            if (mClientsConnected[client_id]) {
                ALOGE(" %s: IGNORING clientFd[%d] for already connected Client[%d]", __FUNCTION__, new_client_fd, client_id);
            } else {
                mClientFdPromises[client_id].set_value(new_client_fd);
                ALOGI(" %s: Assigned clientFd[%d] to Client[%d]", __FUNCTION__, new_client_fd, client_id);
                mClientsConnected[client_id] = true;
            }
        } // end of poll function
    } // end of while
    close(mSocketServerFd);
    mSocketServerFd = -1;
    return true;
}
}  // namespace android
