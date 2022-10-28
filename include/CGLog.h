/*
** Copyright (C) 2022 Intel Corporation
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

#ifndef CG_LOG_H
#define CG_LOG_H
#include <errno.h>

#ifndef LOG_TAG
#define LOG_TAG "CG_LOG"
#endif

#ifdef HOST_BUILD

#define ALOGD(...)       \
    printf("[DEBUG]");   \
    printf(LOG_TAG);     \
    printf(__VA_ARGS__); \
    printf("\n");
#define ALOGI(...)       \
    printf("[INFO] ");   \
    printf(LOG_TAG);     \
    printf(__VA_ARGS__); \
    printf("\n");
#define ALOGW(...)       \
    printf("[WARN] ");   \
    printf(LOG_TAG);     \
    printf(__VA_ARGS__); \
    printf("\n");
#define ALOGE(...)       \
    printf("[ERROR]");   \
    printf(LOG_TAG);     \
    printf(__VA_ARGS__); \
    printf("\n");

#else
#include <android/log.h>
#undef ALOGD
#undef ALOGE
#undef ALOGI
#undef ALOGW
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__);
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__);
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__);
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__);

#endif  // HOST_BUILD

#endif  // CG_LOG_H
