# ScriptHook VI: architecture

Scope: **story mode / offline only, non-commercial.** No anti-cheat or DRM evasion, no online features. When
protection or an online session is present, the hook stays off.

## Layers

```
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  PLUGINS        C/C++ .asi (shvi.h)   │  .NET 8+ scripts (.dll)  │  Lua scripts │
 ├───────────────────────────────────────┼──────────────────────────┼──────────────┤
 │  E  MANAGED / SCRIPT HOSTS            │ shvi_dotnet: hostfxr +   │ shvi_lua:    │
 │     (optional plugins of the core)    │ collectible ALC, async   │ coroutines   │
 │                                       │ ticks on game thread     │ = WAIT       │
 ├───────────────────────────────────────┴──────────────────────────┴──────────────┤
 │  D  C SDK / ABI   shvi_api function table, append-only, size+major/minor gated  │
 ├──────────────────────────────────────────────────────────────────────────────────┤
 │  F  OVERLAY / UI   Present/ExecuteCommandLists hooks (D3D12), Vulkan layer,     │
 │                    ImGui (render thread) + native drawing (game thread), input  │
 ├──────────────────────────────────────────────────────────────────────────────────┤
 │  B  CORE (generic)                                                               │
 │     scanner ─ invoker ─ fiber scheduler ─ plugin host ─ crossmap ─ log          │
 │                 │                                                                │
 │  G  ONLINE GUARD  preflight (AC modules, cmdline) → runtime probe → LATCH        │
 ├──────────────────────────────── GameProfile seam ───────────────────────────────┤
 │  GAME PROFILES  gta6 Xbox (analysis)      │   gta6 PC (placeholder)              │
 │  signatures, offsets, tick source, mode probe, engine functions                  │
 ├──────────────────────────────────────────────────────────────────────────────────┤
 │  C  NATIVEDB + CROSSMAP PIPELINE (offline tooling)                               │
 │     runtime dumper → per-build dump → matcher → crossmap/<build>.txt + DB        │
 ├──────────────────────────────────────────────────────────────────────────────────┤
 │  A  LOADER   proxy DLL picked from the exe's imports (dinput8/version/…)         │
 │              → loader thread → shvi\shvi_core.dll                                │
 └──────────────────────────────────────────────────────────────────────────────────┘
                          game process (GTA VI PC exe)
```

**Generic vs game-specific.** Everything above the `GameProfile` seam (`src/core/game_profile.h`) knows no byte
pattern, offset, hash or script name. A new game or build family means writing one profile. The core, the
SDK, the managed hosts and the plugins don't change.

| Code | Generic | Game-specific |
|---|---|---|
| `src/loader/*` | ✅ jump-stub proxy, any DLL | only the export list (generated) |
| `tools/pick_proxy` | ✅ | — |
| `src/core/pattern.*`, `pe.*`, `log.*`, `crossmap.*`, `joaat.h` | ✅ | — |
| `src/core/fiber_scheduler.*` | ✅ | — |
| `src/core/native_context.h`, `invoker.*`, `engine_call.h` | ✅ call frame, thunks, VI native context | resolver and bindings come from the profile |
| `src/core/online_guard.*` | ✅ policy | probes and module lists come from the profile |
| `src/games/gta6/*` | — | ✅ Xbox signatures, engine table, script runtime model (`vi_script.h`), tick detour, mode probe, script inventory; PC placeholder |

---

## A. Loader

**How it picks the proxy.** `shvi_pick_proxy <game.exe>` reads the import and delay-import tables from disk and
ranks candidates. A candidate must:

1. be imported by the exe (static is best; delay-load only works if it loads before gameplay);
2. live in System32 (if the game ships the DLL, replacing it is fragile);
3. not be listed under `HKLM\…\Session Manager\KnownDLLs`, because those are mapped from `\KnownDlls` and never
   searched for in the app folder;
4. not be an API set (`api-ms-*`) or a side-by-side assembly (comctl32, gdiplus).

Ranking prefers small, boring DLLs: dinput8 > version > winmm > winhttp > xinput. Render DLLs (dxgi, d3d12)
come last, because Steam, RTSS, ReShade and the like hook them too. `--emit <dll> <dir>` then generates the
`.def` and the names list from the real DLL's export table.

**What the proxy does.** All 64 export stubs are one MASM `jmp [table+N*8]` each, so a proxy is just data (the
names list plus a `.def`). `DllMain` loads the real DLL **by absolute System32 path** and fills the table. It
starts one loader thread and returns. The loader thread loads `shvi\shvi_core.dll` and calls
`shvi_core_start()`. Nothing heavy happens under the loader lock. If `shvi_core.dll` is absent, the proxy is a
pure pass-through. `tests/proxy_test.cpp` checks the forwarding end to end.

The core, not the loader, decides whether to act. It checks the exe name and version against the profiles and
stays inert in a launcher or any other process.

**Corrections to the brief.** ScriptHookV's `dinput8.dll` is an ASI loader: it loads `*.asi` files.
`ScriptHookV.dll` is pulled in because each `.asi` links its import library and imports `scriptRegister`,
`nativeCall` and the rest by **mangled C++ name**. ScriptHook VI doesn't use import libraries. Plugins export
`shvi_plugin_load` and receive a function table, so the core can be renamed or relocated without
breaking plugins. Plugins live in `shvi\plugins\`, so they never collide with ScriptHookV `.asi` files in the
game root.

## B. Core

### Patch-tolerant signature scanner (`src/core/pattern.h`)

- **Targets with several candidates.** Each candidate is an IDA pattern plus ops: `Add`, `Rip(trailing)`,
  `Deref`, and `ReadI32` for struct offsets read from code, as NativeMemory does. Candidates are tried in
  order.
- **Uniqueness and validation.** A candidate wins only if every raw match that survives the ops and the
  validator resolves to the same value. If a candidate is ambiguous or yields nothing, the next one is tried.
  Validators check that a table points into the image and that a function points into code.
- **Required vs optional.** Missing optional targets degrade a feature (for example the generic vector
  fix-up replaces the game's routine). Missing required targets keep the core inert. The log names the
  candidate that won, which shows which anchors survived a patch.
- **Retry window.** Protected executables decrypt code at startup, so resolution retries for about two
  minutes before giving up. This is waiting, not tampering.
- **Next steps (not in the skeleton):**
  - A result cache keyed by the `.text` hash, so a known build skips the scan.
  - "Diff mode": when a target fails on a new build, dump the bytes around the old RVA so a new candidate
    can be written in minutes.
  - String-xref anchors (find a string, then the `lea` that references it, then walk to the function start)
    and call-graph anchors, which survive recompiles better than raw instruction bytes.

### Native resolution

The profile exposes `FindTarget(hash)`, which returns a `NativeTarget`: a thunk with a function pointer
(`src/core/engine_call.h`), or nothing. The invoker caches results, negative ones included. A crossmap
(`shvi\crossmap\<build>.txt`) rewrites hashes before lookup.

The GTA VI Xbox binary has a script VM with a new ABI (research plan Q4, outcome (b)), but no native
registry: the VM calls handlers by obfuscated code offset stored in each compiled program, so nothing in
the exe maps names or hashes to handlers (`docs/SCRIPT_MODDING.md` §5). There are two kinds of binding:

- **A VI script native,** once its address is known, is bound with `EngineTable::BindViNative`. Its thunk
  rebuilds the VM's 24-byte `ViNativeContext {args*, ret*, argc, retc}` from ours.
- **A plain engine function** found by signature is bound with `EngineTable::Bind<R(A...)>`. A generated
  thunk unpacks the 8-byte argument slots, calls the function, and writes the result in script layout. An
  engine `Vector3` return is widened to the 24-byte `scrVector`.

The SDK stays hash-based. Names live in their own space, `EngineHash(name) = 0x5649 << 48 | atStringHash(name)`
(`shvi_engine_hash` in the SDK).

### Invocation (`src/core/invoker.h`)

- Fills a `NativeCallContext` (32 argument slots, 4 return slots), calls the handler inside `__try`, then runs
  the vector fix-up.
- A bad argument that faults inside a native returns zeros and gets logged; it doesn't crash the game.
- Two paths:
  - **Gated** (`Invoke`, `Begin/Push/Call`), used by plugins. When the online guard is closed it calls
    nothing.
  - **Unchecked** (`InvokeUnchecked`), used only by the guard's own probes.
- Plugin calls to the game's `WAIT` native become a fiber yield. Running the real handler would put the
  borrowed game thread to sleep instead of the plugin.

### Execution context and model

- **Tick source (profile).** GTA VI (Xbox build): a MinHook detour on the script manager's per-frame update
  (`0x1429DFAF0`, registered with the game skeleton under `0x0B3A5867`). `TickGate` enforces one tick per frame
  on one thread, because two update groups call the same function. The core then runs the fibers on that
  thread. Whether VI natives need the current-script TLS slot (`+0x1478`) set during plugin calls is Q8.
- **Owned thread (planned, opt-in per plugin).** Some plugins need their own script thread: entity
  ownership, or persistence. That means creating a VI script thread object (the classes whose vtables hold
  `script_call`, research plan Q4) and is not designed yet.
- **Scheduler (`src/core/fiber_scheduler.h`).**
  - One fiber per registered script. `Tick()` runs every ready fiber until it calls `Wait()`.
  - The caller fiber is captured on every tick (the game thread may already be a fiber, and that can change
    between ticks; SHVDN detects reloads this way).
  - A script that throws or faults (C++ or SEH) is marked crashed and never resumed. The game and the other
    scripts keep running.
  - Adding and removing scripts is thread-safe; the changes apply at the next tick boundary.

## C. Own NativeDB, crossmap and dumping across versions

**DB schema (JSON, one file per game).**

```json
{ "name": "GET_ENTITY_COORDS", "namespace": "ENTITY",
  "params": [{"type":"Entity","name":"entity"},{"type":"BOOL","name":"alive"}],
  "returns": "Vector3", "vector_out_params": 0,
  "builds": { "3586": {"hash":"0x3FEF770D40960D5A", "handler_rva":"0x…", "fp":"…"} },
  "first_build": "…", "notes": "" }
```

**Pipeline**, runnable on every patch:

1. **Runtime dumper** (a debug-only core mode, offline): walk the registry and write
   `(hash, handler RVA, handler fingerprint)` for the build. The fingerprint is a normalized hash of the first
   N instructions, with displacements and immediates masked, plus the callee count.
2. **Script-side dump.** For each script, list its native table in program order. Scripts change slowly, so
   the same script across two builds gives position-aligned native lists. That is a strong crossmap signal
   when hashes rotate.
3. **Matcher.** Same hash, same native. Otherwise use fingerprint equality, then call-graph neighbourhood
   (BinDiff or Diaphora exports), then script-table alignment. Every pairing carries a confidence score and
   low-confidence pairs go to a review list.
4. **Emit** `crossmap/<build>.txt` (the format `src/core/crossmap.h` reads), the updated DB, and generated SDK
   headers (`natives.h`, C# bindings, Lua stubs) from the DB.
5. **CI check**: every native used by the sample plugins resolves on the new build.

GTA VI has no native hashes at all: handler offsets and the opcode permutation are bound to one build
(research plan Q6). The pipeline therefore keys the DB by handler identity (diffing) and emits a per-build
`{name → handler RVA}` table that the profile binds with `BindViNative`.

## D. C SDK with a stable, versioned ABI (`sdk/include/shvi/shvi.h`)

- Plain C, x64, no mangling, no import library. The host passes `const shvi_api*` to `shvi_plugin_load`.
- `shvi_api` starts with `size`, `abi_major` and `abi_minor`. It is **append-only**: fields never move or
  change meaning. A new function means a minor bump. A breaking change means a major bump, and the host
  refuses plugins built for another major. A plugin checks `api->size >= sizeof(shvi_api)` for the fields it
  was compiled against (`shvi_api_compatible`).
- Status codes, never exceptions, cross the boundary.
- **ABI 1.1.** It appends `tick_register(module, fn, user)`. A plugin built for 1.1 that should
  also load on a 1.0 host checks `SHVI_API_HAS(api, tick_register)` instead of `shvi_api_compatible`.
- **ABI 1.2.** It appends `world_get_all(kind, out, max)` and `entity_ptr(ref)` over 16-byte
  `shvi_entity_ref`s (VI scripts have no int handles). Same rules: script callbacks only, refused once
  the guard latches, `SHVI_E_UNSUPPORTED` for a kind whose pool the profile has not located. The C++
  wrapper's `shvi::world_get_all` / `shvi::entity_ptr` check `SHVI_API_HAS` themselves.
- **ABI 1.3 (current).** It appends `static_ptr(program_hash, module_hash, field)`, VI's `getGlobalPtr`.
  It returns field `field` of module `module_hash`'s statics in the first running instance of
  `program_hash` (module 0 = the program's own statics), using the VM's own import lookup
  (`vi_script.h` `StaticAddress`). Script callbacks only. The wrapper is `shvi::static_ptr<T>`.
- Threading contract:
  - `native_*` work inside script fibers and inside `tick_register` callbacks, on the game thread.
  - `script_wait` works only in fibers. In a tick callback a `WAIT` is ignored: return instead.
  - `keyboard_register` callbacks run on the game window's thread, from a `WndProc` subclass that keeps an
    ANSI window ANSI. They must not call natives.
  - The host refuses (and logs once) natives called from anywhere else. SHV crashes in that case.
  - Tick and keyboard callbacks are SEH-guarded. One that faults is dropped instead of crashing the game.
  - `module_unregister` removes a module's fibers, ticks and keyboard callbacks. The owner of a keyboard
    callback is the module containing `fn`.
- `shvi.hpp` is header-only C++ sugar (`shvi::invoke<R>(hash, args...)`). It never crosses the boundary, so it
  can change freely.

## E. Managed layer: CoreCLR (.NET 8+), plus Lua

**Hosting (`shvi_dotnet.asi`, a normal plugin):**

- Use `nethost` → `hostfxr_initialize_for_runtime_config(shvi.runtimeconfig.json)` →
  `hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)` → an `[UnmanagedCallersOnly]`
  bootstrap in `SHVI.Runtime.dll`. Use a framework-dependent `net8.0` runtimeconfig with `rollForward:
  LatestMajor`. .NET 8 and 10 are installed on this machine.
- Each script folder loads into a **collectible `AssemblyLoadContext`**. Hot reload means: stop the scripts,
  drop every reference, call `alc.Unload()`, then wait on a `WeakReference` with GC until it is collected
  (timeout, then warn about a leak). That replaces SHVDN's AppDomain plus shadow copy. Load from a
  `MemoryStream` so the DLL files stay unlocked.

**Execution model (the key decision).** CoreCLR isn't fiber-safe either; SHVDN learned that the hard way. Don't
run managed code on fibers, and don't use lockstep threads by default. Instead:

- Every managed tick runs **directly on the game thread, on its own stack, not in a fiber**. ABI 1.1 adds
  `tick_register(fn)`: plain callbacks that `OnTick` calls before the fiber scheduler runs, with natives
  allowed. The .NET host registers one, and managed code returns from it every frame. Managed frames are
  never on a fiber stack and never suspended mid-call.
- `await Script.Wait(ms)` / `await Script.Yield()` are C# `async` state machines driven by a custom
  `SynchronizationContext` that the host pumps once per frame on the game thread. No threads, no fibers, no
  TLS swaps, and real `try`/`catch`/`finally` semantics.
- Compatibility mode for blocking-style scripts (`Wait(100)` in a `while(true)` loop): SHVDN's lockstep
  thread with the game TLS block swapped in, opt-in per script, with its known caveats.
- API: generated bindings from the NativeDB (`Function.Call<T>(Hash.X, …)`) and a thin object layer (Ped,
  Vehicle, World) built on natives first, with memory reads only where needed (SHVDN's NativeMemory lesson:
  read offsets from code).

**Lua: recommended as the second host.** Lua 5.4 (or LuaJIT) coroutines *are* WAIT: `coroutine.yield(ms)`
maps 1:1 onto the scheduler, with no stack-switching problems, since a Lua coroutine is heap data. It is tiny
(~300 KB), easy to sandbox (no `io`/`os` by default) and reloads instantly. Good for small mods and menus.

**JS: not now.** QuickJS or V8 add size and a GC with little extra value over C# + Lua. Revisit if the community
asks for it.

## F. Overlay and UI

- **D3D12.**
  - Hook `IDXGISwapChain3::Present` and `ResizeBuffers` from a dummy swapchain's vtable, using inline detours
    so other overlays chain.
  - Capture the game's **direct** `ID3D12CommandQueue` from `ExecuteCommandLists`; D3D12 needs the queue to
    submit ImGui.
  - Use one ImGui DX12 backend with per-backbuffer allocators, an RTV heap and an SRV heap, rebuilt in
    `ResizeBuffers`.
- **Vulkan, if the PC build uses it.** Use an **implicit Vulkan layer** (a manifest plus `vkQueuePresentKHR` /
  `vkCreateSwapchainKHR`). Layers are the official extension mechanism and coexist with RTSS and Steam far
  better than hooks.
- **Threading.** Present runs on the **render thread**; RAGE renders on a separate thread.
  - ImGui never calls natives. UI state crosses threads through a lock-free double buffer: game-thread
    scripts publish view models, and the render thread posts commands back to a queue that the game tick
    drains.
- **Native drawing.** The mod-menu toolkit is game-independent: `sdk/include/shvi/ui.hpp` holds the menus,
  the pure `Layout()` → `DrawList`, and `InputQueue`. `sdk/include/shvi/vi_ui.hpp` (`shvi::vi::EngineBackend`)
  draws a list through three engine names: `UI_DRAW_RECT`, `UI_DRAW_TEXT` and `UI_DISABLE_MENU_CONTROLS`.
  The profile binds those to the game's HUD natives once they are named (no hooks, survives renderer
  changes), or to this overlay's draw list. `samples/menu_demo` is the reference plugin. See
  `docs/SCRIPT_MODDING.md` §4.
- **Input.** Subclass the game window's `WndProc` (`SetWindowLongPtrW`), the basis for `keyboard_register`.
  Block game input while ImGui wants focus through the game's own disable-control function
  (`UI_DISABLE_MENU_CONTROLS`), not by eating raw input.

## G. Mandatory anti-online guard (`src/core/online_guard.h`)

It is **fail-closed and one-way**:

1. **Preflight (before any game code is touched).** If an anti-cheat client module is loaded (the profile lists
   them; none is known for VI yet), or the command line requests online, the core never installs anything. This is the
   opposite of evasion: protection present means the hook stays off.
2. **Runtime probe, every tick.** The profile reports Unknown, Offline, Transition or Online. GTA VI reads
   the game-mode object's current and requested content hashes (research plan Q10). Both must hold the
   story-boot value for Offline.
3. **Arming.** Plugin scripts run only after 30 consecutive Offline ticks. Unknown (loading screens, missing
   probe natives) pauses them.
4. **Latch.** Online or Transition kills every script, and plugin native calls and `global_ptr` return zero
   or NULL for the rest of the process. There is no un-latch; restart the game. An anti-cheat module that
   appears later also latches the guard (checked every 300 ticks).
5. **Transparency.** The hook doesn't hide from the game: clear module names, a plain log, no obfuscation. If
   Rockstar wants to detect it, it can.
6. **Denylist (done).** `shvi\denylist.txt` holds one hash per line, generated from a NativeDB by
   namespace (NETWORK_*, the money and stat natives) and never hand-guessed. The gated invoker refuses
   those natives by the hash the plugin passed or by the crossmapped one, and counts the refusals. The
   guard's own probes bypass it.
7. **Planned:** a per-profile "known online script" list (freemode, the transition scripts) for an
   earlier latch.
   - For GTA VI the equivalent is likely the active content id. The Xbox exe is the `sub_story` product
     and launches modes as `americas://<content>/startContent?MODE=<mode>` (research plan Q10).

The guard sits in the core, below the SDK. A plugin can't reach the unchecked path.

---

## Corrections to the brief

The original brief's corrections all concerned GTA V (the native table layout, hash rotation, thread
borrowing, SHVDN's lockstep thread, DirectX versions). They are background now and live in
[REFERENCE_ANALYSIS.md](REFERENCE_ANALYSIS.md), "Appendix: corrections to the original brief".
