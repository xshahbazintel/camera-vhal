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

#include <iostream>
#include "video_sink.h"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace vhal::client;
using namespace std;

class CameraClient {
private:
    bool is_running;
public :
    int startDummyStreamer();
    void stopDummyStreamer();
    bool IsConnected();
    void RequestCameraCapability();
    void sendTwoCameraConfig();
    void sendOneCameraConfig();
    void sendCameraConfig();
    void sendMultipleCameraConfig();
    void MissingCodecTypeInCameraInfo();
    void MissingResolutionInCameraInfo();
    void MissingFacingInCameraInfo();
    void MissingSensorOrientationInCameraInfo();
    void AllInfoMissingInCameraInfo();

    int instance_id = 10000;
    shared_ptr<VideoSink> video_sink;
};
