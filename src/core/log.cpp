#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace shvi::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}  // namespace

void Init(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Write(const char* fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2112];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, body);

    std::lock_guard lock(g_mutex);
    OutputDebugStringA(line);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}

}  // namespace shvi::log
