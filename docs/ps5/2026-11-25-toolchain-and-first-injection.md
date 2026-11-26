# Toolchain and First Injection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install ps5-payload-sdk in WSL, create `scripthook-vi-ps5`, and reach the first milestone: a payload that injects code into the running GTA VI process, and that code writes a log line from inside the game.

**Architecture:** An ELF payload (`payload/`) is sent to the console's elfldr on port 9021. It finds the game process, attaches with ptrace, maps an executable page in the target, copies in a small position-independent stub, runs it on a borrowed thread, and restores the registers. The stub opens and writes `/data/shvi/hello.log` from inside the game. A host-side tool (`tools/self2elf.py`) turns the fake SELF into a plain ELF for offline analysis.

**Tech Stack:** ps5-payload-sdk v0.50 (clang-18, lld-18, FreeBSD-derived libc), GNU make, Python 3 (WSL) with `unittest`.

**Spec:** `docs/superpowers/specs/2026-11-25-ps5-port-design.md`

## Global Constraints

- Story mode / offline only; the console stays off PSN while modded.
- Ask the user before every console run (each send to port 9021 counts).
- No Sony SDK material; only ps5-payload-sdk / elfldr (open source).
- The eboot (`C:\Users\RTX3050\Downloads\eboot.bin`) and anything derived from it stay local: `work/` is git-ignored and never published.
- No build dependency on `../scripthook-vi`; shared code is copied into `src/shared/`.
- Commits only when the user asks (harness rule); steps that would commit say "checkpoint" instead.
- Toolchain path `/opt/ps5-payload-sdk`, env `PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk`; payload port 9021.

## Review Focus

1. **Game not running when the payload is sent** → the payload logs "game process not found" over klog/notify and exits without touching any process. Test: Task 4 step "no game".
2. **The borrowed thread is inside a syscall when we stop it** → registers must be saved and restored exactly, including `rax`/`rip` around the interrupted syscall, or the game crashes later. Test: Task 4 checks the game keeps running for 60 s after injection.
3. **Two game-like processes (e.g. a second `eboot.bin` from another app)** → the injector picks the one whose title id is GTA VI's, not the first match. Test: Task 4 `find_game` unit check on the `ps`-style list.
4. **SELF with a non-block entry for a segment, or a truncated file** → `self2elf.py` refuses with a clear error rather than writing a partial ELF. Test: Task 2 `test_truncated_self_rejected`.
5. **Payload sent twice** → the second injection must not leak a second mapping silently; it logs the mapping address each time so leaks are visible. Covered by Task 4's log format.

---

### Task 1: Install the toolchain and build the SDK's hello_world

**Files:** none in the project (system install in WSL).

- [ ] **Step 1: Install packages as root (no password prompt)**

Run: `wsl -d Ubuntu-24.04 -u root -- bash -lc "apt-get update && apt-get install -y bash clang-18 lld-18 wget unzip make socat netcat-openbsd python3 python3-pyelftools xxd git"`
Expected: exit 0; `clang-18 --version` prints version 18.

- [ ] **Step 2: Download and unpack the SDK**

Run: `wsl -d Ubuntu-24.04 -u root -- bash -lc "cd /tmp && wget -q https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip && unzip -o -q -d /opt ps5-payload-sdk.zip && ls /opt/ps5-payload-sdk"`
Expected: the directory exists with `include/ps5/kernel.h`, `include/ps5/mdbg.h` and the toolchain files. If the zip unpacks under another name, move it to `/opt/ps5-payload-sdk`.

- [ ] **Step 3: Persist the env var for the default user**

Append `export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk` to the default user's `~/.bashrc`.

- [ ] **Step 4: Build hello_world from a copy of the SDK sample**

Copy the SDK's `samples/hello_world` (from the zip, or a shallow clone of `ps5-payload-dev/sdk` at tag v0.50 into `~/src/ps5-sdk`) and run `make` in it.
Expected: `hello_world.elf` exists; `file hello_world.elf` reports an x86-64 ELF.

- [ ] **Step 5: Record what the SDK headers provide**

Read `include/ps5/kernel.h`, `include/ps5/mdbg.h`, `include/ps5/payload.h` and note the exact names and signatures of the process r/w, ptrace and notify helpers in `docs/SDK_NOTES.md` (created in Task 2's tree). Task 4 uses these names.

### Task 2: Project scaffold and `self2elf.py`

**Files:**
- Create: `README.md`, `.gitignore` (`work/`, `build/`, `*.elf`), `Makefile` (top level, calls `payload/`), `docs/SDK_NOTES.md`
- Create: `src/shared/` (copied from `../scripthook-vi/sdk/include/shvi/ui.hpp`, `vi_ui.hpp`, `shvi.h`, `src/core/joaat.h`, `json.h`, `json_write.h`, with a `SOURCES.md` naming each origin path)
- Create: `tools/self2elf.py`
- Test: `tests/test_self2elf.py`

**Interfaces:**
- Produces: `self2elf.convert(self_bytes: bytes) -> bytes` (plain ELF), CLI `python3 tools/self2elf.py <in.self> <out.elf>`; raises `SelfError(str)` on bad input.

- [ ] **Step 1: `git init` the new project** (local repo only, no remote).

- [ ] **Step 2: Write the failing tests** in `tests/test_self2elf.py` with a synthetic SELF built in the test (header magic `0x1D3D154F`, 2 entries: one non-block, one block entry with props `(1<<20)|0x800|0x4` for phdr 0), an embedded ELF header with one PT_LOAD:
  - `test_plain_segment_copied`: output bytes at the phdr's `p_offset` equal the block entry's data, and output starts with the embedded ELF header + phdrs.
  - `test_encrypted_entry_rejected`: props with bit `0x2` → `SelfError` containing "encrypted".
  - `test_compressed_entry_rejected`: bit `0x8` → `SelfError` containing "compressed".
  - `test_truncated_self_rejected`: entry offset+size past EOF → `SelfError` containing "truncated".

- [ ] **Step 3: Run them to see them fail**

Run: `wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Users/RTX3050/Downloads/temp/scripthook-vi-ps5 && python3 -I -m unittest tests.test_self2elf -v"`
Expected: FAIL (module not found).

- [ ] **Step 4: Implement `convert()`**: ELF at `0x20 + n_entries*32`; output size = max(`p_offset + p_filesz`) over phdrs; copy ELF header and phdr table; for each block entry (`props & 0x800`), segment index = `(props >> 20) & 0xFFF`, copy `filesz` bytes to that phdr's `p_offset`. Non-block entries (signature/digest blocks) are skipped.

- [ ] **Step 5: Run tests to pass**, then run on the real eboot

Run: `python3 -I tools/self2elf.py /mnt/c/Users/RTX3050/Downloads/eboot.bin work/game_prospero_master.elf && python3 -c "from elftools.elf.elffile import ELFFile; e=ELFFile(open('work/game_prospero_master.elf','rb')); print(e['e_entry'], e.num_segments())"`
Expected: tests PASS; entry `70674320` (0x4366790), 14 segments; bytes at file offset 0x4000+0x10 begin `554889e5`.

- [ ] **Step 6: Checkpoint.**

### Task 3: Console smoke test with hello_world

**Files:** `docs/CONSOLE.md` (how to send, the console's IP placeholder `PS5_HOST`, what was seen).

- [ ] **Step 1: Ask the user** for the console IP and to confirm the ELF loader (elfldr on 9021, from their kstuff-lite setup) is running. Ask before sending.
- [ ] **Step 2: Send** `nc -q0 $PS5_HOST 9021 < hello_world.elf` from WSL.
Expected: the user sees the hello_world output (klog or notification). Record the result in `docs/CONSOLE.md`.

### Task 4: Injector payload, first milestone

**Files:**
- Create: `payload/Makefile` (SDK style, output `build/shvi_inject.elf`)
- Create: `payload/main.c` (entry, logging via the SDK's notify/klog helper)
- Create: `payload/proc.c`, `payload/proc.h`: `pid_t find_game(const char *title_id)`; returns -1 if not found, picks the process whose title id matches (default `PPSA` id read from the game's `param.json` / proc info, passed as `argv[1]` or a compile-time default).
- Create: `payload/inject.c`, `payload/inject.h`: `int inject_stub(pid_t pid, const void *code, size_t len, uint64_t *out_addr)`; attach, save regs, remote `mmap` RW, copy, remote `mprotect` RX, set `rip` to the stub with a return-to-`int3` frame, continue, wait for SIGTRAP, restore regs, detach. Returns 0 on success, negative errno-style on failure; never leaves the target stopped.
- Create: `payload/stub.S`: position-independent stub that calls the `open`/`write`/`close` syscalls on `/data/shvi/hello.log` (mkdir `/data/shvi` from the payload first), writing `hello from inside GTA VI pid=<pid>\n`, then `int3`.
- Vendor: ptrace helpers from elfldr (`pt.c`/`pt.h`) under `third_party/elfldr/` with its LICENSE, or the SDK's `mdbg.h` equivalents if Task 1 step 5 shows they cover attach/regs/copy/remote syscall. Note the license in README.

- [ ] **Step 1: Build** `make -C payload`. Expected: `build/shvi_inject.elf`, no warnings at `-Wall -Wextra`.
- [ ] **Step 2: "No game" run** (ask the user first; game closed): send the payload. Expected: notification "shvi: game process not found", nothing else.
- [ ] **Step 3: Injection run** (ask the user first; game in story mode, offline): send the payload. Expected: notification with pid and mapped address; `/data/shvi/hello.log` contains the line (read via the console's FTP or a `cat` payload); the game is still running 60 s later.
- [ ] **Step 4: Re-send once more.** Expected: a second log line and a second address logged; game still running.
- [ ] **Step 5: Record results** in `docs/CONSOLE.md` and `docs/STATUS.md`; checkpoint.

## Next plans (not in this one)

Detours and the script-manager tick hook, the pad hook, the flip-hook renderer, porting the 18 signature targets and the native table against `work/game_prospero_master.elf`.
