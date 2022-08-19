#ifndef HW_EMULATOR_CAMERA_VIRTUALD_CAMERA_FACTORY_H_K
#define HW_EMULATOR_CAMERA_VIRTUALD_CAMERA_FACTORY_H_K

#include <mutex>

#define BPP_NV12 1.5  // 12 bpp

namespace android {

extern bool gIsInFrameI420;
extern bool gIsInFrameH264;
extern bool gIsInFrameMJPG;
extern bool gUseVaapi;

// Max no of cameras supported based on client device request.
extern uint32_t gMaxNumOfCamerasSupported;

// Max supported res width and height out of all cameras.
// Used for input buffer allocation.
extern int32_t gMaxSupportedWidth;
extern int32_t gMaxSupportedHeight;

// Max supported res width and height of each camera.
// This would be vary for each camera based on its
// capability requested by client. And used for metadata updation
// during boot time.
extern int32_t gCameraMaxWidth;
extern int32_t gCameraMaxHeight;

// Camera input res width and height during running
// condition. It would vary based on app's request.
extern int32_t gSrcWidth;
extern int32_t gSrcHeight;

// Input Codec type info based on client device request.
extern uint32_t gCodecType;

// Orientation info of the image sensor based on client device request.
extern uint32_t gCameraSensorOrientation;

// Camera facing as either back or front based on client device request.
// True for back and false for front camera always.
extern bool gCameraFacingBack;

// Indicate client capability info received successfully when it is true.
extern bool gCapabilityInfoReceived;

// Status of metadata update, which helps to sync and update
// each metadata for each camera seperately.
extern bool gStartMetadataUpdate;
extern bool gDoneMetadataUpdate;

enum class VideoBufferType {
    kI420,
    kARGB,
};

struct Resolution {
    int width = gMaxSupportedWidth;
    int height = gMaxSupportedHeight;
};
/// Video buffer and its information
struct VideoBuffer {
    /// Video buffer
    uint8_t* buffer;
    /// Resolution for the Video buffer
    Resolution resolution;
    // Buffer type
    VideoBufferType type;
    ~VideoBuffer() {}

    // To reset allocated buffer.
    void reset() {
        std::fill(buffer, buffer + resolution.width * resolution.height, 0x10);
        uint8_t* uv_offset = buffer + resolution.width * resolution.height;
        std::fill(uv_offset, uv_offset + (resolution.width * resolution.height) / 2, 0x80);
        decoded = false;
    }

    // To clear used buffer based on current resolution.
    void clearBuffer() {
        std::fill(buffer, buffer + gSrcWidth * gSrcHeight, 0x10);
        uint8_t* uv_offset = buffer + gSrcWidth * gSrcHeight;
        std::fill(uv_offset, uv_offset + (gSrcWidth * gSrcHeight) / 2, 0x80);
        decoded = false;
    }

    bool decoded = false;
};

class ClientVideoBuffer {
public:
    static ClientVideoBuffer* ic_instance;

    struct VideoBuffer clientBuf[1];
    unsigned int clientRevCount = 0;
    unsigned int clientUsedCount = 0;

    size_t receivedFrameNo = 0;
    size_t decodedFrameNo = 0;

    static ClientVideoBuffer* getClientInstance() {
        if (ic_instance == NULL) {
            ic_instance = new ClientVideoBuffer();
        }
        return ic_instance;
    }

    ClientVideoBuffer() {
        for (int i = 0; i < 1; i++) {
            clientBuf[i].buffer = new uint8_t[clientBuf[i].resolution.width *
                                              clientBuf[i].resolution.height * BPP_NV12];
        }
        clientRevCount = 0;
        clientUsedCount = 0;
    }

    ~ClientVideoBuffer() {
        for (int i = 0; i < 1; i++) {
            delete[] clientBuf[i].buffer;
        }
    }

    void reset() {
        for (int i = 0; i < 1; i++) {
            clientBuf[i].reset();
        }
        clientRevCount = clientUsedCount = 0;
        receivedFrameNo = decodedFrameNo = 0;
    }

    void clearBuffer() {
        for (int i = 0; i < 1; i++) {
            clientBuf[i].clearBuffer();
        }
        clientRevCount = clientUsedCount = 0;
        receivedFrameNo = decodedFrameNo = 0;
    }
};
extern std::mutex client_buf_mutex;
};  // namespace android

#endif  // HW_EMULATOR_CAMERA_VIRTUALD_CAMERA_FACTORY_H_K
