/*
** Copyright 2018 Intel Corporation
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "cg_codec_vhal"

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#define MAX_DEVICE_NAME_SIZE 21
#define MAX_ALLOWED_PENDING_FRAMES 2

#include <cutils/properties.h>
#include <vector>
#include <mutex>
#include "CGCodec.h"
#include "CGLog.h"
#include "CameraSocketCommand.h"

using namespace std;

//////// @class CGVideoFrame ////////

CGPixelFormat CGVideoFrame::format() {
    switch (m_avframe->format) {
        case AV_PIX_FMT_NV12:
            return CGPixelFormat::NV12;
        case AV_PIX_FMT_YUV420P:
        default:
            return CGPixelFormat::I420;
    }
}

int CGVideoFrame::copy_to_buffer(uint8_t *out_buffer, int *size) {
    ALOGVV("%s E", __func__);

    if (!out_buffer || !size) {
        ALOGW("Bad input parameter.\n");
        return -1;
    }

    int buf_size = av_image_get_buffer_size((AVPixelFormat)m_avframe->format, m_avframe->width,
                                            m_avframe->height, 1);

    int ret =
        av_image_copy_to_buffer(out_buffer, buf_size, (const uint8_t *const *)m_avframe->data,
                                (const int *)m_avframe->linesize, (AVPixelFormat)m_avframe->format,
                                m_avframe->width, m_avframe->height, 1);
    if (ret < 0) {
        ALOGW("Can not copy image to buffer\n");
        return -1;
    }

    *size = buf_size;
    ALOGVV("%s: X", __func__);
    return 0;
}
//////// @class DecodeContext ////////

struct DecodeContext {
    DecodeContext(int codec_type, int resolution_type);
    virtual ~DecodeContext() = default;

    AVCodecParserContext *parser;
    AVCodecContext *avcodec_ctx;
    AVPacket *packet;

    std::mutex mutex_;
    std::vector<AVFrame *> decoded_frames;

    // parameters by configuration
    int codec_type;
    std::pair<int, int> resolution;
};

DecodeContext::DecodeContext(int codec_type, int resolution_type) : codec_type(codec_type) {
    parser = nullptr;
    avcodec_ctx = nullptr;
    packet = nullptr;
    if (resolution_type == int(android::socket::FrameResolution::k480p)) {
        resolution = std::make_pair(640, 480);
    } else if (resolution_type == int(android::socket::FrameResolution::k720p)) {
        resolution = std::make_pair(1280, 720);
    } else if (resolution_type == int(android::socket::FrameResolution::k1080p)) {
        resolution = std::make_pair(1920, 1080);
    }

    ALOGD("Config decode type:%d width:%d height:%d\n", codec_type, resolution.first,
          resolution.second);
}

void DecodeContextDeleter::operator()(DecodeContext *p) { delete p; }

//////// @class HWAccelContext ////////

struct HWAccelContext {
    HWAccelContext(const AVCodec *decoder, AVCodecContext *avcodec_ctx, const char *device_name,
                   int extra_frames);
    virtual ~HWAccelContext();

    AVPixelFormat get_hw_pixel_format() { return m_hw_pix_fmt; }
    bool is_hw_accel_valid() { return m_hw_accel_valid; }

private:
    AVPixelFormat m_hw_pix_fmt = AV_PIX_FMT_NONE;
    AVBufferRef *m_hw_dev_ctx = nullptr;

    bool m_hw_accel_valid = false;
};

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    HWAccelContext *hw_accel_ctx = reinterpret_cast<HWAccelContext *>(ctx->opaque);
    const enum AVPixelFormat hw_pix_fmt = hw_accel_ctx->get_hw_pixel_format();

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt) return *p;
    }

    ALOGW("Failed to get HW pixel format.\n");
    return AV_PIX_FMT_NONE;
}

HWAccelContext::HWAccelContext(const AVCodec *decoder, AVCodecContext *avcodec_ctx,
                               const char *device_name, int extra_frames) {
    const char *device_prefix = "/dev/dri/renderD";
    char device[MAX_DEVICE_NAME_SIZE] = {'\0'};
    char prop_val[PROPERTY_VALUE_MAX] = {'\0'};

    if (!decoder || !avcodec_ctx || !device_name || extra_frames < 0) {
        ALOGW("Invalid parameters for hw accel context.\n");
        return;
    }

    AVHWDeviceType type = av_hwdevice_find_type_by_name(device_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        ALOGW("Device type %s is not supported.\n", device_name);
        return;
    }

    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (config == nullptr) {
            ALOGW("Decoder %s does not support device type %s.\n", decoder->name,
                  av_hwdevice_get_type_name(type));
            return;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            m_hw_pix_fmt = config->pix_fmt;
            break;
        }
    }

    avcodec_ctx->opaque = this;
    avcodec_ctx->get_format = get_hw_format;
    avcodec_ctx->thread_count = 1;  // FIXME: vaapi decoder multi thread issue
    avcodec_ctx->extra_hw_frames = extra_frames;
    avcodec_ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;

    property_get("ro.acg.rnode", prop_val, "0");
    int count = snprintf(device, sizeof(device), "%s%d", device_prefix, 128 + atoi(prop_val));
    if (count < 0 || count > MAX_DEVICE_NAME_SIZE) {
        strcpy(device, "/dev/dri/renderD128");
    }
    ALOGI("%s - device: %s\n", __FUNCTION__, device);
    if ((av_hwdevice_ctx_create(&m_hw_dev_ctx, type, device, NULL, 0)) < 0) {
        ALOGW("Failed to create specified HW device.\n");
        return;
    }
    avcodec_ctx->hw_device_ctx = av_buffer_ref(m_hw_dev_ctx);

    m_hw_accel_valid = true;
}

HWAccelContext::~HWAccelContext() {
    av_buffer_unref(&m_hw_dev_ctx);
    m_hw_accel_valid = false;
}

void HWAccelContextDeleter::operator()(HWAccelContext *p) { delete p; }

//////// @class CGVideoDecoder ////////

CGVideoDecoder::~CGVideoDecoder() { destroy(); }

bool CGVideoDecoder::can_decode() const { return decoder_ready; }

int CGVideoDecoder::init(android::socket::FrameResolution resolution, uint32_t codec_type,
                         const char *device_name, int extra_hw_frames) {
    ALOGVV("%s E", __func__);
    std::lock_guard<std::recursive_mutex> decode_push_lock(push_lock);
    std::lock_guard<std::recursive_mutex> decode_pull_lock(pull_lock);
    decoder_ready = false;

    // Update current init parameters which would be used during re-init.
    this->codec_type = codec_type;
    this->resolution = resolution;
    this->device_name = device_name;

    m_decode_ctx = CGDecContex(new DecodeContext(int(codec_type), int(resolution)));

    AVCodecID codec_id = (codec_type == int(android::socket::VideoCodecType::kH265))
                             ? AV_CODEC_ID_H265
                             : AV_CODEC_ID_H264;

    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (codec == nullptr) {
        ALOGW("Codec id:%d not found!", codec_id);
        return -1;
    }

    AVCodecParserContext *parser = av_parser_init(codec->id);
    if (parser == nullptr) {
        ALOGW("Parser not found!");
        return -1;
    }

    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (c == nullptr) {
        ALOGW("Could not allocate video codec context\n");
        av_parser_close(parser);
        return -1;
    }

    if (device_name != nullptr) {
        m_hw_accel_ctx =
            CGHWAccelContex(new HWAccelContext(codec, c, device_name, extra_hw_frames));
        if (m_hw_accel_ctx->is_hw_accel_valid()) {
            ALOGI("%s Use device %s to accelerate decoding!", __func__, device_name);
        } else {
            ALOGW("%s System doesn't support VAAPI(Video Acceleration API). SW Decoding is used.!",
                  __func__);
        }
    }

    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) {
        ALOGW("Could not allocate packet\n");
        av_parser_close(parser);
        avcodec_free_context(&c);
        return -1;
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        ALOGW("Could not open codec\n");
        av_parser_close(parser);
        avcodec_free_context(&c);
        av_packet_free(&pkt);
        return -1;
    }

    m_decode_ctx->parser = parser;
    m_decode_ctx->avcodec_ctx = c;
    m_decode_ctx->packet = pkt;
    decoder_ready = true;
    return 0;
}

int CGVideoDecoder::decode(const uint8_t *data, int data_size) {
    ALOGVV("%s E", __func__);
    std::lock_guard<std::recursive_mutex> decode_access_lock(push_lock);
    if (!can_decode()) {
        ALOGE("%s Decoder not initialized", __func__);
        return -1;
    }
    if (data == nullptr || data_size <= 0) {
        ALOGE("%s Invalid args: m_decode_ctx: %p, data: %p, data_size: %d", __func__,
              m_decode_ctx.get(), data, data_size);
        return -1;
    }

    AVPacket *pkt = m_decode_ctx->packet;
    AVCodecParserContext *parser = m_decode_ctx->parser;

    while (data_size > 0) {
        ALOGVV("%s data_size: %d\n", __func__, data_size);
        int ret = av_parser_parse2(parser, m_decode_ctx->avcodec_ctx, &pkt->data, &pkt->size, data,
                                   data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            ALOGW("%s Error while parsing\n", __func__);
            return -1;
        } else {
            ALOGVV("%s av_parser_parse2 returned %d pkt->size: %d\n", __func__, ret, pkt->size);
        }

        data += ret;
        data_size -= ret;

        if (pkt->size) {
            if (decode_one_frame(pkt) == AVERROR_INVALIDDATA) {
                ALOGI("%s re-init", __func__);
                flush_decoder();
                destroy();
                if (init((android::socket::FrameResolution)this->resolution, this->codec_type,
                         this->device_name, 0) < 0) {
                    ALOGE("%s re-init failed. %s decoding", __func__, device_name);
                    return -1;
                } else {
                    pkt = m_decode_ctx->packet;
                    parser = m_decode_ctx->parser;
                    continue;
                }
            }
        }
    }

    ALOGVV("%s X", __func__);
    return 0;
}

int CGVideoDecoder::decode_one_frame(const AVPacket *pkt) {
    ALOGVV("%s E", __func__);
    AVCodecContext *c = m_decode_ctx->avcodec_ctx;

    int sent = avcodec_send_packet(c, pkt);
    if (sent < 0) {
        ALOGE("%s Error sending a packet for decoding: %s\n", __func__, av_err2str(sent));
        return sent;
    }

    int decode_stat = 0;
    AVFrame *frame = nullptr;
    while (decode_stat >= 0) {
        if (frame == nullptr) {
            frame = av_frame_alloc();
            if (frame == nullptr) {
                ALOGW("Could not allocate video frame\n");
                return -1;
            }
        }
        decode_stat = avcodec_receive_frame(c, frame);
        if (decode_stat == AVERROR(EAGAIN) || decode_stat == AVERROR_EOF) {
            ALOGVV("%s avcodec_receive_frame returned: %s\n", __func__, av_err2str(decode_stat));
            break;
        } else if (decode_stat < 0) {
            ALOGW("Error during decoding\n");
            av_frame_free(&frame);
            return -1;
        }
        // video info sanity check
        if (frame->width != m_decode_ctx->resolution.first ||
            frame->height != m_decode_ctx->resolution.second ||
            (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_VAAPI)) {
            ALOGW("%s: Camera input res from client is %dx%d, but decoder initialized with %dx%d",
                  __func__, frame->width, frame->height, m_decode_ctx->resolution.first,
                  m_decode_ctx->resolution.second);
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_VAAPI)
                ALOGW("%s: Camera input frame format %d is not matching with Decoder format",
                      __func__, frame->format);
            av_frame_free(&frame);
            return -1;
        }

        if (m_hw_accel_ctx.get() && m_hw_accel_ctx->is_hw_accel_valid()) {
            AVFrame *sw_frame = av_frame_alloc();
            if (sw_frame == nullptr) {
                ALOGW("Could not allocate video frame\n");
                return -1;
            }

            if (frame->format != m_hw_accel_ctx->get_hw_pixel_format()) {
                ALOGW("Decoder HW format mismatch\n");
                return -1;
            }

            /* retrieve data from GPU to CPU */
            int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret < 0) {
                ALOGE("Error transferring the data to system memory: %s\n", av_err2str(ret));
                return -1;
            }

            av_frame_free(&frame);
            frame = sw_frame;
        } else
            ALOGVV("%s Camera VHAL uses SW decoding", __func__);

        // push decoded frame
        {
            std::lock_guard<std::mutex> lock(m_decode_ctx->mutex_);
            m_decode_ctx->decoded_frames.push_back(frame);
            frame = nullptr;
        }
    }

    av_frame_free(&frame);
    ALOGVV("%s X", __func__);
    return 0;
}

int CGVideoDecoder::get_decoded_frame(CGVideoFrame::Ptr cg_frame) {
    std::lock_guard<std::recursive_mutex> decode_access_lock(pull_lock);
    if (!can_decode()) {
        ALOGE("%s Decoder not initialized", __func__);
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_decode_ctx->mutex_);

    if (m_decode_ctx->decoded_frames.empty()) return -1;

    while (m_decode_ctx->decoded_frames.size() > MAX_ALLOWED_PENDING_FRAMES) {
        auto it = m_decode_ctx->decoded_frames.begin();
        AVFrame *frame = *it;
        av_frame_free(&frame);
        m_decode_ctx->decoded_frames.erase(it);
    }
    // return the frame in the front
    auto it = m_decode_ctx->decoded_frames.begin();
    AVFrame *frame = *it;
    cg_frame->ref_frame(frame);
    av_frame_free(&frame);
    m_decode_ctx->decoded_frames.erase(it);

    return 0;
}

/* flush the decoder */
int CGVideoDecoder::flush_decoder() {
    std::lock_guard<std::recursive_mutex> decode_push_lock(push_lock);
    AVCodecContext *c = m_decode_ctx->avcodec_ctx;
    AVPacket *packet = m_decode_ctx->packet;

    packet->data = NULL;
    packet->size = 0;
    packet->buf = NULL;
    packet->side_data = NULL;

    int sent = avcodec_send_packet(c, packet);
    if (sent < 0) {
        ALOGW("%s Error sending a flush packet to decoder", __func__);
        return -1;
    }
    ALOGVV("%s Successfully sent flush packet to decoder: %d", __func__, sent);
    return 0;
}

int CGVideoDecoder::destroy() {
    std::lock_guard<std::recursive_mutex> decode_push_lock(push_lock);
    std::lock_guard<std::recursive_mutex> decode_pull_lock(pull_lock);
    decoder_ready = false;
    if (m_decode_ctx == nullptr)
        return 0;
    av_parser_close(m_decode_ctx->parser);
    avcodec_free_context(&m_decode_ctx->avcodec_ctx);
    av_packet_free(&m_decode_ctx->packet);

    if (!m_decode_ctx->decoded_frames.empty()) {
        std::lock_guard<std::mutex> lock(m_decode_ctx->mutex_);
        for (auto frame : m_decode_ctx->decoded_frames) {
            av_frame_free(&frame);
        }
        m_decode_ctx->decoded_frames.clear();
    }
    m_hw_accel_ctx.reset();
    m_decode_ctx.reset();

    return 0;
}
