#pragma once
#include <android/log.h>
#define XF_TAG "xfade"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, XF_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, XF_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, XF_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, XF_TAG, __VA_ARGS__)
