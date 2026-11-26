// GENERIC. Thread-safe file + debugger log.
#pragma once
#include <string>

namespace shvi::log {

void Init(const std::wstring& path);   // optional; without it only OutputDebugString
void Write(const char* fmt, ...);      // printf-style, appends '\n'

}  // namespace shvi::log

#define SHVI_LOG(...) ::shvi::log::Write(__VA_ARGS__)
