/* ScriptHook VI PS5 runtime: the portable engine (src/engine) plus PS5 glue, linked together with
 * ONE mod into a self-contained ELF (<mod>.elf). See platforms/ps5/README.md.
 *
 * Lifecycle (called by the in-game entry once the loader has mapped the image, or by runtime_main.cpp
 * in standalone "dry run" mode):
 *   shvi_ps5_runtime_start()        create the host, call the mod's shvi_plugin_load
 *   shvi_ps5_runtime_frame()        once per game frame on the script thread (tick hook)
 *   shvi_ps5_runtime_pad(buttons)   the pad state each frame (mapped to SHVI key codes)
 *   shvi_ps5_runtime_block(why)     online guard: stop the mod for the session
 *   shvi_ps5_runtime_stop()
 */
#ifndef SHVI_PS5_RUNTIME_H
#define SHVI_PS5_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shvi_ps5_runtime_config {
    const char* log_path;   /* default /data/shvi/shvi.log; NULL keeps the default */
    const char* game_name;  /* default "GTA VI (PS5)" */
    uint32_t game_build;    /* 0 until a PS5 profile reads it */
    int to_stdout;          /* also print log lines (host tests, dry run) */
} shvi_ps5_runtime_config;

int  shvi_ps5_runtime_start(const shvi_ps5_runtime_config* cfg);
void shvi_ps5_runtime_frame(void);
void shvi_ps5_runtime_pad(uint32_t buttons);
void shvi_ps5_runtime_block(const char* why);
void shvi_ps5_runtime_stop(void);
/* Scripts still able to run, and scripts that crashed (for the dry run report). */
unsigned shvi_ps5_runtime_live(void);
unsigned shvi_ps5_runtime_crashed(void);

#ifdef __cplusplus
}
#endif
#endif /* SHVI_PS5_RUNTIME_H */
