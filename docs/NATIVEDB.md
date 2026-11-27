# GTA VI NativeDB (Xbox 1.0.2.1, build 1794606036)

Written 2026-11-25. A database of every script native in the exe, in the style of the GTA V and RDR2
NativeDBs: hash, name, parameters, return type. It is built by static analysis; on 2026-11-25 the full
table was bound in the running game and a first set of natives was called and read back (see the README).

| | |
|---|---|
| Natives | 6,829 (all handlers in the exe; 6,800 are called by the shipped scripts) |
| With hash, RVA, cell counts, typed parameters and return type | 6,829 |
| With a name | 2,053 |
| Unnamed, with a ranked candidate name | 2,813 |

## 1. Files

| File | What |
|---|---|
| `nativedb/db/1794606036/natives.json` | The database. alloc8or layout (`{NAMESPACE: {"0x<hash>": {name, params, return_type, comment}}}`) plus a `vi` object per native |
| `nativedb/db/1794606036/native_table_full.json` | The same natives in the loader's table format (`crossmap/…json`), with a `confidence` per entry |
| `nativedb/db/1794606036/coverage.md` | Counts by confidence, type and namespace; the guide names that contradict the layout |
| `nativedb/db/1794606036/validation.md` | Hold-out tests of the naming method |
| `sdk/include/shvi/natives_full.hpp` | A typed C++ wrapper for 6,823 natives (`shvi::nativedb::<NS>::<NAME>`, and every named one again in `shvi::nativedb::all`) |
| `sdk/include/shvi/natives_meta.inc` | Name → confidence table for plugins |
| `../analysis/nativedb/` | The pipeline (section 6) |

The `vi` object of an entry:

| Field | Meaning |
|---|---|
| `rva` | Handler RVA in this build |
| `argc`, `retc` | Argument and return size in 8-byte VM cells, as the scripts call it |
| `cells` | Cells per parameter (an entity reference and a Vector3 take 2) |
| `confidence` | How the name was established (section 4) |
| `param_evidence`, `return_evidence` | Why each type was chosen |
| `layout_bracket` | The nearest order-checked names before and after this handler |
| `candidates` | Ranked candidate names for an unnamed native (never used as its name) |
| `call_sites`, `programs` | How often the shipped scripts call it |
| `ref_gta5db`, `ref_rdr3db` | The hash of the same-named native in those databases |

## 2. Hashes

Each handler starts with `cmp dword [ctx+10h], 0 ; js` and, in the cold block, writes one 64-bit constant.
That constant is the native's hash: it is the id the script files carry next to the handler offset.
The hashes are not a hash of the name and not a transform of GTA V or RDR2 hashes (840 combinations
tested in the research notes), so a name can't be checked by hashing it.

## 3. Parameters and return types

All of this is read from the exe and from the game's own scripts. No reference database is involved.

1. **Cell counts** come from the scripts: opcode 139 carries the argument and return cell counts. Our own
   decompiler walks all 745 programs (`collect_calls.js`). 6,617 of 6,800 called handlers read exactly that
   many cells; the rest leave trailing cells unread.
2. **Cell types** come from a dataflow over each handler's instructions (`sigs.py`):

   | Pattern in the handler | Type |
   |---|---|
   | `movss xmm, [args+k]` | `float` |
   | `cmp dword [args+k], 0 ; setnz` | `BOOL` |
   | 32-bit load | `int`, or `Hash` when the scripts mostly pass joaat-looking literals |
   | 16-byte load, or two 8-byte loads copied to one stack slot | a 2-cell value: an entity reference or a Vector3 |
   | 8-byte load with the inline string unpack (`[p+20] & 4`, text at `+40`) | `const char*` (a `System.String`) |
   | other 8-byte load | `Any*` (a script reference), or `Any` when the command takes it by value |
   | cell never read | `Any` (or the literal kind the scripts pass) |

3. **Entity or Vector3.** A 2-cell value is a Vector3 when the command takes it as `float*` / `__m128*`
   (Hex-Rays prototype) or when scripts pass vector literals. Otherwise a union-find over "the result of
   native A is passed as cell k of native B" spreads the known types to the rest.
4. **Entity class.** Handlers resolve a reference through one of seven 49-byte functions, each checking
   one class with `IsA`:

   | Resolver RVA | Class | Type in the DB |
   |---|---|---|
   | `0x526E340` | `CPed` | `Ped` |
   | `0xD35050` | `CVehicle` | `Vehicle` |
   | `0x133BBD0` | `CObject` | `Object` |
   | `0x350170`, `0x7BA630`, `0xC51680`, `0x445650` | `CEntity`, `CDynamicEntity`, `fwEntity`, `0x830B8272` | `Entity` |

   So `Ped`, `Vehicle` and `Object` parameters are what the handler really asks for.
5. **Return type** is how the handler stores the result: `movss` → `float`, `movzx al` → `BOOL`,
   `movsxd` → `int`, zero-extended 32 bits → `Hash`, 16 bytes → entity or Vector3 (resolved as in 3).

A named native also gets parameter **names**, and handle types such as `Cam` or `Blip`, from the GTA V or
RDR2 signature of the same name, but only for parameters whose binary type fits. The layout always follows
the exe: VI's `SET_ENTITY_HEALTH` is `(Entity, int, Entity, BOOL, BOOL)`, not GTA V's three parameters.

What GTA VI changed, as seen across the whole table:

- Entities are 16-byte references (2 cells); a Vector3 is 2 cells (x and y packed in one, z in the next).
- Most strings became hashes: only 35 parameters are real strings.
- "Player" natives take the player's `Ped` reference; a few take an `int` player index (`GET_PLAYER_INDEX`).
- Weapons follow RDR2's inventory model (collections, item GUIDs, attach points).
- Entity creation is asynchronous and takes a parameter block: see section 7.

## 4. Names

There are no native names in the exe. Names come from outside and are accepted on binary evidence.

### The layout order

**Inside each code region, handlers are laid out in ascending order of the C++ command function name.**
The comparison is case-sensitive, and a name sorts after every longer name that it prefixes
(`CommandAddExplosionWithUserVfx` comes before `CommandAddExplosion`). The key is the C++ function, not the
script name: `GET_FINAL_RENDERED_CAM_COORD` is registered in GTA V as `CommandGetRenderedCamCoord`, and in
VI it sits between `GetPosixTime` and `GetSecondsSince…`, where "GetR…" belongs. There are about ten large
regions, each running A to W, and each holding several namespaces.

So a handler's name is bracketed by its neighbours. That gives a check that does not depend on who
proposed the name:

- With GTA V's registration list as the source of sort keys, 97.7% of the guide names that have such a
  key are in exact order (1,010 of 1,034).
- With a CamelCase guess of the public name as the key, 87%.

The command functions behind the handlers follow in the same order reversed, so they add no second
ordering.

### Confidence levels

| `confidence` | Natives | How the name got there |
|---|---|---|
| `verified-high`, `-medium`, `-descriptive`, `-deny` | 62 | Behaviour-checked by hand in IDA: the entries of `crossmap/gta6_xbox_1.0.2.1.json` plus those accepted in `analysis/nativedb/reviews.json` |
| `corroborated` | 1,165 | A guide name whose sort key is in layout order with its neighbours |
| `inferred-high` | 29 | No guide name. The only candidate that fits the bracket and the signature, by a wide margin |
| `inferred-medium` | 194 | As above with a smaller margin, or a reviewed name whose behaviour is only partly read |
| `guide` | 550 | A guide name with a community-invented spelling (leading `_`): it has no sort key, so the layout can't check it |
| `guide-order-conflict` | 53 | A guide name that sorts outside its neighbours: the name is wrong, or the function is named differently |
| `unnamed` | 4,776 | `_0x<hash>` |

The guide is the third-party decompiled script dump. It is never taken on its
word: a guide name is `corroborated` only when the layout agrees, and the two classes the layout can't
confirm are labelled as such. Candidate names come from the GTA V registration list and the GTA V and RDR2
NativeDBs.

### Inferred names

For each run of unnamed handlers between two order-checked names (`align.py`):

1. take the candidates whose sort key falls inside the bracket and that are not used elsewhere;
2. score each candidate against each handler by an edit distance between the reference signature and the
   binary signature (entity class included);
3. find the best order-preserving assignment;
4. for each assigned handler, compute the margin to the best assignment in which it gets another name or
   none, and the chance that an unrelated handler would fit that signature as well.

A name is kept as `inferred-high` or `inferred-medium` by margin and chance fit. Everything weaker is
written to `candidates` and the native stays unnamed.

Hold-out tests (`validation.md`, 5 runs of 300 hidden names) hide known names and try to recover them:

| | `inferred-high` | `inferred-medium` | candidate only |
|---|---|---|---|
| Hidden names all still among the candidates | 100% right (187 of 187) | 90% (291 of 322) | 47% |
| Half of them also removed from the candidates | 95% (96 of 101) | 62% (133 of 213) | 23% |

The second row is what a native new to VI looks like, and is the closer estimate for the handlers that
are still unnamed. That is why the candidate tier is not used for names.

### An independent check: entity class

The class a handler asks for (section 3, point 4) was not used to accept guide names, so it tests them.
For every guide name whose reference signature starts with a Ped, Vehicle or Object and whose handler
resolves a specific class, the two agree in **618 of 618** cases: 409 layout-consistent names, 22
layout-rejected ones and 187 of the guide-only names the layout can't check. So the guide's names are at
least consistent in entity class everywhere this can be tested, including the 53 that sit out of layout
order (which points to a differently named C++ function rather than a wrong name for most of them).

### Reviews

`analysis/nativedb/reviews.json` holds hand decisions applied after the automatic naming: `accept` gives
a native a name after its handler and command were read in IDA (our own names carry a `VI_` prefix), and
`reject` removes a proposed name that the code contradicts. One inferred name has been rejected so far:
`GET_PED_ARMOUR` for `0xBF2AA3CD5056E70C`, which reads a combat-component field. That is the looser tier
doing what its 62% says; treat every `inferred-medium` name as a lead to check.

### What did not work

- Hashing names to get the native hash (earlier work; 0 hits).
- Constants shared between a GTA V command's source body and the VI command: 0 of 368 pairs share one.
- A second ordering from the command functions: it is the handler order mirrored.
- The cfx string list holds no native names.
- Getter/setter twins (`twin_check.py`: does `SET_X` write a field that `GET_X` / `IS_X` reads?). The
  signal is real, 7 of 67 known pairs share a field against 0.23% by chance, but it confirmed no candidate
  name: 0 of the 54 pairs that involve a candidate share a field.

## 5. Using it

- **Loader.** `native_table_full.json` has the loader's format. Installed as
  `<game>\shvi\natives\1794606036.json` it binds every name whose handler id matches at its RVA.
  `shvi_scan <exe> --crossmap native_table_full.json` checks that offline: 6,808 of 6,829 match. The other
  21 have a prologue longer than the 24-byte window `HandlerId` reads, so the loader skips them
  (three are named: `APPLY_PED_BLOOD_DAMAGE_BY_ZONE`, `TASK_GOTO_ENTITY_OFFSET`,
  `TASK_GO_TO_COORD_ANY_MEANS_EXTRA_PARAMS_WITH_CRUISE_SPEED`).
  The default table is still `crossmap/gta6_xbox_1.0.2.1.json` (behaviour-checked names only). Installing
  the full table is a choice: most of its names have not been checked against behaviour.
- **SDK.** `#include <shvi/natives_full.hpp>`; `shvi::nativedb::all::SET_VEHICLE_FIXED(veh)`. The comment
  above each wrapper gives the confidence and the script-level signature. An unbound name returns zeros.
  `shvi::native_exists("NAME")` (ABI 1.5) tells a plugin whether a name is bound.
- **Trainer.** `samples/trainer` is built on these wrappers (`TRAINER.md`).

## 6. Rebuilding (per game patch)

All commands run in `../analysis/nativedb`. `py -I` is the system Python 3; `idat` is IDA 9.5 batch mode
(`../analysis/gta6_xbox/IDAT_USAGE.md`). Inputs: the exe's IDA database, the extracted script programs
(`../extracted/*`), the census from `shvi_nativedb census`, and the reference tables.

| Step | Command | Output | Time |
|---|---|---|---|
| 1 | `idat -A -Sn01_dump.py game.i64` (in `../gta6_xbox`) | `ida_dump.jsonl`: pseudocode of every handler and command | 5 min |
| 2 | `idat -A -Sn02_asm.py game.i64` | `ida_asm.jsonl`: handler instructions | 1 min |
| 3 | `node collect_calls.js <rsc dirs> calls.json` | Script-side cell counts, literal kinds, native-to-native flows | 8 min |
| 4 | `py -I sigs.py` | `sigs.json`: per-handler cell accesses and call parameters | 10 s |
| 5 | `py -I typing_stage.py` | `typed.json`: typed signatures | 10 s |
| 6 | `py -I cands.py` | `cands.json`: candidate names with sort keys and reference signatures | 5 s |
| 7 | `py -I build_db.py` | The database, the table, the header, `coverage.md` | 1 min |
| 8 | `py -I validate.py` | `validation.md` | 15 min |
| 9 | `py -I gen_trainer_data.py` | `samples/trainer/trainer_data.inc` | 1 min |

Steps 4 to 9 are deterministic: the same inputs give the same bytes.

On a new build: rerun `shvi_nativedb census`, steps 1 to 7, then compare. If hashes are stable across
builds (not known yet), names carry over by hash and the layout check re-validates them for free. The
resolver RVAs in `typing_stage.py` (`RESOLVERS`) are per build: `n03_resolvers.py` finds them again.

Helpers (all in `../analysis/nativedb`): `who_calls.py <rva>…` and `who_calls_range.py <lo> <hi>` list natives that reach given engine functions; `n04_dec.py` (IDA) decompiles a list of addresses; `n05_writers.py` (IDA) finds the functions that write a field offset and the natives that reach them; `ref_trainer_gap.py` compares a GTA V trainer's natives with the database. `q.py <regex>…` searches names and candidates; `nb.py <name|rva> [n] [-v]` shows a native's
neighbours in layout order with the script-side argument kinds; `show.py <name|rva>` prints the handler
and command pseudocode; `callsites.js <rsc dirs> <rva>` prints decompiled script functions that call a
native.

## 7. Findings that matter for mods

- **52 natives are async.** Their last argument is a type token for the `Task<T>` they return (scripts
  push it with the float-constant opcode, e.g. `4.26751434e-41f` = `0x76F6`, an index into the calling
  program's types), preceded by a cancellation token. The command builds the task first
  (`0x14192D3C0`: current script thread from TLS `+5240`; a token with bit 31 set indexes the built-in
  type table at `0x14ABC3CB0` instead of the program's), then queues the work. Built-in tokens are the
  last-but-one argument of the registration calls in `0x143DECAF0` (`Task` = `0x8000002E`, `Task`1` =
  `0x8000002F`). On completion the 16-byte result is queued or copied by `0x140BCCB60`, which clamps to
  the destination object's type size (`u16 +112` of the type record). In game, neither built-in type works
  as the token (`TRAINER.md` §5): a plugin cannot call these natives directly, only through a script.
- **Model keys are 64 bits.** `GET_ENTITY_MODEL` returns the full 8-byte cell (model info `+96`); the player's
  model is `0x6B13B6B43F959856`. Neither half is the name's joaat: the joaat of the model name is the `u32` at
  model info `+0x68`, right after the key (2026-11-27: 200 of 327 vehicle model infos matched a known name there,
  none in either half of their key). The model natives look the whole key up (`0x1400BF810`), so a 32-bit hash is not a valid model: `IS_MODEL_A_PED(0x3F959856)` is 0,
  with the full key 1 (2026-11-26). The generated wrappers still type these parameters and returns as a 32-bit
  `Hash`: call model natives with the full cell (the trainer's `ModelOf` / `ModelQuery`).
- **There is no `CREATE_VEHICLE(model, x, y, z, heading)`.** Creation is async, found by layout position:

  | Native | RVA | Cells | Arguments |
  |---|---|---|---|
  | `VI_CREATE_VEHICLE_ASYNC` | `0x4A80260` | 14 | 96-byte block, cancellation token, task type |
  | `VI_CREATE_PED_ASYNC` (+4 siblings) | `0x4BC8CF0` | 14 | model, position, heading, flags, task type |
  | `VI_CREATE_OBJECT_ASYNC` (+2 siblings) | `0x4C129C0` | 11 | model, position, flags, cancellation token, task type |
  | `VI_CREATE_WORLD_ITEM_ASYNC` | `0x4A803B0` | 20 | 144-byte block (item keys, not models), token, task type |

  The vehicle block, from the game's own initializer (`native.func_00D151`) and its caller: model as
  u64 `+0x00`, `-1` `+0x1C`, position `+0x20`, heading `+0x30`, flag bytes `+0x38..+0x4D` (the caller
  leaves `+0x3A` and `+0x45` set), `-1.0f` `+0x54`.
- **Giving weapons** goes through the inventory. `_GIVE_WEAPON_COLLECTION_TO_PED(ped, collection, 0x066532DD)`
  is the common call (610 sites; `0x066532DD` is `LoadOut`). A single item is
  `VI_GIVE_ITEM_TO_PED_ASYNC(ped, 48-byte block, cancellation token, task type)` (`0x4A87F20`, 231
  sites): item hash `+0x10`, count `+0x18`, `-1` `+0x1C`, reason hash `0xE4DF46D5` `+0x24`, flag `+0x28`.
- **Wanted level** is derived from a wanted score (player info `+24360`, level `+24364`). Scripts raise
  it with `VI_RAISE_PLAYER_WANTED_LEVEL(ped, level)` (`0x3E91DB0`); nothing sets it downwards except the
  clear native.
- **Peds have six "vitals"** (`VI_SET_PED_VITAL` / `VI_GET_PED_VITAL` / `VI_GET_PED_VITAL_MAX`, index
  0..5): `{current, max}` pools tied to the damage system. No separate armour native was found.
- **Scripts fetch their entities by name.** The five most-called unnamed natives (about 204,000 call sites
  together) are one template: look a name hash up in the calling script's own registry and return what
  was registered. `VI_GET_SCRIPT_ENTITY_BY_NAME` (123,713 sites), `…_VEHICLE_…` (type tag
  `atStringHash("vehicle")`), `…_PROP_…` (`"prop"`), `…_REFERENCE_…` (tag `0x041F8F66`, not recovered)
  and `VI_GET_SCRIPT_INDEX_BY_NAME`. They resolve in the caller's script scope (TLS `+4532`), so they
  are of use to script hooks, not to a plugin calling from outside a script.
- **A script-owned flag set on entities.** `0x141A33B30` / `0x141171240` set and clear a numbered flag under
  owner tag `0x48391B87` (the hash of the `native` script module). Flag 0 = freeze position, 1 =
  invincible, 2 = collision, 3 = visible, 6 = police ignore player, 7 = everyone ignores player. Six natives
  use it, which ties their names together by behaviour.
- **No vehicle colour or mod setters** were found: the natives sorted where `SetVehicleColour…` would be
  all take `(Vehicle, BOOL)`.
- **1,838 natives return BOOL and 320 return a constant** (stubs in this build).

## 8. Limits

- Mostly static. In game, about 30 names have been exercised so far; all behaved as their
  names say, including 15 that were only layout-corroborated or guide names. A `corroborated` name says the handler sits where a function of that name would be and
  that its signature is compatible; it does not prove behaviour. Only the 62 `verified-*` entries do.
- Argument *meanings* follow the reference signature where the types fit. VI added and reordered
  parameters, so a `BOOL p3` may not mean what it meant in GTA V.
- 4,776 natives are unnamed. Most have no counterpart name in GTA V or RDR2 to find.
- The sort keys of RDR2-only names are guesses (CamelCase of the public name).
- `GET_ENTITY_HEADING` returns radians, not GTA V's degrees: units can differ even when the name fits.
- One build. Nothing is known yet about hash stability across builds or on PC.
