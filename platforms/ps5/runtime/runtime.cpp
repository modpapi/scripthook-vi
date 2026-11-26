// ScriptHook VI PS5 runtime (see runtime.h). Builds for PS5 (prospero-clang) and, for tests, for the
// desktop: the PS5-only parts are behind SHVI_PLATFORM_PS5.
#include "runtime.h"

#include "engine/host.h"
#include "pad_map.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#if defined(SHVI_PLATFORM_PS5)
#  include <ps5/klog.h>
#  include <sys/stat.h>
#endif

// The one mod linked into this image (shvi.h entry points, built with the mod's sources).
extern "C" int shvi_plugin_load(const shvi_api* api);
#if defined(_MSC_VER)  // host tests built with MSVC: no weak symbols, use a linker alias
extern "C" void shvi_plugin_unload(void);
extern "C" void shvi_ps5_no_unload(void) {}
#  pragma comment(linker, "/alternatename:shvi_plugin_unload=shvi_ps5_no_unload")
#  define SHVI_HAS_UNLOAD() (&shvi_plugin_unload != &shvi_ps5_no_unload)
#else
extern "C" __attribute__((weak)) void shvi_plugin_unload(void);
#  define SHVI_HAS_UNLOAD() (&shvi_plugin_unload != nullptr)
#endif

namespace {

using shvi::engine::Host;
using shvi::engine::HostConfig;

char g_owner;  // the image has one module: everything the mod registers belongs to it
std::unique_ptr<Host> g_host;
std::FILE* g_logFile = nullptr;
bool g_stdout = false;
std::mutex g_logMutex;
std::uint32_t g_pad = 0;

void LogLine(const char* line) {
    std::lock_guard lock(g_logMutex);
    if (g_logFile) {
        std::fprintf(g_logFile, "%s\n", line);
        std::fflush(g_logFile);
    }
    if (g_stdout) std::printf("%s\n", line);
#if defined(SHVI_PLATFORM_PS5)
    klog_printf("[shvi] %s\n", line);
#endif
}

void EmitKey(std::uint32_t vk, int down, void*) {
    if (g_host) g_host->Key(vk, down != 0);
}

}  // namespace

extern "C" int shvi_ps5_runtime_start(const shvi_ps5_runtime_config* cfg) {
    if (g_host) return SHVI_E_INVALID_ARG;
    shvi_ps5_runtime_config c{};
    if (cfg) c = *cfg;
    std::string logPath = c.log_path ? c.log_path : "/data/shvi/shvi.log";
#if defined(SHVI_PLATFORM_PS5)
    mkdir("/data", 0777);
    mkdir("/data/shvi", 0777);
#endif
    g_logFile = std::fopen(logPath.c_str(), "a");
    g_stdout = c.to_stdout != 0;

    HostConfig hc;
    hc.gameName = c.game_name ? c.game_name : "GTA VI (PS5)";
    hc.gameBuild = c.game_build;
    hc.platform = "ps5";
    hc.log = &LogLine;
    hc.ownerOf = [](void*) -> void* { return &g_owner; };
    // natives: none bound until a PS5 profile and native table exist (NullNatives, every call returns 0)
    g_host = std::make_unique<Host>(hc);
    g_host->Log("ScriptHook VI PS5 runtime: ABI %d.%d, platform %s", SHVI_ABI_MAJOR, SHVI_ABI_MINOR,
                SHVI_PLATFORM_NAME);
    int rc = shvi_plugin_load(g_host->Api());
    g_host->Log("mod: shvi_plugin_load -> %d", rc);
    if (rc != SHVI_OK) g_host->RemoveOwner(&g_owner);
    return rc;
}

extern "C" void shvi_ps5_runtime_frame(void) {
    if (g_host) g_host->Frame();
}

extern "C" void shvi_ps5_runtime_pad(std::uint32_t buttons) {
    if (!g_host) return;
    shvi_ps5_pad_edges(g_pad, buttons, &EmitKey, nullptr);
    g_pad = buttons;
}

extern "C" void shvi_ps5_runtime_block(const char* why) {
    if (g_host) g_host->Block(why);
}

extern "C" unsigned shvi_ps5_runtime_live(void) {
    return g_host ? static_cast<unsigned>(g_host->Scripts().Live()) : 0;
}

extern "C" unsigned shvi_ps5_runtime_crashed(void) {
    return g_host ? static_cast<unsigned>(g_host->Scripts().Crashed()) : 0;
}

extern "C" void shvi_ps5_runtime_stop(void) {
    if (!g_host) return;
    if (SHVI_HAS_UNLOAD()) shvi_plugin_unload();
    g_host->RemoveOwner(&g_owner);
    g_host->Scripts().Tick();  // reap the mod's scripts
    g_host->Log("runtime stopped");
    g_host.reset();
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}
