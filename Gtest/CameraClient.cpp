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

#define LOG_TAG "CameraClient"
#include "CameraClient.h"
#include <log/log.h>

using namespace std::chrono_literals;
using namespace vhal::client;
using namespace std;
int CameraClient::startDummyStreamer()
{
    is_running = true;
    string socket_path("/ipc");
    UnixConnectionInfo conn_info = { socket_path, instance_id };
    try {
        video_sink = make_shared<VideoSink>(conn_info,
          [&](const VideoSink::camera_config_cmd_t& ctrl_msg) {
            switch (ctrl_msg.cmd) {
                case VideoSink::camera_cmd_t::CMD_OPEN: {
                    ALOGI("%s: Received Open command from Camera VHal", __FUNCTION__);
                    break;
                }
                case VideoSink::camera_cmd_t::CMD_CLOSE:
                    ALOGI("%s: Received Close command from Camera VHal", __FUNCTION__);
                    exit(0);
                default:
                    ALOGI("%s: Unknown Command received, exiting with failure", __FUNCTION__);
                    exit(1);
            }
        });

    } catch (const std::exception& ex) {
        ALOGI("%s: VideoSink creation error : %s", __FUNCTION__,ex.what());
        exit(1);
    }

    ALOGI("%s: Waiting Camera Open callback..", __FUNCTION__);

    while (!video_sink->IsConnected())
        this_thread::sleep_for(100ms);

    // we need to be alive :)
    while (is_running) {
        this_thread::sleep_for(100ms);
    }
    return 0;
}

bool CameraClient::IsConnected() {
    return video_sink->IsConnected();
}

void CameraClient::sendCameraConfig() {
    int noOfCameras = 0;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::MissingCodecTypeInCameraInfo() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].resolution = VideoSink::FrameResolution::k1080p;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
        camera_info[i].facing = VideoSink::CameraFacing::BACK_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::MissingResolutionInCameraInfo() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH264;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
        camera_info[i].facing = VideoSink::CameraFacing::BACK_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::MissingFacingInCameraInfo() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH264;
        camera_info[i].resolution = VideoSink::FrameResolution::k720p;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::MissingSensorOrientationInCameraInfo() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH265;
        camera_info[i].resolution = VideoSink::FrameResolution::k1080p;
        camera_info[i].facing = VideoSink::CameraFacing::BACK_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::AllInfoMissingInCameraInfo() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}


void CameraClient::RequestCameraCapability() {
    if (IsConnected()) {
        ALOGI("%s: Calling GetCameraCapabilty..", __FUNCTION__);
        video_sink->GetCameraCapabilty();
    }
}

void CameraClient::sendOneCameraConfig() {
    int noOfCameras = 1;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH264;
        camera_info[i].resolution = VideoSink::FrameResolution::k1080p;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
        camera_info[i].facing = VideoSink::CameraFacing::BACK_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::sendTwoCameraConfig() {
    int noOfCameras = 2;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH264;
        camera_info[i].resolution = (i==0) ? VideoSink::FrameResolution::k1080p
              : VideoSink::FrameResolution::k720p;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
        camera_info[i].facing = (i == 0) ? VideoSink::CameraFacing::BACK_FACING
              : VideoSink::CameraFacing::FRONT_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::sendMultipleCameraConfig() {
    int noOfCameras = 4;
    std::vector<VideoSink::camera_info_t> camera_info(noOfCameras);
    for (int i = 0; i < noOfCameras; i++) {
        camera_info[i].cameraId = i;
        camera_info[i].codec_type = VideoSink::VideoCodecType::kH264;
        camera_info[i].resolution = (i==0) ? VideoSink::FrameResolution::k1080p
              : VideoSink::FrameResolution::k720p;
        camera_info[i].sensorOrientation = VideoSink::SensorOrientation::ORIENTATION_0;
        camera_info[i].facing = (i == 0) ? VideoSink::CameraFacing::BACK_FACING
              : VideoSink::CameraFacing::FRONT_FACING;
    }

    ALOGI("%s: Calling SetCameraCapabilty..", __FUNCTION__);
    video_sink->SetCameraCapabilty(camera_info);
}

void CameraClient::stopDummyStreamer() {
    is_running = false;
}
