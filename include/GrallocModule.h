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

#ifdef USE_GRALLOC1
#include <hardware/gralloc1.h>
#ifndef GRALLOC_MAPPER4
#include <sync/sync.h>
#endif
#endif

class GrallocModule {
public:
    static GrallocModule &getInstance() {
        static GrallocModule instance;
        return instance;
    }

    int lock(buffer_handle_t handle, int usage, int left, int top, int width, int height,
             void **vaddr) {
        switch (m_major_version) {
            case 0: {
                return mModule->lock(mModule, handle, usage, left, top, width, height, vaddr);
            }
            case 1:
#ifdef USE_GRALLOC1
            {
                gralloc1_rect_t outRect{};
                outRect.left = left;
                outRect.top = top;
                outRect.width = width;
                outRect.height = height;
                return m_gralloc1_lock(m_gralloc1_device, handle, usage, usage, &outRect, vaddr,
                                       -1);
            }
#endif
            default: {
                ALOGE(
                    "[Gralloc] no gralloc module to lock; unknown gralloc major "
                    "version (%d)",
                    m_major_version);
                return -1;
            }
        }
    }

    int lock_ycbcr(buffer_handle_t handle, int usage, int left, int top, int width, int height,
                   struct android_ycbcr *ycbcr) {
        switch (m_major_version) {
            case 0: {
                return mModule->lock_ycbcr(mModule, handle, usage, left, top, width, height, ycbcr);
            }
            case 1:
#ifdef USE_GRALLOC1
            {
                gralloc1_rect_t outRect{};
                outRect.left = left;
                outRect.top = top;
                outRect.width = width;
                outRect.height = height;
                int ret = -1;

                android_flex_layout flex = {};
                int32_t error =
                    m_gralloc1_getNumFlexPlanes(m_gralloc1_device, handle, &flex.num_planes);
                if (error != GRALLOC1_ERROR_NONE) {
                    return error;
                }
                std::vector<android_flex_plane_t> flexPlanes(flex.num_planes);
                flex.planes = flexPlanes.data();

                ret = m_gralloc1_lockflex(m_gralloc1_device, handle, usage, usage, &outRect, &flex,
                                          -1);
                ycbcr->y = flex.planes[0].top_left;
                ycbcr->cb = flex.planes[1].top_left;
                ycbcr->cr = flex.planes[2].top_left;
                ycbcr->ystride = flex.planes[0].v_increment;
                ycbcr->cstride = flex.planes[1].v_increment;
                ycbcr->chroma_step = flex.planes[2].h_increment;
                return ret;
            }
#endif
            default: {
                ALOGE(
                    "[Gralloc] no gralloc module to lock; unknown gralloc major "
                    "version (%d)",
                    m_major_version);
                return -1;
            }
        }
    }

    int unlock(buffer_handle_t handle) {
        switch (m_major_version) {
            case 0: {
                return mModule->unlock(mModule, handle);
            }
            case 1:
#ifdef USE_GRALLOC1
            {
                int32_t fenceFd = -1;
                int error = m_gralloc1_unlock(m_gralloc1_device, handle, &fenceFd);
                if (!error) {
#ifndef GRALLOC_MAPPER4
                    sync_wait(fenceFd, -1);
                    close(fenceFd);
#endif
                }
                return error;
            }
#endif
            default: {
                ALOGE(
                    "[Gralloc] no gralloc module to unlock; unknown gralloc major "
                    "version (%d)",
                    m_major_version);
                return -1;
            }
        }
    }
#ifdef GRALLOC_MAPPER4
    int importBuffer(buffer_handle_t handle, buffer_handle_t *outBuffer) {
        switch (m_major_version) {
            case 1:
#ifdef USE_GRALLOC1
            {
                return m_gralloc1_importbuffer(m_gralloc1_device, handle, outBuffer);
            }
#endif
            default: {
                ALOGE(
                    "[Gralloc] no gralloc module to import; unknown gralloc major "
                    "version (%d)",
                    m_major_version);
                return -1;
            }
        }
    }
#endif
private:
    GrallocModule() {
        const hw_module_t *module = nullptr;
        int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
        if (ret) {
            ALOGE("%s: Failed to get gralloc module: %d", __FUNCTION__, ret);
        }

        mModule = nullptr;
        m_major_version = (module->module_api_version >> 8) & 0xff;
        switch (m_major_version) {
            case 0:
                mModule = reinterpret_cast<const gralloc_module_t *>(module);
                break;
            case 1:
#ifdef USE_GRALLOC1
                gralloc1_open(module, &m_gralloc1_device);
                m_gralloc1_lock = (GRALLOC1_PFN_LOCK)m_gralloc1_device->getFunction(
                    m_gralloc1_device, GRALLOC1_FUNCTION_LOCK);
                m_gralloc1_unlock = (GRALLOC1_PFN_UNLOCK)m_gralloc1_device->getFunction(
                    m_gralloc1_device, GRALLOC1_FUNCTION_UNLOCK);
                m_gralloc1_lockflex = (GRALLOC1_PFN_LOCK_FLEX)m_gralloc1_device->getFunction(
                    m_gralloc1_device, GRALLOC1_FUNCTION_LOCK_FLEX);
                m_gralloc1_getNumFlexPlanes =
                    (GRALLOC1_PFN_GET_NUM_FLEX_PLANES)m_gralloc1_device->getFunction(
                        m_gralloc1_device, GRALLOC1_FUNCTION_GET_NUM_FLEX_PLANES);
#ifdef GRALLOC_MAPPER4
                m_gralloc1_importbuffer = (GRALLOC1_PFN_IMPORT_BUFFER)m_gralloc1_device->getFunction(
                    m_gralloc1_device, GRALLOC1_FUNCTION_IMPORT_BUFFER);
               
#endif
                break;
#endif
            default:
                ALOGE("[Gralloc] unknown gralloc major version (%d)", m_major_version);
                break;
        }
    }
    const gralloc_module_t *mModule;
    uint8_t m_major_version;
#ifdef USE_GRALLOC1
    gralloc1_device_t *m_gralloc1_device = nullptr;
    GRALLOC1_PFN_LOCK m_gralloc1_lock = nullptr;
    GRALLOC1_PFN_UNLOCK m_gralloc1_unlock = nullptr;
    GRALLOC1_PFN_LOCK_FLEX m_gralloc1_lockflex = nullptr;
    GRALLOC1_PFN_GET_NUM_FLEX_PLANES m_gralloc1_getNumFlexPlanes = nullptr;
#ifdef GRALLOC_MAPPER4
    GRALLOC1_PFN_IMPORT_BUFFER m_gralloc1_importbuffer=nullptr;
#endif
#endif
};

#endif
