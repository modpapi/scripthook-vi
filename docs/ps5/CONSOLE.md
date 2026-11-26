# Console runs

How to send ELFs to the jailbroken PS5 (firmware 7.01, kstuff-lite) and what
was seen. **Ask the user before every send** (each is a console action).

## Setup

- `PS5_HOST` = the console's IP on the LAN (set per run; not committed).
- ELF loader: elfldr listening on TCP **9021** (from the user's kstuff-lite
  setup). Send with:

      nc -q0 $PS5_HOST 9021 < <file>.elf

  or `make -C payload test PS5_HOST=<ip>` (uses the SDK's prospero-deploy).

- Story mode / offline only; the console stays off PSN while modded.

## Task 3 - hello_world smoke test

- [ ] Build the SDK sample `hello_world.elf` and send it. Expected: its output
      on klog / a notification. Confirms the loader path before our payload.

Result: _pending user console run._

## Task 4 - injector payload

Payload: `payload/build/shvi_inject.elf` (built; `make -C payload`).

- [ ] **No-game run** (game closed): send the payload. Expected notification:
      `shvi: game process not found`, nothing else.
- [ ] **Injection run** (game in story mode, offline): send the payload.
      Expected: notification `shvi: injected into pid <pid>, wrote
      /data/shvi/hello.log (fd <fd>)`; the file contains
      `hello from inside GTA VI pid=<pid>`; the game is still running 60 s later.
- [ ] **Re-send**: a second line in the log and a second fd; game still running.

Result: _pending user console run._

### First-run unknowns to confirm on hardware

These are modeled on known-good homebrew patterns but not yet run here:

1. **Process name match.** `find_game` matches `ki_comm` / `ki_tdname` against
   `eboot.bin` (override with argv[1]). Confirm the game's actual comm with the
   SDK `ps` payload; adjust the default if it differs.
2. **libkernel symbol resolution.** `resolve()` tries module basenames
   `libkernel.sprx`, `libkernel_sys.sprx`, ... for `open`/`write`/`close`. If
   it logs `resolve ... failed`, read the real module name/symbols from the
   `ps`/dynlib output and update the list.
3. **Borrowed-thread return trampoline.** The int3 is placed at the ELF entry
   (`kernel_dynlib_entry_addr`). If the game crashes on the call, check that
   the entry page is executable and that the SIGTRAP is caught cleanly.

Read `/data/shvi/hello.log` after a run via the console's FTP (port 1337 on
most setups) or a small `cat` payload.
