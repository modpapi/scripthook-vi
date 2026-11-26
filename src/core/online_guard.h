// GENERIC policy, game-specific probes (GameProfile::QueryMode / preflight lists).
//
// Fail-closed and one-way:
//   * Scripts run only after the profile has reported Offline for
//     kArmTicks consecutive ticks.
//   * Unknown (probe natives missing, loading screens) never allows scripts.
//   * Online or Transition latches the guard for the rest of the process:
//     every script is killed and native calls from plugins return zero.
//     There is no "un-latch"; restart the game to mod again.
//   * Preflight: if an anti-cheat client module is loaded, or the command
//     line asks for online, the core never installs its hooks at all.
//     This is the opposite of evasion: we back off when protection is present.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace shvi {

enum class GameMode { Unknown, Offline, Transition, Online };
const char* ToString(GameMode m);

class OnlineGuard {
public:
    static constexpr int kArmTicks = 30;

    struct Preflight {
        bool ok = true;
        std::string reason;
    };
    using ModuleLoaded = std::function<bool(std::wstring_view)>;
    static Preflight Check(std::span<const std::wstring_view> antiCheatModules,
                           std::span<const std::wstring_view> onlineFlags, std::wstring_view commandLine,
                           const ModuleLoaded& isLoaded);

    void Report(GameMode m);  // game thread, once per tick
    void Latch(std::string reason);

    bool Allowed() const { return !latched_.load() && armed_; }
    bool Latched() const { return latched_.load(); }
    std::string Reason() const;
    GameMode Last() const { return last_; }

private:
    std::atomic<bool> latched_{false};
    bool armed_ = false;
    int offlineStreak_ = 0;
    GameMode last_ = GameMode::Unknown;
    mutable std::mutex reasonMutex_;
    std::string reason_;
};

}  // namespace shvi
