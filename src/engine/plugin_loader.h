// PORTABLE. Loads plugins (shvi.h entry points) into a Host: LoadLibrary on Windows, dlopen elsewhere.
#pragma once
#include "host.h"

#include <string>
#include <vector>

namespace shvi::engine {

struct Plugin {
    std::string path;
    void* handle = nullptr;  // HMODULE / dlopen handle
    void* owner = nullptr;   // what the host files its scripts and callbacks under
    int status = SHVI_E_NOT_READY;  // shvi_plugin_load's result, or a loader error
    std::string error;
};

class PluginSet {
public:
    explicit PluginSet(Host& host) : host_(host) {}
    ~PluginSet();
    PluginSet(const PluginSet&) = delete;
    PluginSet& operator=(const PluginSet&) = delete;

    // One plugin file. Returns its status (SHVI_OK when shvi_plugin_load accepted).
    int LoadFile(const std::string& path);
    // Every file in dir ending in ext (".asi", ".so", ".dll"), sorted by name. Returns how many loaded OK.
    int LoadDir(const std::string& dir, const std::string& ext);
    // Calls shvi_plugin_unload where exported, removes owners, closes handles.
    void UnloadAll();

    const std::vector<Plugin>& List() const { return plugins_; }

private:
    Host& host_;
    std::vector<Plugin> plugins_;
};

// The platform's plugin file extension: ".asi" (win64), ".so" (ps5, sim on POSIX), ".dll" (sim on Windows).
const char* DefaultPluginExtension();

}  // namespace shvi::engine
