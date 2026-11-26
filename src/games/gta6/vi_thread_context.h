// GAME-SPECIFIC: GTA VI. The script-thread context plugin calls run in
// (TASKS T2.7, Q8). VI's equivalent of ScriptHookV's changeScriptThread.
//
// Natives read "the current script thread" from the engine TLS block
// (+0x1478, vi_script.h). The tick runs inside the script manager's update,
// where that slot may be empty or belong to whatever ran last. Three ways to
// give plugin calls a thread, behind one switch (shvi\thread_context.txt):
//
//   none   leave TLS as the engine has it (the default until T3.5 decides)
//   scope  the engine's own run-as-script-thread scope: script_scope_enter
//          (instance, thread, scope, activate = 1, id) ... script_scope_leave
//          (scope). 36 engine call sites do exactly this (RESEARCH_PLAN Q4,
//          eighth pass). Preferred: it also marks the thread active and nests.
//   tls    save TLS +0x1478, store the thread, restore it afterwards. The
//          fallback if the scope functions misbehave outside engine callers.
//
// The instance and thread come from the calling OS thread's own TLS instance
// slots (+0xF80 + 8*slot): the tick thread's script instance.
#pragma once
#include "games/gta6/vi_script.h"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace shvi::gta6 {

enum class ThreadContextMode { None, EngineScope, TlsSwap };

constexpr ThreadContextMode ParseThreadContextMode(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    if (s == "scope") return ThreadContextMode::EngineScope;
    if (s == "tls") return ThreadContextMode::TlsSwap;
    return ThreadContextMode::None;  // anything else, including empty: do nothing
}

struct ThreadContextEngine {
    using ScopeEnterFn = void (*)(void* instance, void* thread, void* scope, bool activate, std::uint32_t id);
    using ScopeLeaveFn = void (*)(void* scope);
    ScopeEnterFn enter = nullptr;
    ScopeLeaveFn leave = nullptr;
    std::uintptr_t (*tlsBlock)() = nullptr;  // engine TLS block of the calling thread (gs:[58h] slot 0)
    std::size_t tlsThreadOffset = 0x1478;    // tls_script_thread
};

// The calling thread's first script instance and its thread, from the TLS slots.
struct ThreadPick {
    std::uintptr_t instance = 0, thread = 0;
};
// Plain-data reads only, under SEH: on a worker thread the slots can hold anything (the first in-game
// run with the scope on crashed here, 2026-11-25). A fault means "no instance on this thread".
inline ThreadPick PickTlsInstance(std::uintptr_t tlsBlock, std::size_t slots = script::instance::kSlotKinds) {
    ThreadPick p;
    if (!tlsBlock) return p;
#if defined(_MSC_VER)
    __try {
#endif
    for (std::size_t i = 0; i < slots; ++i) {
        std::uintptr_t inst;
        std::memcpy(&inst, reinterpret_cast<const void*>(tlsBlock + script::instance::kTlsSlots + 8 * i), 8);
        if (!inst) continue;
        std::uintptr_t thr;
        std::memcpy(&thr, reinterpret_cast<const void*>(inst + script::instance::kThread), 8);
        if (!thr) continue;
        std::uint8_t probe;  // the instance id and the thread's state byte must be readable too
        std::memcpy(&probe, reinterpret_cast<const void*>(inst + script::instance::kId), 1);
        std::memcpy(&probe, reinterpret_cast<const void*>(thr + 0x10), 1);
        p.instance = inst;
        p.thread = thr;
        return p;
    }
#if defined(_MSC_VER)
    } __except (1) {
        p = ThreadPick{};
    }
#endif
    return p;
}

// RAII: enters the chosen context in the constructor, leaves it in the
// destructor. With mode None, or with any piece missing, it does nothing and
// Active() is false.
class ScopedScriptThread {
public:
    ScopedScriptThread(ThreadContextMode mode, const ThreadContextEngine& e) : e_(e) {
        if (mode == ThreadContextMode::None || !e.tlsBlock) return;
        std::uintptr_t tls = e.tlsBlock();
        ThreadPick pick = PickTlsInstance(tls);
        if (!pick.thread) return;
        if (mode == ThreadContextMode::EngineScope) {
            if (!e.enter || !e.leave) return;
            std::uint32_t id;
            std::memcpy(&id, reinterpret_cast<const void*>(pick.instance + script::instance::kId), 4);
            e.enter(reinterpret_cast<void*>(pick.instance), reinterpret_cast<void*>(pick.thread), scope_, true, id);
            mode_ = mode;
        } else {
            slot_ = tls + e.tlsThreadOffset;
            std::memcpy(&saved_, reinterpret_cast<const void*>(slot_), 8);
            std::memcpy(reinterpret_cast<void*>(slot_), &pick.thread, 8);
            mode_ = mode;
        }
    }
    ~ScopedScriptThread() {
        if (mode_ == ThreadContextMode::EngineScope) e_.leave(scope_);
        else if (mode_ == ThreadContextMode::TlsSwap) std::memcpy(reinterpret_cast<void*>(slot_), &saved_, 8);
    }
    ScopedScriptThread(const ScopedScriptThread&) = delete;
    ScopedScriptThread& operator=(const ScopedScriptThread&) = delete;
    bool Active() const { return mode_ != ThreadContextMode::None; }

private:
    ThreadContextEngine e_;
    ThreadContextMode mode_ = ThreadContextMode::None;
    std::uintptr_t slot_ = 0, saved_ = 0;
    alignas(16) std::uint8_t scope_[script::scope::kBufferSize] = {};
};

}  // namespace shvi::gta6
