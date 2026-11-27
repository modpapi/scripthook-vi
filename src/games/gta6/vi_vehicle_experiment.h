// Experimental VM wrapper for Xbox build 1794606036. No game code is modified.
#pragma once
#include "games/gta6/vi_script.h"
#include <array>
#include <algorithm>
#include <cstddef>

namespace shvi::gta6::vehicle_experiment {
inline constexpr std::uint32_t kSentinel = 0xFFFFFF01u;
inline constexpr std::uint32_t kProgram = 0x48391B87u;
inline constexpr std::uint32_t kCodeSize = 0x30528u, kPedOffset = 0x18247u;
inline constexpr std::uint32_t kTaskType = 0xAFAu;
inline bool ValidTaskType(std::uint32_t handle, std::uint32_t baseToken, std::uint32_t baseHandle) {
    return handle == kTaskType && baseToken == 0x8000002Fu && baseHandle == 0x8000002Eu;
}
inline constexpr std::array<std::uint8_t, 35> kPedWrapper{{
    0x37,0xAB,0x95,0x7A,0x08,0xB0,0x17,0xE0,0x5E,0xA4,0xEB,0x85,0xC2,0x1C,0xE0,0xAB,
    0x9D,0xAA,0x66,0x33,0x47,0x96,0x6D,0xA7,0xB6,0x1F,0xCD,0x35,0xA8,0x8B,0x22,0x47,
    0xB5,0x8D,0xE6}};
// The donor copies 13 caller cells, appends a closed task type, calls a 14-cell native and returns.
inline std::optional<std::array<std::uint8_t, 35>> Build(const std::uint8_t* donor, std::size_t n) {
    if (!donor || n != kPedWrapper.size() || !std::equal(kPedWrapper.begin(), kPedWrapper.end(), donor))
        return std::nullopt;
    auto bytes = kPedWrapper;
    auto operand = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes[at + 2 + i] = static_cast<std::uint8_t>((value >> (8 * i)) * 0x65u) ^ script::kXboxKeys.a[i];
    };
    operand(19, kTaskType);
    operand(25, 0x4A80260u);
    return bytes;
}
}  // namespace shvi::gta6::vehicle_experiment
