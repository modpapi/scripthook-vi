/* Sample plugin (plain C, ABI 1): calls a GTA VI engine function by name.
 * Build output: hello_engine.asi -> <game>\shvi\plugins\
 *
 * Engine functions are bound by the game profile and named, not numbered:
 * shvi_engine_hash("STRING_HASH") is the game's own atStringHash(const char*, u32 seed),
 * found by signature and validated at startup. Every 5 seconds this logs the
 * game's hash of a string next to the SDK's own, which must agree.
 */
#include <shvi/shvi.h>

#include <stdio.h>
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;
static const shvi_api* g_api;

static void script_main(void* user) {
    char text[160];
    const char* name = "ScriptHookVI";
    (void)user;
    snprintf(text, sizeof(text), "hello_engine: running on %s build %u", g_api->game_name(), g_api->game_build());
    g_api->log(text);
    for (;;) {
        uint32_t game;
        g_api->native_init(shvi_engine_hash("STRING_HASH"));
        g_api->native_push64(shvi_p2u(name));
        g_api->native_push64(0);
        game = (uint32_t)g_api->native_call()[0];
        snprintf(text, sizeof(text), "hello_engine: game atStringHash(\"%s\") = 0x%08X, SDK = 0x%08X", name, game,
                 (uint32_t)shvi_engine_hash(name));
        g_api->log(text);
        g_api->script_wait(5000);
    }
}

SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* api) {
    if (!shvi_api_compatible(api)) return SHVI_E_ABI_MISMATCH;
    g_api = api;
    return api->script_register(&__ImageBase, script_main, NULL);
}
