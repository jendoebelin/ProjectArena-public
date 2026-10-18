#include "log.h"
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(d, m) _mkdir(d)
#endif

static FILE* g_file = nullptr;
static std::mutex g_mu;
static std::string g_early;   // lines from before the file is open (start-up: OpenXR, passthrough, depth), written once it is

void logOpenFile(const char* dir) {
    if (!dir || g_file) return;
    mkdir(dir, 0770);
    std::string p = std::string(dir) + "/projectarena_log.txt", old = p + ".1";
    struct stat st{};
    if (stat(p.c_str(), &st) == 0 && st.st_size > 4 * 1024 * 1024) rename(p.c_str(), old.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_file = fopen(p.c_str(), "a");
    if (g_file) {
        fprintf(g_file, "---- session start ----\n%s", g_early.c_str());
        fflush(g_file);
    }
    g_early.clear();
}

void logWrite(int prio, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const char tag = prio >= ANDROID_LOG_ERROR ? 'E' : prio == ANDROID_LOG_WARN ? 'W' : 'I';
#ifdef __ANDROID__
    __android_log_write(prio, LOG_TAG, buf);
#else
    if (!std::getenv("PROJECTARENA_QUIET")) std::printf("%c %s\n", tag, buf);
#endif
    std::lock_guard<std::mutex> lk(g_mu);
    time_t now = time(nullptr);
    tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d %c %s\n", t.tm_hour, t.tm_min, t.tm_sec, tag, buf);
    if (!g_file) {
        if (g_early.size() < 64 * 1024) g_early += line;
        return;
    }
    fputs(line, g_file);
    fflush(g_file);
}
