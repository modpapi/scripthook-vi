// GENERIC proxy loader. Built once per proxied DLL (dinput8, version, ...):
// the per-proxy file supplies kProxyDll + kProxyExports in .def order.
//
// DllMain:   load the real DLL from System32 (absolute path, never ourselves),
//            fill the stub table, start one loader thread.
// Thread:    LoadLibrary("<game>\shvi\shvi_core.dll") and call shvi_core_start.
//            Runs after the loader lock is released, so the core may load
//            plugins and wait for the game to unpack.
// The loader knows nothing about any game; the core decides whether to act.
#include <windows.h>

#include <cstddef>
#include <cstdint>

extern const wchar_t* const kProxyDll;
extern const char* const kProxyExports[];
extern const std::size_t kProxyExportCount;

extern "C" {
void* g_shvi_proxy_targets[64] = {};
}

namespace {

HMODULE g_real = nullptr;

// Used for exports the real DLL lacks; x64 caller-cleans, so any signature is safe to return from.
std::uintptr_t MissingExport() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

bool LoadReal() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 2 + lstrlenW(kProxyDll) >= MAX_PATH) return false;
    lstrcatW(path, L"\\");
    lstrcatW(path, kProxyDll);
    g_real = LoadLibraryW(path);
    for (std::size_t i = 0; i < kProxyExportCount && i < 64; ++i) {
        void* p = g_real ? reinterpret_cast<void*>(GetProcAddress(g_real, kProxyExports[i])) : nullptr;
        g_shvi_proxy_targets[i] = p ? p : reinterpret_cast<void*>(&MissingExport);
    }
    return g_real != nullptr;
}

DWORD WINAPI LoaderThread(LPVOID self) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);  // the game exe
    if (!n) return 1;
    for (DWORD i = n; i > 0; --i)
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; break; }
    lstrcatW(path, L"shvi\\shvi_core.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;  // not installed: be a plain proxy

    HMODULE core = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core) {
        OutputDebugStringW(L"[shvi] failed to load shvi\\shvi_core.dll\n");
        return 1;
    }
    using StartFn = int (*)();
    auto start = reinterpret_cast<StartFn>(GetProcAddress(core, "shvi_core_start"));
    int rc = start ? start() : -1;
    (void)self;
    return static_cast<DWORD>(rc);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Loading a System32 DLL here is the usual proxy compromise; everything
        // else (core, plugins, scanning) happens on the loader thread.
        LoadReal();
        if (HANDLE t = CreateThread(nullptr, 0, &LoaderThread, inst, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
