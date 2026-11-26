# Trainer (mod-menu logic)

Updated 2026-11-25. `samples/trainer` is a trainer for GTA VI story mode built on the NativeDB
(`NATIVEDB.md`). It is the menu logic only: no D3D12 hook and no renderer of its own. It ran in the game on
2026-11-25: the script started, all 181 features were bound with the full table,
and the calls that were tried behaved as expected. Most features have still only been analysed, not run.

## 1. Parts

| File | What |
|---|---|
| `samples/trainer/trainer.hpp` | The logic: 181 features in 9 categories, availability, per-frame holds, menu building. Header-only; game calls only through `shvi/natives_full.hpp` |
| `samples/trainer/trainer_main.cpp` | The plugin (`trainer.asi`): input, the frame loop, the model list file, drawing through `shvi/vi_ui.hpp` |
| `samples/trainer/trainer_data.inc` | Generated: hash literals the shipped scripts pass to natives (weather types, weapons, ammo types, weapon collections, controls), named where a wordlist name hashes to the literal |
| `tests/trainer_tests.cpp` | 17 tests against a fake host (ctest `trainer_logic`) |
| `tests/trainer_summary.cpp` | Prints the feature counts below |

Drawing goes through `ui::Backend`. The plugin uses `vi::EngineBackend`, which calls `UI_DRAW_RECT` /
`UI_DRAW_TEXT` by name; on the Xbox build the host binds those to its overlay draw list. Another renderer
only has to implement `ui::Backend::Draw`.

## 2. How a feature is gated

Each feature lists the natives it calls. Nothing is called on trust:

1. **Bound?** `Trainer::Refresh()` asks the host for every needed name (`native_exists`, ABI 1.5). A
   feature with a missing native is shown as `[n/a]` and does nothing. With the default native table
   (43 behaviour-checked names) only a handful are available; with
   `nativedb/db/<build>/native_table_full.json` installed, all 181 are bound (seen in game).
2. **Trusted?** The weakest name confidence among its natives decides. A feature is *experimental* when
   that is `guide`, `inferred-medium` or `guide-order-conflict`, or when the code marks an argument as a
   guess (`guess = true`). Experimental features show as `[locked]` until "Allow experimental natives"
   is switched on in Settings, then as `[exp]`.
3. **Held.** A toggle is re-applied every frame (`Hold::Frame`) or every `reapplyMs` (`Hold::Interval`,
   default 750 ms), because game scripts set the same state. Switching it off applies the off value once.

The host's own rules still apply underneath: story mode only (the online guard), handler id check at bind
time, SEH around every call.

## 3. Features

`shvi_trainer_summary` output with every native bound:

| Category | Features | Experimental |
|---|---|---|
| Player | 42 | 8 |
| Weapons | 15 | 9 |
| Vehicle | 34 | 10 |
| Spawn | 11 | 9 |
| Teleport | 18 | 4 |
| Time | 9 | 0 |
| Weather | 7 | 3 |
| World | 21 | 3 |
| Misc | 10 | 0 |
| **Total** | **181** | **46** |

Nine are blocked outright (§5): every spawn and give feature. They either create nothing or fault in the engine.

By weakest native: 40 features rest on behaviour-checked names only, 92 on layout-corroborated names,
2 on strictly inferred names, 32 on guide-only or loosely inferred names and 1 on a name that
contradicts the layout.

- **Player:** invincible, heal, keep health full, restore / keep all vitals, set one vital, max health,
  health recharge, set wanted level, clear wanted, never wanted, max wanted level, disable wanted system,
  everyone ignores player, police ignore player, invisible, opacity, no ragdoll, cannot be damaged, only damaged by player, all
  proofs, infinite stamina, move rate (set / hold), freeze, no gravity (two routes), no collision, launch
  upwards, unlimited breath, jump, activate / disable special ability, auto-walk, accuracy, sweat, put out fire, clean, stop
  action, suicide, change model.
- **Weapons:** infinite clip, refill, keep full, fill clip of best weapon, give weapon collection, give
  one weapon / every listed weapon (async item native), equip weapon, set ammo by type, drop weapon,
  remove all ammo, hide weapons, keep weapons when dead.
- **Vehicle** (the one the player uses): repair, wash, indestructible, auto repair, engine, boost, stop,
  set on wheels, lights, full beam, hazards, interior light, alarm, door locks, open / close / break off
  doors, handbrake, burnout, engine power, top speed limit, steering bias, never fall off, no gravity,
  invisible, burst / fix tyres, mark as owned, launch upwards, put out fire, enter nearest vehicle,
  explode, delete.
- **Spawn:** vehicle / ped / object from a model list or as a copy of the nearest one (or of the player),
  delete nearby peds / vehicles.
- **Teleport** (moves the vehicle when driving): map waypoint (go to / remove), step forward / up / down, nine save
  slots, go to X Y Z (direct, or through the engine's own player teleport that loads the area), heading,
  no-clip along the camera.
- **Time:** hour, minute, ±1 hour, presets, freeze clock, game speed (set / hold), follow system clock.
- **Weather:** type (14 values seen in scripts), hold, random, clear override, rain, wind speed and
  direction.
- **World:** traffic and pedestrian density (ambient peds too), everyone flees, gravity level, clear area, kill / stop nearby pedestrians,
  explode / repair / launch nearby vehicles, explosion ahead, put out fires, no random police / trains /
  boats, pause.
- **Misc:** camera shake (set / stop), clear screen effects, colour filter strength, next radio station,
  log position, log models, log entity counts, status line.

Nearby entities come from the host's pool walk (`world_get_all`), not from a native.

### Features built on natives found in this pass (read in IDA)

| Feature | Native | What the code shows |
|---|---|---|
| Set wanted level | `VI_RAISE_PLAYER_WANTED_LEVEL(ped, level)` (`0x3E91DB0`) | Reaches the only script-side writer of the wanted score and level besides the clear native. It only raises, so the trainer clears first. No effect seen in game yet (below) |
| Vitals | `VI_SET_PED_VITAL`, `VI_GET_PED_VITAL`, `VI_GET_PED_VITAL_MAX` | Six per-ped pools `{current, max}`; lowering one goes through the damage path. The game's own "restore" script sets all six to max, which is what "Restore all vitals" does. Which index is armour is not known |
| Spawn vehicle / ped / object | `VI_CREATE_VEHICLE_ASYNC`, `VI_CREATE_PED_ASYNC`, `VI_CREATE_OBJECT_ASYNC` | Async creators found at the layout positions of `CreateVehicle`, `CreatePed`, `CreateObject`. Blocked: see §5 |
| Give weapon | `VI_GIVE_ITEM_TO_PED_ASYNC` | 48-byte item block as scripts fill it (item hash, count, reason hash, flag). The native accepted it in game; blocked for the task type, see §5 |
| Gravity level | `SET_GRAVITY_LEVEL` | Stores one int in a global; sits where `CommandSetGravityLevel` sorts |
| Police ignore player | `SET_POLICE_IGNORE_PLAYER` (`0x3E90FF0`) | Flag 6 of the script-owned ped flag set; freeze = 0, invincible = 1, collision = 2, visible = 3, everyone ignores = 7 use the same two functions |

### Checked in the running game (2026-11-25)

| Feature or native | What the run showed |
|---|---|
| Status line (`PLAYER_PED_ID`, `GET_ENTITY_COORDS`, heading, speed) | Real position, updated as the game moved the player |
| Set hour (`SET_CLOCK_TIME`) | Clock read 08:00 before, 09:00 after |
| Max health (`SET_PED_MAX_HEALTH`, `SET_ENTITY_HEALTH`) | 130/130 → 200/200 |
| Max wanted level (`SET_MAX_WANTED_LEVEL`, `GET_MAX_WANTED_LEVEL`) | 6 → 3 → 5 |
| Step up / down (`SET_ENTITY_COORDS`) | z moved by the 5 m asked for |
| Heal, invincible on / off, clear wanted | Called without fault; no read-back that would show a change (health was already full, wanted 0) |
| Entity counts (`world_get_all`) | 30 peds, 146 vehicles, 4,290 objects |
| Probe getters | Mutually consistent |
| Heading (`SET_ENTITY_HEADING`) | Not confirmed by read-back (the intro keeps moving the player). Both natives are radians in the code, so the trainer converts from degrees |
| Set wanted level (`VI_RAISE_PLAYER_WANTED_LEVEL`) | Called with 2 and 3: the level still read 0 up to 30 s later, during the intro. Not confirmed; the feature is experimental |
| Restore vitals | The six vitals read -1 (none) for the intro ped, so nothing to restore there. Not confirmed |

`shvi\trainer_autorun.txt` drives such a run without a keyboard: one step per line,
`<seconds> <feature id> [on|off|number]`, counted from the first frame with a player ped; `misc.probe`
logs what the getters return.

## 4. Against the GTA V NativeTrainer

`../ScriptHookV/SDK/samples/NativeTrainer` (and `ImguiTrainer`) is the reference feature list.
`analysis/nativedb/ref_trainer_gap.py` compares the 194 natives those two call with the VI database: 46
are used by this trainer under the same name, 32 more are named in the database, 43 exist only as
unconfirmed candidates, 73 have no counterpart (many of those are GTA V's text drawing and DLC queries).

| NativeTrainer entry | Here | How |
|---|---|---|
| Fix player, invincible | ✅ | `SET_ENTITY_HEALTH`, `SET_ENTITY_INVINCIBLE` (behaviour-checked) |
| Wanted up / down, never wanted | ◐ | Clear and never wanted; "Set wanted level" is experimental (no effect seen in game yet) |
| Police ignored | ✅ | `SET_POLICE_IGNORE_PLAYER`, found by behaviour: it sets flag 6 of the same script-owned flag set in which "everyone ignores" is flag 7 and invincible is flag 1 |
| Fast run, fast swim | ◐ | Move rate (`SET_PED_MOVE_RATE_OVERRIDE`). The matcher's candidates for the player multipliers were read in IDA and are per-viewport angle setters, not multipliers |
| Super jump | ◐ | "Jump" (`TASK_JUMP`) and "Launch upwards" (velocity); no jump-height native |
| Unlimited ability, noiseless | ◐ / ❌ | Activate / disable special ability (guide names, experimental); no meter-fill or noise native identified |
| Add cash | ❌ | `SET_PED_MONEY` exists but only sets the cash a ped carries as loot (a u16 on a ped component), not the player's balance |
| Skin changer, reset skin | ◐ | Change player model (experimental); no component-variation natives identified |
| Teleport (marker, places) | ◐ | Waypoint, slots, coordinates, steps, no-clip; no named place list for the VI map |
| Get all weapons | ◐ | Give weapon collection (guide name, experimental). Giving single weapons is blocked (§5) |
| No reload | ✅ | `SET_PED_INFINITE_AMMO_CLIP` |
| Fire / explosive ammo, explosive melee, vehicle rockets | ❌ | No such natives identified; "Explosion ahead" exists |
| Car spawner | ❌ | The creation native is found and its block decoded, but it cannot be called from a plugin yet (§5). Ped and object spawning through script wrappers is blocked too |
| Wrap in spawned | ❌ | No spawner; "Enter nearest vehicle" exists |
| Paint random, mods | ❌ | Checked: no native in the `SetVehicleColour…` layout bracket takes colour values; none found elsewhere |
| Fix, seat belt, invincible, strong wheels, speed boost | ✅ | Repair, never fall off, indestructible, boost |
| Moon gravity | ◐ | Gravity level (experimental) |
| Random cops / trains / boats | ✅ | Same natives |
| Garbage trucks | ❌ | Candidate only |
| Hour forward / backward, clock paused, sync with system | ✅ | Same natives, plus presets and game speed |
| Weather types, wind, set persist | ✅ / ◐ | `SET_WEATHER_TYPE` (behaviour-checked); the flag arguments are a guess; 5 of 14 type names known |
| Next radio track | ✅ | `SKIP_RADIO_FORWARD` |
| Hide HUD | ❌ | Searched the named set for HUD, radar, minimap, letterbox and UI families: only minimap fog-of-war, blips and world-to-screen natives exist. The UI is driven by data binding, not by hide/show natives |

Beyond the reference: vitals, stamina, opacity, proofs, density sliders, nearby-entity actions, door and
light controls, tyres, save slots, no-clip, game speed, screen-effect reset.

## 5. The async natives (spawn, give weapon)

GTA VI creates entities and gives items through natives that return a task (`NATIVEDB.md` §7). They
build the task first, from their last argument: a token for `Task<T>` in the calling script program's
type space. A plugin is not a script program, so the trainer tried the engine's built-in types instead.
Both were run in the game on 2026-11-25 and both failed:

| Token | Type | What happened |
|---|---|---|
| `0x8000002F` | `System.Threading.Tasks.Task`1` (run 8) | The item-give native returned a task object. Seconds later the garbage collector crashed on it: an object of the open generic type has no runtime type behind its header |
| `0x8000002E` | `System.Threading.Tasks.Task` (run 9) | The native faulted inside task setup (caught), returned nothing, and the game exited shortly after |

What the runs did confirm: a current script thread is present when the trainer ticks (no thread context
setting was needed), and the item-give native accepts the block the trainer builds.

**The script-wrapper route** (runs 10–15). The hook gained `script_function_call` (ABI 1.6), and the
trainer calls the game's own wrappers in module `native` (`func_018247` creates a ped, `func_01BE38` an
object), which pass a valid `Task<T>` themselves. What the runs showed:

| Run | Arguments | Result |
|---|---|---|
| 10 | flags zero | Nothing ran: the thread was still marked "returned", so the VM exited at once. Fixed in the hook |
| 11, 12 | flags zero | The wrapper ran and returned a real task; the game kept running. No ped appeared within 30 s |
| 13, 14 | the flags a shipped script sets (the third flag and the one after the int = 1) | The engine faulted in its pooled-object allocator (`0x1406728F0`): the per-thread table it registers new objects in (script heap `+0x17D8`) is not valid on the worker thread at tick time. The game died in run 13 and survived in run 14 |
| 15 | same, with the engine's script scope entered first (`thread_context.txt` = `scope`) | Crashed at the first tick in the hook's own scope code, which read script-instance pointers from thread storage without checking them. That read is now guarded; the mode is fail-safe but not shown to work |

| 16 | same flags, with the tick moved to just before the game's own script call (inside the engine's scope) | Ordinary natives worked the same; ped creation faulted at the same allocator spot. So scope timing is not the cause. Reverted |

| 17 | same flags, call made under script heap 1 | The call succeeded (status 0, a task came back). The game crashed 3 s later on another thread, in code walking heap 1's pool table |
| 18–21 | same, under script heap 2 | The call succeeds and the game keeps running. No ped appears within 90 s. The task is a live object at return and is zeroed by the next tick, about 30 ms later |
| 22 | same, with the 16-bit count at task `+0x10` raised by one | No difference: zeroed by the next tick |

**What the fault was.** The engine has three script heaps. Each pooled script type (the `Task<T>` the
async natives build is one) has a region in the pool table of heaps 1 and 2 and none in heap 0. A game
script call arrives under heap 0, 1 or 2 depending on the script; the worker thread our tick lands on was
under heap 0 every time, so allocating the task indexed past heap 0's table. The hook now reads the heaps
and, with `shvi\script_heap.txt` (`auto`, `0`, `1` or `2`), enters a heap around `script_function_call`
with the engine's own enter and leave functions. This is fixed-address code for this build only.

**What is still wrong.** A pooled object records the id of the script instance that created it (header
`+0x08`), and the task we get back is freed almost at once. Nothing in the owning script refers to it, so
the script runtime reclaims it; the engine's request keeps only a handle to it. In a shipped script the
task is stored in the calling function's state object, which the script's own stack keeps alive. A plugin
has no such place. So the remaining problem is no longer the call but the lifetime of its result: either
a script context of our own (a script program the game runs for us), or the engine function that builds
the ped once the request is ready, called directly.

So all nine spawn and give features stay **blocked** (`[blocked]` in the menu, never run). The code for
the calls stays, and `settings.runblocked on` in an autorun file re-enables them for a test run.

What is known and reusable: the call path works; the ped native's cells are model, position (2), heading,
three flags, one unused cell, a 12-byte block (2 cells), a flag, a pointer, an int; a finished task holds
the 16-byte entity reference at `+0x28`.

**A ped without spawning.** The second story character's ped always exists, parked off-world
(`VI_GET_SECONDARY_PLAYER_PED`, run 23). "Bring the second character here" (Player) moves it in front of the
player with `SET_ENTITY_COORDS`; run 24 read it back 6 m ahead and still there 22 s later. "Teleport to the
second character" (Teleport) refuses while it is parked.

**Special ability.** Four natives were read from the code (`NATIVEDB.md`, `reviews.json`): an amount
getter, a charge, a reset and a per-part fill. The trainer has "Charge special ability", "Keep special
ability charged" and "Reset special ability", all experimental. Run 27 could not judge them: both abilities
were not enabled at that point of the game.

**Story characters.** Spawn page: "Story character" (22 character hashes from the scripts; `player_jason`,
`player_lucia` and `chester` are named), "Spawn that story character here", "Log which story characters are
available", "Log / Bring / Despawn the spawned character". They use the synchronous persistent-character
natives (`_CREATE_PERSISTENT_CHARACTER`, `_FORCE_SPAWN_PERSCHAR`, `_GET_PERSCHAR_PED_INDEX`, guide names).
Run 39 confirmed the path for `player_jason`. A character is available only while the game has its
definition loaded; the others return no handle.

**Weapons without the async native.** "Give weapon collection" and "Give the largest weapon collection"
(Weapons page) use the synchronous `_GIVE_WEAPON_COLLECTION_TO_PED`. Confirmed in game (runs 47 and 50).
A collection replaces what the ped carries, so weapons cannot be added one by one this way; single-weapon
give stays blocked (§5).

## 6. What is still missing, and why

| Wanted | State |
|---|---|
| Vehicle colours, mods, plates, extras | Looked for: the natives sorted where `SetVehicleColour…` would be all take `(Vehicle, BOOL)`. No colour or mod setter found by signature anywhere in the vehicle code regions |
| Armour as its own value | Not a separate native. Either one of the six vitals or not present in this build |
| Spawning and single-weapon give | Blocked (§5). The script-function call path works; creation needs a thread context in which the engine's pooled allocation works |
| Explosive / fire ammo, super jump, abilities, cash, HUD toggles | No native identified |
| Display names for weapons and models | Only hashes that scripts pass as literals are known |

## 7. Assumptions to check on first run

- Argument meanings where VI added parameters: `SET_ENTITY_COORDS` flags (`false ×4, -1.0f` as most
  scripts pass), `SET_WEATHER_TYPE` flags, `SET_VEHICLE_ON_GROUND_PROPERLY`, `SET_ENTITY_COLLISION`.
- `DELETE_VEHICLE` / `DELETE_PED` take a script reference: the trainer passes the address of a 16-byte
  reference.
- `SET_MAX_WANTED_LEVEL(5)` when "never wanted" is switched off.
- No-clip uses GTA V's camera rotation order (2) and Euler convention.
- Headings are radians in VI (seen in game). The spawner passes the raw heading on; the heading setter converts from degrees.

## 8. Build and install

```bat
cmake --build build --config Release --target trainer shvi_trainer_tests
build\Release\shvi_trainer_tests.exe
```

`cmake --install` puts `trainer.asi` in `game\shvi\plugins`. For the full feature set copy
`nativedb\db\1794606036\native_table_full.json` to `<game>\shvi\natives\1794606036.json`. That replaces
the behaviour-checked table with one whose names are mostly unchecked: see `NATIVEDB.md` §5.

Adding a feature: one `Action` / `Toggle` / `Int` / `Float` / `Choice` call in `Trainer::Register()`
with the list of natives it calls. The tests fail if a listed native is not in the database.
