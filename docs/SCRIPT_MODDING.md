# Modifying GTA VI scripts and building mod menus

Static analysis of the Xbox Series build (`game_scarlett_master.exe`, PE timestamp 1794606036), 2026-11-23.
The IDA database is `../analysis/gta6_xbox/game.i64` and the query scripts are `q17.py` to `q24.py` next to
it. **Nothing here has been observed in a running game.** Where code depends on a fact, the fact is in
`src/games/gta6/vi_script.h` and unit-tested there. The raw findings are in the maintainer research notes (not published)
(Q4, fourth pass).

## 1. What a GTA VI script is

GTA VI keeps RAGE's VM-and-natives shape, but the language on top is new. **Scripts are compiled from a
C# subset.** `0x143DECAF0` registers 102 engine types into a 136-byte-per-entry table at `0x14ABC3CB0`. Each
type is keyed by `atStringHash` of its full name, and 66 of them hash to exact .NET BCL names:

- `System.Object`, `ValueType`, `String`, `Array`, `Enum`;
- `Delegate` and `MulticastDelegate`;
- every primitive type;
- `DateTime`, `TimeSpan`, `Guid`, `Random`, `GC` and `WeakReference`;
- reflection (`Type`, `MemberInfo`, `MethodInfo`, `FieldInfo`, `PropertyInfo`, `BindingFlags`);
- generics (`Dictionary`2`, `IList`1`, `KeyValuePair`2`, `ValueTuple`1-4`, `Span`1`, `Nullable`1`);
- async (`Task`, `Task`1`, `IAsyncStateMachine`, `AsyncTaskMethodBuilder`1`, `TaskAwaiter`1`).

`vi_script.h` `kBuiltinTypes` has all 102 by handle. The test checks every name against the hash the
engine registered.

The rest of the model follows from that:

| Concept | Where | Notes |
|---|---|---|
| Program (one compiled module) | paged resource, version 111; placed by `0x143E091C0`, linked by `0x143E09470` | `+0x10` name hash (the `ScriptStore` key), `+0x2C/+0x30` own types, `+0xE8` bytecode, `+0xF0` constants, `+0x144/+0x148` imports |
| Imports | 24-byte entries, sorted, resolved by name hash at link | Modules link against other modules (shared libraries); entry 0 is the program itself |
| Statics | one block per thread at `thread+0x18`; each module's statics at its import entry's offset | Opcodes 90-92 address `(module index, field offset)`. **There is no GTA V-style global array**: shared state lives in the statics of shared modules |
| Objects | GC heap: 16 MB old space `[0x14ABCE1E8]` with a 512-byte card table, 2 MB young space `0x14ABCE178` | Stores run a write barrier. Objects have an 8-byte header |
| Type handles | bit 31 set: engine type (bit 30 picks dynamic over built-in); bit 31 clear: a type of some program | `Classify()` in `vi_script.h` |
| Method calls | case 138 (engine type, method index), case 141 (virtual; checks an override map at `0x14AAE3870` first) | Built-in type methods are bound to C++ at registration (`0x1415B28D0`) |
| Natives (game API) | case 139: `handler = [0x1498C1810] + u32`, `handler(&{args, ret, argc, retc})` | No names or hashes in the exe. Each program carries its own handler offsets |
| Instructions | u16 opcode, case = `(u16)(raw * 0x3315)`, 530 cases; operand blocks of 8 bytes at `pc+2` and `pc+10` | Operand byte `i` decodes as `((b ^ key[i]) * 0x6D) & 0xFF`, with keys at `0x146178EE0` and `+0x10`. The engine encodes with the inverse (`* 0x65`, then xor) |

The operand codec, the opcode permutation and the tail-jump encoding are checked against the engine's own
encoder (`0x143E09AB0`) in `tests/tests.cpp`. `shvi_scan` reads the keys back from the exe and compares them,
so a PC build with new keys shows up straight away.

## 2. The engine's own script patch system

This is the most useful finding for modding. **RAGE can already patch scripts.**

1. A *patch program* carries a patch table: count at `+0x138`, offset into its data at `+0x134`, 16-byte
   entries `{kind, target program hash, a, b}`. When it is placed, `0x143E09FA0` copies the entries into a
   global map keyed by target hash (`0x1498C18B0`).
2. When any program links (`0x143E09470` → `0x143E09610`), the engine looks its name hash up in that map
   and applies each entry:
   - **kind 1, code patch** (`0x143E09AB0`): save the 18 bytes at `target.code + a`, then write a 14-byte
     **tail jump** (case 505, raw opcode `0xDC55` when the program's obfuscation flag at `+0x111` bit 0 is set, else raw
     `505`) to `patchProgram.code + b`. Case 505 drops the current frame and continues at the destination,
     so this is a function detour inside the VM. `0x143DEB230` restores the bytes on unload.
   - **kind 0, type patch** (`0x143E09750`): override a type's method in the map that case 141 consults.
     This is virtual-method replacement.
3. If the target is already linked when the patch arrives, `0x1429E7C00` applies it immediately.

So in this engine, "modify an existing script" means: get a patch entry into the map, and get code for
the patch to jump to.

## 3. Routes to modify existing scripts

Ordered by how soon each can work. Each one needs a running game for its first confirmation.

**A. Inventory (implemented).** The Xbox profile detours `script_program_link` read-only and logs every
program as it links: name hash, code pointer, type, import and patch counts, the obfuscation flag, and
whether the layout check passed:

```
gta6: script linked … name 0x… code … types … imports … patches …
```

The first runtime session therefore produces the script list, validates the `program::` offsets, and shows
whether shipped patch programs already exist. If one does, that's a sample of the format.

**B. Native-level behaviour changes (engine side, no bytecode).** Most "game behaviour" mods are calls the
scripts make: health, invincibility, wanted level, spawning, time and weather. Find the handler (route in
§5), then either:

- call it from a plugin through `BindViNative` (`engine_call.h`); or
- detour the handler with MinHook to change the result for *every* script that calls it, for example
  forcing the "can be damaged" argument to false for the player.

This needs no script decoding at all, only the handler address.

**C. In-VM function detours (bytecode, mirrors the engine).** Do what the patch system does, with our own
destination. `EncodeTailJump()` produces the exact 14 bytes the engine writes, and `DecodeTailJump()` reads
them back.

- C1. Destination in another script program: needs a program of our own, which means writing the
  container and compiler. That's the long route, and the format will be seen in A first.
- C2. Destination in C++: overwrite the function entry with a **native call (case 139)** whose offset
  points at a trampoline in our DLL, followed by a return.
  - The handler address is `base + u32`, so the trampoline must live within 4 GB above
    `[script_native_base]`. Allocate it there with `VirtualAlloc` at a hinted address.
  - The trampoline receives the script's arguments in `ViNativeContext.args`. That is a ScriptHookV-style
    "script function hook" calling into a plugin.
  - Blocker: the function-entry offsets of the target, which come from disassembling a placed program at
    runtime. Per-opcode lengths and kinds are now known (`vi_script.h` `kCaseLength`/`kCaseKind`; returns
    are cases 144–146). With them, a patch window can be picked on instruction boundaries.

**D. Statics.** Story-progress flags and similar live in module statics: thread `+0x18` + the import
entry's offset + the field offset. A `GlobalPtr`-like SDK call becomes "static of module X, field F". That
needs the module's name hash (from A) and the field layout (from disassembly or reflection metadata).

**Not a route:** decrypting the RPF archives on disk. It isn't needed, because everything above works on
programs the game has already loaded and decrypted itself. It also stays outside this project's rules (no
DRM work, no decrypted scripts in the repo).

## 4. Mod menus

The menu toolkit (`sdk/include/shvi/ui.hpp`) is game-independent and unit-tested. The GTA VI backend is
`sdk/include/shvi/vi_ui.hpp`, `shvi::vi::EngineBackend`. It calls three **engine contract names** through
the normal invoker:

```
UI_DRAW_RECT(float x, float y, float w, float h, u32 rgba)
UI_DRAW_TEXT(const char* utf8, float x, float y, float scale, i32 font, u32 rgba, i32 align)
UI_DISABLE_MENU_CONTROLS()
```

All coordinates are normalised 0..1 with the origin top-left. `align` is 0 for left, 1 for centre and 2 for
right.

`samples/menu_demo` uses it unchanged. Until a profile binds those names, the calls resolve to nothing: the
plugin runs and logs, but nothing draws. Binding them is the remaining work, by one of two routes:

1. **Game HUD natives.** Find the script natives behind rectangle and text drawing (§5), write small
   adapters with the contract signatures, and bind them with `engine_.Bind<…>(EngineHash("UI_DRAW_RECT"), …)`
   in `Gta6Xbox::Resolve` (or the PC profile).
2. **Overlay.** Add a DXGI/D3D12 present hook with an ImGui-style draw list (`ARCHITECTURE.md` §F). The
   adapters push into the overlay's list instead. The Xbox build uses `d3d12_xs`, so the hook point waits for
   the PC exe.

The plugin and menu code are identical on both routes.

## 5. Naming natives (the VI NativeDB)

> **Update (2026-11-23):** a decompiled dump of the 1.0.2.1 scripts supplies names for 1,809 natives. All
> 6,798 natives in it carry a 64-bit ID and handler RVA that match our census. The dump itself is not
> part of this repository. The dump also shows that entity handles are 16-byte GUIDs. The
> steps below still apply to unnamed handlers and to the PC build.

There are no names in the exe, so names come from behaviour or from placed programs:

1. **From programs at runtime (preferred).**
   - Walk each placed program's bytecode from `+0xE8`.
   - Find case-139 instructions: the opcode decodes to 139, operand bytes 0-3 are the handler offset, byte
     4 is argc and byte 5 is retc.
   - Collect `{handler, argc, retc, calling program, call site}`.
   - Walk linearly with `DecodeAt` and `kCaseLength` (`vi_script.h`). Every case has a fixed length (2–18
     bytes), derived from the VM.
2. **Name handlers by diffing.**
   - Compare VI handlers against GTA V's with BinDiff or Diaphora on a GTA V exe, outside this repo. Health,
     invincibility, coords and text handlers are short and distinctive.
   - Keep the result as a per-build table `{EngineHash name → handler RVA}` loaded like the crossmap. The
     handler offsets are build-bound (Q6), so the table must be keyed by build.
3. **Bind.** Use `EngineTable::BindViNative(EngineHash("SET_PLAYER_INVINCIBLE"), handler)` and similar. The
   SDK side is ready: plugins call `invoke<void>(sdk::EngineHash("…"), args…)`.

## 6. Next steps

1. First runtime session (PC build, or under `gta6rt` with no network):
   - read the inventory lines and confirm the `program::` offsets (`LAYOUT-MISMATCH` flags a bad one);
   - see whether patch programs ship;
   - dump one placed program's bytecode to a local file outside the repo.
2. ✅ Per-case instruction lengths and kinds are in `vi_script.h` (TASKS T1.1). What's left is to decode a
   dumped function with them.
3. ✅ Case-139 walker and census (`vi_script.h` `Walk`, `ForEachNativeCall`; link-time census behind
   `shvi\native_census.on`). Player health, pad and the first gameplay names (route B) are in the
   per-build native table. There are no drawing natives to bind: menus draw through the overlay (§4,
   route 2). `UI_DISABLE_MENU_CONTROLS` is bound to DISABLE_CONTROL_ACTION.
4. ✅ Trampoline allocator (`core/trampoline.*`). Still to do: the C2 detour prototype on one harmless
   script function (TASKS T6.3), then the SDK call (ABI 1.4) for script function hooks.
5. ✅ 101 of the 102 built-in types are named; index 73 (`0x64DE629C`) is still unknown.
