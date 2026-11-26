// GAME-SPECIFIC: GTA VI. The game-mode signal for the online guard, and the
// tick gate. Pure functions, so they are unit-tested without the game.
//
// From the Xbox build (ts 1794606036), static analysis:
//   * the game-mode object is *(qword_14AAE3B70), 488 bytes, allocated in
//     init phase 2 (sub_141DB2820);
//   * +0x40 = current mode hash: 0 or joaat("DefaultGameMode") means "none
//     yet" (several systems branch on exactly that);
//   * +0x48 = requested mode, set by sub_141B6B6B0 after parsing the launch
//     URL americas://Story000/startContent?MODE=Story000. The parse yields
//     joaat("Story000") = 0x5B5EEFFE, which is not in the accepted set, so it
//     is replaced by the default 0xFF2E93E0. That made 0xFF2E93E0 the
//     expected story-boot value. The other five accepted values are other content ids.
//
// Observed 2026-11-25 (first run that reached the world, under VIctory, player ped
// present): both fields hold 0x5B5EEFFE = joaat("Story000"). The parsed value is kept,
// not replaced, so that is the story value in practice. The derived default stays
// accepted as well; it has never been seen.
//
// Fail-closed: Offline only when BOTH fields hold the story value. A known
// non-story content id means Transition (the guard latches). Anything never
// seen means Unknown (the guard waits; plugins stay paused).
#pragma once
#include "core/online_guard.h"

#include <array>
#include <cstdint>

namespace shvi::gta6 {

inline constexpr std::uint32_t kDefaultGameMode = 0x31B22B23;  // joaat("DefaultGameMode")
inline constexpr std::uint32_t kStoryMode = 0x5B5EEFFE;        // joaat("Story000"): observed in the story world
inline constexpr std::uint32_t kStoryModeDefault = 0xFF2E93E0; // the default accepted mode (derived, never observed)
inline constexpr std::array<std::uint32_t, 5> kOtherContentModes = {0x609AE393, 0x62C09CC6, 0x8D3A6A32,
                                                                     0xA4F76795, 0xC4986FCD};
inline constexpr std::uint32_t kModeCurrentOffset = 0x40;
inline constexpr std::uint32_t kModeRequestedOffset = 0x48;

constexpr bool IsOtherContent(std::uint32_t m) {
    for (auto o : kOtherContentModes)
        if (o == m) return true;
    return false;
}

constexpr GameMode ClassifyMode(bool haveObject, std::uint32_t current, std::uint32_t requested) {
    if (!haveObject || current == 0 || current == kDefaultGameMode) return GameMode::Unknown;
    if (IsOtherContent(current) || IsOtherContent(requested)) return GameMode::Transition;
    if (current == kStoryMode && requested == kStoryMode) return GameMode::Offline;
    if (current == kStoryModeDefault && requested == kStoryModeDefault) return GameMode::Offline;
    return GameMode::Unknown;
}

// Tick on one thread at a time, at most once per minSpacing microseconds.
// Observed 2026-11-25: script calls come from worker threads, and the first
// thread seen never called again, so a gate pinned for good ticked exactly once.
// The pin therefore moves to another thread once its own has been quiet for
// repinAfter microseconds.
class TickGate {
public:
    explicit TickGate(std::uint64_t minSpacingUs, std::uint64_t repinAfterUs = 250000)
        : spacing_(minSpacingUs), repin_(repinAfterUs) {}
    bool Allow(std::uint32_t threadId, std::uint64_t nowUs) {
        if (!pinned_) {
            pinned_ = true;
            thread_ = threadId;
        } else if (threadId != thread_) {
            if (nowUs - last_ < repin_) return false;
            thread_ = threadId;  // the pinned thread went quiet: follow the scripts
            ++repins_;
        } else if (nowUs - last_ < spacing_) {
            return false;
        }
        last_ = nowUs;
        return true;
    }
    std::uint32_t Thread() const { return thread_; }
    std::uint64_t Repins() const { return repins_; }

private:
    std::uint64_t spacing_;
    std::uint64_t repin_;
    std::uint64_t repins_ = 0;
    std::uint64_t last_ = 0;
    std::uint32_t thread_ = 0;
    bool pinned_ = false;
};

}  // namespace shvi::gta6
