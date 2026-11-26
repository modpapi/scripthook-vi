// GAME-SPECIFIC: GTA VI. Engine adapters behind the SDK's UI_* contract
// (sdk/include/shvi/vi_ui.hpp, TASKS T2.8).
//
//   UI_DISABLE_MENU_CONTROLS  -> PAD.DISABLE_CONTROL_ACTION(0, control, false)
//                               for each control the menu keys overlap.
//                               Handler 0x144BC94F0, behaviour-checked (crossmap).
//   UI_DRAW_RECT, UI_DRAW_TEXT -> the core's overlay draw list (T7.1,
//                               core/overlay_drawlist.h), drawn on the present
//                               thread by core/overlay_d3d12. This build has no
//                               script draw natives: none of the 6,798 natives
//                               the shipped scripts call draws a rect or text,
//                               and none of the 31 uncalled handlers has that
//                               shape (T1.6). The scripts' UI goes through
//                               DATABINDING/UIAPPS. Text is copied; `font` picks
//                               the overlay face (0 regular, 1 bold, 2 display;
//                               others regular); align is clamped to 0..2.
//
// Control actions are atStringHash("INPUT_...") of the rage::InputType names
// (par dump enum rage__InputType; the scripts pass e.g. INPUT_MOVE_LR =
// 0x4D8FB4C1 to IS_CONTROL_JUST_PRESSED).
#pragma once
#include "core/joaat.h"
#include "core/native_context.h"
#include "core/overlay_drawlist.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace shvi::gta6 {

inline constexpr std::array<std::uint32_t, 14> kMenuControls{{
    Joaat("INPUT_FRONTEND_UP"),     Joaat("INPUT_FRONTEND_DOWN"),   Joaat("INPUT_FRONTEND_LEFT"),
    Joaat("INPUT_FRONTEND_RIGHT"),  Joaat("INPUT_FRONTEND_ACCEPT"), Joaat("INPUT_FRONTEND_CANCEL"),
    Joaat("INPUT_FRONTEND_RDOWN"),  Joaat("INPUT_FRONTEND_RRIGHT"), Joaat("INPUT_PHONE"),
    Joaat("INPUT_CHARACTER_WHEEL"), Joaat("INPUT_SELECT_WEAPON_UNARMED"), Joaat("INPUT_ATTACK"),
    Joaat("INPUT_AIM"),             Joaat("INPUT_MELEE_ATTACK"),
}};
static_assert(Joaat("INPUT_MOVE_LR") == 0x4D8FB4C1u, "the hash scripts pass for INPUT_MOVE_LR");

// Calls DISABLE_CONTROL_ACTION(group, control, disableRelated) the way the VM does:
// three 8-byte cells, no results.
inline void DisableControls(rage::ViNativeHandler disableControlAction, std::uint32_t group = 0) {
    if (!disableControlAction) return;
    for (std::uint32_t c : kMenuControls) {
        std::uint64_t args[3] = {group, c, 0};
        std::uint64_t ret[4] = {};
        rage::ViNativeContext ctx{args, ret, 3, 0};
        disableControlAction(&ctx);
    }
}

// UI_DRAW_RECT(x, y, w, h, rgba) -> overlay draw list (back frame).
inline void UiDrawRect(float x, float y, float w, float h, std::uint32_t rgba) {
    overlay::GlobalDrawList().AddRect(x, y, w, h, rgba);
}

// UI_DRAW_TEXT(utf8, x, y, scale, font, rgba, align). Null text is ignored.
inline void UiDrawText(const char* utf8, float x, float y, float scale, std::int32_t font, std::uint32_t rgba,
                       std::int32_t align) {
    if (!utf8) return;
    int a = align < 0 ? 0 : align > 2 ? 2 : align;
    overlay::GlobalDrawList().AddText(std::string_view(utf8), x, y, scale, rgba, a, font);
}

}  // namespace shvi::gta6
