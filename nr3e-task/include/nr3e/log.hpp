#pragma once

#include <android/log.h>
#include <cstdio>

#define NR3E_TAG "nr3e"

#ifdef NR3E_SILENT
#define NRLOGI(...) ((void)0)
#define NRLOGD(...) ((void)0)
#define NRLOGE(...) ((void)0)
#else
#define NRLOGI(...) __android_log_print(ANDROID_LOG_INFO, NR3E_TAG, __VA_ARGS__)
#define NRLOGE(...) __android_log_print(ANDROID_LOG_ERROR, NR3E_TAG, __VA_ARGS__)
#define NRLOGD(...) __android_log_print(ANDROID_LOG_DEBUG, NR3E_TAG, __VA_ARGS__)
#endif
