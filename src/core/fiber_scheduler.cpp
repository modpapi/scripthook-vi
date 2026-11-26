#include "fiber_scheduler.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <exception>

namespace shvi {
namespace {

thread_local void* t_current = nullptr;  // FiberScheduler::Script* of the running fiber

void CallScriptCpp(FiberScheduler::ScriptFn fn, void* user, bool* threw) {
    try {
        fn(user);
    } catch (const std::exception& e) {
        SHVI_LOG("script threw: %s", e.what());
        *threw = true;
    } catch (...) {
        SHVI_LOG("script threw a non-std exception");
        *threw = true;
    }
}

// SEH guard: an access violation in a script kills that script, not the game.
// Destructors of the faulting frames do not run (/EHsc); the script is never resumed.
bool CallScriptSeh(FiberScheduler::ScriptFn fn, void* user, bool* threw, DWORD* code) {
    __try {
        CallScriptCpp(fn, user, threw);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

FiberScheduler::FiberScheduler(Clock clock) : clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return static_cast<std::uint64_t>(GetTickCount64()); };
}

FiberScheduler::~FiberScheduler() {
    for (auto& s : scripts_)
        if (s->fiber && s->fiber != GetCurrentFiber()) DeleteFiber(s->fiber);
}

int FiberScheduler::Add(void* owner, ScriptFn fn, void* user, std::size_t stackSize) {
    auto s = std::make_unique<Script>();
    std::lock_guard lock(mutex_);
    int id = nextId_++;
    *s = Script{id, owner, fn, user, stackSize};
    s->sched = this;
    pending_.push_back(std::move(s));
    return id;
}

void FiberScheduler::RemoveOwner(void* owner) {
    std::lock_guard lock(mutex_);
    removals_.push_back(owner);
}

void FiberScheduler::KillAll() {
    std::lock_guard lock(mutex_);
    killAll_ = true;
}

void __stdcall FiberScheduler::FiberProc(void* param) {
    auto* s = static_cast<Script*>(param);
    RunScript(s);
    // A fiber proc must never return (that would exit the game thread).
    for (;;) SwitchToFiber(s->sched->callerFiber_);
}

void FiberScheduler::RunScript(Script* s) {
    bool threw = false;
    DWORD code = 0;
    if (!CallScriptSeh(s->fn, s->user, &threw, &code)) {
        SHVI_LOG("script %d crashed with SEH exception 0x%08lX; it will not be resumed", s->id, code);
        s->state = State::Crashed;
    } else {
        s->state = threw ? State::Crashed : State::Done;
    }
}

void FiberScheduler::Tick() {
    {
        std::lock_guard lock(mutex_);
        for (auto& p : pending_) scripts_.push_back(std::move(p));
        pending_.clear();
        for (void* owner : removals_)
            for (auto& s : scripts_)
                if (s->owner == owner) s->state = State::Removed;
        removals_.clear();
        if (killAll_)
            for (auto& s : scripts_) s->state = State::Removed;
    }

    // Remember who ticked us: the game thread may already be a fiber (another
    // hook, or the game itself), and it can change between ticks.
    callerFiber_ = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (!callerFiber_) return;

    const std::uint64_t now = clock_();
    for (std::size_t i = 0; i < scripts_.size(); ++i) {
        Script* s = scripts_[i].get();
        if (s->state != State::Ready || now < s->wakeAt) continue;
        if (!s->fiber) {
            s->fiber = CreateFiber(s->stack, &FiberScheduler::FiberProc, s);
            if (!s->fiber) {
                SHVI_LOG("CreateFiber failed (%lu) for script %d", GetLastError(), s->id);
                s->state = State::Crashed;
                continue;
            }
        }
        t_current = s;
        SwitchToFiber(s->fiber);
        t_current = nullptr;
    }
    Reap();
}

void FiberScheduler::Reap() {
    auto dead = [](const std::unique_ptr<Script>& s) { return s->state == State::Removed || s->state == State::Done; };
    for (auto& s : scripts_)
        if (dead(s) && s->fiber) {
            DeleteFiber(s->fiber);
            s->fiber = nullptr;
        }
    scripts_.erase(std::remove_if(scripts_.begin(), scripts_.end(), dead), scripts_.end());
    // Crashed scripts are kept (fiber freed) so Crashed() can report them.
    for (auto& s : scripts_)
        if (s->state == State::Crashed && s->fiber) {
            DeleteFiber(s->fiber);
            s->fiber = nullptr;
        }
}

void FiberScheduler::Wait(std::uint32_t ms) {
    auto* s = static_cast<Script*>(t_current);
    if (!s) return;
    s->wakeAt = s->sched->clock_() + ms;
    SwitchToFiber(s->sched->callerFiber_);
    // Resumed by a later Tick(); t_current was set again by Tick().
}

bool FiberScheduler::InScript() { return t_current != nullptr; }

std::size_t FiberScheduler::Live() const {
    std::lock_guard lock(mutex_);
    std::size_t n = pending_.size();
    for (auto& s : scripts_) n += s->state == State::Ready;
    return n;
}

std::size_t FiberScheduler::Crashed() const {
    std::size_t n = 0;
    for (auto& s : scripts_) n += s->state == State::Crashed;
    return n;
}

}  // namespace shvi
