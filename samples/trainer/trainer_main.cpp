// ScriptHook VI trainer plugin. One source, every platform:
//   PC: trainer.asi -> <game>\shvi\plugins\     PS5: trainer.so -> /data/shvi/plugins/
//
// F4 opens/closes. Numpad 8/2 or arrow keys move, 4/6 or left/right change a value, 5/Enter selects,
// 0/Backspace goes back. With no-clip on, W/S (or numpad 9/3) move along the camera.
//
// All game logic is in trainer.hpp. This file is the host glue: input, the frame loop and drawing
// through whatever the host binds behind shvi/vi_ui.hpp (the overlay draw list on the Xbox build).
// A feature is offered only when the installed native table binds every native it calls; install
// nativedb/db/<build>/native_table_full.json as <game>\shvi\natives\<build>.json to get the full set.
#include "trainer.hpp"

#include <shvi/platform.h>
#include <shvi/vi_ui.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::uint32_t NowMs() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

shvi::ui::InputQueue g_input;
std::atomic<int> g_forward{0}, g_back{0};

void OnKey(std::uint32_t vk, int isDown, void* user) {
    if (vk == 'W' || vk == shvi::ui::vk::Numpad9) g_forward = isDown ? 1 : 0;
    if (vk == 'S' || vk == shvi::ui::vk::Numpad3) g_back = isDown ? 1 : 0;
    shvi::ui::InputQueue::OnKey(vk, isDown, user);
}

// Unattended test: shvi\trainer_autorun.txt, one step per line: "<seconds> <feature id> [on|off|number]".
// Seconds count from the first frame in which the player ped exists. Each step and a status line every
// ten seconds go to shvi.log, so a run can be judged from the log alone.
struct AutoStep {
    float at = 0;
    std::string id, arg;
    bool done = false;
};
std::vector<AutoStep> LoadAutorun(const std::string& shviDir) {
    std::vector<AutoStep> steps;
    std::ifstream in(shviDir + "trainer_autorun.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        AutoStep st;
        char id[96] = {}, arg[64] = {};
        if (std::sscanf(line.c_str(), "%f %95s %63s", &st.at, id, arg) >= 2) {
            st.id = id;
            st.arg = arg;
            steps.push_back(st);
        }
    }
    return steps;
}

// One autorun or command step: a feature id with an optional value, or a settings pseudo step. Logs what it did.
void RunStep(trainer::Trainer& t, const std::string& id, const std::string& arg, const char* when) {
    char b[320];
    if (id == "settings.experimental") {
        t.settings.allowExperimental = arg != "off";
        t.Refresh();
        std::snprintf(b, sizeof(b), "trainer: %s experimental features %s", when, t.settings.allowExperimental ? "allowed" : "locked");
        shvi::api()->log(b);
        return;
    }
    if (id == "settings.pintask") { t.settings.pinTask = arg != "off"; return; }
    if (id == "settings.runblocked") {
        t.settings.runBlocked = arg != "off";
        t.Refresh();
        std::snprintf(b, sizeof(b), "trainer: %s blocked features %s", when, t.settings.runBlocked ? "allowed" : "off");
        shvi::api()->log(b);
        return;
    }
    trainer::Feature* f = t.Find(id);
    if (!f) {
        std::snprintf(b, sizeof(b), "trainer: %s %s: no such feature", when, id.c_str());
        shvi::api()->log(b);
        return;
    }
    if (f->kind == trainer::Kind::Toggle) f->on = arg != "off";
    else if ((f->kind == trainer::Kind::Int || f->kind == trainer::Kind::Choice) && !arg.empty()) f->i = std::atoi(arg.c_str());
    else if (f->kind == trainer::Kind::Float && !arg.empty()) f->f = static_cast<float>(std::atof(arg.c_str()));
    bool ok = t.Activate(*f);
    std::snprintf(b, sizeof(b), "trainer: %s %s %s -> %s%s%s", when, id.c_str(), arg.c_str(), ok ? "called" : "refused",
                  f->available ? "" : " missing ", f->available ? "" : f->missing.c_str());
    shvi::api()->log(b);
}

// "call NAME arg..." in a command file: one native by name, for read-backs while testing a feature. Arguments:
// ped / veh (the player's ped or vehicle, two cells), i:N, u:HEX, f:X, b:0|1. Logs the first two return cells.
void CallNative(trainer::Trainer& t, const std::string& line) {
    char name[96] = {};
    int used = 0;
    if (std::sscanf(line.c_str(), "call %95s%n", name, &used) < 1) return;
    const shvi_api* a = shvi::api();
    std::uint64_t hash = shvi_engine_hash(name);
    char b[320];
    if (!a->native_exists || !a->native_exists(hash)) {
        std::snprintf(b, sizeof(b), "trainer: call %s: not bound", name);
        a->log(b);
        return;
    }
    a->native_init(hash);
    const char* p = line.c_str() + used;
    for (char tok[64]; std::sscanf(p, "%63s%n", tok, &used) == 1; p += used) {
        std::string s = tok;
        const auto& c = t.Context();
        if (s == "ped" || s == "veh") {
            const trainer::Ref& r = s == "ped" ? c.ped : c.veh;
            a->native_push64(r.lo);
            a->native_push64(r.hi);
        } else if (s.rfind("i:", 0) == 0) a->native_push64(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::atoll(s.c_str() + 2))));
        else if (s.rfind("u:", 0) == 0) a->native_push64(std::strtoull(s.c_str() + 2, nullptr, 16));
        else if (s.rfind("f:", 0) == 0) a->native_push64(shvi_f2u(static_cast<float>(std::atof(s.c_str() + 2))));
        else if (s.rfind("b:", 0) == 0) a->native_push64(std::atoi(s.c_str() + 2) ? 1 : 0);
    }
    const std::uint64_t* r = a->native_call();
    std::snprintf(b, sizeof(b), "trainer: call %s -> 0x%llX 0x%llX (as float %.3f, as int %d)", line.c_str() + 5, (unsigned long long)r[0],
                  (unsigned long long)r[1], shvi_u2f(r[0]), static_cast<int>(static_cast<std::int32_t>(r[0])));
    a->log(b);
}

// Commands for a test run: shvi\trainer_cmd.txt, one "<feature id> [on|off|number]" per line, read and deleted
// when it appears. "wait <ms>" pauses between lines; "call ..." calls a native (CallNative). Nothing happens while
// the file does not exist.
struct CommandFile {
    std::string path;
    std::vector<std::pair<std::string, std::string>> queue;
    std::uint32_t lastPoll = 0, resumeAt = 0;
    void Step(trainer::Trainer& t, std::uint32_t now) {
        if (queue.empty() && now - lastPoll > 500) {
            lastPoll = now;
            std::ifstream in(path);
            if (!in) return;
            for (std::string line; std::getline(in, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                char id[96] = {}, arg[64] = {};
                if (line.rfind("call ", 0) == 0) queue.emplace_back("call", line);
                else if (std::sscanf(line.c_str(), "%95s %63s", id, arg) >= 1) queue.emplace_back(id, arg);
            }
            in.close();
            std::remove(path.c_str());
        }
        while (!queue.empty() && now >= resumeAt) {
            auto [id, arg] = queue.front();
            queue.erase(queue.begin());
            if (id == "wait") { resumeAt = now + static_cast<std::uint32_t>(std::atoi(arg.c_str())); return; }
            if (id == "call") { CallNative(t, arg); continue; }
            RunStep(t, id, arg, "cmd");
        }
    }
};

void ScriptMain(void*) {
    using namespace shvi;
    trainer::Trainer t;
    ui::Style style;
    style.maxVisible = 16;
    vi::EngineBackend backend;

    // The shvi folder, from this plugin's own path (<game>\shvi\plugins\trainer.asi, or /data/shvi/ on
    // PS5). Not from the process exe: under a host such as VIctory that is the host's exe, not the game's.
    std::string shviDir;
    {
        char dir[1024] = {};
        shvi_mod_data_dir(dir, sizeof(dir));
        shviDir = dir;
    }
    // Optional model list for the spawner: shvi\trainer_models.txt, one model name or 0xHASH per line.
    {
        std::ifstream in(shviDir + "trainer_models.txt");
        std::vector<std::string> models;
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (!line.empty()) models.push_back(line);
        }
        t.SetModelList(models);
        t.SetModelFile(shviDir + "trainer_models.txt");
    }

    std::vector<AutoStep> autorun = LoadAutorun(shviDir);
    CommandFile commands;
    commands.path = shviDir + "trainer_cmd.txt";
    std::uint32_t worldSince = 0, lastStatusLog = 0;
    if (!autorun.empty()) {
        char b[96];
        std::snprintf(b, sizeof(b), "trainer: autorun with %zu step(s)", autorun.size());
        api()->log(b);
    }

    auto menu = std::make_unique<ui::Menu>("VI TRAINER");
    std::uint32_t lastRefresh = 0;
    bool built = false, lastExperimental = t.settings.allowExperimental;
    int lastUsable = -1, keyLog = 0;

    api()->log("trainer: started, F4 opens the menu");
    for (;;) {
        // The host binds its native table once the game is ready; look again every two seconds and
        // rebuild the menu when what is usable changed.
        std::uint32_t now = NowMs();
        if (!built || now - lastRefresh > 2000 || lastExperimental != t.settings.allowExperimental) {
            lastRefresh = now;
            t.Refresh();
            int usable = 0;
            for (auto& f : t.Features()) usable += t.Usable(f) ? 1 : 0;
            if (!built || usable != lastUsable || lastExperimental != t.settings.allowExperimental) {
                bool wasOpen = built && menu->IsOpen();
                std::vector<int> path = built ? menu->Path() : std::vector<int>{};
                menu = std::make_unique<ui::Menu>("VI TRAINER");
                t.BuildMenu(*menu);
                menu->RestorePath(path);
                menu->SetOpen(wasOpen);
                if (usable != lastUsable) t.LogAvailability();
                lastUsable = usable;
                lastExperimental = t.settings.allowExperimental;
                built = true;
            }
        }

        t.SetNoclipInput(static_cast<float>(g_forward.load() - g_back.load()));
        t.Tick();

        if (!autorun.empty() && trainer::Valid(t.Context().ped)) {
            if (!worldSince) {
                worldSince = now;
                api()->log("trainer: autorun clock started (player ped exists)");
            }
            float secs = (now - worldSince) / 1000.0f;
            if (now - lastStatusLog > 10000) {
                lastStatusLog = now;
                api()->log(("trainer: status " + t.Status()).c_str());
            }
            for (auto& st : autorun) {
                if (st.done || secs < st.at) continue;
                st.done = true;
                char when[32];
                std::snprintf(when, sizeof(when), "autorun t=%.0fs", secs);
                RunStep(t, st.id, st.arg, when);
            }
        }
        if (trainer::Valid(t.Context().ped)) commands.Step(t, now);

        for (ui::Action a; (a = g_input.Pop()) != ui::Action::None;) {
            menu->Handle(a);
            if (keyLog < 60) {  // the first key actions of a session, so a run shows the menu works from the keyboard
                ++keyLog;
                static const char* const kNames[] = {"none", "open", "up", "down", "left", "right", "select", "back"};
                const auto& items = menu->Current().Items();
                int cur = menu->Cursor();
                char b[256];
                std::snprintf(b, sizeof(b), "trainer: key %s -> menu %s, page '%s', item %d of %zu '%s'", kNames[static_cast<int>(a)],
                              menu->IsOpen() ? "open" : "closed", menu->Current().Title().c_str(), cur, items.size(),
                              cur >= 0 && cur < static_cast<int>(items.size()) ? items[static_cast<std::size_t>(cur)].label.c_str() : "");
                api()->log(b);
            }
        }
        if (menu->IsOpen()) vi::DisableMenuControls();

        if (t.settings.showStatus && !t.Status().empty()) {  // a small dark pill at the top left
            ui::DrawCmd bg, d;
            d.kind = ui::DrawCmd::Text;
            d.text = t.Status();
            d.scale = 0.25f;
            d.font = ui::font::Bold;
            d.color = {235, 235, 240, 255};
            const float padX = 0.006f, h = 0.026f;
            bg.kind = ui::DrawCmd::Rect;
            bg.x = 0.008f;
            bg.y = 0.010f;
            bg.w = ui::TextWidth(d.text, d.scale, d.font, style) + 2 * padX;
            bg.h = h;
            bg.color = {0, 0, 0, 170};
            d.x = bg.x + padX;
            d.y = bg.y + (h - d.scale * style.textHeight) * 0.5f;
            ui::DrawCmd edge = bg;
            edge.w = 0.0018f;
            edge.color = style.accent;
            backend.Draw(bg);
            backend.Draw(edge);
            backend.Draw(d);
        }
        ui::Submit(backend, menu->Layout(style));
        wait(0);
    }
}

}  // namespace

extern "C" SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* a) {
    if (!shvi_api_compatible(a)) return SHVI_E_ABI_MISMATCH;
    shvi::api() = a;
    int rc = a->keyboard_register(&OnKey, &g_input);
    if (rc != SHVI_OK) return rc;
    return a->script_register(SHVI_THIS_MODULE, ScriptMain, nullptr);
}
