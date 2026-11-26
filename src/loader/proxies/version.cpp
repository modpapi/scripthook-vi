// Proxy definition for version.dll (imported by most games; not a KnownDLL).
// Order must match version.def. Regenerate with: shvi_pick_proxy --emit version <dir>
#include <cstddef>

extern const wchar_t* const kProxyDll = L"version.dll";
extern const char* const kProxyExports[] = {
    "GetFileVersionInfoA",      "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",    "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW","GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",             "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",          "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",           "VerQueryValueW",
};
extern const std::size_t kProxyExportCount = sizeof(kProxyExports) / sizeof(kProxyExports[0]);
