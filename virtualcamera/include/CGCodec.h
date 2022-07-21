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

#ifndef CG_CODEC_H
#define CG_CODEC_H

#include <fstream>
#include <memory>
#include <stdlib.h>
#include <stdint.h>
#include "CameraSocketCommand.h"
extern "C" {
#include "libavutil/frame.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
}

using namespace std;

/*! @class: CGVideoFrame wraps ffmpeg AVFrame by shared pointer */

enum CGPixelFormat { I420 = 0, NV12 = 1 };

class CGVideoFrame {
public:
    using Ptr = std::shared_ptr<CGVideoFrame>;

    CGVideoFrame() { m_avframe = av_frame_alloc(); }

    virtual ~CGVideoFrame() { av_frame_free(&m_avframe); }

    int ref_frame(const AVFrame *frame) { return av_frame_ref(m_avframe, frame); }

    uint8_t *data(size_t plane) { return m_avframe->data[plane]; }

    int linesize(size_t plane) { return m_avframe->linesize[plane]; }

    int width() { return m_avframe->width; }

    int height() { return m_avframe->height; }

    AVFrame *av_frame() { return m_avframe; }

    CGPixelFormat format();

    /*
     * Copy frame data to data block buffer
     * @param buffer[out]: destination buffer, should be freed by caller
     * @param size[out]: output buffer size
     * */
    int copy_to_buffer(uint8_t **buffer /* out */, int *size /* out */);

    int copy_to_buffer(uint8_t *buffer /* out */, int *size /* out */);

private:
    AVFrame *m_avframe;
};

/*! @class: CGVideoDecoder wraps ffmpeg avcodec for video ES decoding */

struct DecodeContext;
struct DecodeContextDeleter {
    void operator()(DecodeContext *p);
};

struct HWAccelContext;
struct HWAccelContextDeleter {
    void operator()(HWAccelContext *p);
};

typedef std::unique_ptr<DecodeContext, DecodeContextDeleter> CGDecContex;
typedef std::unique_ptr<HWAccelContext, HWAccelContextDeleter> CGHWAccelContex;

/**
 * Required number of additionally allocated bytes at the end of the input bitstream for decoding.
 * This is mainly needed because some optimized bitstream readers read
 * 32 or 64 bit at once and could read over the end.<br>
 * Note: If the first 23 bits of the additional bytes are not 0, then damaged
 * MPEG bitstreams could cause overread and segfault.
 */
#define CG_INPUT_BUFFER_PADDING_SIZE 64

class CGVideoDecoder {
public:
    CGVideoDecoder(){};
    CGVideoDecoder(int codec_type, int resolution_type, const char *device_name = nullptr,
                   int extra_hw_frames = 0);
    virtual ~CGVideoDecoder();

    /**
     * Checks whether decoder init was successful. If true, then decoding
     * apis can be called, otherwise not.
     */
    bool can_decode() const;

    /**
     * Initialize the CGVideoDecoder
     * @param codec_type        see @enum camera_video_codec_t in @file cg_protocol.h
     * @param resolution_type   see @enum camera_video_resolution_t in @file cg_protocol.h
     * @param device_name       the string of hardware acclerator device, such as "vaapi"
     * @param extra_hw_frames   allocate extra frames for hardware acclerator when decoding
     */
    int init(android::socket::VideoCodecType codec_type, android::socket::FrameResolution resolution_type, const char *device_name = nullptr,
             int extra_hw_frames = 0);

    /**
     * Send a piece of ES stream data to decoder, the data must have a padding with a lengh
     * of CG_INPUT_BUFFER_PADDING_SIZE
     * @param data      input buffer
     * @param length    buffer size in bytes without the padding. I.e. the full buffer
     *                  size is assumed to be buf_size + CG_INPUT_BUFFER_PADDING_SIZE.
     */
    int decode(const uint8_t *data, int length);

    /**
     * Get one decoded video frame
     * @param cg_frame  a shared pointer @class CGVideoFrame which wrap ffmpeg av_frame as the
     * output
     */
    int get_decoded_frame(CGVideoFrame::Ptr cg_frame);

    /**
     * @brief Send flush packet to decoder, indicating end of decoding session.
     *
     * @return Returns 0 if decoder acknowledged Flush packet, non-zero if errored.
     */
    int flush_decoder();

    /**
     * @brief Desotroy cg decoder context and ongoing decode requests.
     *
     * @return int
     */
    int destroy();

private:
    CGDecContex m_decode_ctx;        ///<! cg decoder internal context
    CGHWAccelContex m_hw_accel_ctx;  ///<! hw decoding accelerator context
    int decode_one_frame(const AVPacket *pkt);
    bool init_failed_ = false;

    CGVideoDecoder(const CGVideoDecoder &cg_video_decoder);
    CGVideoDecoder &operator=(const CGVideoDecoder &) { return *this; }
};

#endif  // CG_CODEC_H
