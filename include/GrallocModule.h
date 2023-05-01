/*
 * Copyright (C) 2019-2022 Intel Corporation
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


#ifndef EMU_CAMERA_GRALLOC_MODULE_H
#define EMU_CAMERA_GRALLOC_MODULE_H

//#define LOG_NDEBUG 0
#undef ALOGVV
#if defined(LOG_NNDEBUG) && LOG_NNDEBUG == 0
#define ALOGVV ALOGV
#else
#define ALOGVV(...) ((void)0)
#endif

#include <hardware/gralloc.h>
#include <log/log.h>
#include <vector>

#include <android/hardware/graphics/mapper/4.0/IMapper.h>
#include <utils/StrongPointer.h>

using V4Error = android::hardware::graphics::mapper::V4_0::Error;
using V4Mapper = android::hardware::graphics::mapper::V4_0::IMapper;
using android::hardware::hidl_handle;

class GrallocModule {
public:
    static GrallocModule &getInstance() {
        static GrallocModule instance;
        return instance;
    }

    int lock(buffer_handle_t handle, int usage, int left, int top, int width, int height,
             void **vaddr) {
        if (m_gralloc4_mapper != nullptr) {
            native_handle_t* native_handle = const_cast<native_handle_t*>(handle);

            V4Mapper::Rect rect;
            rect.left = left;
            rect.top = top;
            rect.width = width;
            rect.height = height;

            hidl_handle empty_fence_handle;

            V4Error error;
            auto ret = m_gralloc4_mapper->lock(native_handle, usage, rect, empty_fence_handle,
                            [&](const auto& tmp_err, const auto& tmp_vaddr) {
                                error = tmp_err;
                                if (tmp_err == V4Error::NONE) {
                                    *vaddr = tmp_vaddr;
                                }
                            });
            return ret.isOk() && error == V4Error::NONE ? 0 : -1;
        }
        return -1;
    }

    int unlock(buffer_handle_t handle) {
        if (m_gralloc4_mapper != nullptr) {
            native_handle_t* native_handle = const_cast<native_handle_t*>(handle);

            V4Error error;
            auto ret = m_gralloc4_mapper->unlock(native_handle,
                          [&](const auto& tmp_err, const auto&) {
                              error = tmp_err;
                          });
            return ret.isOk() && error == V4Error::NONE ? 0 : -1;
        }
        return -1;
    }

    int import(buffer_handle_t handle, buffer_handle_t* imported_handle) {
        if (m_gralloc4_mapper != nullptr) {
            V4Error error;
            auto ret = m_gralloc4_mapper->importBuffer(handle,
                           [&](const auto& tmp_err, const auto& tmp_buf) {
                               error = tmp_err;
                               if (error == V4Error::NONE) {
                                   *imported_handle = static_cast<buffer_handle_t>(tmp_buf);
                               }
                           });
            return ret.isOk() && error == V4Error::NONE ? 0 : -1;
        }

        *imported_handle = handle;
        return 0;
    }

    int release(buffer_handle_t handle) {
        if (m_gralloc4_mapper != nullptr) {
            native_handle_t* native_handle = const_cast<native_handle_t*>(handle);
            return m_gralloc4_mapper->freeBuffer(native_handle).isOk() ? 0 : 1;
        }

        return 0;
    }

private:
    GrallocModule() {
        m_gralloc4_mapper = V4Mapper::getService();
    }
    android::sp<android::hardware::graphics::mapper::V4_0::IMapper> m_gralloc4_mapper;
};

#endif
