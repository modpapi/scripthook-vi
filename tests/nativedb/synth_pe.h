// The synthetic GTA-VI-like exe the nativedb golden and repro tests run on:
// eight fake native handlers covering the census cases (plain, cold chunk with
// chained unwind, leaf without .pdata, long prologue, indirect call, tail call,
// unresolved leaf start, leaf with a far tail call). Built from hand-assembled bytes; rel32 values are
// computed, not typed in.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shvi::nativedb::test {

inline constexpr std::uint32_t kSynthTimeDateStamp = 0x5EED0001;
inline constexpr std::uint32_t kSynthLockGlobal = 0x6000;
inline constexpr std::uint32_t kSynthAcquire = 0x1000;
inline constexpr std::uint32_t kSynthRelease = 0x1010;
inline constexpr std::uint32_t kSynthC1 = 0x1020;
inline constexpr std::uint32_t kSynthC2 = 0x1030;
inline constexpr std::uint32_t kSynthC3 = 0x1900;  // H8's tail-call target

// Deterministic PE32+ file bytes: x64, image base 0x140000000, TimeDateStamp 0x5EED0001,
// .text 0x1000 (RX), .rdata 0x4000 (unwind infos), .pdata 0x5000, .data 0x6000 (lock global).
std::vector<std::uint8_t> BuildSynthPe();

// What the census must produce, written independently of the census code.
struct SynthExpect {
    std::uint32_t rva;
    std::uint64_t id;
    bool lock;
    std::vector<std::optional<std::uint32_t>> callees;  // nullopt = indirect
    std::string flags;
};
const std::vector<SynthExpect>& SynthExpected();

// Size of the primary .pdata entry starting at rva (from the builder's own table), 0 if none.
std::uint32_t SynthSizeOf(std::uint32_t rva);

}  // namespace shvi::nativedb::test
