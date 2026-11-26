// GENERIC. Cooperative script scheduler on Windows fibers (the classic script-hook model).
//
// Tick() is called once per frame from the game's script thread (the tick
// hook). Each ready script runs on its own fiber until it calls Wait(), which
// switches back to the fiber that called Tick(). Natives therefore always run
// on the game thread, inside the game's script context, with no locking.
//
// Native code only. Managed runtimes (CLR/CoreCLR) are not fiber-aware; the
// managed layer uses async/await on the game thread instead (ARCHITECTURE.md, E).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace shvi {

class FiberScheduler {
public:
    using ScriptFn = void (*)(void* user);
    using Clock = std::function<std::uint64_t()>;  // milliseconds

    explicit FiberScheduler(Clock clock = {});
    ~FiberScheduler();
    FiberScheduler(const FiberScheduler&) = delete;
    FiberScheduler& operator=(const FiberScheduler&) = delete;

    // Thread-safe; the script starts on the next Tick().
    int Add(void* owner, ScriptFn fn, void* user, std::size_t stackSize = 0);
    // Thread-safe; the owner's fibers stop and are freed on the next Tick().
    void RemoveOwner(void* owner);
    // Stop everything (online guard latch). Thread-safe.
    void KillAll();

    void Tick();  // game thread only

    // From inside a script fiber: yield for at least ms. Outside a fiber: no-op.
    static void Wait(std::uint32_t ms);
    static bool InScript();

    std::size_t Live() const;     // scripts that can still run
    std::size_t Crashed() const;

private:
    enum class State { Ready, Done, Crashed, Removed };
    struct Script {
        int id;
        void* owner;
        ScriptFn fn;
        void* user;
        std::size_t stack;
        void* fiber = nullptr;
        std::uint64_t wakeAt = 0;
        State state = State::Ready;
        FiberScheduler* sched = nullptr;
    };

    static void __stdcall FiberProc(void* param);
    static void RunScript(Script* s);
    void Reap();

    Clock clock_;
    void* callerFiber_ = nullptr;
    std::vector<std::unique_ptr<Script>> scripts_;  // game thread only

    mutable std::mutex mutex_;  // guards the pending lists below
    std::vector<std::unique_ptr<Script>> pending_;
    std::vector<void*> removals_;
    bool killAll_ = false;
    int nextId_ = 1;
};

}  // namespace shvi
