#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <ucontext.h>
#endif

namespace shvi::engine {

enum class State { Ready, Done, Crashed, Removed };

struct Scheduler::Caller {
#if defined(_WIN32)
    void* fiber = nullptr;
#else
    ucontext_t ctx{};
#endif
};

struct Scheduler::Script {
    int id = 0;
    void* owner = nullptr;
    ScriptFn fn = nullptr;
    void* user = nullptr;
    std::size_t stack = 0;
    std::uint64_t wakeAt = 0;
    State state = State::Ready;
    Scheduler* sched = nullptr;
    bool started = false;
#if defined(_WIN32)
    void* fiber = nullptr;
#else
    ucontext_t ctx{};
    std::unique_ptr<unsigned char[]> stackMem;
#endif
};

namespace {

thread_local Scheduler::Script* t_current = nullptr;

void RunBody(Scheduler::Script* s) {
    try {
        s->fn(s->user);
        s->state = State::Done;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[shvi] script %d threw: %s\n", s->id, e.what());
        s->state = State::Crashed;
    } catch (...) {
        std::fprintf(stderr, "[shvi] script %d threw a non-std exception\n", s->id);
        s->state = State::Crashed;
    }
}

#if defined(_WIN32)
void __stdcall FiberEntry(void* p);
#else
// makecontext passes int arguments: split the pointer in two.
void UcontextEntry(unsigned lo, unsigned hi);
#endif

}  // namespace

// Friend-free access: the entry trampolines only need the caller context, reached through sched.
struct SchedulerAccess {
#if defined(_WIN32)
    static void* CallerFiber(Scheduler::Script* s);
#else
    static ucontext_t* CallerCtx(Scheduler::Script* s);
#endif
};

namespace {
#if defined(_WIN32)
void __stdcall FiberEntry(void* p) {
    auto* s = static_cast<Scheduler::Script*>(p);
    RunBody(s);
    for (;;) SwitchToFiber(SchedulerAccess::CallerFiber(s));  // a fiber proc must never return
}
#else
void UcontextEntry(unsigned lo, unsigned hi) {
    auto* s = reinterpret_cast<Scheduler::Script*>((static_cast<std::uintptr_t>(hi) << 32) | lo);
    RunBody(s);
    setcontext(SchedulerAccess::CallerCtx(s));
}
#endif
}  // namespace

Scheduler::Scheduler(Clock clock) : clock_(std::move(clock)), caller_(std::make_unique<Caller>()) {
    if (!clock_)
        clock_ = [] {
            using namespace std::chrono;
            return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        };
}

Scheduler::~Scheduler() {
#if defined(_WIN32)
    for (auto& s : scripts_)
        if (s->fiber) DeleteFiber(s->fiber);
#endif
}

int Scheduler::Add(void* owner, ScriptFn fn, void* user, std::size_t stackSize) {
    auto s = std::make_unique<Script>();
    std::lock_guard lock(mutex_);
    s->id = nextId_++;
    s->owner = owner;
    s->fn = fn;
    s->user = user;
    s->stack = stackSize ? stackSize : kDefaultStack;
    s->sched = this;
    int id = s->id;
    pending_.push_back(std::move(s));
    return id;
}

void Scheduler::RemoveOwner(void* owner) {
    std::lock_guard lock(mutex_);
    removals_.push_back(owner);
}

void Scheduler::KillAll() {
    std::lock_guard lock(mutex_);
    killAll_ = true;
}

void Scheduler::Tick() {
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
#if defined(_WIN32)
    caller_->fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (!caller_->fiber) return;
#endif
    const std::uint64_t now = clock_();
    for (std::size_t i = 0; i < scripts_.size(); ++i) {
        Script* s = scripts_[i].get();
        if (s->state != State::Ready || now < s->wakeAt) continue;
        if (!s->started) {
            s->started = true;
#if defined(_WIN32)
            s->fiber = CreateFiber(s->stack, &FiberEntry, s);
            if (!s->fiber) { s->state = State::Crashed; continue; }
#else
            s->stackMem.reset(new (std::nothrow) unsigned char[s->stack]);
            if (!s->stackMem || getcontext(&s->ctx) != 0) { s->state = State::Crashed; continue; }
            s->ctx.uc_stack.ss_sp = s->stackMem.get();
            s->ctx.uc_stack.ss_size = s->stack;
            s->ctx.uc_link = &caller_->ctx;
            auto p = reinterpret_cast<std::uintptr_t>(s);
            makecontext(&s->ctx, reinterpret_cast<void (*)()>(&UcontextEntry), 2,
                        static_cast<unsigned>(p & 0xFFFFFFFFu), static_cast<unsigned>(p >> 32));
#endif
        }
        t_current = s;
#if defined(_WIN32)
        SwitchToFiber(s->fiber);
#else
        swapcontext(&caller_->ctx, &s->ctx);
#endif
        t_current = nullptr;
    }
    Reap();
}

void Scheduler::Reap() {
    auto dead = [](const std::unique_ptr<Script>& s) { return s->state == State::Removed || s->state == State::Done; };
    for (auto& s : scripts_) {
        if (s->state == State::Ready) continue;
#if defined(_WIN32)
        if (s->fiber) { DeleteFiber(s->fiber); s->fiber = nullptr; }
#else
        s->stackMem.reset();
#endif
    }
    scripts_.erase(std::remove_if(scripts_.begin(), scripts_.end(), dead), scripts_.end());
}

void Scheduler::Wait(std::uint32_t ms) {
    Script* s = t_current;
    if (!s) return;
    s->wakeAt = s->sched->clock_() + ms;
#if defined(_WIN32)
    SwitchToFiber(s->sched->caller_->fiber);
#else
    swapcontext(&s->ctx, &s->sched->caller_->ctx);
#endif
}

bool Scheduler::InScript() { return t_current != nullptr; }

std::size_t Scheduler::Live() const {
    std::lock_guard lock(mutex_);
    std::size_t n = pending_.size();
    for (auto& s : scripts_) n += s->state == State::Ready ? 1 : 0;
    return n;
}

std::size_t Scheduler::Crashed() const {
    std::size_t n = 0;
    for (auto& s : scripts_) n += s->state == State::Crashed ? 1 : 0;
    return n;
}

#if defined(_WIN32)
void* SchedulerAccess::CallerFiber(Scheduler::Script* s) { return s->sched->caller_->fiber; }
#else
ucontext_t* SchedulerAccess::CallerCtx(Scheduler::Script* s) { return &s->sched->caller_->ctx; }
#endif

}  // namespace shvi::engine
