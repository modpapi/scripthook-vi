// GENERIC. HostInfo for a hosted start: a host process (VIctory) that mapped the
// title image itself calls shvi_core's `victory_on_title_mapped` export with
// this struct before any title code runs (spec §6.1-6.2). The contract is the
// host's: it names no ScriptHook VI symbol. Header-only and pure.
#pragma once
#include "game_profile.h"
#include "pe.h"
#include "shvi/shvi.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace shvi {

struct VictoryTitleInfo {  // ABI with VIctory, version 1
    std::uint32_t struct_size;  // sizeof, for growth: larger is accepted
    std::uint32_t version;      // 1
    void* image_base;           // title exe as mapped
    std::uint64_t image_size;
    const wchar_t* exe_path;    // original path, e.g. ...\game_scarlett_master.exe
};
static_assert(sizeof(VictoryTitleInfo) == 32);

inline constexpr std::uint32_t kVictoryTitleInfoVersion = 1;

inline int ValidateTitleInfo(const VictoryTitleInfo* info) {
    if (!info) return SHVI_E_INVALID_ARG;
    if (info->struct_size < sizeof(VictoryTitleInfo)) return SHVI_E_ABI_MISMATCH;
    if (info->version != kVictoryTitleInfoVersion) return SHVI_E_ABI_MISMATCH;
    if (!info->image_base || !info->exe_path || !*info->exe_path) return SHVI_E_INVALID_ARG;
    return SHVI_OK;
}

// exe = the mapped image; exePath/exeName/exeDir from the original path (so
// <exeDir>\shvi\ lives next to the real exe); version from the image.
inline HostInfo HostInfoFromTitle(HMODULE base, std::wstring_view exePath) {
    HostInfo h;
    h.exe = base;
    h.exePath.assign(exePath);
    auto slash = h.exePath.find_last_of(L"\\/");
    h.exeDir = slash == std::wstring::npos ? std::wstring() : h.exePath.substr(0, slash);
    h.exeName = slash == std::wstring::npos ? h.exePath : h.exePath.substr(slash + 1);
    if (auto v = pe::GetModuleVersion(base)) h.version = *v;
    return h;
}

}  // namespace shvi
