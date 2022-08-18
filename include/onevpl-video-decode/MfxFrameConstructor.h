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
 */

#ifndef MFX_FRAME_CONSTRUCTOR_H
#define MFX_FRAME_CONSTRUCTOR_H

#include <memory>
#include <vector>
#include <map>
#include <log/log.h>
#include <mfxvideo++.h>

#define MFX_MAX(A, B) (((A) > (B)) ? (A) : (B))
#define MFX_MIN(A, B) (((A) < (B)) ? (A) : (B))

enum MfxBitstreamState
{
    MfxBS_HeaderAwaiting = 0,
    MfxBS_HeaderCollecting = 1,
    MfxBS_HeaderWaitSei = 2,
    MfxBS_HeaderObtained = 3,
    MfxBS_Resetting = 4,
};

class MfxFrameConstructor
{
public:
    MfxFrameConstructor();
    ~MfxFrameConstructor();

    // Loads data
    mfxStatus Load(const mfxU8* data, mfxU32 size, mfxU64 pts, bool is_header_available, bool is_complete_frame);
    // unloads previously sent buffer, copy data to internal buffer if needed
    mfxStatus Unload();
    // gets bitstream with data
    std::shared_ptr<mfxBitstream> GetMfxBitstream();

private:
    // Loads header.
    mfxStatus loadHeader(const mfxU8* data, mfxU32 size, bool is_header_available);
    // increase buffer capacity with saving of buffer content (realloc)
    mfxStatus bufferRealloc(mfxU32 add_size);
    // increase buffer capacity without saving of buffer content (free/malloc)
    mfxStatus bufferAlloc (mfxU32 new_size);
    // cleaning up of internal buffers
    mfxStatus clearBuffer();

    MfxBitstreamState mBsState;

    // pointer to current bitstream
    std::shared_ptr<mfxBitstream> mBsCurrent;
    // saved stream header to be returned after seek if no new header will be found
    std::shared_ptr<mfxBitstream> mBsHeader;
    // buffered data: seq header or remained from previos sample
    std::shared_ptr<mfxBitstream> mBsBuffer;
    // data from input sample (case when buffering and copying is not needed)
    std::shared_ptr<mfxBitstream> mBsIn;

    // EOS flag
    bool mBsEos;

    // some statistics:
    mfxU32 mBufferReallocs;
    mfxU32 mBstBufCopyBytes;

};

#endif /* MFX_FRAME_CONSTRUCTOR_H */
