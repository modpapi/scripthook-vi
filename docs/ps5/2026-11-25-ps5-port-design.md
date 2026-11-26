# ScriptHook VI for PS5: design (short)

Approved by the user on 2026-11-25.

## Intent

Port the ScriptHook VI mod menu and trainer (`../scripthook-vi`) to the user's jailbroken PS5, for story mode only.

## Known facts

- Console: firmware 7.01 with kstuff-lite.
- Game binary: `C:\Users\RTX3050\Downloads\eboot.bin`, the user's own dump. It is a decrypted fake SELF: none of its 12 entries is encrypted or compressed, and the ELF header is at 0x1a0 (type 0xfe10, entry 0x4366790). The embedded build path is `game_prospero_master.elf`. PROCPARAM gives SDK 0x07000038 (7.00).
- Host: Windows 11 with WSL2 Ubuntu-24.04.

## Approaches

| | Approach | Trade-off | Decision |
|---|---|---|---|
| A | Injector ELF payload sent to the ELF loader; it attaches to the running game and maps our code in | Fast rebuild and resend loop; misses the game's boot, which a menu does not need | **Chosen by the user** |
| B | Game-loaded plugin (patch the game's fself so it loads our module) | Hooks are in place from the start; every change needs a re-sign and reinstall | Later, as packaging, if wanted |
| C | External memory editing only | No tick and no safe native calls, so it cannot host a menu | Dropped |

## Design

- **Toolchain:** ps5-payload-sdk (open source, Clang/LLD) installed in WSL Ubuntu. Proven by building its hello-world and sending it to the console's ELF loader.
- **Project:** `temp/scripthook-vi-ps5`, with no build dependency on the Windows tree.
  - `payload/`: the injector. It finds the GTA VI process, attaches, maps our code in, starts it, then detaches.
  - `src/platform/`: ELF instead of PE, signal handlers instead of SEH, `ucontext` instead of fibers, our own x86-64 detours instead of MinHook, pad-read hook for input, video-out flip hook with CPU drawing for the menu.
  - `src/shared/`: game-independent code copied from scripthook-vi (`ui.hpp`, `vi_ui.hpp`, C ABI, json, joaat, crossmap, guard, trainer logic). Copied, not referenced.
  - `tools/`: an ELF/SELF loader for the offline signature scan against the eboot.
- **First milestone:** the payload attaches to the running game and writes a log line from inside the game. No hooks yet.

## Constraints

- Story mode / offline only. The online guard stays, and the console stays off PSN while modded.
- Ask the user before every console run.
- No Sony SDK material; only open-source homebrew tooling.
- The eboot and anything derived from the game binary stay local; nothing is uploaded.

## Left to the builder

- Directory details below the top level, the build system (Makefile following the SDK's examples), and test layout.
- How the injector maps code (ptrace plus mmap in the target, or kernel r/w via kstuff), chosen after the toolchain works and the SDK's options are known.
