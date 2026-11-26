# ScriptHook VI on PS5

The PS5 platform of the one ScriptHook VI engine. Story mode and offline only; keep the console off PSN.
Merged from the former `scripthook-vi-ps5` repository (history up to `15420bf`); its design notes are in
[`docs/ps5/`](../../docs/ps5/).

## Layout

| Path | What |
|---|---|
| `runtime/` | the PS5 runtime: the portable engine (`src/engine`) + log, pad mapping, lifecycle. Linked with one mod into `<mod>.elf` by `shvi_add_mod` |
| `runtime/runtime_main.cpp` | the image's standalone entry: the **dry run** (mod in its own process, no game) |
| `runtime/pad_map.c` | DualSense buttons → SHVI key codes (L1+R3 = F4, d-pad = arrows, Cross = Return, Circle = Back) |
| `payload/` | `shvi_loader.elf`, sent to the ELF loader: finds the game, runs code inside it, lists mod images |
| `tools/self2elf.py` | converts a decrypted fake-signed SELF (your own dump) to a plain ELF for offline analysis |
| `tests/` | host tests: `test_self2elf.py`, `test_proc.c` (`make test`) |
| `work/` | game-derived files: git-ignored, never committed or shared |

## Build

```sh
# with the shvi CLI (Linux, or WSL from Windows): builds every mod image + the loader
shvi build --target ps5
# or plain CMake on this repo (samples as PS5 images + shvi_loader.elf)
cmake -S . -B build/ps5 -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/toolchains/ps5.cmake && cmake --build build/ps5
make -C platforms/ps5 test        # host tests (python + C)
```

Toolchain: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.50 (GPLv3, John Törnblom), clang-18,
lld-18. `shvi doctor --install-ps5-sdk` fetches the SDK into `~/.shvi/ps5-payload-sdk`.

## Run (ask before every console action)

1. FTP `<mod>.elf` to `/data/shvi/mods/`.
2. Start GTA VI, story mode, offline.
3. Send `shvi_loader.elf` to elfldr: `nc -q0 <ip> 9021 < shvi_loader.elf` (or `shvi deploy ps5 --host <ip>`).

Dry run: send `<mod>.elf` itself. Expected notification: `shvi: dry run done, 600 frames, 1 script(s) live, 0 crashed`.

## Status

| Piece | State |
|---|---|
| Toolchain, CMake toolchain file, mod images + loader build | ✅ built in WSL and CI |
| Runtime: engine, ucontext scheduler, log, pad mapping | ✅ host-tested (Windows + Linux); 🧪 on console (dry run not yet run) |
| Loader milestone 1: find the game, remote-call `open`/`write`/`close` inside it | 🧪 built, not yet run on hardware (`docs/ps5/CONSOLE.md`) |
| Map a mod image into the game (W^X: JIT shared memory) and call `shvi_ps5_runtime_start` | 📐 next |
| Script-tick detour → `shvi_ps5_runtime_frame`, pad-read hook → `shvi_ps5_runtime_pad` | 📐 |
| PS5 signature profile + native table (from `work/game_prospero_master.elf`) | 📐 until then `native_exists` = 0 |
| Online guard (game mode read) → `shvi_ps5_runtime_block` | 📐 |
| Drawing: `UI_DRAW_*` bound to a flip-hook renderer | 📐 |

## Licensing

The payload is built against the ps5-payload-sdk (GPLv3). The ptrace privilege swap in `payload/inject.c`
follows that SDK's `samples/test_privileges/main.c`; the process walk in `payload/proc.c` follows its
`samples/ps/main.c`. No third-party source is vendored.
