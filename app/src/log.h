#pragma once
#ifdef __ANDROID__
#include <android/log.h>
#else   // desktop builds (tests, tools): same API on stdout
enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };
#endif
#if defined(__GNUC__) || defined(__clang__)
#define LOG_PRINTF_ATTR __attribute__((format(printf, 2, 3)))
#else
#define LOG_PRINTF_ATTR
#endif
#define LOG_TAG "ProjectArena"
// Every line also goes to <externalDataPath>/projectarena_log.txt once logOpenFile() is called: the Quest logcat ring
// buffer is only 256 KiB and system spam overwrites a match within minutes.
void logOpenFile(const char* dir);
void logWrite(int prio, const char* fmt, ...) LOG_PRINTF_ATTR;
#define LOGI(...) logWrite(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) logWrite(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) logWrite(ANDROID_LOG_ERROR, __VA_ARGS__)
