// ScriptHook VI - GTA VI drawing backend for shvi/ui.hpp.
//
// GTA VI has no public native names, so the backend calls three engine
// functions by name (sdk::EngineHash). The profile binds them to whatever
// draws in that build: the game's own HUD functions once they are found
// (native discovery, docs/SCRIPT_MODDING.md), or the overlay (ARCHITECTURE §F).
// Until a profile binds them the calls do nothing: the invoker logs an
// unknown hash once and returns zeros, so a menu plugin loads and runs blind.
//
// Contract (all coordinates normalised 0..1, origin top-left; colour 0xRRGGBBAA):
//   UI_DRAW_RECT(float x, float y, float w, float h, u32 rgba)
//   UI_DRAW_TEXT(const char* utf8, float x, float y, float scale, i32 font, u32 rgba, i32 align)
//                align 0 left (x = left edge), 1 centre, 2 right (x = right edge)
//   UI_DISABLE_MENU_CONTROLS()   keep menu keys away from the game this frame
// Call these from a script fiber or tick callback (they run on the game thread).
#pragma once
#include "shvi.hpp"
#include "ui.hpp"

#include <cstdint>

namespace shvi::vi {

namespace engine {
inline constexpr std::uint64_t UI_DRAW_RECT = sdk::EngineHash("UI_DRAW_RECT");
inline constexpr std::uint64_t UI_DRAW_TEXT = sdk::EngineHash("UI_DRAW_TEXT");
inline constexpr std::uint64_t UI_DISABLE_MENU_CONTROLS = sdk::EngineHash("UI_DISABLE_MENU_CONTROLS");
}  // namespace engine

constexpr std::uint32_t PackRgba(const ui::Color& c) {
    return (std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | std::uint32_t(c.a);
}

class EngineBackend final : public ui::Backend {
public:
    void Draw(const ui::DrawCmd& c) override {
        if (c.kind == ui::DrawCmd::Rect) {
            invoke<void>(engine::UI_DRAW_RECT, c.x, c.y, c.w, c.h, PackRgba(c.color));
            return;
        }
        int align = c.align == ui::Align::Left ? 0 : c.align == ui::Align::Center ? 1 : 2;
        invoke<void>(engine::UI_DRAW_TEXT, c.text.c_str(), c.x, c.y, c.scale, c.font, PackRgba(c.color), align);
    }
};

inline void DisableMenuControls() { invoke<void>(engine::UI_DISABLE_MENU_CONTROLS); }

}  // namespace shvi::vi
