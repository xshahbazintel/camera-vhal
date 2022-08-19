/*
 * Copyright (C) 2011 The Android Open Source Project
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
 */

/*
 * Contains implementation of the camera HAL layer in the system running
 * under the emulator.
 *
 * This file contains only required HAL header, which directs all the API calls
 * to the VirtualCameraFactory class implementation, wich is responsible for
 * managing virtual cameras.
 */

#include "VirtualCameraFactory.h"

/*
 * Required HAL header.
 */
camera_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            //the camera module api version is changed to 2.4 as the android expects the
            //api version to be 2.4 and higher for android version greater than Q
            // this fix was added as part of VTS cases execution
            //.module_api_version = CAMERA_MODULE_API_VERSION_2_3,
            .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = CAMERA_HARDWARE_MODULE_ID,
            .name = "Virtual Camera Module",
            .author = "The Android Open Source Project",
            .methods = &android::VirtualCameraFactory::mCameraModuleMethods,
            .dso = NULL,
            .reserved = {0},
        },
    .get_number_of_cameras = android::VirtualCameraFactory::get_number_of_cameras,
    .get_camera_info = android::VirtualCameraFactory::get_camera_info,
    .set_torch_mode = android::VirtualCameraFactory::set_torch_mode,
    .set_callbacks = android::VirtualCameraFactory::set_callbacks,
    .get_vendor_tag_ops = android::VirtualCameraFactory::get_vendor_tag_ops,
    .open_legacy = android::VirtualCameraFactory::open_legacy};
