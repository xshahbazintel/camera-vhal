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

//#define LOG_NDEBUG 0

#include "onevpl-video-decode/MfxDecoder.h"

MfxDecoder::MfxDecoder() {
    ALOGV("%s", __func__);

    mDecImplementation = MFX_IMPL_AUTO_ANY | MFX_IMPL_VIA_ANY;

    mDecodeMemType = SYSTEM_MEMORY;
    mOutFrameSurface = nullptr;
    mOutSurfaceNum = 0;

    mMfxLoader = nullptr;
    mMfxDecSession = nullptr;

    mResWidth = 0;
    mResHeight = 0;

    mIsDecoderInitialized = false;

    memset(&mMfxVideoDecParams, 0, sizeof(mMfxVideoDecParams));
}

MfxDecoder::~MfxDecoder() {
    ALOGV("%s", __func__);
}

mfxStatus MfxDecoder::ResetSettings(uint32_t codec_type) {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_sts = MFX_ERR_NONE;
    memset(&mMfxVideoDecParams, 0, sizeof(mMfxVideoDecParams));

    // Update video decoder params.
    mMfxVideoDecParams.mfx.FrameInfo.BitDepthLuma = 8;
    mMfxVideoDecParams.mfx.FrameInfo.BitDepthChroma = 8;
    mMfxVideoDecParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    mMfxVideoDecParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    mMfxVideoDecParams.mfx.FrameInfo.Width = mResWidth;
    mMfxVideoDecParams.mfx.FrameInfo.Height = mResHeight;
    mMfxVideoDecParams.mfx.FrameInfo.CropX = 0;
    mMfxVideoDecParams.mfx.FrameInfo.CropY = 0;
    mMfxVideoDecParams.mfx.FrameInfo.CropW = mResWidth;
    mMfxVideoDecParams.mfx.FrameInfo.CropH = mResHeight;
    mMfxVideoDecParams.mfx.FrameInfo.FrameRateExtN = 30;
    mMfxVideoDecParams.mfx.FrameInfo.FrameRateExtD = 1;
    mMfxVideoDecParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

    mMfxVideoDecParams.AsyncDepth = 1;
    mMfxVideoDecParams.mfx.NumThread = 0;
    mMfxVideoDecParams.IOPattern = (mDecodeMemType == SYSTEM_MEMORY) ?
        MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    switch (codec_type) {
        case DECODER_H264:
            mMfxVideoDecParams.mfx.CodecId = MFX_CODEC_AVC;
            break;
        case DECODER_H265:
            mMfxVideoDecParams.mfx.CodecId = MFX_CODEC_HEVC;
            break;
        default:
            ALOGE("%s: unhandled codec type!", __func__);
            mfx_sts =  MFX_ERR_NOT_IMPLEMENTED;
            break;
    }

    // Create mfx frame constructor to make decoding bitstream.
    mMfxFrameConstructor = std::make_unique<MfxFrameConstructor>();

    return mfx_sts;
}

mfxStatus MfxDecoder::Init(uint32_t codec_type, uint32_t width, uint32_t height) {
    ALOGI("%s: codec_type = %d, width = %u, height = %u",
          __func__, codec_type, width, height);

    mfxStatus mfx_sts = MFX_ERR_NONE;
    uint32_t impl_index = 0;
    mfxConfig cfg[2];
    mfxVariant cfgVal[2];

    ClearFrameSurface();

    mResWidth = width;
    mResHeight = height;

    mfx_sts = ResetSettings(codec_type);
    if (mfx_sts != MFX_ERR_NONE) {
        ALOGE("%s: Unsupported codec type, failed to continue", __func__);
        return mfx_sts;
    }

    // Load the OneVPL dispatcher handler.
    mMfxLoader = MFXLoad();
    if (mMfxLoader == nullptr) {
        ALOGE("%s: MFXLoad failed.", __func__);
        return MFX_ERR_NULL_PTR;
    }

    // Creates the dispatcher internal configuration.
    cfg[0] = MFXCreateConfig(mMfxLoader);
    if (cfg[0] == nullptr) {
        ALOGE("%s: Failed to create cfg[0] MFX configuration", __func__);
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
        return MFX_ERR_UNKNOWN;
    }

    // Adds mfx config filter property.
    cfgVal[0].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[0].Data.U32 = (mDecImplementation == MFX_IMPL_SOFTWARE) ? MFX_IMPL_TYPE_SOFTWARE :
                                                                     MFX_IMPL_TYPE_HARDWARE;
    mfx_sts = MFXSetConfigFilterProperty(cfg[0], (const mfxU8 *) "mfxImplDescription.Impl",
                                         cfgVal[0]);
    if (mfx_sts != MFX_ERR_NONE) {
        ALOGE("%s: Failed to add cfgVal[0] mfx config filter property. ret = %d",
              __func__, mfx_sts);
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
        return mfx_sts;
    }

    // Creates another dispatcher internal configuration.
    cfg[1] = MFXCreateConfig(mMfxLoader);
    if (cfg[1] == nullptr) {
        ALOGE("%s: Failed to create cfg[1] MFX configuration", __func__);
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
        return MFX_ERR_UNKNOWN;
    }

    // Adds another mfx config filter property.
    cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = MFX_VERSION;
    mfx_sts = MFXSetConfigFilterProperty(cfg[1],
              (const mfxU8 *) "mfxImplDescription.ApiVersion.Version", cfgVal[1]);
    if (mfx_sts != MFX_ERR_NONE) {
        ALOGE("%s: Failed to add cfgVal[1] mfx config filter property. ret = %d",
              __func__, mfx_sts);
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
        return mfx_sts;
    }

    while (true) {
        // Enumerate all implementations until get a valid one.
        mfxImplDescription *idesc;
        mfx_sts = MFXEnumImplementations(mMfxLoader, impl_index,
                                         MFX_IMPLCAPS_IMPLDESCSTRUCTURE, (mfxHDL *)&idesc);

        if (mfx_sts == MFX_ERR_NOT_FOUND) {
            ALOGE("%s: Failed to find an available implementation", __func__);
            break;
        } else if (mfx_sts != MFX_ERR_NONE) {
            impl_index++;
            continue;
        }

        ALOGI("%s: OneVPL - impl_index = %d, API version: %d.%d, Implementation type: %s, "
              "Acceleration Mode: %s", __func__, impl_index, idesc->ApiVersion.Major,
              idesc->ApiVersion.Minor, (idesc->Impl == MFX_IMPL_TYPE_SOFTWARE) ?
              "SW" : "HW", (idesc->AccelerationMode == MFX_ACCEL_MODE_VIA_VAAPI) ?
              "Linux-VAAPI" : "Non-VAAPI");

	// Create mfx session.
        mfx_sts = MFXCreateSession(mMfxLoader, impl_index, &mMfxDecSession);

        MFXDispReleaseImplDescription(mMfxLoader, idesc);

        if (mfx_sts == MFX_ERR_NONE) {
            ALOGI("%s: Created mfx session successfully!", __func__);
            break;
        }

        impl_index++;
    }

    if (MFX_ERR_NONE != mfx_sts) {
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
        ALOGE("%s: Failed to create a mfx session. ret = %d", __func__, mfx_sts);
        return mfx_sts;
    }

    ALOGV("%s - X", __func__);
    return mfx_sts;
}

void MfxDecoder::ClearFrameSurface() {
    ALOGV("%s - E", __func__);

    std::lock_guard<std::mutex> lock(mMemMutex);
    mOutFrameSurfList.clear();

    if (mOutFrameSurface) {
        for (uint32_t i = 0; i < mOutSurfaceNum; i++) {
            if (mOutFrameSurface[i].Data.Y) {
                free(mOutFrameSurface[i].Data.Y);
                mOutFrameSurface[i].Data.Y = nullptr;
            }
        }
        free(mOutFrameSurface);
    }
    mOutFrameSurface = nullptr;
    mOutSurfaceNum = 0;
    ALOGV("%s - X", __func__);
}

void MfxDecoder::Release() {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_sts = MFX_ERR_NONE;

    // Terminates the current decoding operation and deallocates all memory.
    mfx_sts = MFXVideoDECODE_Close(mMfxDecSession);
    if (mfx_sts == MFX_ERR_NONE) {
        ALOGI("%s: Current decoding operation terminated successfully.", __func__);
    } else {
        ALOGE("%s: Current decoding couldn't be terminated. Failed.", __func__);
    }

    // Clear all the memories allocated.
    ClearFrameSurface();

    // Closing MFX session.
    MFXClose(mMfxDecSession);
    mMfxDecSession = nullptr;

    // Unload MFX loader.
    if (mMfxLoader) {
        MFXUnload(mMfxLoader);
        mMfxLoader = nullptr;
    }

    mIsDecoderInitialized = false;
    mResWidth = 0;
    mResHeight = 0;
    ALOGI("%s: Decoder closed and released successfully!", __func__);
}

uint32_t MfxDecoder::GetAvailableSurfaceIndex() {
    uint32_t index = 0xFF;
    for (uint32_t i = 0; i < mOutSurfaceNum; i++) {
        if (!mOutFrameSurface[i].Data.Locked) {
            index = i;
            break;
        }
    }
    ALOGV("%s, index = %d", __func__, index);

    return index;
}

void MfxDecoder::GetAvailableSurface(mfxFrameSurface1 **pWorkSurface) {
    std::lock_guard<std::mutex> lock(mMemMutex);

    uint32_t idx = GetAvailableSurfaceIndex();
    if (idx >= mOutSurfaceNum) {
        ALOGE("%s: Allocated buffer is full!", __func__);
        return;
    }

    *pWorkSurface = &mOutFrameSurface[idx];
    ALOGV("%s, pWorkSurface = %p", __func__, *pWorkSurface);
}

mfxStatus MfxDecoder::PrepareSurfaces() {
    ALOGV("%s: E", __func__);

    mfxStatus mfx_sts = MFX_ERR_NONE;

    mOutFrameSurface = (mfxFrameSurface1*)calloc(mOutSurfaceNum, sizeof(mfxFrameSurface1));
    if (mOutFrameSurface == NULL) {
        ALOGE("%s: memory allocation failed!", __func__);
        mfx_sts = MFX_ERR_MEMORY_ALLOC;
    } else {
        for (uint32_t i = 0; i < mOutSurfaceNum; i++) {
            uint8_t *pData = (uint8_t *)malloc(mResWidth * mResHeight * 2);
            if (pData == NULL) {
                ALOGE("%s: cannot allocate the surface data!", __func__);
                mfx_sts = MFX_ERR_MEMORY_ALLOC;
                break;
            }
            mOutFrameSurface[i].Data.Y = pData;
            mOutFrameSurface[i].Data.U = pData + mResWidth * mResHeight;
            mOutFrameSurface[i].Data.V = mOutFrameSurface[i].Data.U + 1;
            mOutFrameSurface[i].Data.MemType = MFX_MEMTYPE_SYSTEM_MEMORY;
            mOutFrameSurface[i].Data.PitchLow = ONEVPL_ALIGN32(mResWidth);
            mOutFrameSurface[i].Info = mMfxVideoDecParams.mfx.FrameInfo;
        }
    }

    return mfx_sts;
}

mfxStatus MfxDecoder::InitDecoder(mfxBitstream **bit_stream) {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_sts = MFX_ERR_NONE;

    // Parses the input bitstream and fills the m_mfxVideoParam structure.
    mfx_sts = MFXVideoDECODE_DecodeHeader(mMfxDecSession, *bit_stream,
                                          &mMfxVideoDecParams);
    ALOGV("%s, mfx_sts = %d", __func__, mfx_sts);

    if (mfx_sts == MFX_ERR_NULL_PTR) {
        mfx_sts = MFX_ERR_MORE_DATA;
    }

    if (mfx_sts == MFX_ERR_NONE) {
        // Query required surfaces number for decoder.
        mfxFrameAllocRequest decRequest = {};
        mfx_sts = MFXVideoDECODE_QueryIOSurf(mMfxDecSession, &mMfxVideoDecParams,
                                             &decRequest);
        if (mfx_sts == MFX_ERR_NONE) {
            mOutSurfaceNum = MFX_MAX(decRequest.NumFrameSuggested,
                                     MIN_NUMBER_OF_REQUIRED_FRAME_SURFACE);
            ALOGV("%s: decRequest.NumFrameSuggested = %d", __func__, decRequest.NumFrameSuggested);
            ALOGV("%s: decRequest.NumFrameMin = %d", __func__, decRequest.NumFrameMin);
            ALOGV("%s: mOutSurfaceNum = %d", __func__, mOutSurfaceNum);
        } else {
            ALOGE("QueryIOSurf failed");
            mfx_sts = MFX_ERR_UNKNOWN;
        }
    }

    if (mfx_sts == MFX_ERR_NONE && mDecodeMemType == SYSTEM_MEMORY) {
        mfx_sts = PrepareSurfaces();
        if (mfx_sts == MFX_ERR_NONE) {
            ALOGV("%s: PrepareSurfaces success!", __func__);
            mfx_sts = MFXVideoCORE_SetFrameAllocator(mMfxDecSession, nullptr);
            if (mfx_sts == MFX_ERR_NONE)
                ALOGV("%s: SetFrameAllocator success!", __func__);
            else
                ALOGE("%s: SetFrameAllocator failed", __func__);
        } else {
            ALOGE("%s: PrepareSurfaces failed", __func__);
        }
    }

    if (mfx_sts == MFX_ERR_NONE) {
	// Allocates memory and prepares tables and necessary structures for decoding.
        mfx_sts = MFXVideoDECODE_Init(mMfxDecSession, &mMfxVideoDecParams);

        if (mfx_sts == MFX_WRN_PARTIAL_ACCELERATION) {
            ALOGW("%s: [warning] returns MFX_WRN_PARTIAL_ACCELERATION", __func__);
            mfx_sts = MFX_ERR_NONE;
        } else if (mfx_sts == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            ALOGW("%s: [warning] returns MFX_WRN_INCOMPATIBLE_VIDEO_PARAM", __func__);
            mfx_sts = MFX_ERR_NONE;
        }

        if (mfx_sts == MFX_ERR_NONE) {
            // Retrieves current working parameters to the specified output structure.
            mfx_sts = MFXVideoDECODE_GetVideoParam(mMfxDecSession, &mMfxVideoDecParams);

            if (mfx_sts == MFX_ERR_NONE) {
                ALOGV("%s: Decoder initialized successfully!", __func__);
                mIsDecoderInitialized = true;
            }
        }
    }

    if (mfx_sts != MFX_ERR_NONE) {
        ALOGE("%s: Failed!, ret = %d", __func__, mfx_sts);
        Release();
    }

    ALOGV("%s - X", __func__);
    return mfx_sts;
}

mfxStatus MfxDecoder::DecodeFrame(uint8_t *pData, size_t size) {
    ALOGV("%s - E", __func__);

    mfxStatus mfx_sts = MFX_ERR_NONE;

    mfx_sts = mMfxFrameConstructor->Load(pData, size, 0, false, true);
    if (mfx_sts == MFX_ERR_NONE) {
        ALOGV("%s: Loaded compressed frame successfully!", __func__);
    } else {
        ALOGE("%s: Failed to load compressed frame, ret = %d", __func__, mfx_sts);
        return mfx_sts;
    }

    mfxBitstream *bs = mMfxFrameConstructor->GetMfxBitstream().get();

    if (!mIsDecoderInitialized) {
        mfx_sts = InitDecoder(&bs);
        if(mfx_sts != MFX_ERR_NONE) {
           if (mfx_sts == MFX_ERR_MORE_DATA) {
               // Not enough data for InitDecoder should not cause an error
               mfx_sts = MFX_ERR_NONE;
           } else {
               ALOGE("%s: Couldn't initialize mfx decoder", __func__);
               return mfx_sts;
           }
        }

        if (!mIsDecoderInitialized) {
            ALOGE("%s: Couldn't initialize mfx decoder", __func__);
            return mfx_sts;
        }
    }

    do {
        // Check bitsream is empty or not
        if (bs && bs->DataLength == 0) {
            ALOGE("%s: bitsream is empty, unable to continue!", __func__);
            break;
        }
        ALOGV("%s: bs->DataLength = %d, bs->DataOffset = %d", __func__,
              bs->DataLength, bs->DataOffset);

        mfxFrameSurface1 *pWorkSurface = NULL, *pOutSurface = NULL;

        GetAvailableSurface(&pWorkSurface);
        if (!pWorkSurface) {
            ALOGE("%s: Couldn't find available surface!", __func__);
            mfx_sts = MFX_ERR_NOT_ENOUGH_BUFFER;
            break;
        }

        mfxSyncPoint sync_point;

        // Decodes the input bitstream to a single output frame.
        mfx_sts = MFXVideoDECODE_DecodeFrameAsync(mMfxDecSession, bs, pWorkSurface,
                                                  &pOutSurface, &sync_point);

        // When the hardware acceleration device (GPU) is busy.
        if (mfx_sts == MFX_WRN_DEVICE_BUSY) {
            ALOGW("%s: GPU HW is busy!  Try sync operation now", __func__);
            mfx_sts = MFXVideoCORE_SyncOperation(mMfxDecSession, sync_point, MFX_TIMEOUT_INFINITE);
            if (mfx_sts == MFX_ERR_NONE) {
                ALOGV("%s: Sync operation success!", __func__);
            } else {
                ALOGE("%s: Sync operation failed, mfx_sts = %d", __func__, mfx_sts);
            }
        }

        if (mfx_sts == MFX_ERR_NONE) {
            ALOGV("%s: Decoding is successfull!!!", __func__);
            ALOGV("%s: pOutSurface->Data.Locked = %u, pOutSurface->Info.CropW = %u,"
                  "pOutSurface->Info.CropH = %u, pOutSurface->Info.Width = %u,"
                  "pOutSurface->Info.Height = %u, pOutSurface->Data.TimeStamp = %llu ",
                  __func__, pOutSurface->Data.Locked, pOutSurface->Info.CropW,
                  pOutSurface->Info.CropH, pOutSurface->Info.Width,
                  pOutSurface->Info.Height, pOutSurface->Data.TimeStamp);

            // Copy decoded frame into internal decoder buffer.
            {
                std::lock_guard<std::mutex> lock(mMemMutex);
                mOutFrameSurfList.push_back(pOutSurface);
                break;
            }
        } else if (mfx_sts > 0) {
            ALOGV("%s: Decoding unsuccessfull since no frame received in between. "
                  "Will retry again. ret = %d", __func__, mfx_sts);
        } else {
            ALOGE("%s: Decoding Failed. ret = %d", __func__, mfx_sts);
        }
    } while (mfx_sts > 0);

    mMfxFrameConstructor->Unload();

    ALOGV("%s - X", __func__);
    return mfx_sts;
}

bool MfxDecoder::GetOutput(YCbCrLayout &out) {
    ALOGV("%s - E", __func__);

    bool output_available = false;
    mfxFrameSurface1 *surface_out = NULL;

    {
        std::lock_guard<std::mutex> lock(mMemMutex);
        if (!mIsDecoderInitialized || mOutFrameSurfList.empty()) {
            ALOGV("%s: Decoded output is not available", __func__);
            return output_available;
        } else {
            ALOGV("%s: Decoded output is available", __func__);
            surface_out = mOutFrameSurfList.front();
            mOutFrameSurfList.remove(surface_out);
            output_available = true;
        }
    }

    // Copy decoded frame into internal camera buffer.
    if (output_available) {
        // NV12 to I420
        uint8_t *srcY = surface_out->Data.Y;
        uint8_t *srcU = surface_out->Data.U;
        uint8_t *srcV = srcU + 1;
        uint8_t *dstY = (uint8_t*)out.y;
        uint8_t *dstU = (uint8_t*)out.cb;
        uint8_t *dstV = (uint8_t*)out.cr;
        uint32_t crop_width = surface_out->Info.CropW;
        uint32_t crop_height = surface_out->Info.CropH;
        uint32_t input_width = surface_out->Info.Width;
        uint32_t input_height = surface_out->Info.Height;

        ALOGV("%s: crop_width = %u, crop_height = %u, pitch = %u, input_width = %u, "
              "input_height = %u", __func__, crop_width, crop_height, surface_out->Data.Pitch,
              input_width, input_height);
        ALOGV("%s: yStride = %u, cStride = %u, chromaStep = %u", __func__, out.yStride,
              out.cStride, out.chromaStep);

        if (out.chromaStep == 1) {
            // Y
            for (uint32_t i = 0; i < crop_height; i++) {
                memcpy(dstY, srcY, crop_width);
                srcY += surface_out->Data.Pitch;
                dstY += out.yStride;
            }
            // U/V
            for (uint32_t i = 0; i < crop_height / 2; i++) {
                for (uint32_t j = 0; j < crop_width / 2; j++) {
                    memcpy(dstU + j, srcU + j*2, 1);
                    memcpy(dstV + j, srcV + j*2, 1);
                }
                srcU += surface_out->Data.Pitch;
                srcV += surface_out->Data.Pitch;
                dstU += out.cStride;
                dstV += out.cStride;
            }
        } else if (out.chromaStep == 2) {
            if (input_width >= crop_width || input_height >= crop_height) {
                // Y
                for (uint32_t i = 0; i < crop_height; i++) {
                    memcpy(dstY, srcY, crop_width);
                    srcY += surface_out->Data.Pitch;
                    dstY += out.yStride;
                }
                // U/V
                for (uint32_t i = 0; i < crop_height / 2; i++) {
                    for (uint32_t j = 0; j < crop_width / 2; j++) {
                        memcpy(dstU + j*2, srcU + j*2, 1);
                        memcpy(dstV + j*2, srcV + j*2, 1);
                    }
                    srcU += surface_out->Data.Pitch;
                    srcV += surface_out->Data.Pitch;
                    dstU += out.cStride;
                    dstV += out.cStride;
                }
            }
        }

        surface_out = NULL;
    }

    return output_available;
}
