# Status

## Done

- **Task 1** - ps5-payload-sdk v0.50 installed in WSL; hello_world builds; SDK
  header API recorded in `SDK_NOTES.md`.
- **Task 2** - project scaffold; `tools/self2elf.py` converts the eboot fake
  SELF to a plain ELF (`work/game_prospero_master.elf`). Host tests pass.
- **Task 4 (build)** - injector payload builds clean (`payload/build/shvi_inject.elf`,
  `-Wall -Wextra`, no warnings):
  - `proc.c` / `proc_select.c` - find the game process (sysctl KERN_PROC +
    pure selection policy; 6 host unit tests pass).
  - `inject.c` - borrowed-thread remote-call engine; realizes the hello
    milestone by remote-calling the game's `open`/`write`/`close`.
  - `main.c` - entry: find game, mkdir `/data/shvi`, inject, notify.

## Blocked on a console run (ask the user first)

- **Task 3** - send `hello_world.elf` to elfldr (needs console IP).
- **Task 4 steps 2-4** - no-game run, injection run, re-send. See
  `docs/CONSOLE.md` for the expected notifications and the first-run unknowns
  to confirm on hardware.

## Deviations from the plan

- The plan's inject path (remote `mmap` RW + copy a `stub.S` + `mprotect` RX +
  run) assumes RWX anonymous memory. PS5 enforces W^X, so executable allocation
  is a JIT dance deferred to the code-mapping task. Milestone 1 instead
  remote-calls the game's own already-executable `open`/`write`/`close`, which
  exercises the same attach/run/restore pipeline. No `stub.S`.

## Next (later plans)

- Code mapping into the target (JIT shared memory) to run our own bytes.
- Detours + script-manager tick hook, pad input hook, flip-hook renderer.
- Port the 18 signature targets and the native table against
  `work/game_prospero_master.elf`, then the trainer/mod-menu UI.
