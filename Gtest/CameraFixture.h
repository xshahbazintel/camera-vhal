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

#define ONE_CAMERA_CLIENT 1
#define TWO_CAMERA_CLIENT 2
#define NO_CAMERA_PRESENT 0
#define INVALID_WAV 5
#define INVALID_ORIENTATION_360 360
#define INVALID_k2160p 5
#define FRONT_FACING_SECOND 2

#include <stdio.h>
#include <cstdlib>
#include <thread>
#include "gtest/gtest.h"
#include "CameraClient.h"
#include "CapabilitiesHelper.h"
#include "VirtualCameraFactory.h"

using namespace android;
class CameraFixture : public ::testing::Test
{
public:
    int client_id = 0;
    /*! Creates an instance of CameraFixture */
    CameraFixture();

    /*! Destroys an instance of CameraFixture */
    ~CameraFixture();

    virtual void SetUp();

    virtual void TearDown();

    std::thread* t1 = nullptr;
    CameraClient mCameraClient;
    CapabilitiesHelper mCapabilitiesHelper;

private:
    /*! Disabled Copy Constructor */
    CameraFixture(const CameraFixture&);

    /*! Disabled Assignment operator */
    CameraFixture& operator=(const CameraFixture&);

    void runStreamer();

protected:
    camera_module_callbacks_t* callbacks;
    static void test_camera_device_status_change(
      const struct camera_module_callbacks*, int /*camera_id*/,
      int /*new_status*/) {}

    static void test_torch_mode_status_change(
      const struct camera_module_callbacks*, const char* /*camera_id*/,
      int /*new_status*/) {}

    void SetCallback() {
        callbacks =
	      (camera_module_callbacks_t*)malloc(sizeof(camera_module_callbacks_t));
        callbacks->camera_device_status_change = test_camera_device_status_change;
        callbacks->torch_mode_status_change = test_torch_mode_status_change;
        gVirtualCameraFactory.set_callbacks(callbacks);
    }
    void DestroyCallbacks() {
        delete callbacks;
	callbacks = nullptr;
    }

};

