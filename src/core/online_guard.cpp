#include "online_guard.h"

#include "log.h"

#include <cwctype>

namespace shvi {

const char* ToString(GameMode m) {
    switch (m) {
    case GameMode::Offline: return "offline";
    case GameMode::Transition: return "transition";
    case GameMode::Online: return "online";
    default: return "unknown";
    }
}

static std::string Narrow(std::wstring_view w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

static bool ContainsNoCase(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (std::size_t k = 0; k < needle.size() && eq; ++k)
            eq = std::towlower(hay[i + k]) == std::towlower(needle[k]);
        if (eq) return true;
    }
    return false;
}

OnlineGuard::Preflight OnlineGuard::Check(std::span<const std::wstring_view> antiCheatModules,
                                          std::span<const std::wstring_view> onlineFlags,
                                          std::wstring_view commandLine, const ModuleLoaded& isLoaded) {
    for (auto m : antiCheatModules) {
        if (isLoaded && isLoaded(m))
            return {false, "anti-cheat client module loaded (" + Narrow(m) +
                               "): online-capable session, ScriptHook VI stays off"};
    }
    for (auto f : onlineFlags) {
        if (ContainsNoCase(commandLine, f))
            return {false, "command line requests online (" + Narrow(f) + ")"};
    }
    return {};
}

void OnlineGuard::Latch(std::string reason) {
    if (latched_.exchange(true)) return;
    {
        std::lock_guard lock(reasonMutex_);
        reason_ = std::move(reason);
    }
    SHVI_LOG("online guard LATCHED: %s. All scripts stopped until the game restarts.", Reason().c_str());
}

std::string OnlineGuard::Reason() const {
    std::lock_guard lock(reasonMutex_);
    return reason_;
}

void OnlineGuard::Report(GameMode m) {
    if (m != last_) SHVI_LOG("online guard: mode %s -> %s", ToString(last_), ToString(m));
    last_ = m;
    if (latched_.load()) return;
    switch (m) {
    case GameMode::Online:
    case GameMode::Transition:
        Latch(std::string("game reported ") + ToString(m));
        armed_ = false;
        break;
    case GameMode::Offline:
        if (++offlineStreak_ >= kArmTicks) armed_ = true;
        break;
    case GameMode::Unknown:
        offlineStreak_ = 0;
        armed_ = false;
        break;
    }
}

}  // namespace shvi
