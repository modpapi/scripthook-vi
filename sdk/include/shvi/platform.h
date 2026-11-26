/*
 * ScriptHook VI - platform helpers for plugins (header only, optional).
 *
 * Lets one mod source build for every SHVI platform (see shvi.h, "Platforms") without
 * #ifdefs in the mod: where the plugin file is, and where its data lives.
 *
 *   shvi_mod_path(buf, n)   full path of this plugin binary
 *   shvi_mod_data_dir(buf, n)  the "shvi" folder, with a trailing separator:
 *       win64  <game>\shvi\           (the plugin is <game>\shvi\plugins\<mod>.asi)
 *       ps5    /data/shvi/            (the plugin is /data/shvi/plugins/<mod>.so)
 *       sim    the folder above the plugin's folder, like win64
 *
 * Both return the string length, or 0 on failure (buf is then "").
 */
#ifndef SHVI_PLATFORM_H
#define SHVI_PLATFORM_H

#include "shvi.h"

#include <stddef.h>
#include <string.h>

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

#ifdef __cplusplus
extern "C" {
#endif

#define SHVI_PS5_DATA_DIR "/data/shvi/"

SHVI_INLINE size_t shvi_mod_path(char* buf, size_t n) {
    if (!buf || !n) return 0;
    buf[0] = 0;
#if defined(_WIN32)
    {
        HMODULE m = NULL;
        DWORD len;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)SHVI_THIS_MODULE, &m))
            return 0;
        len = GetModuleFileNameA(m, buf, (DWORD)n);
        if (len == 0 || len >= n) { buf[0] = 0; return 0; }
        return len;
    }
#elif defined(SHVI_PLATFORM_PS5)
    /* The PS5 runtime loads plugins from a fixed folder and does not expose dladdr()
       portably; the plugin's own name is not needed by the SDK. */
    {
        size_t len = strlen(SHVI_PS5_DATA_DIR "plugins/");
        if (len >= n) return 0;
        memcpy(buf, SHVI_PS5_DATA_DIR "plugins/", len + 1);
        return len;
    }
#else
    {
        Dl_info info;
        size_t len;
        if (!dladdr(SHVI_THIS_MODULE, &info) || !info.dli_fname) return 0;
        len = strlen(info.dli_fname);
        if (len >= n) return 0;
        memcpy(buf, info.dli_fname, len + 1);
        return len;
    }
#endif
}

SHVI_INLINE size_t shvi_mod_data_dir(char* buf, size_t n) {
#if defined(SHVI_PLATFORM_PS5)
    size_t len = strlen(SHVI_PS5_DATA_DIR);
    if (!buf || len >= n) return 0;
    memcpy(buf, SHVI_PS5_DATA_DIR, len + 1);
    return len;
#else
    /* <...>/shvi/plugins/<mod> -> <...>/shvi/ */
    size_t len = shvi_mod_path(buf, n), i, cut = 0, seps = 0;
    if (!len) return 0;
    for (i = len; i > 0; --i) {
        if (buf[i - 1] == '/' || buf[i - 1] == '\\') {
            if (++seps == 2) { cut = i; break; }
        }
    }
    if (!cut) { buf[0] = 0; return 0; }
    buf[cut] = 0;
    return cut;
#endif
}

#ifdef __cplusplus
}
#endif
#endif /* SHVI_PLATFORM_H */
