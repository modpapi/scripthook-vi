# How the references do it

Sources read for this page (2026-11-23): Give-Two/ScriptHookV (`Scripting/NativeInvoker.*`,
`ScriptEngine.cpp`, `ScriptManager.cpp`, `ScriptThread.h`, `Hooking/Hooking.cpp`, `HashMapData.h` in part),
citizenfx/fivem `code/components/rage-scripting-five` (`scrEngine.h/.cpp`, `scrThread.h`,
`ScriptHandlerMgr.cpp`), and scripthookvdotnet `main` (`source/core/Scripting/{Script,ScriptDomain}.cs`,
`source/core/Game/{NativeFunc,NativeMemory}.cs`, `source/loader/DllMain.cpp`). Where only part of a file was
read, this page says so. The original ScriptHookV is closed source; what it does is inferred from its SDK and
from these reimplementations.

## 1. Locating the natives table

| | How |
|---|---|
| **Give-Two** | Pattern `48 8D 0D ? ? ? ? 48 8B 14 FA E8 ? ? ? ?`: `.add(3).rip(4)` is the registration table (`lea rcx, [table]`), `.add(12).rip(4)` is the game's own `GetNativeHandler(table, hash)` (the `call`). It calls the game function rather than walking the table itself. |
| **fivem** | Pattern `76 32 48 8B 53 40`, `+9`, RIP-relative: the table. It walks the table itself (`GetNativeHandlerDo`). |
| **SHVDN** | Doesn't. It imports `nativeInit`/`nativePush64`/`nativeCall` from ScriptHookV.dll by their mangled C++ names. |

**Table layout (GTA V):** 256 buckets indexed by `hash & 0xFF`. Each bucket is a linked list of nodes holding
up to **7** entries: `{u64 next1, u64 next2, handler[7], u32 numEntries1, u32 numEntries2, pad, hashes[7×16 B]}`
(`#pragma pack(1)`). The next pointer, entry count and hashes are **XOR-obfuscated with their own addresses**:
next = `&next1 ^ next1 ^ next2`, count = `&numEntries1 ^ numEntries1 ^ numEntries2`, hash *i* is decoded from
the 16 bytes at `&next1 + 0x54 + 16*i`. In fivem the handlers themselves are stored raw.

**A detail the brief doesn't mention.** Script programs resolve their natives **once, at load**, into a
per-program table of handler pointers. Scripts don't look up hashes per call. So swapping a handler pointer
in the registration table only affects scripts loaded *afterwards*. To intercept a native for scripts that are
already running, detour the handler's code (Give-Two hooks WAIT with an inline detour) or patch each
program's table.

## 2. Invocation and the vector fix-up

The call context is the same in both reimplementations (fivem `scrThread.h`, Give-Two `NativeInvoker.h`):

```
+0x00 void*      m_pReturn       // return slots (a Vector3 return uses 3 slots)
+0x08 uint32     m_nArgCount
+0x10 void*      m_pArgs         // 8-byte slots
+0x18 uint32     m_nDataCount    // pending vector fix-ups
+0x20 scrVector* outVectors[4]   // fivem names (Give-Two keeps this as reserved space)
+0x40 scrVec3N   inVectors[4]    // alignas(16)
+0x80 pad[96]                    // sizeof = 0xE0
```

**Why a fix-up exists.** In scripts, a vector takes three 8-byte slots (`{x,pad,y,pad,z,pad}`, 24 bytes).
Engine code uses aligned 16-byte vectors. When a native takes a `Vector3*` **out** parameter, the handler
writes into `inVectors[n]`, stores the caller's pointer in `outVectors[n]` and increments `m_nDataCount`. After
the handler returns, someone has to copy each `inVectors[i]` back into `*outVectors[i]` in script layout. The
VM does this itself. An external invoker must do it explicitly:

- **fivem:** `NativeInvoke::Invoke` calls `cxt.SetVectorResults()` unconditionally after the handler.
- **Give-Two:** it calls the **game's own** routine, found by `83 79 18 00 48 8B D1 74 4A FF 4A 18`
  (`cmp dword [rcx+18h],0` checks `m_nDataCount`, `dec dword [rdx+18h]` counts it down).
- **SHVDN:** nothing in `NativeFunc.cs`, because SHV's `nativeCall` already did it.

ScriptHook VI implements the copy generically (`rage::GenericVectorFixup`). A profile can supply the game's
routine instead. `tests/tests.cpp: invoker_vector_fixup` covers it.

## 3. Script-thread impersonation

There are two models in the references, and the brief merges them. They are different:

- **Borrowing a real thread (Give-Two, and SHV as the community understands it).** No GtaThread is created.
  `ScriptManager::MainFiber()` finds the game's running thread whose `ScriptHash` is `main_persistent` (this
  can be changed with `changeScriptThread`). It calls `SetActiveThread(that)`, runs the plugin fibers, and
  restores the previous thread. The active thread is read from TLS: `*(gs:[0x58])[0] + 0x830`, which is
  `rage::tlsContext::m_script_thread`.
- **Owning a real thread (fivem).** `scrEngine::CreateThread(GtaThread*)` takes a free slot (`ThreadId == 0`)
  in the `pgPtrCollection<GtaThread>`, assigns `ThreadId = (*scrThreadId)++`, names it `scr_%d` and records it
  in `g_ownedThreads`. If scripts aren't initialised yet, the call is queued and replayed from a startup hook
  (`83 FB FF 0F 84 D6 00 00 00` −0x37). It hooks `GetThreadById` so custom threads resolve by ID. The script
  handler (`CGameScriptHandlerMgr::AttachScript`) is what gives the thread entity ownership. fivem reads the
  TLS offset from code (`48 8B 04 D0 4A 8B 14 00 48 8B 01 F3 44 0F 2C 42 20`, −4) instead of hard-coding it.

Borrowing is simpler and patch-friendlier. Owning is needed when scripts must own entities or survive the
borrowed script being killed.

## 4. Execution model: fibers vs threads

**Give-Two (fibers).** It detours the handler of `SYSTEM::WAIT` (`0x4EDE34FBADD967A6`). Every WAIT any game
script executes calls `MainFiber()` first. The main fiber comes from `ConvertThreadToFiber` (or
`GetCurrentFiber` if the thread is already a fiber). Each script's `Tick()` either `CreateFiber`s on its first
run or `SwitchToFiber`s into the script. `Wait(ms)` stores `wakeAt = timeGetTime()+ms` and switches back to
the main fiber. Everything runs on the game's script thread, so natives are safe.

**fivem (no fibers).** It hooks a per-frame function (pattern `48 83 EC 20 80 B9 ? 01 00 00 00 8B FA` −0xB),
runs its owned threads with `thread->Run(0)`, and resets them on `CTheScripts::Shutdown`. Its scripting
runtimes (Lua/JS/C#) get their own scheduling on top.

**SHVDN (lockstep thread with a borrowed TLS block).** This corrects the brief:

- The CLR does **not** run on SHV's fiber. `DllMain` cites issue #976: fibers cause random .NET crashes when
  the runtime's vectored exception handler runs. `ClrThreadProc` notes that the CLR caches stack limits and
  raises false stack overflows on a foreign fiber stack. It is about the CLR and fibers. The files read don't
  mention RAGE Plugin Hook or C++ try/catch.
- `ScriptMain` (the SHV fiber) and `ClrThreadProc` hand control back and forth with two auto-reset events:
  `SetEvent(continue)`, `WaitForSingleObject(wait)`, `scriptWait(0)`. The CLR thread runs **while the game
  thread is blocked**. It is strict lockstep, not a free-running thread.
- Natives *are* called from the CLR thread. Before each call, `SetTlsContext` swaps `gs:[0x58]` (the TEB's
  TLS pointer) to the **game thread's TLS block**, through `ExecuteTaskWithGameThreadTlsContext`. That works
  only because the game thread is parked at that moment.
- Each `Script` can also have its own background `Thread` with two `SemaphoreSlim`s (`_waitEvent`,
  `_continueEvent`). `Script.Wait` releases one and blocks on the other. `ScriptDomain.DoTick` drains a
  `ConcurrentQueue<IScriptTask>` on the domain thread. (ScriptDomain.cs was only partly read.)

So "they haven't found a way to tick on the game's main thread" is half right. Managed code never runs *on*
the game thread. It runs on a thread that impersonates the game thread's TLS while that thread waits.

## 5. NativeMemory (SHVDN)

- **Scanner:** `MemScanner.FindPatternBmh(pattern, mask)` (masked pattern, `x` exact / `?` wildcard; the
  name says Boyer-Moore-Horspool) and `FindPatternNaive(..., start)`. MemScanner.cs wasn't read, so which
  sections it scans is unconfirmed.
- **RIP-relative:** `Rel32(addr, off)` and `Rel32<T>` read the disp32 at `off`. Offset 3 covers a 7-byte
  `mov rax,[rip+x]`. Negative offsets walk back from an anchor to the function start (`Rel32(address,-0xA)`).
  Struct offsets come straight from instruction bytes (`*(int*)(address+0x25)`), so field offsets follow
  patches automatically.
- **Examples:** ped pool `48 8B 05 ? ? ? ? 41 0F BF C8 0F BF 40 10`, vehicle pool
  `48 8B 05 ? ? ? ? F3 0F 59 F6 48 8B 08`, object pool `48 8B 05 ? ? ? ? 8B 78 10 85 FF`. The time-scale
  global is `F3 0F 11 05 …` then +1, because SET_TIME_SCALE writes the second element. The local-player
  function is `33 DB E8 ? ? ? ? 48 85 C0 74 07 48 8B 40 20 8B 58 18`.
- **Versioning:** patterns branch on `GameFileVersion` (for example `>= 1.0.3788.0` for the script GUID pool)
  and on SHV's `getGameVersion()` enum. Globals come from SHV's `getGlobalPtr` export.

**Lesson for ScriptHook VI.** Structure offsets should be *read from code* next to a signature, not
hard-coded. The scanner's `ReadI32` op exists for this (`tls_script_thread` in the GTA V profile).

## 6. Crossmap

- **Give-Two:** `HashMapData.h` is an X-macro `X(OLD_HASH, NEW_HASH, OFFSET, NAME, VERSION)`. On retail it
  can take the handler straight from `base + OFFSET`. Otherwise it maps the hash and calls `GetNativeHandler`.
- **fivem:** `MapNative(hash)` runs before every lookup. `g_nativeBlockedBeforeBuild` gates natives by
  minimum build.

---

## Appendix: corrections to the original brief (GTA V)

- **"Natives table (64-bit hash → handler)."** It's a 256-bucket table (`hash & 0xFF`) of 7-entry nodes with
  XOR-obfuscated links, counts and hashes. More important, script programs **bind natives at load time**, so
  intercepting natives means detouring handler code, not swapping table entries
  (§1 above).
- **"Hashes change on every version, that's why SHV breaks."** That was true in GTA V's early PC years: the
  crossmaps (fivem's universal crossmap, Give-Two's `HashMapData.h`) exist because hashes were rotated.
  Rotation stopped years ago (2018-era builds; check exact build numbers against NativeDB history). Today SHV
  breaks on updates mainly because of its **build whitelist** and changed patterns and struct offsets.
- **"ScriptHookV hooks a function that runs every frame and impersonates a script thread."** The open
  reimplementation (Give-Two) detours `WAIT` and **borrows** the running `main_persistent` thread
  (`SetActiveThread`). It doesn't create one. fivem is the one that **creates and registers** real
  `GtaThread`s. These are two different techniques with different costs.
- **SHVDN "dedicated thread".** It is a lockstep thread, not a free-running one. Natives are called from it
  with the game thread's TLS block swapped in (`gs:[0x58]`) while the game thread is parked. The documented
  reason is that the CLR isn't fiber-safe (stack limits, VEH; issue #976). RAGE Plugin Hook and C++ try/catch
  don't come up in the code. Managed code has never run on the game thread itself because SHVDN's design
  doesn't allow it. The async-on-game-thread model above fixes that.
- **DX:** Legacy offers DX10, 10.1 and 11; Enhanced is DX12 only. Correct apart from that.
- **Give-Two/ScriptHookV is outdated**, as suspected. Its patterns target old Legacy builds; use it for
  structure, not signatures.
