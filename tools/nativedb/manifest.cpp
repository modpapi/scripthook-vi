#include "manifest.h"

#include "core/json_write.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace shvi::nativedb {

std::string FormatManifest(const BuildInfo& b, std::vector<InputRec> inputs, std::vector<OutputRec> outputs) {
    using namespace jsonw;
    std::sort(inputs.begin(), inputs.end(), [](const InputRec& x, const InputRec& y) {
        return x.role != y.role ? x.role < y.role : x.name < y.name;
    });
    std::sort(outputs.begin(), outputs.end(), [](const OutputRec& x, const OutputRec& y) { return x.path < y.path; });

    Object build{{"key", b.key}, {"machine", "0x" + Hex32(b.machine).substr(6)}, {"image_size", Hex32(b.imageSize)}};
    Array in;
    for (const auto& r : inputs) {
        Object o{{"role", r.role}, {"name", r.name}, {"sha256", r.sha256}, {"size", r.size}};
        if (!r.pin.empty()) o["pin"] = r.pin;
        if (r.role == "exe" && !build.count("exe"))
            build["exe"] = Object{{"name", r.name}, {"sha256", r.sha256}, {"size", r.size}};
        in.push_back(std::move(o));
    }
    Array out;
    for (const auto& r : outputs) out.push_back(Object{{"path", r.path}, {"sha256", r.sha256}, {"size", r.size}});

    Object root{
        {"build", std::move(build)},
        {"tool", Object{{"name", "shvi_nativedb"}, {"version", SHVI_TOOL_VERSION}, {"schema", 1},
                        {"zydis", SHVI_ZYDIS_COMMIT}, {"zycore", SHVI_ZYCORE_COMMIT}}},
        {"inputs", std::move(in)},
        {"outputs", std::move(out)},
    };
    if (b.lock) {
        root["census"] = Object{
            {"handlers", b.handlers},
            {"lock", Object{{"global", Hex32(b.lock->global)}, {"acquire", Hex32(b.lock->acquire)},
                            {"release", Hex32(b.lock->release)}, {"votes", b.lock->votes},
                            {"voters", b.lock->voters}}},
        };
    }
    return Dump(root);
}

std::string Utf8Basename(const std::wstring& path) {
    std::size_t cut = path.find_last_of(L"\\/");
    std::wstring base = cut == std::wstring::npos ? path : path.substr(cut + 1);
    if (base.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, base.data(), static_cast<int>(base.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, base.data(), static_cast<int>(base.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fclose(f) == 0) && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

}  // namespace shvi::nativedb
