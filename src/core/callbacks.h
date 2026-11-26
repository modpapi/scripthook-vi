// GENERIC. Owner-tagged C callback lists for the C ABI:
//   * CallbackList<>                    ABI 1.1 tick_register: plain per-frame
//                                       callbacks on the game thread, outside fibers
//   * CallbackList<std::uint32_t, int>  keyboard_register: (vk, is_down) from the
//                                       game window's WndProc thread
//
// Thread-safe: Add/RemoveOwner/Run may come from different threads. Run calls a
// snapshot without holding the lock, so a callback may register more callbacks.
// Each call is SEH-guarded; a callback that faults is dropped (the same crash
// isolation the fiber scheduler gives scripts) instead of taking the game down.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace shvi {

template <class... A>
class CallbackList {
public:
    using Fn = void (*)(A..., void* user);

    bool Add(void* owner, Fn fn, void* user) {
        if (!fn) return false;
        std::lock_guard lock(mu_);
        entries_.push_back({owner, fn, user, next_++});
        return true;
    }

    void RemoveOwner(void* owner) {
        std::lock_guard lock(mu_);
        std::erase_if(entries_, [owner](const Entry& e) { return e.owner == owner; });
    }

    void Run(A... a) {
        std::vector<Entry> snapshot;
        {
            std::lock_guard lock(mu_);
            snapshot = entries_;
        }
        std::vector<std::uint64_t> failed;
        for (const Entry& e : snapshot)
            if (!Guarded(e.fn, a..., e.user)) failed.push_back(e.id);
        if (failed.empty()) return;
        faults_ += failed.size();
        std::lock_guard lock(mu_);
        std::erase_if(entries_, [&](const Entry& e) {
            for (auto id : failed)
                if (id == e.id) return true;
            return false;
        });
    }

    std::size_t Size() const {
        std::lock_guard lock(mu_);
        return entries_.size();
    }
    std::size_t FaultCount() const { return faults_; }

private:
    struct Entry {
        void* owner;
        Fn fn;
        void* user;
        std::uint64_t id;
    };

    // No C++ objects in here: __try can't coexist with unwinding in one frame.
    static bool Guarded(Fn fn, A... a, void* user) {
        __try {
            fn(a..., user);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::uint64_t next_ = 1;
    std::atomic<std::size_t> faults_{0};
};

struct KeyEvent {
    std::uint32_t vk;
    bool down;
};

// WM_KEYDOWN / WM_KEYUP / WM_SYSKEYDOWN / WM_SYSKEYUP -> (vk, down). Auto-repeat
// arrives as further "down" events, as classic script-hook keyboard handlers expect.
inline std::optional<KeyEvent> TranslateKeyMessage(UINT msg, WPARAM w, LPARAM /*l*/) {
    bool down;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: down = true; break;
    case WM_KEYUP:
    case WM_SYSKEYUP: down = false; break;
    default: return std::nullopt;
    }
    auto vk = static_cast<std::uint32_t>(w & 0xFF);
    if (vk == 0) return std::nullopt;
    return KeyEvent{vk, down};
}

}  // namespace shvi
