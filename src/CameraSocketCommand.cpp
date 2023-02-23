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

#include "CameraSocketCommand.h"

namespace android {
namespace socket {
const std::unordered_map<CameraSessionState, std::string> kCameraSessionStateNames = {
    {CameraSessionState::kNone, "None"},
    {CameraSessionState::kCameraOpened, "Camera opened"},
    {CameraSessionState::kDecodingStarted, "Decoding started"},
    {CameraSessionState::kCameraClosed, "Camera closed"},
    {CameraSessionState::kDecodingStopped, "Decoding stopped"},
};

const char* camera_type_to_str(int type) {
    switch (type) {
        case REQUEST_CAPABILITY:
            return "REQUEST_CAPABILITY";
        case CAPABILITY:
            return "CAPABILITY";
        case CAMERA_CONFIG:
            return "CAMERA_CONFIG";
        case CAMERA_DATA:
            return "CAMERA_DATA";
        case ACK:
            return "ACK";
        default:
            return "invalid";
    }
}

const char* codec_type_to_str(uint32_t type) {
    switch (type) {
        case int(android::socket::VideoCodecType::kH264):
            return "H264";
        case int(android::socket::VideoCodecType::kH265):
            return "H265";
        case int(android::socket::VideoCodecType::kAV1):
            return "AV1";
        default:
            return "invalid";
    }
}

const char* resolution_to_str(uint32_t resolution) {
    switch (resolution) {
        case int(android::socket::FrameResolution::k480p):
            return "480p";
        case int(android::socket::FrameResolution::k720p):
            return "720p";
        case int(android::socket::FrameResolution::k1080p):
            return "1080p";
        default:
            return "invalid";
    }
}
}  // namespace socket
}  // namespace android
