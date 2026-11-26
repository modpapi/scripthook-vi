// Sample plugin (C++, ABI 1): a mod menu with shvi/ui.hpp, drawn through the
// GTA VI engine backend (shvi/vi_ui.hpp).
// Build output: menu_demo.asi -> <game>\shvi\plugins\
//
// F4 opens/closes. Numpad 8/2 or arrow keys move, 4/6 or left/right change a
// value, 5/Enter selects, 0/Backspace goes back.
//
// The backend calls UI_DRAW_RECT / UI_DRAW_TEXT / UI_DISABLE_MENU_CONTROLS by
// name. Until the profile binds those (native discovery or the overlay), the
// menu runs and logs, but nothing appears on screen.
#include <shvi/shvi.hpp>
#include <shvi/ui.hpp>
#include <shvi/vi_ui.hpp>
#include <shvi/natives_vi.hpp>  // generated from the native table (tools/docgen/gen_natives.js)

#include <windows.h>

#include <cstdio>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

shvi::ui::InputQueue g_input;

struct State {
    bool showStatus = true;
    float statusScale = 0.40f;
    int accent = 0;
    int presses = 0;
    unsigned frames = 0;
};

void ScriptMain(void*) {
    using namespace shvi;
    State st;
    ui::Style style;
    const ui::Color accents[] = {{20, 90, 160, 235}, {160, 30, 30, 235}, {30, 130, 60, 235}, {120, 60, 160, 235}};

    ui::Menu menu("SCRIPTHOOK VI");
    ui::Page& settings = menu.AddPage("SETTINGS");
    ui::Page& about = menu.AddPage("ABOUT");
    ui::Page& player = menu.AddPage("PLAYER");
    menu.Root()
        .Toggle("Show status line", &st.showStatus)
        .Float("Status size", &st.statusScale, 0.20f, 0.80f, 0.05f)
        .Submenu("Player", player)
        .Submenu("Settings", settings)
        .Submenu("About", about);
    // Natives by name (shvi/natives_vi.hpp). Each one is bound only if the host's
    // native table matches this build; otherwise it returns zeros.
    namespace N = shvi::natives;
    player
        .Button("Heal", [] {
            shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
            N::ENTITY::SET_ENTITY_HEALTH(me, N::ENTITY::GET_ENTITY_MAX_HEALTH(me, false), shvi_entity_ref{0, 0},
                                         false, false);
        })
        .Button("Clear wanted level", [] { N::PLAYER::CLEAR_PLAYER_WANTED_LEVEL(N::PLAYER::PLAYER_PED_ID()); })
        .Button("Log position and health", [] {
            shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
            N::Vec3 p = N::ENTITY::GET_ENTITY_COORDS(me, true);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "menu_demo: player at %.1f %.1f %.1f, health %d, wanted %d", p.x, p.y,
                          p.z, N::ENTITY::GET_ENTITY_HEALTH(me), N::PLAYER::GET_PLAYER_WANTED_LEVEL(me));
            api()->log(msg);
        })
        .Button("Set time to noon", [] { N::CLOCK::SET_CLOCK_TIME(12, 0, 0, false); });
    settings
        .Choice("Header colour", &st.accent, {"Blue", "Red", "Green", "Purple"},
                [&] { style.header = accents[st.accent]; })
        .Int("Visible rows", &style.maxVisible, 4, 16, 1);
    about
        .Button("Say hello in shvi.log", [&] {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "menu_demo: hello #%d", ++st.presses);
            api()->log(msg);
        })
        .Button("Close menu", [&] { menu.SetOpen(false); });

    vi::EngineBackend backend;
    api()->log("menu_demo: started, F4 opens the menu");
    for (;;) {
        ++st.frames;
        for (ui::Action a; (a = g_input.Pop()) != ui::Action::None;) menu.Handle(a);
        if (menu.IsOpen()) vi::DisableMenuControls();

        if (st.showStatus) {
            char text[96];
            std::snprintf(text, sizeof(text), "%s  build %u  frame %u  timer %d", api()->game_name(),
                          api()->game_build(), st.frames, N::MISC::GET_GAME_TIMER());
            ui::DrawCmd d;
            d.kind = ui::DrawCmd::Text;
            d.text = text;
            d.x = 0.01f;
            d.y = 0.01f;
            d.scale = st.statusScale;
            d.color = {255, 255, 255, 255};
            backend.Draw(d);
        }
        ui::Submit(backend, menu.Layout(style));
        wait(0);
    }
}

}  // namespace

extern "C" SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* a) {
    if (!shvi_api_compatible(a)) return SHVI_E_ABI_MISMATCH;
    shvi::api() = a;
    int rc = a->keyboard_register(&shvi::ui::InputQueue::OnKey, &g_input);
    if (rc != SHVI_OK) return rc;
    return a->script_register(&__ImageBase, ScriptMain, nullptr);
}
