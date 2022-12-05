/**
 *
 * Copyright (c) 2022 Intel Corporation
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

//#define LOG_NDEBUG 0

#include <iomanip>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <fstream>
#include "onevpl-video-decode/MfxFrameConstructor.h"

using namespace std;

MfxFrameConstructor::MfxFrameConstructor()
    : mBsState(MfxBS_HeaderAwaiting),
      mBsEos(false),
      mBufferReallocs(0),
      mBstBufCopyBytes(0) {
    mBsHeader = std::make_shared<mfxBitstream>();
    mBsBuffer = std::make_shared<mfxBitstream>();
    mBsIn = std::make_shared<mfxBitstream>();

    memset(&(*mBsHeader), 0, sizeof(*mBsHeader));
    memset(&(*mBsBuffer), 0, sizeof(*mBsBuffer));
    memset(&(*mBsIn), 0, sizeof(*mBsIn));
}

MfxFrameConstructor::~MfxFrameConstructor() {
    if (mBsBuffer->Data) {
        free(mBsBuffer->Data);
        mBsBuffer->Data = nullptr;
    }

    free(mBsHeader->Data);
    mBsHeader->Data = nullptr;
}

mfxStatus MfxFrameConstructor::loadHeader(const mfxU8* data, mfxU32 size, bool is_header_available) {
    mfxStatus mfx_res = MFX_ERR_NONE;

    if (!data || !size) mfx_res = MFX_ERR_NULL_PTR;
    if (MFX_ERR_NONE == mfx_res) {
        if (is_header_available) {
            // if new header arrived after reset we are ignoring previously collected header data
            if (mBsState == MfxBS_Resetting) {
                mBsState = MfxBS_HeaderObtained;
            } else if (size) {
                mfxU32 needed_MaxLength = 0;
                mfxU8* new_data = nullptr;

                needed_MaxLength = mBsHeader->DataOffset + mBsHeader->DataLength + size; // offset should be 0
                if (mBsHeader->MaxLength < needed_MaxLength) {
                    // increasing buffer capacity if needed
                    new_data = (mfxU8*)realloc(mBsHeader->Data, needed_MaxLength);
                    if (new_data) {
                        // setting new values
                        mBsHeader->Data = new_data;
                        mBsHeader->MaxLength = needed_MaxLength;
                    } else {
                        mfx_res = MFX_ERR_MEMORY_ALLOC;
                    }
                }
                if (MFX_ERR_NONE == mfx_res && new_data) {
                    mfxU8* buf = mBsHeader->Data + mBsHeader->DataOffset + mBsHeader->DataLength;

                    std::copy(data, data + size, buf);
                    mBsHeader->DataLength += size;
                }
                if (MfxBS_HeaderAwaiting == mBsState) mBsState = MfxBS_HeaderCollecting;
            }
        } else {
            // We have generic data. In case we are in Resetting state (i.e. seek mode)
            // we attach header to the bitstream, other wise we are moving in Obtained state.
            if (MfxBS_HeaderCollecting == mBsState) {
                // As soon as we are receving first non header data we are stopping collecting header
                mBsState = MfxBS_HeaderObtained;
            }
            else if (MfxBS_Resetting == mBsState) {
                // if reset detected and we have header data buffered - we are going to load it
                mfx_res = bufferRealloc(mBsHeader->DataLength);
                if (MFX_ERR_NONE == mfx_res) {
                    mfxU8* buf = mBsBuffer->Data + mBsBuffer->DataOffset + mBsBuffer->DataLength;

                    std::copy(mBsHeader->Data + mBsHeader->DataOffset,
                        mBsHeader->Data + mBsHeader->DataOffset + mBsHeader->DataLength, buf);
                    mBsBuffer->DataLength += mBsHeader->DataLength;
                    mBstBufCopyBytes += mBsHeader->DataLength;
                }
                mBsState = MfxBS_HeaderObtained;
            }
        }
    }
    return mfx_res;
}

mfxStatus MfxFrameConstructor::Load(const mfxU8* data, mfxU32 size, mfxU64 pts,
                                           bool is_header_available, bool is_complete_frame) {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_res = MFX_ERR_NONE;

    if (!data) {
        mfx_res = MFX_ERR_NULL_PTR;
        return mfx_res;
    }

    if (size == 0) {
        mfx_res = MFX_ERR_UNKNOWN;
        return mfx_res;
    }

    mfx_res = loadHeader(data, size, is_header_available);
    if ((MFX_ERR_NONE == mfx_res) && mBsBuffer->DataLength) {
        mfx_res = bufferRealloc(size);
        if (MFX_ERR_NONE == mfx_res) {
            mfxU8* buf = mBsBuffer->Data + mBsBuffer->DataOffset + mBsBuffer->DataLength;

            std::copy(data, data + size, buf);
            mBsBuffer->DataLength += size;
            mBstBufCopyBytes += size;
        }
    }

    if (MFX_ERR_NONE == mfx_res) {
        if (mBsBuffer->DataLength)
            mBsCurrent = mBsBuffer;
        else {
            mBsIn->Data = (mfxU8*)data;
            mBsIn->DataOffset = 0;
            mBsIn->DataLength = size;
            mBsIn->MaxLength = size;
            if (is_complete_frame)
                mBsIn->DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME;

            mBsCurrent = mBsIn;
        }
        mBsCurrent->TimeStamp = pts;
    } else
        mBsCurrent = nullptr;

    return mfx_res;
}

mfxStatus MfxFrameConstructor::Unload() {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_res = MFX_ERR_NONE;

    mfx_res = clearBuffer();

    return mfx_res;
}

mfxStatus MfxFrameConstructor::bufferRealloc(mfxU32 add_size) {
    mfxStatus mfx_res = MFX_ERR_NONE;
    mfxU32 needed_MaxLength = 0;
    mfxU8* new_data = nullptr;

    if (add_size) {
        needed_MaxLength = mBsBuffer->DataOffset + mBsBuffer->DataLength + add_size; // offset should be 0
        if (mBsBuffer->MaxLength < needed_MaxLength) {
            // increasing buffer capacity if needed
            new_data = (mfxU8*)realloc(mBsBuffer->Data, needed_MaxLength * 2);
            if (new_data) {
                // collecting statistics
                ++mBufferReallocs;
                if (new_data != mBsBuffer->Data) mBstBufCopyBytes += mBsBuffer->MaxLength;
                // setting new values
                mBsBuffer->Data = new_data;
                mBsBuffer->MaxLength = needed_MaxLength;
            }
            else mfx_res = MFX_ERR_MEMORY_ALLOC;
        }
    }
    return mfx_res;
}

mfxStatus MfxFrameConstructor::bufferAlloc(mfxU32 new_size) {
    mfxStatus mfx_res = MFX_ERR_NONE;
    mfxU32 needed_MaxLength = 0;

    if (new_size) {
        needed_MaxLength = new_size;
        if (mBsBuffer->MaxLength < needed_MaxLength) {
            // increasing buffer capacity if needed
            free(mBsBuffer->Data);
            mBsBuffer->Data = nullptr;
            mBsBuffer->Data = (mfxU8*)malloc(needed_MaxLength * 2);
            mBsBuffer->MaxLength = needed_MaxLength;
            ++mBufferReallocs;
        }
        if (!(mBsBuffer->Data)) {
            mBsBuffer->MaxLength = 0;
            mfx_res = MFX_ERR_MEMORY_ALLOC;
        }
    }
    return mfx_res;
}

mfxStatus MfxFrameConstructor::clearBuffer() {
    mfxStatus mfx_res = MFX_ERR_NONE;
    ALOGV("%s, mBsBuffer->DataLength=%d,mBsIn->DataLength=%d", __func__,
          mBsBuffer->DataLength, mBsIn->DataLength);
    ALOGV("%s, mBsBuffer->DataOffset=%d,mBsIn->DataOffset=%d", __func__,
          mBsBuffer->DataOffset, mBsIn->DataOffset);

    if (nullptr != mBsCurrent) {
        if (mBsCurrent == mBsBuffer) {
            if (mBsBuffer->DataLength && mBsBuffer->DataOffset) {
                // shifting data to the beginning of the buffer
                memmove(mBsBuffer->Data, mBsBuffer->Data + mBsBuffer->DataOffset, mBsBuffer->DataLength);
                mBstBufCopyBytes += mBsBuffer->DataLength;
            }
            mBsBuffer->DataOffset = 0;
        }
        if ((mBsCurrent == mBsIn) && mBsIn->DataLength) {
            // copying data from mBsIn to bst_Buf
            // Note: we read data from mBsIn, thus here bst_Buf is empty
            mfx_res = bufferAlloc(mBsIn->DataLength);
            if (MFX_ERR_NONE == mfx_res) {
                std::copy(mBsIn->Data + mBsIn->DataOffset,
                    mBsIn->Data + mBsIn->DataOffset + mBsIn->DataLength, mBsBuffer->Data);
                mBsBuffer->DataOffset = 0;
                mBsBuffer->DataLength = mBsIn->DataLength;
                mBsBuffer->TimeStamp  = mBsIn->TimeStamp;
                mBsBuffer->DataFlag   = mBsIn->DataFlag;
                mBstBufCopyBytes += mBsIn->DataLength;
            }
            mBsIn = std::make_shared<mfxBitstream>();
            memset(&(*mBsIn), 0, sizeof(*mBsIn));
        }
        mBsCurrent = nullptr;
    }
    return mfx_res;
}

std::shared_ptr<mfxBitstream> MfxFrameConstructor::GetMfxBitstream() {
    std::shared_ptr<mfxBitstream> bst;
    ALOGV("%s: mBsBuffer->DataLength = %d, mBsIn->DataLength = %d", __func__,
          mBsBuffer->DataLength, mBsIn->DataLength);

    if (mBsBuffer->Data && mBsBuffer->DataLength) {
        bst = mBsBuffer;
    } else if (mBsIn->Data && mBsIn->DataLength) {
        bst = mBsIn;
    } else {
        bst = mBsBuffer;
    }

    return bst;
}
