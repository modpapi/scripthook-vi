// shvi_sim: run ScriptHook VI plugins offline, with no game.
//
// It hosts the portable engine (src/engine) with a simulated clock and a fake native backend, loads
// plugins, runs N frames and feeds scripted key presses. Natives return zeros, except a few pure ones
// it can answer exactly (STRING_HASH). Draw calls of shvi/vi_ui.hpp (UI_DRAW_TEXT) can be printed, so
// a menu can be checked from text. Built for the desktop: Windows (loads .dll / .asi), Linux, macOS (.so).
//
//   shvi_sim <plugin-file|dir> [options]
//     --frames N          frames to run (default 300)
//     --frame-ms N        simulated ms per frame (default 16)
//     --key F:KEY         at frame F press and release KEY (F4, Up, Down, Left, Right, Return, Back,
//                         Numpad0..9, a letter, or 0xNN); repeatable, or comma-separated
//     --bind NAME[,NAME]  natives that exist (native_exists = 1); --bind-all: every native exists
//     --draw              print UI_DRAW_TEXT calls (the menu as text) on the last frame that drew
//     --trace             print every native call (hash and argument count)
//     --block-at F        latch the online guard at frame F (scripts must stop)
//     --expect TEXT       exit 1 unless a log line contains TEXT (repeatable)
//
// Exit status: 0 when every plugin loaded, no script crashed and every --expect matched.
#include "engine/host.h"
#include "engine/plugin_loader.h"

#include <shvi/shvi.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace shvi::engine;

namespace {

std::uint64_t g_now = 0;
std::vector<std::string> g_log;

void LogLine(const char* s) {
    std::printf("%s\n", s);
    g_log.emplace_back(s);
}

std::uint32_t KeyCode(const std::string& k) {
    static const std::map<std::string, std::uint32_t> named = {
        {"F4", 0x73}, {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27}, {"RETURN", 0x0D},
        {"ENTER", 0x0D}, {"BACK", 0x08}, {"BACKSPACE", 0x08}, {"ESCAPE", 0x1B}, {"SPACE", 0x20}};
    std::string u;
    for (char c : k) u += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    if (auto it = named.find(u); it != named.end()) return it->second;
    if (u.rfind("NUMPAD", 0) == 0 && u.size() == 7 && u[6] >= '0' && u[6] <= '9') return 0x60 + (u[6] - '0');
    if (u.rfind("0X", 0) == 0) return static_cast<std::uint32_t>(std::strtoul(u.c_str() + 2, nullptr, 16));
    if (u.size() == 1) return static_cast<std::uint8_t>(u[0]);
    std::fprintf(stderr, "shvi_sim: unknown key '%s'\n", k.c_str());
    std::exit(2);
}

class SimNatives final : public NativeBackend {
public:
    bool bindAll = false, trace = false, draw = false;
    std::set<std::uint64_t> bound;
    std::vector<std::string> frameText, lastText;
    std::map<std::uint64_t, unsigned> calls;

    void Begin(std::uint64_t h) override {
        hash_ = h;
        args_.clear();
        std::memset(ret_, 0, sizeof(ret_));
    }
    void Push(std::uint64_t v) override { args_.push_back(v); }
    std::uint64_t* Call() override {
        ++calls[hash_];
        if (trace) std::printf("[native] 0x%016llX (%zu args)\n", static_cast<unsigned long long>(hash_), args_.size());
        static const std::uint64_t kStringHash = shvi::sdk::EngineHash("STRING_HASH");
        static const std::uint64_t kDrawText = shvi::sdk::EngineHash("UI_DRAW_TEXT");
        if (hash_ == kStringHash && !args_.empty() && args_[0]) {
            ret_[0] = static_cast<std::uint32_t>(shvi_engine_hash(reinterpret_cast<const char*>(args_[0])));
        } else if (hash_ == kDrawText && draw && !args_.empty() && args_[0]) {
            frameText.emplace_back(reinterpret_cast<const char*>(args_[0]));
        }
        return ret_;
    }
    bool Exists(std::uint64_t h) override { return bindAll || bound.count(h) != 0; }

    void EndFrame() {
        if (!frameText.empty()) lastText.swap(frameText);
        frameText.clear();
    }

private:
    std::uint64_t hash_ = 0;
    std::vector<std::uint64_t> args_;
    std::uint64_t ret_[4] = {};
};

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t a = 0;
    while (a <= s.size()) {
        std::size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: shvi_sim <plugin-file|dir> [--frames N] [--frame-ms N] [--key F:KEY]... [--bind NAMES]\n"
                 "                [--bind-all] [--draw] [--trace] [--block-at F] [--expect TEXT]...\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    std::string target = argv[1];
    int frames = 300, frameMs = 16, blockAt = -1;
    std::multimap<int, std::uint32_t> keys;
    std::vector<std::string> expects;
    SimNatives natives;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) std::exit(Usage());
            return argv[++i];
        };
        if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--frame-ms") frameMs = std::atoi(next().c_str());
        else if (a == "--key") {
            for (auto& k : Split(next())) {
                auto c = k.find(':');
                if (c == std::string::npos) return Usage();
                keys.emplace(std::atoi(k.substr(0, c).c_str()), KeyCode(k.substr(c + 1)));
            }
        } else if (a == "--bind") {
            for (auto& n : Split(next())) natives.bound.insert(shvi::sdk::EngineHash(n));
        } else if (a == "--bind-all") natives.bindAll = true;
        else if (a == "--draw") natives.draw = true;
        else if (a == "--trace") natives.trace = true;
        else if (a == "--block-at") blockAt = std::atoi(next().c_str());
        else if (a == "--expect") expects.push_back(next());
        else return Usage();
    }

    HostConfig cfg;
    cfg.gameName = "GTA VI (simulated)";
    cfg.gameBuild = 0;
    cfg.platform = "sim";
    cfg.log = &LogLine;
    cfg.clock = [] { return g_now; };
    cfg.natives = &natives;
    Host host(cfg);
    PluginSet plugins(host);

    int failed = 0;
    if (std::filesystem::is_directory(target)) {
        int n = 0;
        for (const char* ext : {".so", ".dll", ".asi"}) {
            int before = static_cast<int>(plugins.List().size());
            plugins.LoadDir(target, ext);
            n += static_cast<int>(plugins.List().size()) - before;
        }
        if (n == 0) {
            LogLine("shvi_sim: no plugins found");
            return 1;
        }
    } else {
        plugins.LoadFile(target);
    }
    for (auto& p : plugins.List()) failed += p.status != SHVI_OK ? 1 : 0;

    for (int f = 0; f < frames; ++f) {
        if (f == blockAt) host.Block("simulated online session");
        auto range = keys.equal_range(f);
        for (auto it = range.first; it != range.second; ++it) {
            host.Key(it->second, true);
            host.Key(it->second, false);
        }
        host.Frame();
        natives.EndFrame();
        g_now += static_cast<std::uint64_t>(frameMs);
    }

    if (natives.draw && !natives.lastText.empty()) {
        std::printf("---- last frame's text (%zu items) ----\n", natives.lastText.size());
        for (auto& t : natives.lastText) std::printf("  %s\n", t.c_str());
    }
    std::size_t crashed = host.Scripts().Crashed();
    std::printf("shvi_sim: %d frame(s), %zu plugin(s), %d failed to load, %zu script(s) live, %zu crashed, %zu "
                "distinct natives called\n",
                frames, plugins.List().size(), failed, host.Scripts().Live(), crashed, natives.calls.size());
    int missing = 0;
    for (auto& e : expects) {
        bool hit = false;
        for (auto& l : g_log) hit = hit || l.find(e) != std::string::npos;
        if (!hit) {
            std::printf("shvi_sim: expected log text not seen: %s\n", e.c_str());
            ++missing;
        }
    }
    plugins.UnloadAll();
    return (failed || crashed || missing) ? 1 : 0;
}
