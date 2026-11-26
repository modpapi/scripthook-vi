#include "plugin_loader.h"

#include <algorithm>
#include <filesystem>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace shvi::engine {

namespace {

void* OpenLib(const std::string& path, std::string& err) {
#if defined(_WIN32)
    std::wstring w = std::filesystem::path(path).wstring();
    HMODULE m = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) err = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
    return m;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        err = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void* Sym(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void CloseLib(void* handle) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

}  // namespace

const char* DefaultPluginExtension() {
#if defined(SHVI_PLATFORM_WIN64)
    return ".asi";
#elif defined(_WIN32)
    return ".dll";
#else
    return ".so";
#endif
}

PluginSet::~PluginSet() { UnloadAll(); }

int PluginSet::LoadFile(const std::string& path) {
    Plugin p;
    p.path = path;
    p.handle = OpenLib(path, p.error);
    if (!p.handle) {
        p.status = SHVI_E_INVALID_ARG;
        host_.Log("plugin %s: %s", path.c_str(), p.error.c_str());
        plugins_.push_back(p);
        return p.status;
    }
    auto load = reinterpret_cast<shvi_plugin_load_fn>(Sym(p.handle, "shvi_plugin_load"));
    if (!load) {
        p.status = SHVI_E_INVALID_ARG;
        p.error = "no shvi_plugin_load export";
        host_.Log("plugin %s: %s, skipped", path.c_str(), p.error.c_str());
        CloseLib(p.handle);
        p.handle = nullptr;
        plugins_.push_back(p);
        return p.status;
    }
    // The owner is the module base where the platform can tell (it is what dladdr / GetModuleHandleEx
    // return for any address inside the plugin), else the handle.
    void* base = PlatformModuleOf(reinterpret_cast<void*>(load));
    p.owner = base ? base : p.handle;
    host_.SetLoadingOwner(p.owner);
    p.status = load(host_.Api());
    host_.SetLoadingOwner(nullptr);
    host_.Log("plugin %s: load -> %d", std::filesystem::path(path).filename().string().c_str(), p.status);
    if (p.status != SHVI_OK) {
        host_.RemoveOwner(p.owner);
        CloseLib(p.handle);
        p.handle = nullptr;
    }
    plugins_.push_back(p);
    return p.status;
}

int PluginSet::LoadDir(const std::string& dir, const std::string& ext) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension().string() == ext) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    int ok = 0;
    for (auto& f : files) ok += LoadFile(f) == SHVI_OK ? 1 : 0;
    return ok;
}

void PluginSet::UnloadAll() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (!it->handle) continue;
        if (auto unload = reinterpret_cast<shvi_plugin_unload_fn>(Sym(it->handle, "shvi_plugin_unload"))) unload();
        host_.RemoveOwner(it->owner);
    }
    // Scripts of removed owners are reaped on the next Tick; run one so no stack points into a closed image.
    host_.Scripts().Tick();
    for (auto& p : plugins_)
        if (p.handle) {
            CloseLib(p.handle);
            p.handle = nullptr;
        }
}

}  // namespace shvi::engine
