// Project templates for `shvi new`. Each returns the files of a new mod project (path -> content).
// Every combination is compiled for real by CI (tools/shvi-cli/test/e2e.test.ts), so a template that
// stops building fails the SDK build, not the user's first try.
import type { Language, Manifest, Target, TemplateId } from "./project";

export interface TemplateInfo {
  id: TemplateId;
  label: string;
  hint: string;
  languages: Language[];
}

export const TEMPLATE_INFO: TemplateInfo[] = [
  { id: "blank", label: "Blank script", hint: "one script loop that logs; the smallest mod", languages: ["c", "cpp"] },
  { id: "menu", label: "Mod menu", hint: "a menu (F4 / L1+R3) with pages, toggles and actions", languages: ["cpp"] },
  {
    id: "trainer",
    label: "Trainer",
    hint: "mod-menu trainer: player, vehicle, world; gated by native availability",
    languages: ["cpp"],
  },
];

export interface GenOptions {
  manifest: Manifest;
  ci: boolean; // write .github/workflows/build.yml
  repo: string; // owner/name of the ScriptHook VI GitHub repo the SDK is downloaded from
}

export type FileMap = Record<string, string>;

export function render(o: GenOptions): FileMap {
  const m = o.manifest;
  const files: FileMap = {};
  const ext = m.language === "c" ? "c" : "cpp";
  files["shvi.json"] = JSON.stringify(m, null, 2) + "\n";
  files["CMakeLists.txt"] = cmake(m, ext);
  files[".gitignore"] = "build/\ndist/\n.vs/\n.vscode/\n*.user\n";
  files["README.md"] = readme(m);
  files[`src/main.${ext}`] = source(m);
  if (m.template === "trainer") files["src/features.hpp"] = trainerFeatures();
  if (o.ci) files[".github/workflows/build.yml"] = workflow(m, o.repo);
  return files;
}

function cmake(m: Manifest, ext: string): string {
  return `# ${m.name}: a ScriptHook VI mod. Built for every platform from this one file:
#   shvi build --target pc|ps5|sim       (or plain CMake, see README.md)
cmake_minimum_required(VERSION 3.24)
project(${m.id} VERSION ${m.version.split(/[-+]/)[0]} LANGUAGES C CXX)

# The SDK: an unpacked shvi-sdk release, or a ScriptHook VI checkout. The CLI passes it; plain CMake
# users set SHVI_SDK (environment) or -DSHVI_SDK_DIR=<path>.
set(SHVI_SDK_DIR "$ENV{SHVI_SDK}" CACHE PATH "ScriptHook VI SDK root (contains sdk/cmake/ShviMod.cmake)")
if(NOT EXISTS "\${SHVI_SDK_DIR}/sdk/cmake/ShviMod.cmake")
  message(FATAL_ERROR "ScriptHook VI SDK not found: set SHVI_SDK or -DSHVI_SDK_DIR (shvi sdk install gets one)")
endif()
include("\${SHVI_SDK_DIR}/sdk/cmake/ShviMod.cmake")

enable_testing()
shvi_add_mod(${m.id} SOURCES src/main.${ext})

# PS5: also build the ELF loader payload that goes with the mod image.
shvi_add_ps5_loader()
`;
}

const targetNames: Record<Target, string> = { pc: "PC (Windows, .asi)", ps5: "PS5 (.elf mod image + shvi_loader.elf)" };

function readme(m: Manifest): string {
  return `# ${m.name}

${m.description || "A ScriptHook VI mod for GTA VI story mode."}

Targets: ${m.targets.map((t) => targetNames[t]).join(", ")}. Language: ${m.language === "c" ? "C" : "C++"}.

## Build

\`\`\`sh
shvi build                 # every target in shvi.json
shvi build --target pc     # Windows: build/pc/shvi/plugins/${m.id}.asi
shvi build --target ps5    # Linux/WSL with ps5-payload-sdk: build/ps5/${m.id}.elf + shvi_loader.elf
shvi test                  # run the mod offline in the simulator (no game needed)
shvi package               # dist/${m.id}-${m.version}-<target>.zip, ready to share
\`\`\`

Without the CLI (SHVI_SDK = an SDK folder):

\`\`\`sh
cmake -S . -B build/pc -A x64 && cmake --build build/pc --config Release                       # PC
cmake -S . -B build/ps5 -DCMAKE_TOOLCHAIN_FILE=$SHVI_SDK/sdk/cmake/toolchains/ps5.cmake && cmake --build build/ps5   # PS5
\`\`\`

## Install

- **PC**: copy \`${m.id}.asi\` to \`<game>/shvi/plugins/\` (ScriptHook VI must be installed).
- **PS5**: copy \`${m.id}.elf\` to \`/data/shvi/mods/\` (FTP), then send \`shvi_loader.elf\` to the ELF loader
  (port 9021), or \`shvi deploy --host <console ip>\`. Sending \`${m.id}.elf\` itself runs a dry run outside the game.

Controls: F4 on PC, L1 + R3 on the PS5 pad; arrows / d-pad move, Enter / Cross select, Backspace / Circle back.

Story mode and offline only: ScriptHook VI stops every mod as soon as it detects an online session.
`;
}

function source(m: Manifest): string {
  if (m.template === "blank" && m.language === "c") return blankC(m);
  if (m.template === "blank") return blankCpp(m);
  if (m.template === "menu") return menuCpp(m);
  return trainerCpp(m);
}

const header = (m: Manifest) => `${m.name} (${m.id} ${m.version}): a ScriptHook VI mod.
// One source for every platform: the SDK builds it as ${m.id}.asi (PC) or ${m.id}.elf (PS5).`;

function blankC(m: Manifest): string {
  return `/* ${header(m).replace("\n//", "\n *")}
 */
#include <shvi/shvi.h>

#include <stdio.h>

static const shvi_api* g_api;

static void script_main(void* user) {
    char line[160];
    unsigned ticks = 0;
    (void)user;
    snprintf(line, sizeof(line), "${m.id}: running on %s (%s build %u)", g_api->game_name(), SHVI_PLATFORM_NAME,
             g_api->game_build());
    g_api->log(line);
    for (;;) {
        snprintf(line, sizeof(line), "${m.id}: alive, %u second(s)", (ticks += 5));
        g_api->log(line);
        g_api->script_wait(5000); /* yield to the game for 5 s */
    }
}

SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* api) {
    if (!shvi_api_compatible(api)) return SHVI_E_ABI_MISMATCH;
    g_api = api;
    return api->script_register(SHVI_THIS_MODULE, script_main, NULL);
}
`;
}

function blankCpp(m: Manifest): string {
  return `// ${header(m)}
#include <shvi/shvi.hpp>

#include <cstdio>

namespace {

void ScriptMain(void*) {
    using namespace shvi;
    char line[160];
    std::snprintf(line, sizeof(line), "${m.id}: running on %s (%s build %u)", api()->game_name(), SHVI_PLATFORM_NAME,
                  api()->game_build());
    api()->log(line);
    for (unsigned seconds = 5;; seconds += 5) {
        std::snprintf(line, sizeof(line), "${m.id}: alive, %u second(s)", seconds);
        api()->log(line);
        wait(5000);  // yield to the game for 5 s
    }
}

}  // namespace

extern "C" SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* a) {
    if (!shvi_api_compatible(a)) return SHVI_E_ABI_MISMATCH;
    shvi::api() = a;
    return a->script_register(SHVI_THIS_MODULE, ScriptMain, nullptr);
}
`;
}

function menuCpp(m: Manifest): string {
  const title = m.name.toUpperCase().replace(/"/g, "'");
  return `// ${header(m)}
//
// F4 (PC) or L1 + R3 (PS5) opens the menu. Arrows / d-pad move, Left/Right change a value,
// Enter / Cross select, Backspace / Circle go back.
#include <shvi/natives_vi.hpp>  // typed natives (bound per game build by the host)
#include <shvi/shvi.hpp>
#include <shvi/ui.hpp>
#include <shvi/vi_ui.hpp>

#include <cstdio>

namespace {

namespace N = shvi::natives;
shvi::ui::InputQueue g_input;

void ScriptMain(void*) {
    using namespace shvi;
    ui::Style style;
    ui::Menu menu("${title}");
    bool showStatus = true;
    int presses = 0;

    ui::Page& player = menu.AddPage("PLAYER");
    ui::Page& world = menu.AddPage("WORLD");
    ui::Page& settings = menu.AddPage("SETTINGS");
    menu.Root()
        .Submenu("Player", player)
        .Submenu("World", world)
        .Submenu("Settings", settings)
        .Button("Say hello in the log", [&] {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "${m.id}: hello #%d", ++presses);
            api()->log(msg);
        });
    player
        .Button("Heal", [] {
            shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
            N::ENTITY::SET_ENTITY_HEALTH(me, N::ENTITY::GET_ENTITY_MAX_HEALTH(me, false), shvi_entity_ref{0, 0}, false,
                                         false);
        })
        .Button("Clear wanted level", [] { N::PLAYER::CLEAR_PLAYER_WANTED_LEVEL(N::PLAYER::PLAYER_PED_ID()); });
    world
        .Button("Morning (08:00)", [] { N::CLOCK::SET_CLOCK_TIME(8, 0, 0, false); })
        .Button("Noon (12:00)", [] { N::CLOCK::SET_CLOCK_TIME(12, 0, 0, false); })
        .Button("Night (22:00)", [] { N::CLOCK::SET_CLOCK_TIME(22, 0, 0, false); });
    settings.Toggle("Show status line", &showStatus).Int("Visible rows", &style.maxVisible, 4, 16, 1);

    vi::EngineBackend backend;
    api()->log("${m.id}: started; F4 / L1+R3 opens the menu");
    for (;;) {
        for (ui::Action a; (a = g_input.Pop()) != ui::Action::None;) menu.Handle(a);
        if (menu.IsOpen()) vi::DisableMenuControls();
        if (showStatus) {
            ui::DrawCmd d;
            d.kind = ui::DrawCmd::Text;
            d.text = "${m.name.replace(/"/g, "'")} ${m.version}";
            d.x = 0.01f;
            d.y = 0.01f;
            d.scale = 0.35f;
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
    return a->script_register(SHVI_THIS_MODULE, ScriptMain, nullptr);
}
`;
}

function trainerFeatures(): string {
  return `// Trainer features: each one names the natives it needs, and the menu offers it only when the
// host's native table binds all of them for the running game build (shvi::native_exists). The same
// mod therefore runs on a build or platform where some natives are not mapped yet; it just shows less.
#pragma once
#include <shvi/natives_vi.hpp>
#include <shvi/shvi.hpp>

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace trainer {

namespace N = shvi::natives;

struct Feature {
    std::string label;
    std::vector<const char*> natives;  // all must exist for the feature to be offered
    bool Available() const {
        for (const char* n : natives)
            if (!shvi::native_exists(n)) return false;
        return true;
    }
};

struct State {
    bool godMode = false, neverWanted = false, lockTime = false;
    int hour = 12;
    bool godApplied = false;
};

// Per-frame holds: what a toggle keeps true while it is on.
inline void Tick(State& s) {
    shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
    if (s.godMode != s.godApplied && shvi::native_exists("SET_ENTITY_INVINCIBLE")) {
        N::ENTITY::SET_ENTITY_INVINCIBLE(me, s.godMode);
        s.godApplied = s.godMode;
    }
    if (s.neverWanted && shvi::native_exists("CLEAR_PLAYER_WANTED_LEVEL")) N::PLAYER::CLEAR_PLAYER_WANTED_LEVEL(me);
    if (s.lockTime && shvi::native_exists("SET_CLOCK_TIME")) N::CLOCK::SET_CLOCK_TIME(s.hour, 0, 0, false);
}

inline void Heal() {
    shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
    N::ENTITY::SET_ENTITY_HEALTH(me, N::ENTITY::GET_ENTITY_MAX_HEALTH(me, false), shvi_entity_ref{0, 0}, false, false);
}

inline void TeleportForward(float metres) {
    shvi_entity_ref me = N::PLAYER::PLAYER_PED_ID();
    N::Vec3 to = N::ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(me, N::Vec3{0.0f, metres, 0.0f, 0.0f});
    N::ENTITY::SET_ENTITY_COORDS(me, to, false, false, false, false, 0.0f);
}

inline void RepairLastVehicle() {
    shvi_entity_ref v = N::PLAYER::GET_PLAYERS_LAST_VEHICLE();
    if (v.lo || v.hi) N::ENTITY::SET_ENTITY_HEALTH(v, N::ENTITY::GET_ENTITY_MAX_HEALTH(v, false), shvi_entity_ref{0, 0}, false, false);
}

}  // namespace trainer
`;
}

function trainerCpp(m: Manifest): string {
  const title = m.name.toUpperCase().replace(/"/g, "'");
  return `// ${header(m)}
//
// A mod-menu trainer. F4 (PC) or L1 + R3 (PS5) opens it. Features are in features.hpp; each is
// offered only when the running build binds the natives it needs, and the menu is rebuilt when the
// host binds more (it binds its native table once the game is ready).
#include "features.hpp"

#include <shvi/ui.hpp>
#include <shvi/vi_ui.hpp>

#include <cstdio>
#include <memory>

namespace {

shvi::ui::InputQueue g_input;
trainer::State g_state;
float g_teleport = 5.0f;

const trainer::Feature kHeal{"Heal", {"PLAYER_PED_ID", "SET_ENTITY_HEALTH", "GET_ENTITY_MAX_HEALTH"}};
const trainer::Feature kGod{"God mode", {"PLAYER_PED_ID", "SET_ENTITY_INVINCIBLE"}};
const trainer::Feature kWanted{"Never wanted", {"PLAYER_PED_ID", "CLEAR_PLAYER_WANTED_LEVEL"}};
const trainer::Feature kTeleport{"Teleport forward", {"PLAYER_PED_ID", "GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS", "SET_ENTITY_COORDS"}};
const trainer::Feature kRepair{"Repair last vehicle", {"GET_PLAYERS_LAST_VEHICLE", "SET_ENTITY_HEALTH", "GET_ENTITY_MAX_HEALTH"}};
const trainer::Feature kTime{"Lock time of day", {"SET_CLOCK_TIME"}};

int CountAvailable() {
    int n = 0;
    for (const auto* f : {&kHeal, &kGod, &kWanted, &kTeleport, &kRepair, &kTime}) n += f->Available() ? 1 : 0;
    return n;
}

std::unique_ptr<shvi::ui::Menu> BuildMenu() {
    using namespace shvi;
    auto menu = std::make_unique<ui::Menu>("${title}");
    ui::Page& player = menu->AddPage("PLAYER");
    ui::Page& vehicle = menu->AddPage("VEHICLE");
    ui::Page& world = menu->AddPage("WORLD");
    menu->Root().Submenu("Player", player).Submenu("Vehicle", vehicle).Submenu("World", world);

    if (kHeal.Available()) player.Button(kHeal.label, [] { trainer::Heal(); });
    if (kGod.Available()) player.Toggle(kGod.label, &g_state.godMode);
    if (kWanted.Available()) player.Toggle(kWanted.label, &g_state.neverWanted);
    if (kTeleport.Available())
        player.Float("Teleport distance (m)", &g_teleport, 1.0f, 50.0f, 1.0f, 0)
            .Button(kTeleport.label, [] { trainer::TeleportForward(g_teleport); });
    if (kRepair.Available()) vehicle.Button(kRepair.label, [] { trainer::RepairLastVehicle(); });
    if (kTime.Available()) world.Toggle(kTime.label, &g_state.lockTime).Int("Hour", &g_state.hour, 0, 23, 1);
    return menu;
}

void ScriptMain(void*) {
    using namespace shvi;
    ui::Style style;
    vi::EngineBackend backend;
    std::unique_ptr<ui::Menu> menu;
    int available = -1;
    unsigned frame = 0;

    api()->log("${m.id}: started; F4 / L1+R3 opens the trainer");
    for (;; ++frame) {
        // Rebuild when what the host binds changes (checked twice a second).
        if (frame % 30 == 0) {
            int now = CountAvailable();
            if (now != available) {
                bool open = menu && menu->IsOpen();
                menu = BuildMenu();
                menu->SetOpen(open);
                available = now;
                char msg[96];
                std::snprintf(msg, sizeof(msg), "${m.id}: %d of 6 features available on this build", available);
                api()->log(msg);
            }
        }
        for (ui::Action a; (a = g_input.Pop()) != ui::Action::None;) menu->Handle(a);
        if (menu->IsOpen()) vi::DisableMenuControls();
        trainer::Tick(g_state);
        ui::Submit(backend, menu->Layout(style));
        wait(0);
    }
}

}  // namespace

extern "C" SHVI_PLUGIN_EXPORT int shvi_plugin_load(const shvi_api* a) {
    if (!shvi_api_compatible(a)) return SHVI_E_ABI_MISMATCH;
    shvi::api() = a;
    int rc = a->keyboard_register(&shvi::ui::InputQueue::OnKey, &g_input);
    if (rc != SHVI_OK) return rc;
    return a->script_register(SHVI_THIS_MODULE, ScriptMain, nullptr);
}
`;
}

function workflow(m: Manifest, repo: string): string {
  const pc = m.targets.includes("pc");
  const ps5 = m.targets.includes("ps5");
  let jobs = `  simulate:
    # The mod in the offline simulator: loads, runs 300 frames, walks the menu. No game needed.
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install the shvi CLI and SDK
        run: |
          curl -fsSL https://github.com/${repo}/releases/download/v${m.sdk.version}/install.sh | bash -s -- --version ${m.sdk.version}
          echo "$HOME/.shvi/bin" >> "$GITHUB_PATH"
      - run: shvi test --target sim
`;
  if (pc)
    jobs += `
  pc:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install the shvi CLI and SDK
        shell: pwsh
        run: |
          & ([scriptblock]::Create((irm https://github.com/${repo}/releases/download/v${m.sdk.version}/install.ps1))) -Version ${m.sdk.version}
          "$HOME\\.shvi\\bin" | Out-File -FilePath $env:GITHUB_PATH -Append
      - run: shvi build --target pc
      - run: shvi test --target pc
      - run: shvi package --target pc
      - uses: actions/upload-artifact@v4
        with:
          name: ${m.id}-pc
          path: dist/*-pc.zip
`;
  if (ps5)
    jobs += `
  ps5:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install the shvi CLI, SDK and ps5-payload-sdk
        run: |
          curl -fsSL https://github.com/${repo}/releases/download/v${m.sdk.version}/install.sh | bash -s -- --version ${m.sdk.version}
          echo "$HOME/.shvi/bin" >> "$GITHUB_PATH"
          "$HOME/.shvi/bin/shvi" doctor --install-ps5-sdk
          echo "PS5_PAYLOAD_SDK=$HOME/.shvi/ps5-payload-sdk" >> "$GITHUB_ENV"
      - run: shvi build --target ps5
      - run: shvi package --target ps5
      - uses: actions/upload-artifact@v4
        with:
          name: ${m.id}-ps5
          path: dist/*-ps5.zip
`;
  return `# Builds ${m.name} for every target on each push, and attaches the packages to tagged releases.
name: build

on:
  push:
    branches: [main]
    tags: ["v*"]
  pull_request:

jobs:
${jobs}
  release:
    if: startsWith(github.ref, 'refs/tags/v')
    needs: [${["simulate", pc ? "pc" : "", ps5 ? "ps5" : ""].filter(Boolean).join(", ")}]
    runs-on: ubuntu-latest
    permissions:
      contents: write
    steps:
      - uses: actions/download-artifact@v4
        with:
          path: dist
          merge-multiple: true
      - uses: softprops/action-gh-release@v2
        with:
          files: dist/*.zip
`;
}
