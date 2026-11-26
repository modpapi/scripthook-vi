// Integration test: load a built proxy DLL by path and call through its stubs.
// Proves the MASM jump table forwards to the real System32 DLL.
#include <windows.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 64;
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) { printf("FAIL load %lu\n", GetLastError()); return 1; }
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    auto fn = reinterpret_cast<SizeFn>(GetProcAddress(proxy, "GetFileVersionInfoSizeW"));
    if (!fn) { printf("FAIL no export\n"); return 1; }
    wchar_t k32[MAX_PATH];
    GetSystemDirectoryW(k32, MAX_PATH);
    lstrcatW(k32, L"\\kernel32.dll");
    DWORD h = 0, n = fn(k32, &h);
    printf("%s GetFileVersionInfoSizeW via proxy = %lu\n", n > 0 ? "PASS" : "FAIL", n);
    return n > 0 ? 0 : 1;
}
