
#include "ConnectionsListener.h"

#define LOG_TAG "ConnectionsListener"
#include <log/log.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cutils/properties.h>
#include <CameraSocketCommand.h>

namespace android {


ConnectionsListener::ConnectionsListener(std::string suffix)
    : Thread(/*canCallJava*/ false),
      mRunning{true},
      mSocketServerFd{-1} {
    std::string sock_path = "/ipc/camera-socket" + suffix;
    char *k8s_env_value = getenv("K8S_ENV");
    mSocketPath = (k8s_env_value != NULL && !strcmp(k8s_env_value, "true")) ? "/conn/camera-socket"
                                                                            : sock_path.c_str();
    ALOGI("%s camera socket server path is %s", __FUNCTION__, mSocketPath.c_str());
    char buf[PROPERTY_VALUE_MAX] = {
        '\0',
    };
    int num_clients = 1;
    mClientFdPromises.resize(num_clients);
}

status_t ConnectionsListener::requestExitAndWait() {
    ALOGE("%s: Not implemented. Use requestExit + join instead", __FUNCTION__);
    return INVALID_OPERATION;
}

int ConnectionsListener::getClientFd(int clientId) {
    return mClientFdPromises[clientId].get_future().get();
}

void ConnectionsListener::requestExit() {
    Mutex::Autolock al(mMutex);

    ALOGV("%s: Requesting thread exit", __FUNCTION__);
    mRunning = false;
    ALOGV("%s: Request exit complete.", __FUNCTION__);
}

status_t ConnectionsListener::readyToRun() {
    Mutex::Autolock al(mMutex);

    return OK;
}


bool ConnectionsListener::threadLoop() {
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
        ALOGI(LOG_TAG " %s camera socket server file %s will created. ", __FUNCTION__,
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
        android::socket::camera_header_t header = {};
        uint32_t client_id = 0;
        mClientFdPromises[client_id].set_value(new_client_fd);
        ALOGI(LOG_TAG " %s: Assigned clientFd[%d] to Client[%d]", __FUNCTION__, new_client_fd, client_id);
    }
    close(mSocketServerFd);
    mSocketServerFd = -1;
    return true;
}
}  // namespace android
