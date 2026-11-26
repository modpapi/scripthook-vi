/*
 * ScriptHook VI - C SDK (stable ABI).
 *
 * ABI rules (see docs/ARCHITECTURE.md, "SDK ABI"):
 *   - Plain C, x64, no C++ types, no name mangling. Plugins never link an import
 *     library: the host hands them a function table (shvi_api) at load time.
 *   - shvi_api is append-only. A field never moves, changes type or meaning.
 *     New functions go at the end and bump SHVI_ABI_MINOR; api->size tells a
 *     plugin which fields exist. Breaking changes bump SHVI_ABI_MAJOR, and the
 *     host refuses plugins built for another major.
 *   - Every function that can fail returns an int status (SHVI_OK = 0).
 *
 * Threading: native_* and script_wait may only be called from a script
 * callback (a fiber the host runs on the game's script thread). Never call
 * them from shvi_plugin_load or from your own threads.
 *
 * Scope: story mode / offline only. The host stops every script permanently
 * as soon as it detects an online session; plugins can't override that.
 */
#ifndef SHVI_H
#define SHVI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Platforms -------------------------------------------------------------
   One SDK, one source tree per mod, one binary per platform:
     SHVI_PLATFORM_WIN64  PC / Windows x64: the mod is a DLL named <mod>.asi
     SHVI_PLATFORM_PS5    PS5 (ps5-payload-sdk, prospero-clang): an ELF shared object <mod>.so
     SHVI_PLATFORM_SIM    desktop simulator (shvi_sim): runs a mod offline with no game, for
                          tests and CI (a .so on Linux/macOS, a .dll on Windows)
   The SDK's CMake passes -DSHVI_PLATFORM_<NAME>=1; otherwise it is detected here. */
#if !defined(SHVI_PLATFORM_WIN64) && !defined(SHVI_PLATFORM_PS5) && !defined(SHVI_PLATFORM_SIM)
#  if defined(__PROSPERO__) || defined(__PS5__)
#    define SHVI_PLATFORM_PS5 1
#  elif defined(_WIN64)
#    define SHVI_PLATFORM_WIN64 1
#  else
#    define SHVI_PLATFORM_SIM 1
#  endif
#endif
#if defined(SHVI_PLATFORM_WIN64)
#  define SHVI_PLATFORM_NAME "win64"
#elif defined(SHVI_PLATFORM_PS5)
#  define SHVI_PLATFORM_NAME "ps5"
#else
#  define SHVI_PLATFORM_NAME "sim"
#endif

#if defined(_MSC_VER)
#  define SHVI_INLINE static __inline
#else
#  define SHVI_INLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHVI_ABI_MAJOR 1
#define SHVI_ABI_MINOR 6

#define SHVI_OK               0
#define SHVI_E_INVALID_ARG   -1
#define SHVI_E_NOT_READY     -2
#define SHVI_E_BLOCKED       -3 /* online guard latched */
#define SHVI_E_UNSUPPORTED   -4
#define SHVI_E_ABI_MISMATCH  -5

typedef void (*shvi_script_fn)(void* user);
typedef void (*shvi_keyboard_fn)(uint32_t vk, int is_down, void* user);
typedef void (*shvi_tick_fn)(void* user);

/* ABI 1.4: one D3D12 present, as the overlay sees it (DXGI/D3D12 pointers as void*
   so this header needs no DirectX headers). */
typedef struct shvi_present_info {
    uint32_t struct_size;              /* sizeof(shvi_present_info), for growth */
    uint32_t width, height, format;    /* back buffer size; format is a DXGI_FORMAT */
    void* swap_chain;                  /* IDXGISwapChain3* */
    void* device;                      /* ID3D12Device* */
    void* queue;                       /* ID3D12CommandQueue* the swap chain presents on */
    void* command_list;                /* ID3D12GraphicsCommandList*, open; back buffer bound as RTV 0
                                          in RENDER_TARGET state. Record only: never Close or execute it. */
    uint32_t back_buffer_index;
} shvi_present_info;
typedef void (*shvi_present_fn)(const shvi_present_info* info, void* user);

/* GTA VI scripts hold entities as 16-byte references, not int handles. */
typedef struct shvi_entity_ref { uint64_t lo, hi; } shvi_entity_ref;
#define SHVI_ENTITY_PED     0
#define SHVI_ENTITY_VEHICLE 1
#define SHVI_ENTITY_OBJECT  2
#define SHVI_ENTITY_PICKUP  3

typedef struct shvi_api {
    /* --- header, never changes --------------------------------------- */
    uint32_t size;        /* sizeof(shvi_api) as built by the host        */
    uint16_t abi_major;
    uint16_t abi_minor;

    /* --- ABI 1.0 ------------------------------------------------------- */
    void        (*log)(const char* message);
    const char* (*game_name)(void);            /* profile name             */
    uint32_t    (*game_build)(void);           /* exe build number          */
    int         (*is_blocked)(void);           /* 1 once the guard latched  */

    /* Register a script fiber. module identifies your plugin: pass SHVI_THIS_MODULE
       (on Windows that is &__ImageBase; elsewhere any address inside your binary). */
    int  (*script_register)(void* module, shvi_script_fn fn, void* user);
    void (*module_unregister)(void* module);
    void (*script_wait)(uint32_t ms);

    /* Native invocation, ScriptHookV-style. native_call returns a pointer to
       the return slots (valid until the next native_init). */
    void      (*native_init)(uint64_t hash);
    void      (*native_push64)(uint64_t value);
    uint64_t* (*native_call)(void);

    uint64_t* (*global_ptr)(int32_t index);    /* NULL if unsupported      */
    /* Called on the game window's thread with (virtual key, 1 = down / 0 = up);
       key repeat arrives as more downs. Don't call natives from it: set a flag
       and act on it in your script. Removed by module_unregister. */
    int       (*keyboard_register)(shvi_keyboard_fn fn, void* user);

    /* --- ABI 1.1 ------------------------------------------------------- */
    /* Plain per-frame callback on the game thread, run before the script
       fibers, NOT in a fiber: natives are allowed, script_wait is not (return
       instead). For hosts that drive their own scheduling (the .NET host).
       Removed by module_unregister. Check SHVI_API_HAS(api, tick_register). */
    int       (*tick_register)(void* module, shvi_tick_fn fn, void* user);

    /* --- ABI 1.2 ------------------------------------------------------- */
    /* ScriptHookV's worldGetAll*: fills up to `max` references to the live
       entities of `kind` (SHVI_ENTITY_*) and returns how many exist (may be
       more than max), or a negative SHVI_E_* status. A kind whose pool the
       profile has not located returns SHVI_E_UNSUPPORTED. Script callbacks
       only, like native_call. Check SHVI_API_HAS(api, world_get_all). */
    int       (*world_get_all)(uint32_t kind, shvi_entity_ref* out, int max);
    /* ScriptHookV's getScriptHandleBaseAddress: the engine object behind a
       reference, or NULL. Read-only use is the safe default. Script callbacks only. */
    void*     (*entity_ptr)(const shvi_entity_ref* ref);

    /* --- ABI 1.3 ------------------------------------------------------- */
    /* GTA VI has no global array: shared script state lives in module
       statics, one copy per script instance. Returns the address of byte
       `field` of module `module_hash`'s statics in the first running
       instance of program `program_hash` (module_hash 0 = the program's own
       statics), or NULL. Hashes are atStringHash of program names (the
       ScriptStore keys). Script callbacks only. Check SHVI_API_HAS(api, static_ptr). */
    void*     (*static_ptr)(uint32_t program_hash, uint32_t module_hash, uint32_t field);

    /* --- ABI 1.4 ------------------------------------------------------- */
    /* Draw on the game's D3D12 back buffer (ScriptHookV's presentCallbackRegister).
       fn runs on the present thread every frame after the overlay's own draw list,
       with `info` valid only during the call. It may record into info->command_list
       and must not close or execute it. Natives are refused there (it is neither a
       script nor a tick callback). A callback that faults is unregistered. Returns
       SHVI_OK, SHVI_E_INVALID_ARG (null, duplicate, or not registered) or
       SHVI_E_BLOCKED. Check SHVI_API_HAS(api, present_register). */
    int       (*present_register)(shvi_present_fn fn, void* user);
    int       (*present_unregister)(shvi_present_fn fn, void* user);

    /* --- ABI 1.5 ------------------------------------------------------- */
    /* 1 if `hash` (as passed to native_init) is bound to something callable in
       this game build, else 0. Lets a menu grey out what the installed native
       table does not provide instead of calling into nothing. Any thread.
       Check SHVI_API_HAS(api, native_exists). */
    int       (*native_exists)(uint64_t hash);

    /* --- ABI 1.6 ------------------------------------------------------- */
    /* Runs a function of a linked script program on the script thread the
       current tick borrowed: `program_hash` is the program's name hash (the
       ScriptStore key), `code_offset` the function's offset in its code.
       `args`/`argc` and `ret`/`retc` are 8-byte cells. Returns SHVI_OK,
       SHVI_E_NOT_READY (program not linked, or no script thread right now),
       SHVI_E_INVALID_ARG, SHVI_E_BLOCKED or SHVI_E_UNSUPPORTED. The function
       runs to its return; an async one hands back its task at once. Script
       callbacks only. Check SHVI_API_HAS(api, script_function_call). */
    int       (*script_function_call)(uint32_t program_hash, uint32_t code_offset, const uint64_t* args,
                                      uint32_t argc, uint64_t* ret, uint32_t retc);
} shvi_api;

/* Plugin entry points (exported by the plugin with these exact C names). */
#if defined(_WIN32)
#  define SHVI_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define SHVI_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
typedef int  (*shvi_plugin_load_fn)(const shvi_api* api);
typedef void (*shvi_plugin_unload_fn)(void);

/* Helpers -------------------------------------------------------------- */
SHVI_INLINE uint64_t shvi_f2u(float f)   { uint32_t b; memcpy(&b, &f, 4); return b; }
SHVI_INLINE float    shvi_u2f(uint64_t u){ uint32_t b = (uint32_t)u; float f; memcpy(&f, &b, 4); return f; }
SHVI_INLINE uint64_t shvi_p2u(const void* p) { return (uint64_t)(uintptr_t)p; }

/* Engine functions (GTA VI) are named, not numbered: their hash is
   SHVI_ENGINE_HASH_TAG | atStringHash(name). atStringHash folds ASCII case,
   hashes '\' as '/', and with a leading '"' stops at the closing quote.
   shvi_engine_hash("STRING_HASH") is what a profile binds that name under. */
#define SHVI_ENGINE_HASH_TAG 0x5649000000000000ULL /* "VI" */
SHVI_INLINE uint64_t shvi_engine_hash(const char* s) {
    uint32_t h = 0;
    int quoted = (*s == '"');
    if (quoted) ++s;
    for (; *s && !(quoted && *s == '"'); ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        else if (c == '\\') c = '/';
        h += (uint8_t)c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return SHVI_ENGINE_HASH_TAG | h;
}

/* True if the host's table has `field` (fields are append-only). Lets a plugin
   built against a newer minor still run on an older host. */
#define SHVI_API_HAS(api, field)     ((api)->size >= offsetof(shvi_api, field) + sizeof(((shvi_api*)0)->field))

/* Checks that the host speaks our major and has every field we were built with. */
SHVI_INLINE int shvi_api_compatible(const shvi_api* api) {
    return api && api->abi_major == SHVI_ABI_MAJOR && api->size >= sizeof(shvi_api);
}

#ifdef __cplusplus
}
#endif

/* SHVI_THIS_MODULE: the handle a plugin passes to script_register, tick_register and
   module_unregister. Older plugins pass the module base (&__ImageBase, which the linker
   provides). The SDK passes a per-module anchor instead, on every platform, and the host
   maps it to its module (GetModuleHandleEx on Windows, dladdr on ELF platforms). */
#ifdef __cplusplus
extern "C" {
#endif
/* Not const: a const variable at namespace scope has internal linkage in C++, and the anchor must be
   one object per module (weak / selectany merges the copies from every translation unit). */
#if defined(_MSC_VER)
__declspec(selectany) char shvi_module_anchor = 0;
#else
__attribute__((weak, visibility("hidden"))) char shvi_module_anchor = 0;
#endif
#ifdef __cplusplus
}
#endif
#define SHVI_THIS_MODULE ((void*)&shvi_module_anchor)

#endif /* SHVI_H */
