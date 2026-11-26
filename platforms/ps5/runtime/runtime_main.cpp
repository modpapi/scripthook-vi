// Standalone entry of a PS5 mod image (<mod>.elf): the "dry run".
//
// Sent to the console's ELF loader on its own, the image runs the runtime and the mod in its OWN process
// for a few seconds at 60 frames per second, with no game attached (natives return zeros), then reports
// by notification and in /data/shvi/shvi.log. It proves the mod and runtime build, load and schedule on
// the console before the injector maps the image into the game (platforms/ps5/README.md, "Status").
//
// Built for the desktop too (ctest ps5_runtime_dryrun), where it prints to stdout instead.
//
//   argv[1]  seconds to run (default 10)
//   argv[2]  "menu" = press the menu combo after one second and walk the menu (exercises key input)
#include "runtime.h"

#include <shvi/shvi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "pad_map.h"

#if defined(SHVI_PLATFORM_PS5)
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, std::size_t, int);
static void Notify(const char* msg) {
    notify_request_t req;
    std::memset(&req, 0, sizeof(req));
    std::snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#else
static void Notify(const char* msg) { std::printf("[notify] %s\n", msg); }
#endif

int main(int argc, char** argv) {
    int seconds = argc >= 2 ? std::atoi(argv[1]) : 10;
    bool menu = argc >= 3 && std::strcmp(argv[2], "menu") == 0;
    if (seconds <= 0) seconds = 10;

    shvi_ps5_runtime_config cfg{};
#if !defined(SHVI_PLATFORM_PS5)
    cfg.log_path = "shvi_ps5_dryrun.log";
#endif
    cfg.to_stdout = 1;
    cfg.game_name = "GTA VI (PS5 dry run, no game)";
    int rc = shvi_ps5_runtime_start(&cfg);
    if (rc != SHVI_OK) {
        char m[96];
        std::snprintf(m, sizeof(m), "shvi: mod refused to load (%d)", rc);
        Notify(m);
        shvi_ps5_runtime_stop();
        return 1;
    }

    // A scripted pad sequence for "menu": L1+R3 (open), down, down, cross, circle, L1+R3 (close).
    const struct { int frame; std::uint32_t buttons; } pad[] = {
        {60, SHVI_PAD_L1}, {61, SHVI_PAD_L1 | SHVI_PAD_R3}, {62, 0}, {80, SHVI_PAD_DOWN}, {81, 0},
        {90, SHVI_PAD_DOWN}, {91, 0}, {100, SHVI_PAD_CROSS}, {101, 0}, {120, SHVI_PAD_CIRCLE}, {121, 0},
        {140, SHVI_PAD_L1}, {141, SHVI_PAD_L1 | SHVI_PAD_R3}, {142, 0},
    };
    const int frames = seconds * 60;
    std::size_t next = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        if (menu)
            while (next < sizeof(pad) / sizeof(pad[0]) && pad[next].frame == f) shvi_ps5_runtime_pad(pad[next++].buttons);
        shvi_ps5_runtime_frame();
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(16667LL * (f + 1)));
    }

    unsigned live = shvi_ps5_runtime_live(), crashed = shvi_ps5_runtime_crashed();
    char m[160];
    std::snprintf(m, sizeof(m), "shvi: dry run done, %d frames, %u script(s) live, %u crashed", frames, live, crashed);
    Notify(m);
    shvi_ps5_runtime_stop();
    return crashed ? 2 : 0;
}
