/**
 * @file CameraSocketCommand.h
 * @author Shakthi Prashanth M (shakthi.prashanth.m@intel.com)
 * @brief  Implementation of protocol between camera vhal and cloud client such
 *         as streamer or cg-proxy.
 * @version 0.1
 * @date 2021-02-15
 *
 * Copyright (c) 2021 Intel Corporation
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
 */

#ifndef CAMERA_SOCKET_COMMAND_H
#define CAMERA_SOCKET_COMMAND_H

#include <cstdint>
#include <unordered_map>
#include <string>

namespace android {

namespace socket {

enum class VideoCodecType { kH264 = 0 };
enum class FrameResolution { k480p = 0, k720p, k1080p };

struct CameraFrameInfo {
    VideoCodecType codec_type = VideoCodecType::kH264;
    FrameResolution resolution = FrameResolution::k480p;
    uint32_t reserved[4];
};

enum class CameraOperation { kOpen = 11, kClose = 12, kNone = 13 };

enum class CameraSessionState {
    kNone,
    kCameraOpened,
    kCameraClosed,
    kDecodingStarted,
    kDecodingStopped
};

extern const std::unordered_map<CameraSessionState, std::string> kCameraSessionStateNames;

enum class CameraVHalVersion {
    kV1 = 0,  // decode out of camera vhal
    kV2 = 1,  // decode in camera vhal
};

// has default values.
struct CameraConfig {
    CameraVHalVersion version = CameraVHalVersion::kV2;
    CameraOperation operation = CameraOperation::kNone;
    CameraFrameInfo frame_info;
};
}  // namespace socket
}  // namespace android

#endif /* CAMERA_SOCKET_COMMAND_H */
