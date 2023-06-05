/*
 * Copyright (C) 2023 Intel Corporation
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

#define LOG_TAG "CapabilitiesHelper"
#include <CapabilitiesHelper.h>
#include "CameraSocketCommand.h"
#include <log/log.h>

namespace android {

using namespace socket;

bool CapabilitiesHelper::IsCodecTypeValid(uint32_t CodecType) {
    if ((CodecType == uint32_t(VideoCodecType::kH264))
          || (CodecType == uint32_t(VideoCodecType::kH265))
          || (CodecType == uint32_t(VideoCodecType::kAV1))) {
        return true;
    } else
        return false;
}

bool CapabilitiesHelper::IsResolutionValid(uint32_t Resolution) {
    if ((Resolution == uint32_t(FrameResolution::k480p))
          || (Resolution == uint32_t(FrameResolution::k720p))
          || (Resolution == uint32_t(FrameResolution::k1080p))) {
        return true;
    } else
        return false;
}

bool CapabilitiesHelper::IsSensorOrientationValid(uint32_t Orientation) {
    if ((Orientation == uint32_t(SensorOrientation::ORIENTATION_0))
          || (Orientation == uint32_t(SensorOrientation::ORIENTATION_90))
          || (Orientation == uint32_t(SensorOrientation::ORIENTATION_180))
          || (Orientation == uint32_t(SensorOrientation::ORIENTATION_270))) {
        return true;
    } else
        return false;
}

bool CapabilitiesHelper::IsCameraFacingValid(uint32_t Facing) {
    if ((Facing == uint32_t(CameraFacing::BACK_FACING))
          || (Facing == uint32_t(CameraFacing::FRONT_FACING))) {
        return true;
    } else
        return false;
}

} //namespace android
