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

void ScriptMain(void*) {
    using namespace shvi;
    trainer::Trainer t;
    ui::Style style;
    style.maxVisible = 14;
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
    }

    std::vector<AutoStep> autorun = LoadAutorun(shviDir);
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
                menu = std::make_unique<ui::Menu>("VI TRAINER");
                t.BuildMenu(*menu);
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
                char b[320];
                if (st.id == "settings.experimental") {  // pseudo step: the Settings switch
                    t.settings.allowExperimental = st.arg != "off";
                    t.Refresh();
                    api()->log(t.settings.allowExperimental ? "trainer: autorun experimental natives allowed" : "trainer: autorun experimental natives locked");
                    continue;
                }
                if (st.id == "settings.pintask") { t.settings.pinTask = st.arg != "off"; continue; }
                if (st.id == "settings.runblocked") {  // pseudo step: retry features the trainer has blocked
                    t.settings.runBlocked = st.arg != "off";
                    t.Refresh();
                    api()->log(t.settings.runBlocked ? "trainer: autorun blocked features allowed" : "trainer: autorun blocked features off");
                    continue;
                }
                trainer::Feature* f = t.Find(st.id);
                if (!f) {
                    std::snprintf(b, sizeof(b), "trainer: autorun %s: no such feature", st.id.c_str());
                    api()->log(b);
                    continue;
                }
                if (f->kind == trainer::Kind::Toggle) f->on = st.arg != "off";
                else if ((f->kind == trainer::Kind::Int || f->kind == trainer::Kind::Choice) && !st.arg.empty()) f->i = std::atoi(st.arg.c_str());
                else if (f->kind == trainer::Kind::Float && !st.arg.empty()) f->f = static_cast<float>(std::atof(st.arg.c_str()));
                bool ok = t.Activate(*f);
                std::snprintf(b, sizeof(b), "trainer: autorun t=%.0fs %s %s -> %s%s%s", secs, st.id.c_str(), st.arg.c_str(),
                              ok ? "called" : "refused", f->available ? "" : " missing ", f->available ? "" : f->missing.c_str());
                api()->log(b);
            }
        }

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

        if (t.settings.showStatus && !t.Status().empty()) {
            ui::DrawCmd d;
            d.kind = ui::DrawCmd::Text;
            d.text = t.Status();
            d.x = 0.01f;
            d.y = 0.01f;
            d.scale = 0.34f;
            d.color = {255, 255, 255, 255};
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
