#include "host.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif !defined(SHVI_PLATFORM_PS5)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <dlfcn.h>
#endif

namespace shvi::engine {

namespace {
Host* g_host = nullptr;
}

void* PlatformModuleOf(void* address) {
    if (!address) return nullptr;
#if defined(_WIN32)
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
#elif defined(SHVI_PLATFORM_PS5)
    return nullptr;  // the PS5 runtime passes its own ownerOf (it records each plugin's load range)
#else
    Dl_info info;
    if (dladdr(address, &info) && info.dli_fbase) return info.dli_fbase;
    return nullptr;
#endif
}

// The C ABI thunks. Each forwards to the one Host.
struct HostAbi {
    static Host& H() { return *g_host; }

    static void Log(const char* m) { H().Log("[plugin] %s", m ? m : "(null)"); }
    static const char* GameName() { return g_host ? H().cfg_.gameName.c_str() : ""; }
    static std::uint32_t GameBuild() { return g_host ? H().cfg_.gameBuild : 0; }
    static int IsBlocked() { return g_host && H().Blocked() ? 1 : 0; }

    static int ScriptRegister(void* module, shvi_script_fn fn, void* user) {
        if (!g_host || !fn) return SHVI_E_INVALID_ARG;
        if (H().Blocked()) return SHVI_E_BLOCKED;
        H().scheduler_.Add(H().OwnerOf(module), fn, user);
        return SHVI_OK;
    }
    static void ModuleUnregister(void* module) {
        if (g_host) H().RemoveOwner(H().OwnerOf(module));
    }
    static void ScriptWait(std::uint32_t ms) { Scheduler::Wait(ms); }

    static bool InCallback() {
        if (Scheduler::InScript() || H().inTick_) return true;
        if (!H().warnedOffThread_) {
            H().warnedOffThread_ = true;
            H().Log("native_call outside a script callback refused");
        }
        return false;
    }
    static void NativeInit(std::uint64_t hash) {
        if (g_host) H().natives_->Begin(hash);
    }
    static void NativePush64(std::uint64_t v) {
        if (g_host) H().natives_->Push(v);
    }
    static std::uint64_t* NativeCall() {
        if (!g_host) return nullptr;
        Host& h = H();
        std::memset(h.zero_, 0, sizeof(h.zero_));
        if (h.Blocked() || !InCallback()) return h.zero_;
        return h.natives_->Call();
    }
    static int NativeExists(std::uint64_t hash) { return g_host && H().natives_->Exists(hash) ? 1 : 0; }

    static std::uint64_t* GlobalPtr(std::int32_t) { return nullptr; }

    static int KeyboardRegister(shvi_keyboard_fn fn, void* user) {
        if (!g_host || !fn) return SHVI_E_INVALID_ARG;
        if (H().Blocked()) return SHVI_E_BLOCKED;
        std::lock_guard lock(H().cbMutex_);
        H().keys_.push_back({H().OwnerOf(reinterpret_cast<void*>(fn)), reinterpret_cast<void*>(fn), user});
        return SHVI_OK;
    }
    static int TickRegister(void* module, shvi_tick_fn fn, void* user) {
        if (!g_host || !fn) return SHVI_E_INVALID_ARG;
        if (H().Blocked()) return SHVI_E_BLOCKED;
        std::lock_guard lock(H().cbMutex_);
        H().ticks_.push_back({H().OwnerOf(module), reinterpret_cast<void*>(fn), user});
        return SHVI_OK;
    }

    // Not provided by the portable engine (they need a game profile): the plugin sees UNSUPPORTED/NULL
    // and gates the feature, as on a Windows build whose profile lacks the target.
    static int WorldGetAll(std::uint32_t, shvi_entity_ref*, int) { return SHVI_E_UNSUPPORTED; }
    static void* EntityPtr(const shvi_entity_ref*) { return nullptr; }
    static void* StaticPtr(std::uint32_t, std::uint32_t, std::uint32_t) { return nullptr; }
    static int PresentRegister(shvi_present_fn, void*) { return SHVI_E_UNSUPPORTED; }
    static int PresentUnregister(shvi_present_fn, void*) { return SHVI_E_INVALID_ARG; }
    static int ScriptFunctionCall(std::uint32_t, std::uint32_t, const std::uint64_t*, std::uint32_t, std::uint64_t*,
                                  std::uint32_t) {
        return SHVI_E_UNSUPPORTED;
    }
    static int ScriptObjectPin(std::uint64_t, std::uint64_t*) { return SHVI_E_UNSUPPORTED; }
    static int ScriptObjectUnpin(std::uint64_t) { return SHVI_E_UNSUPPORTED; }
};

Host::Host(HostConfig cfg)
    : cfg_(std::move(cfg)),
      natives_(cfg_.natives ? cfg_.natives : &nullNatives_),
      scheduler_(cfg_.clock) {
    g_host = this;
    api_.size = sizeof(shvi_api);
    api_.abi_major = SHVI_ABI_MAJOR;
    api_.abi_minor = SHVI_ABI_MINOR;
    api_.log = &HostAbi::Log;
    api_.game_name = &HostAbi::GameName;
    api_.game_build = &HostAbi::GameBuild;
    api_.is_blocked = &HostAbi::IsBlocked;
    api_.script_register = &HostAbi::ScriptRegister;
    api_.module_unregister = &HostAbi::ModuleUnregister;
    api_.script_wait = &HostAbi::ScriptWait;
    api_.native_init = &HostAbi::NativeInit;
    api_.native_push64 = &HostAbi::NativePush64;
    api_.native_call = &HostAbi::NativeCall;
    api_.global_ptr = &HostAbi::GlobalPtr;
    api_.keyboard_register = &HostAbi::KeyboardRegister;
    api_.tick_register = &HostAbi::TickRegister;
    api_.world_get_all = &HostAbi::WorldGetAll;
    api_.entity_ptr = &HostAbi::EntityPtr;
    api_.static_ptr = &HostAbi::StaticPtr;
    api_.present_register = &HostAbi::PresentRegister;
    api_.present_unregister = &HostAbi::PresentUnregister;
    api_.native_exists = &HostAbi::NativeExists;
    api_.script_function_call = &HostAbi::ScriptFunctionCall;
    api_.script_object_pin = &HostAbi::ScriptObjectPin;
    api_.script_object_unpin = &HostAbi::ScriptObjectUnpin;
}

Host::~Host() {
    if (g_host == this) g_host = nullptr;
}

Host* Host::Current() { return g_host; }

void Host::Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (cfg_.log) cfg_.log(line);
    else std::fprintf(stderr, "%s\n", line);
}

void* Host::OwnerOf(void* address) const {
    if (loadingOwner_) return loadingOwner_;
    void* m = cfg_.ownerOf ? cfg_.ownerOf(address) : PlatformModuleOf(address);
    return m ? m : address;
}

void Host::Frame() {
    if (Blocked()) return;
    std::vector<Callback> ticks;
    {
        std::lock_guard lock(cbMutex_);
        ticks = ticks_;
    }
    inTick_ = true;
    for (auto& t : ticks) reinterpret_cast<shvi_tick_fn>(t.fn)(t.user);
    inTick_ = false;
    scheduler_.Tick();
}

void Host::Key(std::uint32_t vk, bool down) {
    if (Blocked()) return;
    std::vector<Callback> keys;
    {
        std::lock_guard lock(cbMutex_);
        keys = keys_;
    }
    for (auto& k : keys) reinterpret_cast<shvi_keyboard_fn>(k.fn)(vk, down ? 1 : 0, k.user);
}

void Host::Block(const char* why) {
    if (blocked_.exchange(true)) return;
    Log("online guard latched: %s; every script is stopped for this session", why ? why : "?");
    scheduler_.KillAll();
    std::lock_guard lock(cbMutex_);
    ticks_.clear();
    keys_.clear();
}

void Host::RemoveOwner(void* owner) {
    scheduler_.RemoveOwner(owner);
    std::lock_guard lock(cbMutex_);
    auto gone = [owner](const Callback& c) { return c.owner == owner; };
    ticks_.erase(std::remove_if(ticks_.begin(), ticks_.end(), gone), ticks_.end());
    keys_.erase(std::remove_if(keys_.begin(), keys_.end(), gone), keys_.end());
}

}  // namespace shvi::engine
