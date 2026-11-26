# ScriptHook VI SDK: one mod, every platform

This SDK builds a GTA VI story-mode mod from **one source tree** for:

| Target | Output | Runs in | Status |
|---|---|---|---|
| `pc` | `<mod>.asi` (Windows DLL) | ScriptHook VI core (`dinput8.dll` + `shvi/shvi_core.dll`), today under the VIctory-based layer | in game ✅ |
| `ps5` | `<mod>.elf` (mod + PS5 runtime + engine, one PIE ELF in payload format) and `shvi_loader.elf` | the console's ELF loader: dry run in its own process; injection into the game is the next milestone | builds ✅ · console 🧪 |
| `sim` | `<mod>.so` / the PC `.asi` | `shvi_sim`, the offline simulator: no game, scripted keys, menu as text | ✅ (CI) |

Story mode and offline only. Every host stops all mods as soon as it detects an online session.

## Quick start (the `shvi` CLI)

```sh
# install (single binary + SDK into ~/.shvi)
curl -fsSL https://github.com/modpapi/scripthook-vi/releases/latest/download/install.sh | bash       # Linux / macOS / WSL
irm https://github.com/modpapi/scripthook-vi/releases/latest/download/install.ps1 | iex              # Windows PowerShell
# or from a checkout:  cd tools/shvi-cli && bun link     (then `shvi` runs the sources)

shvi doctor                 # what this machine can build, and how to fix the rest
shvi new                    # guided: name, platforms (PC, PS5), language (C++ / C), template, CI workflow
cd my-mod
shvi build                  # every target in shvi.json   (--target pc|ps5|sim)
shvi test                   # build + run the mod in the simulator (ctest)
shvi sim                    # run it in the simulator and print the menu
shvi package                # dist/<id>-<version>-pc.zip / -ps5.zip
shvi deploy ps5 --host 192.168.1.50            # FTP the image, send shvi_loader.elf (asks first)
shvi deploy ps5 --host 192.168.1.50 --dry-run  # send the mod image itself: runs outside the game
shvi deploy pc --game "D:\Games\GTA VI"
```

`shvi new` without flags asks everything. For scripts: `shvi new --yes --name "My Mod" --targets pc,ps5 --lang cpp --template trainer`.

### Templates

| Template | Language | What you get |
|---|---|---|
| `blank` | C or C++ | one script loop that logs every 5 s |
| `menu` | C++ | a menu (F4 / L1+R3) with Player, World and Settings pages |
| `trainer` | C++ | a mod-menu trainer: heal, god mode, never wanted, teleport, vehicle repair, lock time; every feature is offered only when the running build binds its natives |

### A generated project

```
my-mod/
  shvi.json                    name, id, version, language, template, targets, SDK version
  CMakeLists.txt               include(ShviMod.cmake) + shvi_add_mod(my_mod SOURCES src/main.cpp)
  src/main.cpp                 your mod (src/features.hpp for the trainer)
  .github/workflows/build.yml  simulate + pc (windows) + ps5 (ubuntu) jobs; zips attached to v* releases
  README.md  .gitignore
```

## What each platform needs

| Target | Host | Tools |
|---|---|---|
| `pc` | Windows | Visual Studio 2022+ with C++ (MSVC x64), CMake ≥ 3.24 (VS ships one) |
| `ps5` | Linux, or WSL from Windows | [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.50 (`shvi doctor --install-ps5-sdk`), clang-18, lld-18, CMake |
| `sim` | Linux / macOS (on Windows the PC build is simulated) | any C++20 compiler, CMake |

Only open-source homebrew tooling is used for PS5: no Sony SDK material.

## Without the CLI

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_mod LANGUAGES C CXX)
include($ENV{SHVI_SDK}/sdk/cmake/ShviMod.cmake)
enable_testing()
shvi_add_mod(my_mod SOURCES src/main.cpp)   # .asi / .elf / .so depending on the platform
shvi_add_ps5_loader()                       # PS5 only: also build shvi_loader.elf
```

```sh
cmake -S . -B build/pc -A x64 && cmake --build build/pc --config Release                                  # pc
cmake -S . -B build/ps5 -DCMAKE_TOOLCHAIN_FILE=$SHVI_SDK/sdk/cmake/toolchains/ps5.cmake && cmake --build build/ps5   # ps5
cmake -S . -B build/sim && cmake --build build/sim && ctest --test-dir build/sim                          # sim
```

`shvi_add_mod(<name> SOURCES ... [INCLUDES ...] [DEFINES ...] [LIBS ...] [SIM_ARGS ...] [NO_INSTALL])`.
On `pc` and `sim` it also builds `shvi_sim` and adds a `<name>_sim` test (300 frames, opens and walks the menu).

## Writing portable mods

The C ABI (`shvi/shvi.h`) is the same on every platform. Three rules keep a mod portable:

1. **Register with `SHVI_THIS_MODULE`**, never `&__ImageBase`:
   `api->script_register(SHVI_THIS_MODULE, ScriptMain, nullptr)`.
2. **No OS headers in the mod.** For files next to the mod use `shvi/platform.h`:
   `shvi_mod_data_dir(buf, n)` gives `<game>\shvi\` on PC and `/data/shvi/` on PS5. Use `<chrono>` for time.
3. **Gate on availability.** `shvi::native_exists("NAME")` (ABI 1.5) says whether the running build binds a
   native. Natives are bound per game build *and* per platform; a mod that checks runs everywhere and offers
   what is there. `world_get_all`, `present_register`, `script_function_call` return `SHVI_E_UNSUPPORTED`
   where a platform lacks them.

Input is the same everywhere: keys arrive as Windows virtual-key codes (`shvi::ui::vk`). On PS5 the runtime
maps the pad: **L1 + R3 → F4** (menu), d-pad → arrows, Cross → Return, Circle → Back, Square → Numpad5,
Triangle → Numpad0. `shvi::ui::InputQueue` works unchanged.

Platform macros, if you really need one: `SHVI_PLATFORM_WIN64`, `SHVI_PLATFORM_PS5`, `SHVI_PLATFORM_SIM`,
and the string `SHVI_PLATFORM_NAME`.

## The simulator

```
shvi_sim <plugin-file|dir> [--frames N] [--frame-ms N] [--key F:KEY]... [--bind NAMES] [--bind-all]
                           [--draw] [--trace] [--block-at F] [--expect TEXT]...
```

It hosts the portable engine with a simulated clock. Natives return zeros, except pure ones it answers
exactly (`STRING_HASH`). `--draw` prints the last frame's `UI_DRAW_TEXT` calls, i.e. your menu as text.
`--block-at` latches the online guard and checks that the mod stops. The exit status is non-zero if a plugin
fails to load, a script crashes, or an `--expect` text never appears in the log.

## PS5 specifics

- A PS5 mod image links **one** mod with the runtime (`platforms/ps5/runtime`) and the portable engine
  (`src/engine`) into a payload-format PIE ELF. The game process cannot `dlopen` our code (we map it in
  ourselves), so there are no shared plugins on PS5: one mod is one image.
- **Dry run.** Sent on its own to the ELF loader, `<mod>.elf` runs the mod for 10 s at 60 fps in its own
  process (natives return zeros) and reports by notification and in `/data/shvi/shvi.log`. That checks
  that the image loads, schedules and takes pad input on the console before any game is involved.
- **`shvi_loader.elf`** finds the game process, proves it can run code inside it (milestone 1: the game writes
  `/data/shvi/hello.log`), and lists the images in `/data/shvi/mods/`. Mapping an image into the game and
  hooking the script tick is the next milestone (see `platforms/ps5/README.md`).
- Natives: no PS5 native table exists yet, so `native_exists` is 0 for everything on PS5 and gated
  features stay hidden. The table comes from the PS5 NativeDB census, like the PC one.

## ABI and versions

`shvi.h` is append-only (ABI 1.6). The CLI and the SDK are released together; `shvi.json` pins
`sdk.version`, and `shvi` downloads that SDK release into `~/.shvi/sdk/<version>` on first use.
`--sdk <dir>` or `SHVI_SDK` points at a checkout instead.
