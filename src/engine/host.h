// PORTABLE. The plugin-facing engine: builds the shvi_api table (sdk/include/shvi/shvi.h) on top
// of a platform's native backend, clock and log, and drives scripts, tick and key callbacks.
//
// Used by the offline simulator (tools/sim) and the PS5 runtime (platforms/ps5/runtime). The
// Windows game core (src/core) predates it and keeps its own, in-game verified, implementation
// of the same ABI; both obey the same contract, which tests/engine checks against this one.
//
// One Host per process (the C ABI has no context pointer); Host::Current() is that instance.
#pragma once
#include "scheduler.h"

#include <shvi/shvi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace shvi::engine {

// How natives reach the game on this platform. All calls come from a script or tick callback.
class NativeBackend {
public:
    virtual ~NativeBackend() = default;
    virtual void Begin(std::uint64_t hash) = 0;
    virtual void Push(std::uint64_t value) = 0;
    virtual std::uint64_t* Call() = 0;  // return slots, valid until the next Begin
    virtual bool Exists(std::uint64_t hash) = 0;
};

// A backend with nothing bound: every call returns zeros. The default until a platform has a
// native table for the running build (PS5 today).
class NullNatives final : public NativeBackend {
public:
    void Begin(std::uint64_t) override { ret_[0] = ret_[1] = ret_[2] = ret_[3] = 0; }
    void Push(std::uint64_t) override {}
    std::uint64_t* Call() override { return ret_; }
    bool Exists(std::uint64_t) override { return false; }

private:
    std::uint64_t ret_[4] = {};
};

struct HostConfig {
    std::string gameName = "GTA VI";
    std::uint32_t gameBuild = 0;
    std::string platform = SHVI_PLATFORM_NAME;
    std::function<void(const char*)> log;       // one line, no newline
    Scheduler::Clock clock;                     // ms; default steady_clock
    NativeBackend* natives = nullptr;           // default NullNatives
    std::function<void*(void*)> ownerOf;        // address -> module; default: platform lookup
};

class Host {
public:
    explicit Host(HostConfig cfg);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    static Host* Current();
    const shvi_api* Api() const { return &api_; }

    // Once per frame on the script thread: tick callbacks, then script fibers.
    void Frame();
    // Input from the platform (window messages, pad mapping); any thread.
    void Key(std::uint32_t vk, bool down);
    // Online guard: stop every script for the rest of the session.
    void Block(const char* why);
    bool Blocked() const { return blocked_.load(); }
    void RemoveOwner(void* owner);

    // While a plugin's load function runs, everything it registers belongs to it, whatever handle
    // it passes (the loader sets this; platforms without dladdr rely on it).
    void SetLoadingOwner(void* owner) { loadingOwner_ = owner; }

    Scheduler& Scripts() { return scheduler_; }
    void Log(const char* fmt, ...);
    void* OwnerOf(void* address) const;

private:
    friend struct HostAbi;
    struct Callback { void* owner; void* fn; void* user; };

    HostConfig cfg_;
    NullNatives nullNatives_;
    NativeBackend* natives_;
    Scheduler scheduler_;
    shvi_api api_{};
    std::atomic<bool> blocked_{false};
    std::mutex cbMutex_;
    std::vector<Callback> ticks_, keys_;
    bool inTick_ = false;
    void* loadingOwner_ = nullptr;
    bool warnedOffThread_ = false;
    std::uint64_t zero_[4] = {};
};

// The platform's own address -> module lookup (GetModuleHandleEx / dladdr). nullptr if unknown.
void* PlatformModuleOf(void* address);

}  // namespace shvi::engine
