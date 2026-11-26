// PORTABLE. Cooperative script scheduler for every platform that is not the Windows game core
// (the offline simulator, the PS5 runtime). Same model as src/core/fiber_scheduler.h: Tick() runs
// once per frame on the thread that owns the game's script context, and each ready script runs on
// its own stack until it calls Wait(), which switches back to Tick().
//
// Stacks: Windows fibers on Windows, ucontext (makecontext/swapcontext) elsewhere. A script that
// throws a C++ exception is marked crashed and never resumed; on POSIX a hardware fault in a
// script is NOT recovered here (the platform runtime decides whether to install signal guards).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace shvi::engine {

class Scheduler {
public:
    using ScriptFn = void (*)(void* user);
    using Clock = std::function<std::uint64_t()>;  // milliseconds

    explicit Scheduler(Clock clock = {});
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Thread-safe; the script starts on the next Tick().
    int Add(void* owner, ScriptFn fn, void* user, std::size_t stackSize = 0);
    // Thread-safe; the owner's scripts stop and are freed on the next Tick().
    void RemoveOwner(void* owner);
    void KillAll();  // the online guard latched

    void Tick();  // script thread only

    // From inside a script: yield for at least ms. Outside a script: no-op.
    static void Wait(std::uint32_t ms);
    static bool InScript();

    std::size_t Live() const;
    std::size_t Crashed() const;

    static constexpr std::size_t kDefaultStack = 1u << 20;  // 1 MiB: menus build std::function trees

    struct Script;  // public for the platform entry trampoline

private:
    friend struct SchedulerAccess;
    void Reap();

    Clock clock_;
    struct Caller;
    std::unique_ptr<Caller> caller_;
    std::vector<std::unique_ptr<Script>> scripts_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Script>> pending_;
    std::vector<void*> removals_;
    bool killAll_ = false;
    int nextId_ = 1;
};

}  // namespace shvi::engine
