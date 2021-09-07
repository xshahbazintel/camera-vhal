#ifndef HW_EMULATOR_CAMERA_VIRTUALD_CAMERA_FACTORY_H_K
#define HW_EMULATOR_CAMERA_VIRTUALD_CAMERA_FACTORY_H_K

#include <mutex>

#define BPP_NV12 1.5  // 12 bpp

namespace android {

extern bool gIsInFrameI420;
extern bool gIsInFrameH264;
extern bool gUseVaapi;

// Camera max input supported res width and height.
// This would be used for buffer allocation
// based on client capability.
extern int32_t srcCameraWidth;
extern int32_t srcCameraHeight;

// Camera input res width and height during running
// condition. It would vary based on app's request.
extern int32_t srcWidth;
extern int32_t srcHeight;

extern bool gCapabilityInfoReceived;

enum class VideoBufferType {
    kI420,
    kARGB,
};

struct Resolution {
    int width = srcCameraWidth;
    int height = srcCameraHeight;
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
        std::fill(buffer, buffer + srcWidth * srcHeight, 0x10);
        uint8_t* uv_offset = buffer + srcWidth * srcHeight;
        std::fill(uv_offset, uv_offset + (srcWidth * srcHeight) / 2, 0x80);
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
