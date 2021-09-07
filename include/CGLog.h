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
