// GENERIC tool: pick the proxy DLL for a game from its import table, and
// generate the proxy definition (.def + names .cpp) from the real DLL.
//
//   shvi_pick_proxy <game.exe>                 rank candidates
//   shvi_pick_proxy --emit <dll> <out_dir>     write <dll>.def and <dll>.cpp
//
// Rules: a usable proxy is imported (directly or delay-loaded) by the exe,
// lives in System32, and is NOT a KnownDLL (those are mapped from
// \KnownDlls and never searched for in the app folder). api-ms-*/ext-ms-*
// API sets are never candidates. Ranking prefers small, input/version DLLs
// over render DLLs (dxgi/d3d*), which overlays like Steam/RTSS/ReShade hook.
#include "core/pe.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::set<std::string> KnownDlls() {
    std::set<std::string> out;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        BYTE data[512];
        DWORD nlen = 256, dlen = sizeof(data), type = 0;
        if (RegEnumValueW(key, i, name, &nlen, nullptr, &type, data, &dlen) != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        std::wstring v(reinterpret_cast<wchar_t*>(data), dlen / sizeof(wchar_t));
        while (!v.empty() && v.back() == 0) v.pop_back();
        std::string s;
        for (wchar_t c : v) s.push_back(static_cast<char>(c));
        out.insert(Lower(s));
    }
    RegCloseKey(key);
    return out;
}

std::wstring System32() {
    wchar_t p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    return p;
}

int Score(const std::string& dll) {
    static const std::pair<const char*, int> table[] = {
        {"dinput8.dll", 100}, {"version.dll", 95}, {"winmm.dll", 85}, {"winhttp.dll", 75},
        {"xinput1_4.dll", 70}, {"xinput1_3.dll", 70}, {"dsound.dll", 60}, {"hid.dll", 50},
        {"dbghelp.dll", 40}, {"dxgi.dll", 10}, {"d3d11.dll", 5}, {"d3d12.dll", 5},
    };
    for (auto& [n, s] : table)
        if (dll == n) return s;
    return 30;
}

int Rank(const wchar_t* exe) {
    shvi::pe::ImageFile img;
    std::string err;
    if (!img.Load(exe, &err)) {
        fprintf(stderr, "cannot read %ls: %s\n", exe, err.c_str());
        return 1;
    }
    if (img.Machine() != IMAGE_FILE_MACHINE_AMD64) fprintf(stderr, "warning: not an x64 image\n");

    auto known = KnownDlls();
    std::wstring sys = System32();
    struct Row { std::string dll; bool delay; int score; std::string verdict; };
    std::vector<Row> rows;
    auto consider = [&](const std::string& raw, bool delay) {
        std::string dll = Lower(raw);
        if (dll.rfind("api-ms-", 0) == 0 || dll.rfind("ext-ms-", 0) == 0) return;
        Row r{dll, delay, Score(dll), "ok"};
        if (known.count(dll)) r.verdict = "KnownDLL (cannot be proxied)", r.score = -1;
        else if (dll == "comctl32.dll" || dll == "gdiplus.dll")
            r.verdict = "side-by-side assembly (manifest-bound), avoid", r.score = -1;
        else if (GetFileAttributesW((sys + L"\\" + Widen(dll)).c_str()) == INVALID_FILE_ATTRIBUTES)
            r.verdict = "not in System32 (game-shipped DLL; replace = fragile)", r.score = -1;
        rows.push_back(r);
    };
    for (auto& d : img.Imports()) consider(d, false);
    for (auto& d : img.DelayImports()) consider(d, true);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

    printf("%-28s %-6s %-5s %s\n", "dll", "load", "score", "verdict");
    for (auto& r : rows)
        printf("%-28s %-6s %5d %s\n", r.dll.c_str(), r.delay ? "delay" : "static", r.score, r.verdict.c_str());
    auto best = std::find_if(rows.begin(), rows.end(), [](const Row& r) { return r.score >= 0; });
    if (best == rows.end()) {
        printf("\nno proxyable import found. Note: packed executables often hide imports until runtime;\n"
               "check the import table after unpacking, or fall back to version.dll/winmm.dll if loaded early.\n");
        return 2;
    }
    printf("\nrecommended proxy: %s%s\n", best->dll.c_str(),
           best->delay ? " (delay-loaded: it is only loaded when first used; check it loads before gameplay)" : "");
    return 0;
}

int Emit(const std::string& dllArg, const std::wstring& outDir) {
    std::string dll = Lower(dllArg);
    if (dll.size() < 4 || dll.substr(dll.size() - 4) != ".dll") dll += ".dll";
    std::string stem = dll.substr(0, dll.size() - 4);
    shvi::pe::ImageFile img;
    std::string err;
    if (!img.Load(System32() + L"\\" + Widen(dll), &err)) {
        fprintf(stderr, "cannot read System32\\%s: %s\n", dll.c_str(), err.c_str());
        return 1;
    }
    auto names = img.ExportNames();
    if (names.size() > 64) {
        fprintf(stderr, "%zu exports; proxy_stubs.asm has 64 stubs. Add stubs first.\n", names.size());
        return 1;
    }
    std::ofstream def(outDir + L"\\" + Widen(stem) + L".def");
    def << "LIBRARY " << stem << "\nEXPORTS\n";
    static const std::set<std::string> priv = {"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer",
                                               "DllUnregisterServer", "DllInstall"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        char line[256];
        snprintf(line, sizeof(line), "    %s=shvi_stub_%02zu%s\n", names[i].c_str(), i,
                 priv.count(names[i]) ? " PRIVATE" : "");
        def << line;
    }
    std::ofstream cpp(outDir + L"\\" + Widen(stem) + L".cpp");
    cpp << "// Generated by shvi_pick_proxy --emit " << stem << ". Order matches " << stem << ".def.\n"
        << "#include <cstddef>\n\nextern const wchar_t* const kProxyDll = L\"" << dll << "\";\n"
        << "extern const char* const kProxyExports[] = {\n";
    for (auto& n : names) cpp << "    \"" << n << "\",\n";
    cpp << "};\nextern const std::size_t kProxyExportCount = sizeof(kProxyExports) / sizeof(kProxyExports[0]);\n";
    printf("wrote %s.def and %s.cpp (%zu exports). Add `%s` to SHVI_PROXIES in CMakeLists.txt.\n", stem.c_str(),
           stem.c_str(), names.size(), stem.c_str());
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2) return Rank(argv[1]);
    if (argc == 4 && std::wstring(argv[1]) == L"--emit") {
        std::string dll;
        for (wchar_t* p = argv[2]; *p; ++p) dll.push_back(static_cast<char>(*p));
        return Emit(dll, argv[3]);
    }
    fprintf(stderr, "usage:\n  shvi_pick_proxy <game.exe>\n  shvi_pick_proxy --emit <dll> <out_dir>\n");
    return 64;
}
