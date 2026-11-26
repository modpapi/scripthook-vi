// shvi_core.dll: wires the generic core to a game profile and exposes the
// C ABI to plugins. Started by the proxy loader on its own thread (never
// under the loader lock).
//
// Game folder layout:
//   <game>\dinput8.dll                 proxy loader (or another proxy, see pick_proxy)
//   <game>\shvi\shvi_core.dll          this
//   <game>\shvi\shvi.log
//   <game>\shvi\crossmap\<build>.txt   optional hash crossmap
//   <game>\shvi\denylist.txt          optional natives plugins may never call
//   <game>\shvi\plugins\*.asi          plugins (C ABI, shvi.h)
#include "callbacks.h"
#include "crossmap.h"
#include "denylist.h"
#include "fiber_scheduler.h"
#include "game_profile.h"
#include "host_info.h"
#include "invoker.h"
#include "log.h"
#include "online_guard.h"
#include "overlay_d3d12.h"
#include "overlay_drawlist.h"
#include "shvi/shvi.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shvi {
namespace {

struct Core {
    HostInfo host;
    std::unique_ptr<GameProfile> profile;
    std::unique_ptr<Invoker> invoker;
    FiberScheduler scheduler;
    OnlineGuard guard;
    Crossmap crossmap;
    Denylist denylist;
    CallbackList<> tickCallbacks;              // ABI 1.1 tick_register
    ReadyGate ready;                           // T2.6: game in play + guard
    CallbackList<std::uint32_t, int> keys;     // keyboard_register
    std::atomic<HWND> window{nullptr};
    std::atomic<LONG_PTR> origWndProc{0};
    bool windowAnsi = false;
    std::vector<std::wstring_view> antiCheat;
    std::vector<HMODULE> plugins;
    std::uint64_t waitHash = 0;
    std::string gameName;
    // native_init/push/call state for the WAIT interception
    bool pendingWait = false;
    std::uint32_t pendingWaitMs = 0;
    int pushes = 0;
    std::uint32_t ticks = 0;
    std::atomic<bool> warnedOffThread{false};
};
Core* g = nullptr;
alignas(16) std::uint64_t g_zero[4] = {};
thread_local bool t_inTickCallback = false;  // natives allowed, WAIT not

// ------------------------------------------------------------------ keyboard
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto orig = reinterpret_cast<WNDPROC>(g->origWndProc.load());
    if (auto ev = TranslateKeyMessage(msg, w, l); ev && !g->guard.Latched())
        g->keys.Run(ev->vk, ev->down ? 1 : 0);
    return g->windowAnsi ? CallWindowProcA(orig, hwnd, msg, w, l) : CallWindowProcW(orig, hwnd, msg, w, l);
}

BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(out) = hwnd;
    return FALSE;
}

// Subclass the game window once it exists (it may appear after we start).
void TryInstallKeyboardHook() {
    if (g->window.load() || g->keys.Size() == 0) return;
    HWND hwnd = nullptr;
    EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (!hwnd) return;
    g->windowAnsi = !IsWindowUnicode(hwnd);  // keep an ANSI window ANSI
    // Publish the original proc BEFORE swapping it in: the window thread may
    // deliver a message to HookedWndProc as soon as the swap happens.
    LONG_PTR orig = g->windowAnsi ? GetWindowLongPtrA(hwnd, GWLP_WNDPROC) : GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!orig) return;
    g->origWndProc = orig;
    auto hook = reinterpret_cast<LONG_PTR>(&HookedWndProc);
    LONG_PTR prev = g->windowAnsi ? SetWindowLongPtrA(hwnd, GWLP_WNDPROC, hook)
                                  : SetWindowLongPtrW(hwnd, GWLP_WNDPROC, hook);
    if (prev != orig) {  // changed under us (or failed): put back what was there, try later
        if (prev) {
            if (g->windowAnsi) SetWindowLongPtrA(hwnd, GWLP_WNDPROC, prev);
            else SetWindowLongPtrW(hwnd, GWLP_WNDPROC, prev);
        }
        return;
    }
    g->window = hwnd;
    SHVI_LOG("keyboard: subclassed window %p (%s)", static_cast<void*>(hwnd), g->windowAnsi ? "ANSI" : "Unicode");
}

// ------------------------------------------------------------------ tick
// Logged from the tick (every 300 ticks) only when something but the frame count changed.
void LogOverlayStatusIfChanged() {
    static overlay::Status last{};
    static bool logged = false;
    overlay::Status s = overlay::GetStatus();
    if (logged && s.attached == last.attached && s.source == last.source && s.width == last.width &&
        s.height == last.height && s.format == last.format && s.disabled == last.disabled)
        return;
    logged = true;
    last = s;
    const char* src = s.source == overlay::QueueSource::Creation          ? "creation hook"
                      : s.source == overlay::QueueSource::ExecuteFallback ? "ExecuteCommandLists"
                                                                          : "none";
    SHVI_LOG("overlay status: %s, queue via %s, %ux%u format %u, %llu frames drawn%s",
             s.attached ? "attached" : "not attached", src, s.width, s.height, s.format,
             static_cast<unsigned long long>(s.frames), s.disabled ? ", DISABLED" : "");
}

// The guard latched: drop the menu and stop all overlay GPU work (the core is inert from here on).
void OverlayOffForLatch() {
    overlay::FinishTickDrawList(false);
    overlay::SetEnabled(false);
}

void OnTick() {  // game script thread, once per frame
    if (!g || g->guard.Latched()) return;

    // Cheap periodic re-check: an anti-cheat client that shows up later latches us.
    if (g->ticks % 60 == 0) TryInstallKeyboardHook();
    if (++g->ticks % 300 == 0) {
        for (auto m : g->antiCheat)
            if (GetModuleHandleW(std::wstring(m).c_str()))
                g->guard.Latch("anti-cheat client module appeared at runtime");
        LogOverlayStatusIfChanged();
    }
    if (!g->guard.Latched()) g->guard.Report(g->profile->QueryMode(*g->invoker));
    if (g->guard.Latched()) {
        g->scheduler.KillAll();
        g->scheduler.Tick();  // reaps the fibers
        OverlayOffForLatch();  // the guard just latched: the menu vanishes, the overlay goes inert
        return;
    }
    bool open = g->ready.Update(g->profile->GameReady(), g->guard.Allowed());
    if (g->ready.Changed())
        SHVI_LOG("game-ready gate %s", open ? "open: plugins run" : "closed: plugins paused");
    if (!open) {
        if (g->ready.Changed()) overlay::FinishTickDrawList(false);  // gate just closed: drop the last frame
        return;
    }
    t_inTickCallback = true;
    g->tickCallbacks.Run();
    t_inTickCallback = false;
    if (g->guard.Latched()) {
        OverlayOffForLatch();
        return;
    }
    g->scheduler.Tick();
    if (g->guard.Latched()) OverlayOffForLatch();
    else overlay::FinishTickDrawList(true);
}

// ------------------------------------------------------------------ C ABI
void RemovePresentOwner(HMODULE owner);  // ABI 1.4, below
HMODULE ModuleOf(const void* address);    // below
// Plugins pass SHVI_THIS_MODULE: &__ImageBase, or (SDK >= the multi-platform SDK) a per-module
// anchor inside the image. Both name the same owner once mapped to the containing module.
void* OwnerOf(void* module) {
    HMODULE m = module ? ModuleOf(module) : nullptr;
    return m ? static_cast<void*>(m) : module;
}
void ApiLog(const char* m) { SHVI_LOG("[plugin] %s", m ? m : "(null)"); }
const char* ApiGameName() { return g ? g->gameName.c_str() : ""; }
std::uint32_t ApiGameBuild() { return g && g->profile ? g->profile->Build() : 0; }
int ApiIsBlocked() { return g && g->guard.Latched() ? 1 : 0; }

int ApiScriptRegister(void* module, shvi_script_fn fn, void* user) {
    if (!g || !fn) return SHVI_E_INVALID_ARG;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    g->scheduler.Add(OwnerOf(module), fn, user);
    return SHVI_OK;
}
void ApiModuleUnregister(void* module) {
    if (!g) return;
    module = OwnerOf(module);
    g->scheduler.RemoveOwner(module);
    g->tickCallbacks.RemoveOwner(module);
    g->keys.RemoveOwner(module);
    RemovePresentOwner(static_cast<HMODULE>(module));
}
void ApiScriptWait(std::uint32_t ms) { FiberScheduler::Wait(ms); }

void ApiNativeInit(std::uint64_t hash) {
    if (!g || !g->invoker) return;
    g->pendingWait = g->waitHash && hash == g->waitHash;
    g->pendingWaitMs = 0;
    g->pushes = 0;
    g->invoker->Begin(hash);
}
void ApiNativePush64(std::uint64_t v) {
    if (!g || !g->invoker) return;
    if (g->pendingWait && g->pushes == 0) g->pendingWaitMs = static_cast<std::uint32_t>(v);
    ++g->pushes;
    g->invoker->Push64(v);
}
std::uint64_t* ApiNativeCall() {
    std::memset(g_zero, 0, sizeof(g_zero));
    if (!g || !g->invoker) return g_zero;
    if (!FiberScheduler::InScript() && !t_inTickCallback) {  // not on the game thread in a callback: refuse
        if (!g->warnedOffThread.exchange(true))
            SHVI_LOG("native_call outside a script callback refused (natives need the game's script thread)");
        return g_zero;
    }
    if (g->pendingWait) {
        g->pendingWait = false;
        if (FiberScheduler::InScript()) FiberScheduler::Wait(g->pendingWaitMs);
        // in a tick callback WAIT would stall the game thread: ignored (return instead)
        return g_zero;
    }
    return g->invoker->Call();
}
int ApiNativeExists(std::uint64_t hash) {
    if (!g || !g->invoker) return 0;
    return g->invoker->HasMapped(hash) ? 1 : 0;
}
std::uint64_t* ApiGlobalPtr(std::int32_t index) {
    if (!g || !g->profile || g->guard.Latched()) return nullptr;
    return g->profile->GlobalPtr(index);
}
HMODULE ModuleOf(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
}
int ApiKeyboardRegister(shvi_keyboard_fn fn, void* user) {
    if (!g || !fn) return SHVI_E_INVALID_ARG;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    // ABI 1.0 passes no module: the owner is the module that contains fn.
    g->keys.Add(ModuleOf(reinterpret_cast<const void*>(fn)), fn, user);
    return SHVI_OK;
}
int ApiTickRegister(void* module, shvi_tick_fn fn, void* user) {
    if (!g || !fn) return SHVI_E_INVALID_ARG;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    g->tickCallbacks.Add(OwnerOf(module), fn, user);
    return SHVI_OK;
}

bool InGameCallback(const char* what) {
    if (FiberScheduler::InScript() || t_inTickCallback) return true;
    if (!g->warnedOffThread.exchange(true))
        SHVI_LOG("%s outside a script callback refused (it needs the game's script thread)", what);
    return false;
}
int ApiWorldGetAll(std::uint32_t kind, shvi_entity_ref* out, int max) {
    if (!g || !g->profile || max < 0 || (max > 0 && !out)) return SHVI_E_INVALID_ARG;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    if (!InGameCallback("world_get_all")) return SHVI_E_NOT_READY;
    static_assert(sizeof(shvi_entity_ref) == 2 * sizeof(std::uint64_t));
    return g->profile->WorldGetAll(kind, reinterpret_cast<std::uint64_t*>(out), max);
}
void* ApiEntityPtr(const shvi_entity_ref* ref) {
    if (!g || !g->profile || !ref || g->guard.Latched()) return nullptr;
    if (!InGameCallback("entity_ptr")) return nullptr;
    return g->profile->EntityPtr(reinterpret_cast<const std::uint64_t*>(ref));
}

void* ApiStaticPtr(std::uint32_t program, std::uint32_t module, std::uint32_t field) {
    if (!g || !g->profile || g->guard.Latched()) return nullptr;
    if (!InGameCallback("static_ptr")) return nullptr;
    return g->profile->StaticPtr(program, module, field);
}

int ApiScriptFunctionCall(std::uint32_t program, std::uint32_t offset, const std::uint64_t* args, std::uint32_t argc,
                          std::uint64_t* ret, std::uint32_t retc) {
    if (!g || !g->profile) return SHVI_E_NOT_READY;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    if ((argc && !args) || (retc && !ret) || argc > 64 || retc > 64) return SHVI_E_INVALID_ARG;
    if (!InGameCallback("script_function_call")) return SHVI_E_NOT_READY;
    return g->profile->CallScriptFunction(program, offset, args, argc, ret, retc);
}

// ABI 1.4: each plugin registration is its own overlay callback (a trampoline with
// {fn, user} as its user pointer), so a faulting plugin is dropped alone. Entries
// are never freed: a present in flight on this thread may still hold one.
struct PresentEntry {
    shvi_present_fn fn;
    void* user;
    HMODULE owner;
    bool active;
};
std::mutex g_presentMu;
std::vector<PresentEntry*> g_presents;

void PresentTrampoline(const overlay::PresentContext& c, void* p) {
    auto* e = static_cast<PresentEntry*>(p);
    if (!g || g->guard.Latched()) return;
    shvi_present_info info{};
    info.struct_size = sizeof(info);
    info.width = c.width;
    info.height = c.height;
    info.format = c.format;
    info.swap_chain = c.swapChain;
    info.device = c.device;
    info.queue = c.queue;
    info.command_list = c.list;
    info.back_buffer_index = c.backBufferIndex;
    e->fn(&info, e->user);
}

int ApiPresentRegister(shvi_present_fn fn, void* user) {
    if (!g || !fn) return SHVI_E_INVALID_ARG;
    if (g->guard.Latched()) return SHVI_E_BLOCKED;
    // The overlay's lock is taken outside g_presentMu: a present callback may call us.
    auto* e = new PresentEntry{fn, user, ModuleOf(reinterpret_cast<const void*>(fn)), true};
    {
        std::lock_guard<std::mutex> lock(g_presentMu);
        for (PresentEntry* o : g_presents)
            if (o->active && o->fn == fn && o->user == user) {
                delete e;
                return SHVI_E_INVALID_ARG;
            }
        g_presents.push_back(e);
    }
    int rc = overlay::RegisterPresent(&PresentTrampoline, e);
    if (rc != SHVI_OK) {
        std::lock_guard<std::mutex> lock(g_presentMu);
        e->active = false;
    }
    return rc;
}

// Marks matching entries inactive, then drops them from the overlay (outside g_presentMu).
template <class Match>
int DropPresents(Match match) {
    std::vector<PresentEntry*> drop;
    {
        std::lock_guard<std::mutex> lock(g_presentMu);
        for (PresentEntry* e : g_presents)
            if (e->active && match(*e)) {
                e->active = false;
                drop.push_back(e);
            }
    }
    for (PresentEntry* e : drop) overlay::UnregisterPresent(&PresentTrampoline, e);  // may be gone (faulted)
    return static_cast<int>(drop.size());
}

int ApiPresentUnregister(shvi_present_fn fn, void* user) {
    if (!g || !fn) return SHVI_E_INVALID_ARG;
    return DropPresents([&](const PresentEntry& e) { return e.fn == fn && e.user == user; }) ? SHVI_OK
                                                                                            : SHVI_E_INVALID_ARG;
}

void RemovePresentOwner(HMODULE owner) {
    DropPresents([&](const PresentEntry& e) { return e.owner == owner; });
}

const shvi_api g_api = {
    sizeof(shvi_api), SHVI_ABI_MAJOR, SHVI_ABI_MINOR,
    ApiLog, ApiGameName, ApiGameBuild, ApiIsBlocked,
    ApiScriptRegister, ApiModuleUnregister, ApiScriptWait,
    ApiNativeInit, ApiNativePush64, ApiNativeCall,
    ApiGlobalPtr, ApiKeyboardRegister,
    ApiTickRegister,  // 1.1
    ApiWorldGetAll, ApiEntityPtr,  // 1.2
    ApiStaticPtr,                  // 1.3
    ApiPresentRegister, ApiPresentUnregister,  // 1.4
    ApiNativeExists,                           // 1.5
    ApiScriptFunctionCall,                     // 1.6
};

// ------------------------------------------------------------------ startup
std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void LoadPlugins() {
    std::wstring dir = g->host.exeDir + L"\\shvi\\plugins\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.asi").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        SHVI_LOG("no plugins in %s", Narrow(dir).c_str());
        return;
    }
    do {
        std::wstring path = dir + fd.cFileName;
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            SHVI_LOG("plugin %s: LoadLibrary failed (%lu)", Narrow(fd.cFileName).c_str(), GetLastError());
            continue;
        }
        auto load = reinterpret_cast<shvi_plugin_load_fn>(GetProcAddress(m, "shvi_plugin_load"));
        if (!load) {
            SHVI_LOG("plugin %s: no shvi_plugin_load export, skipped",
                     Narrow(fd.cFileName).c_str());
            FreeLibrary(m);
            continue;
        }
        int rc = load(&g_api);
        SHVI_LOG("plugin %s: load returned %d", Narrow(fd.cFileName).c_str(), rc);
        if (rc != SHVI_OK) {
            g->scheduler.RemoveOwner(m);
            continue;  // kept loaded: it may have started threads; unloading is phase 2 (hot reload)
        }
        g->plugins.push_back(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Everything after the first Resolve: more tries while the code is still packed, then the
// invoker, plugins and the tick hook. Runs on the caller's thread, or on a background thread
// for a hosted start whose first Resolve asked for a retry.
int FinishStart(std::unique_ptr<Core> core, pe::Module exe, ResolveResult rr, std::string scanLog) {
    HostInfo& host = core->host;
    for (int attempt = 1; attempt < 240 && rr == ResolveResult::Retry; ++attempt) {  // ~2 min: packed code
        Sleep(500);
        rr = core->profile->Resolve(host, exe, &scanLog);
    }
    SHVI_LOG("signature scan:\n%s", scanLog.c_str());
    if (rr != ResolveResult::Ok) {
        SHVI_LOG("required signatures missing for %s build %u; staying inert", core->gameName.c_str(),
                 core->profile->Build());
        overlay::SetEnabled(false);
        return SHVI_E_UNSUPPORTED;
    }

    GameProfile* prof = core->profile.get();
    core->invoker = std::make_unique<Invoker>([prof](std::uint64_t h) { return prof->FindTarget(h); },
                                              prof->VectorFixup());
    Core* raw = core.get();
    core->invoker->SetGate([raw] { return raw->guard.Allowed(); });
    std::wstring cm = host.exeDir + L"\\shvi\\crossmap\\" + std::to_wstring(prof->Build()) + L".txt";
    if (core->crossmap.LoadFile(cm)) {
        core->invoker->SetCrossmap(&core->crossmap);
        SHVI_LOG("crossmap: %zu entries", core->crossmap.size());
    }
    std::size_t badDeny = 0;
    if (core->denylist.LoadFile(host.exeDir + L"\\shvi\\denylist.txt", &badDeny)) {
        core->invoker->SetDenylist(&core->denylist);
        SHVI_LOG("denylist: %zu natives denied (%zu bad lines)", core->denylist.size(), badDeny);
    }
    core->waitHash = prof->WaitNativeHash();

    g = core.release();  // lives for the process
    LoadPlugins();
    if (!g->profile->InstallTick(&OnTick)) {
        SHVI_LOG("tick hook failed; staying inert");
        overlay::SetEnabled(false);
        g->guard.Latch("tick hook failed");
        return SHVI_E_UNSUPPORTED;
    }
    SHVI_LOG("ready: %zu plugin(s); scripts start after %d offline ticks", g->plugins.size(), OnlineGuard::kArmTicks);
    return SHVI_OK;
}

// hosted: the host (VIctory) calls us before title code runs, so Resolve runs once here and a
// Retry finishes on a background thread instead of blocking the host.
int Start(HostInfo hostIn, bool hosted) {
    auto core = std::make_unique<Core>();
    core->host = std::move(hostIn);
    HostInfo& host = core->host;

    CreateDirectoryW((host.exeDir + L"\\shvi").c_str(), nullptr);
    log::Init(host.exeDir + L"\\shvi\\shvi.log");
    SHVI_LOG("ScriptHook VI core, ABI %d.%d. Host %s %s (%s start)", SHVI_ABI_MAJOR, SHVI_ABI_MINOR,
             Narrow(host.exeName).c_str(), host.version.ToString().c_str(), hosted ? "hosted" : "proxy");

    std::vector<std::unique_ptr<GameProfile>> profiles = CreateGta6Profiles();
    for (auto& p : profiles)
        if (p->Matches(host)) { core->profile = std::move(p); break; }
    if (!core->profile) {
        SHVI_LOG("unsupported host executable; staying inert");
        return SHVI_E_UNSUPPORTED;
    }
    core->gameName = core->profile->Name();

    // Preflight guard: before touching any game code.
    core->antiCheat = core->profile->AntiCheatModules();
    auto flags = core->profile->OnlineCommandLineFlags();
    auto pre = OnlineGuard::Check(core->antiCheat, flags, GetCommandLineW(),
                                  [](std::wstring_view m) { return GetModuleHandleW(std::wstring(m).c_str()) != nullptr; });
    if (!pre.ok) {
        SHVI_LOG("preflight refused: %s", pre.reason.c_str());
        return SHVI_E_BLOCKED;
    }

    // T7.1 overlay: generic DXGI hooks, in before the host creates its swap chain when we can.
    // A failure is logged and the hook runs on without drawing.
    // shvi\overlay.off: no DXGI hooks at all (to tell the overlay's effects apart from the host's).
    if (GetFileAttributesW((host.exeDir + L"\\shvi\\overlay.off").c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHVI_LOG("overlay: off (shvi\\overlay.off)");
    } else if (overlay::Install(&overlay::GlobalDrawList())) {
        bool pattern = GetFileAttributesW((host.exeDir + L"\\shvi\\overlay_test.on").c_str()) != INVALID_FILE_ATTRIBUTES;
        overlay::SetTestPattern(pattern);
        SHVI_LOG("overlay: installed%s", pattern ? "; test pattern on (shvi\\overlay_test.on)" : "");
    } else {
        SHVI_LOG("overlay: not installed; plugins run without drawing");
    }

    auto exe = pe::Module::FromLoaded(host.exe);
    if (!exe) {
        overlay::SetEnabled(false);
        return SHVI_E_UNSUPPORTED;
    }
    std::string scanLog;
    ResolveResult rr = core->profile->Resolve(host, *exe, &scanLog);
    if (hosted && rr == ResolveResult::Retry) {
        SHVI_LOG("hosted start: signatures need runtime data; resolving on a background thread");
        std::thread(FinishStart, std::move(core), std::move(*exe), rr, std::move(scanLog)).detach();
        return SHVI_OK;
    }
    return FinishStart(std::move(core), std::move(*exe), rr, std::move(scanLog));
}

int Start() {  // proxy path: the host exe is the process image
    HostInfo host;
    host.exe = GetModuleHandleW(nullptr);
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(host.exe, buf, static_cast<DWORD>(std::size(buf)));
    host = HostInfoFromTitle(host.exe, buf);
    return Start(std::move(host), false);
}

// shvi_core_start and victory_on_title_mapped share one start per process.
int StartOnce(int (*start)(const void*), const void* arg) {
    static std::once_flag once;
    static int rc = SHVI_E_NOT_READY;
    std::call_once(once, [&] { rc = start(arg); });
    return rc;
}

}  // namespace
}  // namespace shvi

extern "C" __declspec(dllexport) int shvi_core_start() {
    return shvi::StartOnce([](const void*) { return shvi::Start(); }, nullptr);
}

// Hosted start (spec §6.1): VIctory calls this once with the title image it mapped, before
// any title code runs. Nonzero = refused (VIctory logs it and runs on).
extern "C" __declspec(dllexport) int __cdecl victory_on_title_mapped(const shvi::VictoryTitleInfo* info) {
    if (int rc = shvi::ValidateTitleInfo(info); rc != SHVI_OK) return rc;
    return shvi::StartOnce(
        [](const void* p) {
            auto* i = static_cast<const shvi::VictoryTitleInfo*>(p);
            return shvi::Start(shvi::HostInfoFromTitle(static_cast<HMODULE>(i->image_base), i->exe_path), true);
        },
        info);
}

extern "C" __declspec(dllexport) const shvi_api* shvi_get_api(std::uint32_t abiMajor) {
    return abiMajor == SHVI_ABI_MAJOR ? &shvi::g_api : nullptr;
}
