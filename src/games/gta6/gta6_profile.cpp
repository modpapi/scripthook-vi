// GAME-SPECIFIC: GTA VI.
//
// Two profiles:
//
//  * Gta6Xbox: the Xbox Series build (game_scarlett_master.exe, PE timestamp
//    1794606036, RSG-LLVM 0.0.55 / clang 19.1.7, GXDK April 2025). Every
//    pattern below was taken from that binary in IDA and is re-checked
//    offline by `shvi_scan devbuild\game_scarlett_master.exe`. The console
//    exe can't run on PC by itself, so this profile is mainly the verified
//    signature set and the engine-call table that a PC build will start from.
//    Tick: a MinHook detour on the script manager's update (Q7 option (b)).
//    Guard: the game-mode object's current/requested mode hashes (Q10);
//    Offline only while both hold the story-boot value. Both are derived
//    statically and have not been observed in a running game.
//
//  * Gta6PcPlaceholder: matches nothing until the PC build exists.
//
// What the Xbox binary showed (docs/RESEARCH_PLAN.md, Q4 findings, third pass):
//   - there IS a script VM, with a new ABI (outcome (b)):
//       script_call 0x143DE1F10 pushes args onto the thread's 8-byte-slot
//         stack (+0x90), saves/sets TLS +0x1478 = current script thread, and
//         runs the VM (thunk 0x143DE2540 -> 0x143DE2550);
//       the VM reads 16-bit opcodes and dispatches on (u16)(op * 0x3315)
//         through a 530-entry jump table (an opcode permutation, likely
//         per build), with operands de-obfuscated (xor/mul/pshufb);
//   - natives: VM case 139 calls handler = [native_base] + u32 operand;
//     there is no name/hash registry for them (Q5);
//   - the runtime is .NET-like: 102 built-in types registered under
//     joaat("System.Object") etc., programs are linked modules with imports
//     and their own types, and the engine has its own script patch system
//     (tail jumps, case 505). See games/gta6/vi_script.h and Q4 fourth pass;
//   - mission structure is also data: PSIN state machines keyed by
//     case-folded joaat (Leonida/docs/mission_logic.md);
//   - ScriptStore is a streaming asset store (type id 89);
//   - the script manager's per-frame update 0x1429DFAF0 is registered with
//     the game-skeleton registrar under hash 0x0B3A5867 (tick candidate, Q7).
// Engine functions are bound by signature through core/engine_call.h; RAGE
// handlers will join them once the VM's native dispatch is found.
#include "core/engine_call.h"
#include "core/game_profile.h"
#include "core/log.h"
#include "core/pattern.h"
#include "core/probes.h"
#include "games/gta6/gta6_mode.h"
#include "games/gta6/vi_ecs.h"
#include "games/gta6/vi_script.h"
#include "games/gta6/vi_vehicle_experiment.h"
#include "games/gta6/vi_thread_context.h"
#include "games/gta6/vi_ui_adapters.h"
#include "core/native_table.h"

#include <unordered_map>
#include "shvi/shvi.h"
#include <mutex>
#include <MinHook.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

namespace shvi {
namespace {

bool IEquals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

namespace target {
constexpr std::string_view kStringHash = "string_hash";          // u32(const char*, u32 seed)
constexpr std::string_view kScriptStore = "script_store";        // streaming module object, type 89
constexpr std::string_view kParamFloat = "param_get_float";      // float(u32 hash)
constexpr std::string_view kParamInt = "param_get_int";          // i32(u32 hash)
constexpr std::string_view kParamVector = "param_get_vector";    // (out*, u32 hash), layout unverified
constexpr std::string_view kParamTable = "param_table";          // global the getters read
constexpr std::string_view kScriptCall = "script_call";          // (thread, fnRef*, argc, args*, ret*, retc)
constexpr std::string_view kScriptVmEntry = "script_vm_entry";   // VM run(thread), via its thunk
constexpr std::string_view kVmOpcodeMul = "vm_opcode_multiplier"; // VALUE: opcode = (u16)(raw * mul)
constexpr std::string_view kTlsScriptThread = "tls_script_thread"; // VALUE: TLS block offset of current thread
constexpr std::string_view kScriptMgrUpdate = "script_mgr_update"; // per-frame update, tick candidate (Q7)
constexpr std::string_view kGameModeObj = "game_mode_obj";        // address of the global holding the mode object*
constexpr std::string_view kModeLayout = "game_mode_layout";      // anchor: mov dword [rcx+48h], joaat("DefaultGameMode")
constexpr std::string_view kProgramLink = "script_program_link";  // void(program*): imports, types, engine patches
constexpr std::string_view kBuiltinTypes = "script_builtin_types"; // 102 x 136-byte type records
constexpr std::string_view kNativeBase = "script_native_base";    // global: handler = [base] + operand
constexpr std::string_view kOperandKeyA = "script_operand_key";   // 8-byte xor key for the first operand block
constexpr std::string_view kOperandMul = "script_operand_mul";    // u16 per-byte multiplier (0x6D)
// Script instances and the engine's run-as-script-thread scope (TASKS T1.3)
constexpr std::string_view kInstanceGet = "script_instance_get";   // instance*(program*, u32 tls slot): get or create
constexpr std::string_view kInstances = "script_instances";        // {u32 cap, u32 count, slot** items}; *item = instance*
constexpr std::string_view kScopeEnter = "script_scope_enter";     // (instance*, thread* or 0, scope*, bool activate, u32 id)
constexpr std::string_view kScopeLeave = "script_scope_leave";     // (scope*)
// Entities (TASKS T1.7/T1.8, vi_ecs.h)
constexpr std::string_view kEntityResolve = "entity_resolve";       // object*(const ref16*): any script object, or 0
constexpr std::string_view kEntityResolvePed = "entity_resolve_ped"; // object*(const ref16*): IsA(CPed) or 0
constexpr std::string_view kEntityMakeRef = "entity_make_ref";      // (object*, ref16* out, 2): object -> reference
constexpr std::string_view kEcsMainWorld = "ecs_main_world";        // world record the fixed-slot lookups read
constexpr std::string_view kPlayerPedObj = "player_ped_object";     // global: the player's CPed object* (0 when none)
constexpr std::string_view kPoolPeds = "pool_peds";                 // global: pool* holding CPed objects (vi_ecs.h pool::)
constexpr std::string_view kPoolVehicles = "pool_vehicles";         // global: pool* of the "Vehicles" pool config
constexpr std::string_view kPoolObjects = "pool_objects";           // global: pool* of the "Object" pool config
// Build id (T1.13): char[64] the platform init copies the title version into (0x143DA1560)
constexpr std::string_view kVersionString = "game_version_string";
// Streaming (T1.10): (strIndex* out, const char* path) -> strIndex of an already registered object, or
// 0x3FFFFF. Extension -> type -> module, slot 2 = find by name hash (ScriptStore's 0x141D3D110 only looks up).
constexpr std::string_view kStreamingFind = "streaming_find_object";
// Streaming registration (T1.10): module* modules[type]; a module (fwAssetStore-like) registers a
// name with vtable slot 28: AddSlot(module, strIndex* out, u32 nameHash), which pops a free slot and
// inserts name -> index into the module's name map (streaming_name_map_insert). Slot 2 finds.
constexpr std::string_view kStreamingModules = "streaming_modules";        // module* [type], index 255 = none
constexpr std::string_view kStreamingMapInsert = "streaming_name_map_insert"; // (map* = module + 0x80, u32 hash, u32 index)
constexpr std::size_t kModuleSlotFind = 2;   // (module, strIndex* out, {u32 kind, u32 hash}*) -> 0x3FFFFF if absent
constexpr std::size_t kModuleSlotAdd = 28;   // (module, strIndex* out, u32 hash), ScriptStore 0x142994200
}  // namespace target


std::vector<scan::Target> XboxTargets() {
    using namespace scan;
    std::vector<Target> t;
    // atStringHash-shaped: wrapper calls the partial hash, then the joaat
    // finalisation (key*9; key ^= key>>11; key += key<<15). The binary has
    // THREE wrappers of this exact shape. Called through `shvi_scan --hash`:
    // 0x144FC21D0 and 0x14221F180 both behave as atStringHash (case fold,
    // '\' = '/', quote-aware); 0x144DD1900 is a raw case-sensitive data hash.
    // Candidate 0 pins 0x144FC21D0's inner prologue (null check, '"' test);
    // candidate 1 is the bare shape, which leaves the two equivalent
    // functions and so stays ambiguous (by design: the scanner wants one).
    // Either way the validator calls the function and keeps only what equals Joaat().
    // That needs executable code: true in-game, and shvi_scan maps the image
    // executable before scanning.
    t.push_back({target::kStringHash,
                 {{"48 83 EC 28 E8 17 00 00 00 8D 04 C0 89 C1 C1 E9 0B 31 C1 89 C8 C1 E0 0F 01 C8 48 83 C4 28 C3 "
                   "CC 48 85 C9 74 ? 44 0F B6 01 89 D0 41 83 F8 22",
                   {}, "0x144FC21D0, Xbox ts 1794606036"},
                  {"48 83 EC 28 E8 ? ? ? ? 8D 04 C0 89 C1 C1 E9 0B 31 C1 89 C8 C1 E0 0F 01 C8 48 83 C4 28 C3",
                   {}, "bare wrapper shape; validator picks the case-folding one"}},
                 true,
                 [](std::uintptr_t fn) { return probes::StringHashMatchesJoaat(fn, nullptr); }});
    // ScriptStore static init: lea rcx, store; lea rdx, "ScriptStore"; mov r8d, 89; mov r9d, 1
    t.push_back({target::kScriptStore,
                 {{"48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 41 B8 59 00 00 00 41 B9 01 00 00 00",
                   {Add(3), Rip()}, "0x149DD7BC8 via 0x145CD9BA0, Xbox ts 1794606036"},
                  {"48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 41 B8 ? 00 00 00 41 B9 01 00 00 00 C7 44 24 28 31 00 00 00",
                   {Add(3), Rip()}, "type id changed"}},
                 true});
    // Hash-keyed parameter getters, called as `mov ecx, hash; call` from hot
    // SIMD code (0x1417CBB00). Value semantics unverified at runtime.
    t.push_back({target::kParamFloat,
                 {{"56 48 8B 15 ? ? ? ? 8B 02 48 85 C0 74 ? 41 89 C8 48 8B 52 08", {}, "0x141B89C10, Xbox ts 1794606036"}},
                 false});
    t.push_back({target::kParamInt,
                 {{"56 48 8B 15 ? ? ? ? 8B 42 20 48 85 C0 74 ? 41 89 C8 48 8B 52 28", {}, "0x141B89E10, Xbox ts 1794606036"}},
                 false});
    t.push_back({target::kParamVector,
                 {{"56 57 4C 8B 05 ? ? ? ? 41 8B 40 10 48 85 C0 74 ? 41 89 D1 4D 8B 40 18", {}, "0x141B89D10, Xbox ts 1794606036"}},
                 false});
    t.push_back({target::kParamTable,
                 {{"56 48 8B 15 ? ? ? ? 8B 02 48 85 C0 74 ? 41 89 C8 48 8B 52 08", {Add(4), Rip()},
                   "rip global read by param_get_float"}},
                 false});

    // --- script VM (outcome (b)); optional until the native path is known ---
    t.push_back({target::kScriptCall,
                 {{"41 57 41 56 41 54 56 57 55 53 48 83 EC 30 48 8B 41 08 48 89 CE 65 48 8B 0C 25 58 00 00 00 "
                   "4C 89 CB 44 89 C5 49 89 D6",
                   {}, "0x143DE1F10, Xbox ts 1794606036"},
                  // VIctory rewrites the 9-byte gs:[58h] read (call rel32 + 4-byte nop); same length.
                  {"41 57 41 56 41 54 56 57 55 53 48 83 EC 30 48 8B 41 08 48 89 CE ? ? ? ? ? ? ? ? ? "
                   "4C 89 CB 44 89 C5 49 89 D6",
                   {}, "same, TLS read rewritten by a host (VIctory)"}},
                 false});
    // mov r12,[r14+tls]; mov [r14+tls],rsi; call vm; mov [r14+tls],r12
    t.push_back({target::kScriptVmEntry,
                 {{"49 89 B6 ? ? ? ? E8 ? ? ? ? 4D 89 A6", {Add(8), Rip()}, "thunk 0x143DE2540 in script_call"}},
                 false});
    t.push_back({target::kTlsScriptThread,
                 {{"4C 8B 30 4D 8B A6 ? ? ? ? 49 89 B6 ? ? ? ? E8", {Add(6), ReadI32()},
                   "0x1478 in script_call, Xbox ts 1794606036"}},
                 false,
                 [](std::uintptr_t off) { return off >= 0x100 && off < 0x10000 && (off & 7) == 0; }});
    // movzx eax, word [rsi]; ...; imul eax, eax, MUL; movzx ebx, ax; jmp [r12+rbx*8]
    t.push_back({target::kVmOpcodeMul,
                 {{"0F B7 06 4C 89 FE 69 C0 ? ? ? ? 0F B7 D8 41 FF 24 DC", {Add(8), ReadI32()},
                   "0x3315 at 0x143DE266B, Xbox ts 1794606036"},
                  {"69 C7 ? ? ? ? 48 89 8C 24 ? ? ? ? 0F B7 D8 41 FF 24 DC", {Add(2), ReadI32()},
                   "first-dispatch copy at 0x143DE2601"}},
                 false,
                 [](std::uintptr_t mul) { return (mul & 1) != 0 && mul < 0x10000; }});  // odd = bijection on u16
    // lea rcx, update; mov edx, 0x0B3A5867; call skeleton_register (two update groups, same function)
    t.push_back({target::kScriptMgrUpdate,
                 {{"48 8D 0D ? ? ? ? BA 67 58 3A 0B E8", {Add(3), Rip()},
                   "0x1429DFAF0 registered in 0x1458A04D0 and 0x1458A1490"}},
                 false});

    // --- game mode (guard, Q10) ---
    // init phase 8: mov rcx, [g_mode]; add rsp, 28h; pop rdi; pop rsi; jmp setup (sub_141DB2820)
    t.push_back({target::kGameModeObj,
                 {{"48 8B 0D ? ? ? ? 48 83 C4 28 5F 5E E9", {Add(3), Rip()}, "qword_14AAE3B70, Xbox ts 1794606036"},
                  {"48 8B 35 ? ? ? ? 48 89 F1 E8 ? ? ? ? 8B 4E 40", {Add(3), Rip()}, "phase 16: mov rsi,[g]; ...; mov ecx,[rsi+40h]"}},
                 false});
    // sub_141B6B6B0: mov rsi, rcx; mov dword [rcx+48h], 31B22B23h. Proves the +0x48 field for this build.
    t.push_back({target::kModeLayout,
                 {{"48 89 CE C7 41 48 23 2B B2 31", {}, "0x141B6B6BE, Xbox ts 1794606036"}},
                 false});

    // --- script runtime (Q4 fourth pass); optional, used by the inventory and tools ---
    // cmp word [rcx+18h], 6Fh (resource version 111); int3; mov rax, [rsi+148h] (imports)
    t.push_back({target::kProgramLink,
                 {{"56 57 53 48 83 EC 30 66 83 79 18 6F 48 89 CE 74 01 CC 48 8B 86 48 01 00 00",
                   {}, "0x143E09470, Xbox ts 1794606036"}},
                 false});
    // &table[17 * i]: shl rcx, 7; lea rcx, [rcx+rax*8]; lea rax, table; add rax, rcx; ret
    t.push_back({target::kBuiltinTypes,
                 {{"89 C8 48 89 C1 48 C1 E1 07 48 8D 0C C1 48 8D 05 ? ? ? ? 48 01 C8 C3", {Add(16), Rip()},
                   "0x14ABC3CB0 via 0x14121E850, Xbox ts 1794606036"}},
                 false});
    // first program load: lea rcx, anchor; sub rcx, rax; mov [base], rcx
    t.push_back({target::kNativeBase,
                 {{"48 8D 0D ? ? ? ? 48 29 C1 48 89 0D", {Add(13), Rip()}, "0x1498C1810 in 0x141C18340, Xbox ts 1794606036"}},
                 false});
    // VM prologue: lea rsi, [rcx+2]; vpxor xmm0, xmm0, [key]; vpmovzxbw; vpmullw
    t.push_back({target::kOperandKeyA,
                 {{"48 8D 71 02 C5 F9 EF 05 ? ? ? ? C4 E2 79 30 C0 C5 F9 D5 C6", {Add(8), Rip()},
                   "0x146178EE0 in 0x143DE2550, Xbox ts 1794606036"}},
                 false});
    // VM prologue: vpbroadcastw xmm6, [mul]; mov rcx, [rax]
    t.push_back({target::kOperandMul,
                 {{"C4 E2 79 79 35 ? ? ? ? 48 8B 08 48 8B 40 08", {Add(5), Rip()}, "word_14615957C, Xbox ts 1794606036"}},
                 false});

    // --- script instances and thread scope (T1.3); optional, nothing uses them yet ---
    // gs:58h TLS block; lea r14, [rax+r8*8+0F80h]; mov rax, [same]; test; jz; cmp [rax+380h], rcx
    t.push_back({target::kInstanceGet,
                 {{"41 57 41 56 56 57 48 83 EC 28 65 48 8B 04 25 58 00 00 00 41 89 D0 48 8B 00 4E 8D B4 C0 ? ? 00 00 "
                   "4A 8B 84 C0 ? ? 00 00 48 85 C0 74 ? 48 39 88",
                   {}, "0x144EAC510, Xbox ts 1794606036"}},
                 false});
    // clear pass over all instances: mov esi, [vec+4]; test rsi,rsi; jz; mov rdi, [vec+8]
    t.push_back({target::kInstances,
                 {{"8B 35 ? ? ? ? 48 85 F6 74 ? 48 8B 3D ? ? ? ? 31 DB 48 8B 04 DF 48 8B 00", {Add(2), Rip(), Add(-4)},
                   "xmmword_1498FADC8 via 0x143EFA130, Xbox ts 1794606036"},
                  {"4D 89 3E E8 ? ? ? ? 8B 05 ? ? ? ? 3B 05", {Add(10), Rip(), Add(-4)}, "push in script_instance_get"}},
                 false});
    t.push_back({target::kScopeEnter,
                 {{"41 57 41 56 41 54 56 57 53 48 83 EC 28 4C 8D 7C 24 ? 49 89 CE 44 89 CB 4C 89 C7 48 89 D6 4C 89 F9 E8",
                   {}, "0x140066080, Xbox ts 1794606036"}},
                 false});
    // mov rsi,[rcx+10h]; ...; mov rax,[r14+tls]; cmp byte [rax+11h], 0
    t.push_back({target::kScopeLeave,
                 {{"41 56 56 57 55 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B 71 10 48 89 CF 4C 8B 30 49 8B 86 ? ? 00 00 "
                   "80 78 11 00",
                   {}, "0x140065FD0, Xbox ts 1794606036"},
                  // VIctory rewrites the 9-byte gs:[58h] read here too (seen live 2026-11-25): without this
                  // candidate the leave function is not found and the scope mode silently falls back to none.
                  {"41 56 56 57 55 53 48 83 EC 20 ? ? ? ? ? ? ? ? ? 48 8B 71 10 48 89 CF 4C 8B 30 49 8B 86 ? ? 00 00 "
                   "80 78 11 00",
                   {}, "same, TLS read rewritten by a host (VIctory)"}},
                 false});

    // --- entities (T1.7/T1.8); optional, used by the entity API ---
    // cmp qword [rcx], 0; jz; mov eax, [gen]; mov rsi, rcx; mov r15, FFFFFFFF00000000h; mov r12, 1_00000000h
    t.push_back({target::kEntityResolve,
                 {{"41 57 41 56 41 54 56 57 53 48 83 EC 38 48 83 39 00 0F 84 ? ? ? ? 8B 05 ? ? ? ? 48 89 CE "
                   "49 BF 00 00 00 00 FF FF FF FF 49 BC 00 00 00 00 01 00 00 00 48 8D 3D",
                   {}, "0x1404372D0, Xbox ts 1794606036"}},
                 false});
    // call entity_resolve; test; jz; mov rsi, rax; mov rax, [rax]; mov edx, atStringHash("CPed"); call [rax+8]
    t.push_back({target::kEntityResolvePed,
                 {{"56 48 83 EC 20 E8 ? ? ? ? 48 85 C0 74 ? 48 89 C6 48 8B 00 BA 59 16 82 94 48 89 F1 FF 50 08 "
                   "84 C0 74 ? 48 89 F0",
                   {}, "0x14526E340, Xbox ts 1794606036"}},
                 false});
    // zero a 16-byte temp; cmp r8d, 2; jz; cmp r8d, 1; jnz; mov rcx, rdi; call
    t.push_back({target::kEntityMakeRef,
                 {{"41 56 56 57 53 48 83 EC 38 C5 F8 57 C0 48 89 D6 48 89 CF C5 F8 29 44 24 20 41 83 F8 02 74 ? "
                   "41 83 F8 01 75 ? 48 89 F9 E8",
                   {}, "0x1402A6520, Xbox ts 1794606036"}},
                 false});
    // Every fixed-slot lookup: and eax, 3E0000h; cmp eax, 60000h; jnz; and ecx, 1FFFFh;
    // imul ecx, [record+14h]. All hits must agree on the record (record = disp target - 0x14).
    t.push_back({target::kEcsMainWorld,
                 {{"89 C8 25 00 00 3E 00 3D 00 00 06 00 75 ? 81 E1 FF FF 01 00 0F AF 0D", {Add(23), Rip(), Add(-0x14)},
                   "ymmword_14ABBEEB8 via 0x144D9F7C0, Xbox ts 1794606036"}},
                 false});
    // Handler 0x144504480's command 0x145172940: zero out; mov rcx, [player]; test; jz; mov r8d, 2; jmp entity_make_ref
    t.push_back({target::kPlayerPedObj,
                 {{"C5 F8 57 C0 48 89 CA C5 F8 11 01 48 8B 0D ? ? ? ? 48 85 C9 74 ? 41 B8 02 00 00 00 E9", {Add(14), Rip()},
                   "qword_14AB56E38 via 0x145172940, Xbox ts 1794606036"}},
                 false});
    // 0x140D36890 turns a CPed (from entity_resolve_ped) into a pool handle:
    // mov rcx, [pool]; xor edx, edx; sub rbx, [rcx]; mov r8d, [rcx+8]; mov rax, rbx; div r8; mov rcx, [rcx+28h]
    t.push_back({target::kPoolPeds,
                 {{"48 89 C3 EB ? 48 8B 0D ? ? ? ? 31 D2 48 2B 19 44 8B 41 08 48 89 D8 49 F7 F0 48 8B 49 28", {Add(8), Rip()},
                   "qword_14ABB6918 via 0x140D36890, Xbox ts 1794606036"}},
                 false});
    // Pool creation (T1.8): size = pool_config_size(cfg, n, atStringHash(name), 1); pool = new(56);
    // pool_init(pool, size, item size); mov [global], pool. The name hash pins the pool:
    // "Vehicles" 0x74B31BE3 (0x1424DE2E0), "Object" 0x39958261 (0x145AD8EC0). Same shape
    // and pool_init (0x1416CBE00) as the "Peds" pool 0x8DA12117 (0x141D811A0).
    t.push_back({target::kPoolVehicles,
                 {{"41 B8 E3 1B B3 74 41 B9 01 00 00 00 48 8B 48 08 8B 50 10 E8 ? ? ? ? B9 38 00 00 00 89 C6 E8 ? ? ? ? "
                   "48 89 C7 48 85 C0 74 ? 48 89 F9 89 F2 41 B8 ? ? 00 00 48 C7 47 20 00 00 00 00 E8 ? ? ? ? 48 89 3D",
                   {Add(71), Rip()}, "qword_14ABBE8C8 via 0x1424DE2E0, Xbox ts 1794606036"}},
                 false});
    t.push_back({target::kPoolObjects,
                 {{"41 B8 61 82 95 39 41 B9 01 00 00 00 48 8B 48 08 8B 50 10 E8 ? ? ? ? 31 C9 B2 05 89 C6 E8 ? ? ? ? "
                   "48 8D 4C 24 20 48 89 C2 E8 ? ? ? ? B9 38 00 00 00 E8 ? ? ? ? 48 89 C7 48 85 C0 74 ? 48 89 F9 89 F2 "
                   "41 B8 ? ? 00 00 48 C7 47 20 00 00 00 00 E8 ? ? ? ? 48 8B 4C 24 20 48 89 3D",
                   {Add(98), Rip()}, "qword_14ABBE2E8 via 0x145AD8EC0, Xbox ts 1794606036"}},
                 false});
    // Build id (T1.13). The exe has no version resource; the title version
    // ("major.minor.patch...") is copied from the platform's init parameters at
    // boot: lea rcx, version; lea rdx, src; mov r8d, 64; ...; call strncpy-like.
    // The engine reads it back with sscanf("%d.%d.%d") (0x145C38400). Empty
    // until then, so the profile reads it on the game thread, not at Resolve.
    t.push_back({target::kVersionString,
                 {{"48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 41 B8 40 00 00 00 48 89 3D ? ? ? ? E8", {Add(3), Rip()},
                   "0x146C32222 in 0x143DA1560, Xbox ts 1794606036"},
                  {"48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 49 89 D8 4D 89 F1 89 03 41 89 06 41 89 07", {Add(3), Rip()},
                   "sscanf(version, \"%d.%d.%d\") in 0x145C38400"}},
                 false});
    // Streaming lookup (T1.10): *out = 0xFFFFFFF; ext = strrchr(path, '.'); type = ext_to_type(ext)
    // (0x14056EE50); module = modules[type] (0x1455AEC90, table qword_14764AA10);
    // module->vtable[2](&index, {3, hash(name)}). Slot 2 is a pure lookup in the module's name map
    // (ScriptStore 0x141D3D110: hash chain at +0x90/+0x98, entries at +0x80, miss = 0x3FFFFF), so this
    // finds objects; it does not register new ones. Nothing calls it yet.
    // Registration half (T1.10, q73/q74): modules[type] is read by 0x1455AEC90
    // (cmp ecx, 255; jz; movsxd rax, ecx; lea rdx, table; mov rax, [rdx+rax*8]; ret).
    t.push_back({target::kStreamingModules,
                 {{"81 F9 FF 00 00 00 74 0F 48 63 C1 48 8D 15 ? ? ? ? 48 8B 04 C2 C3", {Add(14), Rip()},
                   "qword_14764AA10 via 0x1455AEC90, Xbox ts 1794606036"}},
                 false});
    // The name-map insert every AddSlot (vtable slot 28) ends with: map = module + 0x80,
    // buckets at map +0x10, bucket count at map +0x18, free entry at map +0x1C.
    t.push_back({target::kStreamingMapInsert,
                 {{"44 8B 51 1C 41 89 D1 41 83 FA FF 74 ? 44 89 C8 31 D2 45 89 D2 F7 71 18", {},
                   "0x1455AA720, Xbox ts 1794606036"}},
                 false});
    t.push_back({target::kStreamingFind,
                 {{"41 56 56 57 55 53 48 81 EC 50 01 00 00 49 89 D6 48 89 CE C7 01 FF FF FF 0F E8 ? ? ? ? 4C 89 F1 E8",
                   {}, "0x142756B60, Xbox ts 1794606036"}},
                 false});
    return t;
}

// --- script inventory: a read-only detour on program link ---
// Logs each program as it is linked (name hash, code, types, imports, patches)
// so the first runtime session shows which scripts exist and checks the
// layout in vi_script.h. It never writes to the program.
struct LinkHook {
    void* target = nullptr;
    void (*original)(void*) = nullptr;
    std::atomic<int> logged{0};
};
LinkHook g_link;
constexpr int kMaxInventoryLines = 4000;

struct ProgramInfo {
    std::uint32_t name, types, imports, patches;
    std::uintptr_t code;
    bool obfuscated, valid;
};

// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool ReadProgram(std::uintptr_t p, ProgramInfo* out) {
    __try {
        gta6::script::ProgramView v(p);
        out->valid = v.LooksValid();
        out->name = v.NameHash();
        out->types = v.TypeCount();
        out->imports = v.ImportCount();
        out->patches = v.PatchCount();
        out->code = v.Code();
        out->obfuscated = v.Obfuscated();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- case-139 census (TASKS T2.2): off unless <game>\shvi\native_census.on exists ---
// For each linked program: walk its code, list every native call as
// {program, site, handler rva, argc, retc, handler id} in shvi\native_census.tsv.
// The program's code size is not known statically (RESEARCH_PLAN Q4, T1.11):
// the walk is bounded by the data block that follows the code (+0xF0 - +0xE8),
// and the log line says how many 2-byte resyncs that took, which shows whether
// the bound was right.
struct Census {
    std::FILE* out = nullptr;
    std::uintptr_t nativeBaseGlobal = 0, imageBase = 0;
    std::size_t imageSize = 0;
    std::atomic<int> programs{0};
};
Census g_census;
constexpr int kMaxCensusPrograms = 2000;
constexpr std::size_t kMaxCodeBytes = 64u << 20;
constexpr std::size_t kMaxCallsPerProgram = 1u << 16;
gta6::script::NativeCall g_censusCalls[kMaxCallsPerProgram];

struct CensusResult {
    std::uint32_t name;
    std::size_t calls, instructions, resyncs, codeSize;
    std::uintptr_t nativeBase;
    bool ok;
};

// No C++ objects with destructors in here (__try).
bool SehCensus(std::uintptr_t p, CensusResult* r) {
    __try {
        namespace sc = gta6::script;
        sc::ProgramView v(p);
        r->name = v.NameHash();
        std::uintptr_t code = v.Code(), data = v.Data();
        if (!v.LooksValid() || !code || data <= code || data - code > kMaxCodeBytes) return false;
        r->codeSize = data - code;
        r->nativeBase = *reinterpret_cast<const std::uintptr_t*>(g_census.nativeBaseGlobal);
        std::size_t n = 0;
        auto st = sc::ForEachNativeCall(reinterpret_cast<const std::uint8_t*>(code), r->codeSize, v.Obfuscated(),
                                        sc::kXboxKeys, sc::kXboxOpcodeMul, [&](const sc::NativeCall& c) {
                                            if (n < kMaxCallsPerProgram) g_censusCalls[n++] = c;
                                        });
        r->calls = n;
        r->instructions = st.instructions;
        r->resyncs = st.resyncs;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehHandlerId(std::uintptr_t fn, std::uint64_t* id) {
    __try {
        // The id sits in the cold block the `js` jumps to, which is past the handler's body: 64 bytes
        // reached none of them on the live image (run of 2026-11-25). A page is enough for a short jump;
        // a read past the mapping is caught below.
        auto v = gta6::script::HandlerId(reinterpret_cast<const std::uint8_t*>(fn), 4096);
        if (!v) return false;
        *id = *v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CensusProgram(void* program) {
    if (!g_census.out || g_census.programs.fetch_add(1) >= kMaxCensusPrograms) return;
    CensusResult r{};
    if (!SehCensus(reinterpret_cast<std::uintptr_t>(program), &r)) {
        std::fprintf(g_census.out, "# program %p: unreadable or no code/data bound, skipped\n", program);
        return;
    }
    std::fprintf(g_census.out, "# program 0x%08X: %zu bytes, %zu instructions, %zu resyncs, %zu native calls\n", r.name,
                 r.codeSize, r.instructions, r.resyncs, r.calls);
    for (std::size_t i = 0; i < r.calls; ++i) {
        const auto& c = g_censusCalls[i];
        std::uintptr_t fn = r.nativeBase + c.handlerOffset;
        std::uint64_t id = 0;
        bool haveId = fn > g_census.imageBase && fn < g_census.imageBase + g_census.imageSize && SehHandlerId(fn, &id);
        std::fprintf(g_census.out, "%08X\t%X\t%llX\t%u\t%u\t%s%016llX\n", r.name, c.site,
                     static_cast<unsigned long long>(fn - g_census.imageBase), c.argc, c.retc, haveId ? "" : "?",
                     static_cast<unsigned long long>(id));
    }
    std::fflush(g_census.out);
}

// Only allocated diagnostic bytecode is written. Existing code, sizes and frame PCs stay intact.
bool ExperimentalVehicleEnabled() {
    const char* value = std::getenv("SHVI_EXPERIMENTAL_VEHICLE");
    return value && std::strcmp(value, "1") == 0;
}
bool SehVehicleDonor(std::uintptr_t program, std::uint8_t* donor, std::uintptr_t* code) {
    __try {
        namespace ve = gta6::vehicle_experiment;
        const auto count = *reinterpret_cast<const std::uint32_t*>(program + 0x2C);
        const auto types = *reinterpret_cast<const std::uintptr_t*>(program + 0x30);
        if (!types || count <= ve::kTaskType ||
            *reinterpret_cast<const std::uint32_t*>(program + 0xF8) != ve::kCodeSize) return false;
        const auto record = types + ve::kTaskType * gta6::script::type::kStride;
        const auto base = *reinterpret_cast<const std::uintptr_t*>(record + 0x20);
        if (!base || !ve::ValidTaskType(*reinterpret_cast<const std::uint32_t*>(record + 0x1C),
                                      *reinterpret_cast<const std::uint32_t*>(record + 0x34),
                                      *reinterpret_cast<const std::uint32_t*>(base + 0x1C))) return false;
        *code = *reinterpret_cast<const std::uintptr_t*>(program + 0xE8);
        if (!*code) return false;
        std::memcpy(donor, reinterpret_cast<const void*>(*code + ve::kPedOffset), ve::kPedWrapper.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Last 256 bytes of the committed read-write view holding the program's end; they and at least 4 KiB
// before them must be zero. The program ends at the larger of its code end and its import table end.
bool SehVehicleTail(std::uintptr_t program, std::uintptr_t code, const std::uint8_t* bytes, std::size_t n,
                    std::uintptr_t* out) {
    __try {
        namespace ve = gta6::vehicle_experiment;
        const auto imports = *reinterpret_cast<const std::uintptr_t*>(program + gta6::script::program::kImports);
        const auto importCount = *reinterpret_cast<const std::uint32_t*>(program + gta6::script::program::kImportCount);
        std::uintptr_t last = code + ve::kCodeSize;
        if (imports > last && importCount < 4096) last = std::max(last, imports + importCount * 24u);
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(last), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            memory.Protect != PAGE_READWRITE) return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        const auto stub = regionEnd - 0x100;
        if (stub < last + 0x1000 || stub - code > UINT32_MAX) return false;
        const auto* p = reinterpret_cast<const std::uint8_t*>(stub - 0x1000);
        bool zero = true, ours = std::equal(bytes, bytes + n, reinterpret_cast<const std::uint8_t*>(stub));
        for (std::size_t i = 0; i < 0x1100 && !ours; ++i) zero = zero && p[i] == 0;
        if (!zero && !ours) return false;
        if (!ours) std::memcpy(reinterpret_cast<void*>(stub), bytes, n);
        *out = stub;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool VehicleStubOffset(std::uintptr_t program, std::uint32_t* offset) {
    namespace ve = gta6::vehicle_experiment;
    std::array<std::uint8_t, ve::kPedWrapper.size()> donor{};
    std::uintptr_t code = 0;
    if (!SehVehicleDonor(program, donor.data(), &code)) {
        SHVI_LOG("vehicle experiment: program or task metadata validation failed");
        return false;
    }
    auto bytes = ve::Build(donor.data(), donor.size());
    if (!bytes) {
        SHVI_LOG("vehicle experiment: donor bytecode differs from expected wrapper");
        return false;
    }
    struct Stub { std::uintptr_t code; std::uint32_t offset; };
    static std::mutex mutex;
    static std::unordered_map<std::uintptr_t, Stub> stubs;
    std::lock_guard lock(mutex);
    if (auto i = stubs.find(program); i != stubs.end() && i->second.code == code) {
        *offset = i->second.offset;
        return true;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto end = code + UINT32_MAX;
    auto candidate = (code + ve::kCodeSize + granularity - 1) & ~(granularity - 1);
    while (candidate < end && end - candidate >= info.dwPageSize) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(candidate), &memory, sizeof(memory))) break;
        if (memory.State == MEM_FREE) {
            void* stub = VirtualAlloc(reinterpret_cast<void*>(candidate), info.dwPageSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (stub) {
                const auto address = reinterpret_cast<std::uintptr_t>(stub);
                if (address < code || address - code > UINT32_MAX) {
                    VirtualFree(stub, 0, MEM_RELEASE);
                    return false;
                }
                std::memcpy(stub, bytes->data(), bytes->size());
                DWORD old = 0;
                if (!VirtualProtect(stub, info.dwPageSize, PAGE_READONLY, &old)) {
                    VirtualFree(stub, 0, MEM_RELEASE);
                    return false;
                }
                *offset = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stub) - code);
                stubs[program] = {code, *offset}; // The VM may retain the bytecode, so keep it for the process.
                SHVI_LOG("vehicle experiment: standalone wrapper offset 0x%X, original code unchanged", *offset);
                return true;
            }
        }
        const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (next <= candidate || next > end) break;
        candidate = (next + granularity - 1) & ~(granularity - 1);
    }
    // Emulated guest memory reserves the whole window, so nothing is MEM_FREE. Fall back to the zero
    // tail of the committed view that ends the program (after its import table), far from its last byte.
    std::uintptr_t tail = 0;
    if (!SehVehicleTail(program, code, bytes->data(), bytes->size(), &tail)) {
        SHVI_LOG("vehicle experiment: no free page near the code and no usable zero tail");
        return false;
    }
    *offset = static_cast<std::uint32_t>(tail - code);  // not cached: re-checked on every call
    SHVI_LOG("vehicle experiment: wrapper in program tail at %p, offset 0x%X, original code unchanged",
             reinterpret_cast<void*>(tail), *offset);
    return true;
}

// Linked programs by name hash, for ABI 1.6 script_function_call. Programs stay loaded for the session
// in this build; a relinked name replaces its entry.
std::mutex g_programsMu;
std::unordered_map<std::uint32_t, std::uintptr_t> g_programs;

void ProgramLinkDetour(void* program) {
    g_link.original(program);
    CensusProgram(program);
    {
        ProgramInfo pi{};
        if (ReadProgram(reinterpret_cast<std::uintptr_t>(program), &pi) && pi.valid) {
            std::lock_guard lock(g_programsMu);
            g_programs[pi.name] = reinterpret_cast<std::uintptr_t>(program);
        }
    }
    if (g_link.logged.fetch_add(1) >= kMaxInventoryLines) return;
    ProgramInfo i{};
    if (!ReadProgram(reinterpret_cast<std::uintptr_t>(program), &i)) {
        SHVI_LOG("gta6: script linked %p (unreadable)", program);
        return;
    }
    SHVI_LOG("gta6: script linked %p name 0x%08X code %p types %u imports %u patches %u%s%s", program, i.name,
             reinterpret_cast<void*>(i.code), i.types, i.imports, i.patches, i.obfuscated ? " obfuscated" : "",
             i.valid ? "" : " LAYOUT-MISMATCH");
}

bool InstallLinkHook(std::uintptr_t fn) {
    if (!fn || g_link.target) return false;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original = nullptr;
    auto target = reinterpret_cast<void*>(fn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ProgramLinkDetour), &original) != MH_OK) return false;
    g_link.original = reinterpret_cast<void (*)(void*)>(original);
    g_link.target = target;
    return MH_EnableHook(target) == MH_OK;
}

// --- thread layout check: a read-only detour on script_call (TASKS T1.2) ---
// Logs the first threads the VM runs and whether they fit vi_script.h's
// thread layout (ThreadView::LooksValid). It reads, never writes, and only
// for the first kMaxThreadLines distinct threads.
using ScriptCallFn = std::int64_t (*)(void*, void*, std::uint32_t, const void*, void*, std::uint32_t);
struct CallHook {
    void* target = nullptr;
    ScriptCallFn original = nullptr;
    std::atomic<int> logged{0};
    std::atomic<std::uintptr_t> seen[32] = {};
    std::atomic<std::uint64_t> calls{0};  // T1.4: scripts resume through new script_calls, so this grows per frame
};
CallHook g_call;
constexpr int kMaxThreadLines = 32;

struct ThreadInfo {
    std::uintptr_t owner, statics, root, current;
    std::uint32_t depth;
    std::uint8_t state;
    std::size_t frames;
    bool valid;
};

// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool ReadThread(std::uintptr_t t, ThreadInfo* out) {
    __try {
        gta6::script::ThreadView v(t);
        out->valid = v.LooksValid();
        out->owner = v.Owner();
        out->statics = v.Statics();
        out->root = v.RootProgram();
        out->state = v.State();
        out->depth = v.CallDepth();
        out->frames = out->valid ? v.FrameCount() : 0;
        out->current = out->valid ? v.CurrentProgram() : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FirstSighting(std::uintptr_t t) {
    for (auto& s : g_call.seen) {
        std::uintptr_t cur = s.load(std::memory_order_relaxed);
        if (cur == t) return false;
        if (cur == 0 && s.compare_exchange_strong(cur, t)) return true;
        if (cur == t) return false;
    }
    return false;  // table full: stop logging
}

void TickAfterScriptCall();  // below, with the tick state
void TickAfterScriptCall();
thread_local void* t_tickThread = nullptr;

// script_call's function reference: code offset, then the program (0x143DE1F10 reads [fn] and [fn+8]).
struct ScriptFnRef {
    std::uint32_t offset;
    std::uint32_t pad;
    std::uintptr_t program;
};
// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool SehScriptCall(ScriptCallFn call, void* thread, ScriptFnRef* ref, std::uint32_t argc, const std::uint64_t* args,
                   std::uint64_t* ret, std::uint32_t retc) {
    __try {
        // The VM loop runs only while the thread's state byte (+0x10) is 1 (running). After the game's own
        // call it is 2 (returned), and a call made then does nothing and hands the arguments back (run 10).
        *(reinterpret_cast<std::uint8_t*>(thread) + 0x10) = 1;
        call(thread, ref, argc, args, ret, retc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- script heaps (runs 13-17, Xbox ts 1794606036 only; addresses are RVAs of that build) ---
// The engine has three script heaps (pointers at 0x14ABC3C20 + 8*i). A thread's current one is TLS +5208
// (index, 4 = none) and +5232 (pointer). Every pooled script type has a region in a heap's table
// (+6104: count +72, 104-byte entries +80, key = pool id); allocating a type under a heap that has no
// region for it indexes past the table and faults (0x1406728F0). A worker thread at tick time has
// whichever heap its last job left, so a script function we call may allocate under the wrong one.
struct ScriptHeaps {
    std::uintptr_t imageBase = 0;
    int mode = -2;  // -2 off, -1 auto (the heap that holds most pool ids), 0..2 fixed
    static constexpr std::uintptr_t kHeapTable = 0xABC3C20, kPoolCount = 0x98C16EC, kPools = 0x98C16E8;
    static constexpr std::uintptr_t kEnter = 0x644B10, kLeave = 0x644920;  // (saved*, index) / (saved*)
};
ScriptHeaps g_heaps;
std::uintptr_t EngineTlsBlock();  // defined below
struct HeapProbe {
    std::uint32_t index = 4, pools = 0;
    std::uintptr_t current = 0, heap[3] = {};
    std::uint32_t count[3] = {}, keys[3] = {}, poolHits[3] = {};
};
bool SehProbeHeaps(std::uintptr_t base, std::uintptr_t tls, HeapProbe* o) {
    __try {
        o->index = *reinterpret_cast<const std::uint32_t*>(tls + 5208);
        o->current = *reinterpret_cast<const std::uintptr_t*>(tls + 5232);
        std::uint32_t pools = *reinterpret_cast<const std::uint32_t*>(base + ScriptHeaps::kPoolCount);
        if (pools > 4096) pools = 4096;
        o->pools = pools;
        for (int i = 0; i < 3; ++i) {
            std::uintptr_t h = *reinterpret_cast<const std::uintptr_t*>(base + ScriptHeaps::kHeapTable + 8 * i);
            o->heap[i] = h;
            if (!h) continue;
            std::uint32_t n = *reinterpret_cast<const std::uint32_t*>(h + 6104 + 72);
            std::uintptr_t e = *reinterpret_cast<const std::uintptr_t*>(h + 6104 + 80);
            o->count[i] = n;
            if (!e || n > 65536) continue;
            for (std::uint32_t j = 0; j < n; ++j) {
                std::uint32_t key = *reinterpret_cast<const std::uint32_t*>(e + 104ull * j);
                if (!key) continue;
                ++o->keys[i];
                for (std::uint32_t k = 0; k < pools; ++k)
                    if (*reinterpret_cast<const std::uint32_t*>(base + ScriptHeaps::kPools + 48ull * k + 8) == key) {
                        ++o->poolHits[i];
                        break;
                    }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Experiment (run 58): the engine keeps a creation task through a handle in the heap's handle table
// (+5512 free list, +5520 slots, +5532 stride, +5536 and +5552 bitmaps; slot = object | type << 56, type 1
// as written by 0x14192D3C0). Type 1 looks like a weak handle: the task is reclaimed after about 3 s. This
// finds the slot of `object` in `heap` and rewrites it as type 2, setting its bit in the second bitmap.
// mode 1: type byte only; mode 2: type byte and the +5552 bit. Returns the slot index, or -1.
int SehPinHandle(std::uintptr_t heap, std::uintptr_t object, int mode, std::uint64_t* raw, std::uint32_t* info) {
    __try {
        auto slots = *reinterpret_cast<const std::uintptr_t*>(heap + 5520);
        std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(heap + 5532);
        std::uint32_t cap = *reinterpret_cast<const std::uint32_t*>(heap + 5528);
        info[0] = stride; info[1] = cap; info[2] = *reinterpret_cast<const std::uint32_t*>(heap + 5568);
        if (!slots || !stride || stride > 64 || cap > 1u << 20) return -1;
        for (std::uint32_t i = 0; i < cap; ++i) {
            auto slot = reinterpret_cast<std::uint64_t*>(slots + static_cast<std::uintptr_t>(stride) * i);
            std::uint64_t v = *slot;
            // The script's reference is the object + 0x18 (pool header before it): the handle may hold either.
            std::uint64_t lo = v & 0x000000FFFFFFFFFFull;
            if (lo != (object & 0x000000FFFFFFFFFFull) && lo != ((object - 0x18) & 0x000000FFFFFFFFFFull)) continue;
            *raw = v;
            if ((v >> 56) == 1) {
                *slot = (v & 0x00FFFFFFFFFFFFFFull) | (2ull << 56);
                if (mode >= 2) {
                    auto strong = *reinterpret_cast<std::uintptr_t*>(heap + 5552);
                    if (strong) _InterlockedOr(reinterpret_cast<volatile long*>(strong + 4 * (i >> 5)), 1l << (i & 31));
                }
            }
            return static_cast<int>(i);
        }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
// Copies up to `max` slots of a heap's handle table; returns how many (0 on any fault).
std::uint32_t SehSnapshotHandles(std::uintptr_t heap, std::uint64_t* out, std::uint32_t max) {
    __try {
        auto slots = *reinterpret_cast<const std::uintptr_t*>(heap + 5520);
        std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(heap + 5532);
        std::uint32_t cap = *reinterpret_cast<const std::uint32_t*>(heap + 5528);
        if (!slots || stride != 8) return 0;
        if (cap > max) cap = max;
        std::memcpy(out, reinterpret_cast<const void*>(slots), static_cast<std::size_t>(cap) * 8);
        return cap;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool SehWriteSlot(std::uintptr_t heap, std::uint32_t index, std::uint64_t value) {
    __try {
        auto slots = *reinterpret_cast<const std::uintptr_t*>(heap + 5520);
        *reinterpret_cast<std::uint64_t*>(slots + 8ull * index) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// --- ABI 1.7 pins (2026-11-26, from the code; Xbox ts 1794606036 only) ---
// Each script heap has handle tables 64 bytes apart from +5512; a handle is 0x80000000 | heap << 28 |
// slot << 4 | table. Table 0 is weak: the async natives register their Task there (0x14192D3C0), so the
// runtime reclaims the task once no script refers to it (runs 18-22 and 58). Table 2 is what the engine
// uses to hold script objects from C++: 0x14381A110(holder, object) enters heap 2, allocates a table-2
// handle and registers it with the current script instance; 0x14205D410(holder) releases it (both seen
// on the three callback fields an engine object keeps, 0x141D0A430). A holder is 16 bytes:
// {handle, instance id, registration, second handle}.
struct ScriptPins {
    static constexpr std::uintptr_t kPin = 0x381A110, kUnpin = 0x205D410;
    static constexpr std::uint32_t kSlots = 256;
    struct alignas(16) Holder {
        std::uint32_t w[4];
    };
    Holder holders[kSlots] = {};
    bool used[kSlots] = {};
    std::mutex mu;
};
ScriptPins g_pins;
bool SehCurrentScriptInstance(std::uintptr_t tls, std::uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uintptr_t*>(tls + 5240);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehPinObject(std::uintptr_t base, ScriptPins::Holder* h, std::uint64_t object) {
    __try {
        reinterpret_cast<char (*)(std::uint32_t*, std::uint64_t)>(base + ScriptPins::kPin)(h->w, object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SehUnpinObject(std::uintptr_t base, ScriptPins::Holder* h) {
    __try {
        reinterpret_cast<std::int64_t (*)(std::uint32_t*)>(base + ScriptPins::kUnpin)(h->w);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ped creation gates (0x14115B2F0): a completed task with a null ped comes back when any fails. Diagnostic for
// the ped wrapper calls: the owning script instance (0x144F61E80, 0 = none or not allowed), the model info with
// and without the deny list (0x1400BF810; type at +153 & 7 must be 6), and the ped pool's two counters.
struct SpawnDiag {
    std::uint32_t owner = 0, typeChecked = 0xFF, typeAny = 0xFF, pool24 = 0, pool28 = 0;
    std::uintptr_t info = 0, infoAny = 0;
    // the owner check, step by step: TLS +5240 -> +8 -> id +840; registry lookup; state +836; vtable +88
    std::uint32_t id = 0, state = 0xFF;
    std::uintptr_t tlsObj = 0, inst = 0, mayCreate = 0;
    int mayCreateResult = -1;
};
bool SehSpawnDiag(std::uintptr_t base, std::uint64_t model, SpawnDiag* d) {
    __try {
        reinterpret_cast<char (*)(std::uint32_t*)>(base + 0x4F61E80)(&d->owner);
        reinterpret_cast<std::uint64_t (*)(std::uint32_t*)>(base + 0x4F61F00)(&d->id);
        d->tlsObj = *reinterpret_cast<const std::uintptr_t*>(EngineTlsBlock() + 5240);
        if (d->id) {
            d->inst = reinterpret_cast<std::uintptr_t (*)(std::uint32_t)>(base + 0x4F61F30)(d->id);
            if (d->inst) {
                d->state = *reinterpret_cast<const std::uint8_t*>(d->inst + 836);
                d->mayCreate = *reinterpret_cast<const std::uintptr_t*>(*reinterpret_cast<const std::uintptr_t*>(d->inst) + 88);
                if (d->mayCreate == base + 0x4870) d->mayCreateResult = 2;  // the default: allowed
                else d->mayCreateResult = reinterpret_cast<char (*)(std::uintptr_t)>(d->mayCreate)(d->inst) ? 1 : 0;
            }
        }
        std::uint32_t idx = 0;
        auto lookup = reinterpret_cast<std::uintptr_t (*)(std::uint64_t, std::uint32_t*, char)>(base + 0xBF810);
        d->info = lookup(model, &idx, 0);
        d->infoAny = lookup(model, &idx, 1);
        if (d->info) d->typeChecked = *reinterpret_cast<const std::uint8_t*>(d->info + 153) & 7;
        if (d->infoAny) d->typeAny = *reinterpret_cast<const std::uint8_t*>(d->infoAny + 153) & 7;
        auto pool = *reinterpret_cast<const std::uintptr_t*>(base + 0xAAD3CD0);
        if (pool) {
            d->pool24 = *reinterpret_cast<const std::uint32_t*>(pool + 24);
            d->pool28 = *reinterpret_cast<const std::uint32_t*>(pool + 28);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint64_t g_snapBefore[3][8192], g_snapAfter[3][8192];
int g_pinMode = 0;  // shvi\pin_task.txt: 1 or 2

bool SehReadHeapIndex(std::uintptr_t tls, std::uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uint32_t*>(tls + 5208);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The call, under heap `index` when it is 0..2 (the engine's own enter/leave pair), else as is.
bool SehScriptCallInHeap(ScriptCallFn call, void* thread, ScriptFnRef* ref, std::uint32_t argc,
                         const std::uint64_t* args, std::uint64_t* ret, std::uint32_t retc, std::uintptr_t base,
                         int index) {
    alignas(16) std::uint32_t saved[16] = {};
    bool entered = false;
    bool ok = true;
    __try {
        if (index >= 0 && index < 3 && base) {
            reinterpret_cast<std::uint32_t* (*)(std::uint32_t*, std::uint32_t)>(base + ScriptHeaps::kEnter)(
                saved, static_cast<std::uint32_t>(index));
            entered = true;
        }
        *(reinterpret_cast<std::uint8_t*>(thread) + 0x10) = 1;
        call(thread, ref, argc, args, ret, retc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (entered) {
        __try {
            reinterpret_cast<std::int64_t (*)(std::uint32_t*)>(base + ScriptHeaps::kLeave)(saved);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    return ok;
}

std::int64_t ScriptCallDetour(void* thread, void* fn, std::uint32_t argc, const void* args, void* ret,
                              std::uint32_t retc) {
    auto t = reinterpret_cast<std::uintptr_t>(thread);
    g_call.calls.fetch_add(1, std::memory_order_relaxed);
    if (g_call.logged.load(std::memory_order_relaxed) < kMaxThreadLines && FirstSighting(t)) {
        g_call.logged.fetch_add(1);
        ThreadInfo i{};
        if (!ReadThread(t, &i)) {
            SHVI_LOG("gta6: script thread %p (unreadable)", thread);
        } else {
            SHVI_LOG("gta6: script thread %p state %u depth %u frames %zu owner %p statics %p root %p current %p%s",
                     thread, i.state, i.depth, i.frames, reinterpret_cast<void*>(i.owner),
                     reinterpret_cast<void*>(i.statics), reinterpret_cast<void*>(i.root),
                     reinterpret_cast<void*>(i.current), i.valid ? "" : " LAYOUT-MISMATCH");
        }
    }
    // Tick source: after the outermost script_call on a thread returns (scripts run through
    // script_call every frame; nested calls from inside a script are skipped).
    if (g_heaps.imageBase) {
        static std::atomic<std::uint32_t> seen{0};  // bit i: a game script_call arrived with heap index i
        std::uint32_t idx = 5;
        SehReadHeapIndex(EngineTlsBlock(), &idx);
        if (idx > 5) idx = 5;
        std::uint32_t bit = 1u << idx, old = seen.fetch_or(bit);
        if (!(old & bit)) SHVI_LOG("gta6: a game script_call arrived under script heap index %u (thread %p)", idx, thread);
    }
    thread_local int depth = 0;
    // Tick after the game's outermost call on this thread returns. Ticking just before it instead (inside
    // the engine's script scope) was tried in run 16: ordinary natives behave the same there and ped
    // creation still faults in the pooled allocator, so the proven placement stays.
    ++depth;
    std::int64_t r = g_call.original(thread, fn, argc, args, ret, retc);
    if (--depth == 0) {
        t_tickThread = thread;  // idle between the game's own calls: ABI 1.6 may run script functions on it
        TickAfterScriptCall();
        t_tickThread = nullptr;
    }
    return r;
}

bool InstallCallHook(std::uintptr_t fn) {
    if (!fn || g_call.target) return false;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original = nullptr;
    auto target = reinterpret_cast<void*>(fn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ScriptCallDetour), &original) != MH_OK) return false;
    g_call.original = reinterpret_cast<ScriptCallFn>(original);
    g_call.target = target;
    return MH_EnableHook(target) == MH_OK;
}

// --- tick: after the outermost script_call (live finding 2026-11-24) ---
// script_mgr_update (0x1429DFAF0) turned out to be a skeleton *init* function (it switches on
// the init mode: 1 core, 8 session), so it never runs per frame. Scripts do run through
// script_call every frame, on the game's script thread.
struct ViHook {
    GameProfile::TickFn tick = nullptr;
    gta6::TickGate gate{15000};  // >= 15 ms apart: scripts call script_call many times a frame
    gta6::ThreadContextMode threadMode = gta6::ThreadContextMode::None;  // T2.7, shvi\thread_context.txt
    gta6::ThreadContextEngine threadEngine;
};
ViHook g_vi;

// gs:[58h] is the TLS array; slot 0 is the engine's TLS block (script_instance_get,
// script_call: mov rax, gs:58h; mov rax, [rax]).
// Under a host that emulates the title's TLS (VIctory), every `mov rax, gs:[58h]` in the image is rewritten
// to `call thunk` + a 4-byte nop, and gs:[58h] of the real thread is the HOST's TLS array: reading it gave
// index 0 / pointer 0 in run 17 and crashed the scope mode in run 15. When the rewritten form is seen at a
// known site, the thunk it calls is used instead (it returns the title's TLS array in rax).
using TlsArrayThunk = std::uintptr_t (*)();
TlsArrayThunk g_tlsArrayThunk = nullptr;
void ResolveTlsThunk(std::uintptr_t siteWithGsRead) {  // address of the 9-byte read
    if (!siteWithGsRead) return;
    auto b = reinterpret_cast<const std::uint8_t*>(siteWithGsRead);
    if (b[0] != 0xE8) return;  // 65 48 8B 04 25 58 00 00 00 on real hardware
    std::int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    g_tlsArrayThunk = reinterpret_cast<TlsArrayThunk>(siteWithGsRead + 5 + rel);
}
std::uintptr_t EngineTlsBlock() {
    auto arr = reinterpret_cast<const std::uintptr_t*>(g_tlsArrayThunk ? g_tlsArrayThunk() : __readgsqword(0x58));
    return arr ? arr[0] : 0;
}

// T2.8: UI_DISABLE_MENU_CONTROLS, once DISABLE_CONTROL_ACTION is bound from the native table.
rage::ViNativeHandler g_disableControlAction = nullptr;
void UiDisableMenuControls() { gta6::DisableControls(g_disableControlAction); }

std::uint64_t NowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(c.QuadPart) * 1000000ull / static_cast<std::uint64_t>(freq.QuadPart);
}

// Runs after each outermost script_call. The gate pins the first thread and spaces ticks
// (scripts call many times a frame), so plugins tick about once a frame on the script thread.
void TickAfterScriptCall() {
    if (!g_vi.tick) return;
    static std::atomic<std::uint64_t> ticks{0};
    // One tick at a time: script calls arrive on several worker threads.
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    if (busy.test_and_set(std::memory_order_acquire)) return;
    struct Release { ~Release() { busy.clear(std::memory_order_release); } } release;
    if (!g_vi.gate.Allow(GetCurrentThreadId(), NowUs())) return;
    std::uint64_t n = ticks.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) SHVI_LOG("gta6: first tick (after script_call, thread %lu)", GetCurrentThreadId());
    // T1.4 runtime check: script_call rate per tick (async continuations resume through it).
    static std::uint64_t lastCalls = 0;
    if (n % 600 == 599) {
        std::uint64_t c = g_call.calls.load(std::memory_order_relaxed);
        SHVI_LOG("gta6: script_call %.1f per tick over the last 600 ticks; tick thread %lu, moved %llu time(s)",
                 double(c - lastCalls) / 600.0, g_vi.gate.Thread(), static_cast<unsigned long long>(g_vi.gate.Repins()));
        lastCalls = c;
    }
    gta6::ScopedScriptThread ctx(g_vi.threadMode, g_vi.threadEngine);
    g_vi.tick();
}

// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool ReadModeFields(std::uintptr_t global, std::uint32_t* cur, std::uint32_t* req) {
    __try {
        auto obj = *reinterpret_cast<const std::uintptr_t*>(global);
        if (!obj) return false;
        *cur = *reinterpret_cast<const std::uint32_t*>(obj + gta6::kModeCurrentOffset);
        *req = *reinterpret_cast<const std::uint32_t*>(obj + gta6::kModeRequestedOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Entity API helpers (ABI 1.2). No C++ objects in these frames (__try).
using EntityResolveFn = void* (*)(const std::uint64_t* ref);
using EntityMakeRefFn = void (*)(void* obj, std::uint64_t* out, int mode);
std::size_t SehLiveItems(std::uintptr_t poolGlobal, std::uintptr_t* out, std::size_t max, bool* ok) {
    __try {
        auto p = *reinterpret_cast<const std::uintptr_t*>(poolGlobal);
        *ok = true;
        return p ? gta6::ecs::LiveItems(p, out, max) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return 0;
    }
}
bool SehMakeRef(EntityMakeRefFn fn, std::uintptr_t obj, std::uint64_t* out) {
    __try {
        fn(reinterpret_cast<void*>(obj), out, 2);  // mode 2: what every script command passes
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = out[1] = 0;
        return false;
    }
}
// T1.13: copies the version string once it is non-empty. False while empty or unreadable.
bool SehReadVersion(std::uintptr_t at, char* out, std::size_t n) {
    __try {
        const char* p = reinterpret_cast<const char*>(at);
        std::size_t i = 0;
        for (; i + 1 < n && p[i]; ++i) out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? p[i] : '?';
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// T1.9 (static): the player's CPed object global is set once a player ped exists
// and cleared by 0x1427FA0E0 on teardown. 1 = set, 0 = null, -1 = unreadable.
int SehReadPlayerPed(std::uintptr_t global) {
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(global) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void* SehResolve(EntityResolveFn fn, const std::uint64_t* ref) {
    __try {
        return fn(ref);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// T2.4: the first live script instance running program `prog`, then the VM's
// static formula on its thread. Read-only; no C++ objects with destructors here.
std::uintptr_t SehStaticPtr(std::uintptr_t instances, std::uint32_t prog, std::uint32_t module, std::uint32_t field) {
    __try {
        namespace sc = gta6::script;
        std::uintptr_t found = 0;
        sc::ForEachInstance(instances, [&](std::uintptr_t inst, std::uintptr_t thr) {
            if (found || !thr) return;
            std::uintptr_t p = *reinterpret_cast<const std::uintptr_t*>(inst + sc::instance::kProgram);
            if (!p || sc::ProgramView(p).NameHash() != prog) return;
            found = sc::StaticAddress(thr, module ? module : prog, field);
        });
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string ReadSmallFile(const std::wstring& path) {
    std::string s;
    if (std::FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[256];
        std::size_t n = std::fread(buf, 1, sizeof buf, f);
        s.assign(buf, n);
        std::fclose(f);
    }
    return s;
}

class Gta6Xbox final : public GameProfile {
public:
    const char* Name() const override { return "GTA VI (Xbox Series build, analysis profile)"; }
    bool Matches(const HostInfo& host) const override {
        return IEquals(host.exeName, L"game_scarlett_master.exe");
    }
    std::uint32_t Build() const override { return build_; }
    std::vector<std::wstring_view> AntiCheatModules() const override { return {}; }
    std::vector<std::wstring_view> OnlineCommandLineFlags() const override { return {}; }
    std::vector<scan::Target> ScanTargets() const override { return XboxTargets(); }

    ResolveResult Resolve(const HostInfo& host, const pe::Module& exe, std::string* log) override {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe.image.begin);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe.image.begin + dos->e_lfanew);
        build_ = nt->FileHeader.TimeDateStamp;

        auto targets = XboxTargets();
        scan::Report r = scan::ResolveAll(scan::Scanner(exe), targets);
        if (log) *log = r.log;
        if (!r.Ok()) return ResolveResult::Retry;

        std::uintptr_t hashFn = r.Get(target::kStringHash);  // already probe-validated
        engine_.BindAddress<std::uint32_t(const char*, std::uint32_t)>(EngineHash("STRING_HASH"), hashFn);
        engine_.BindAddress<float(std::uint32_t)>(EngineHash("PARAM_GET_FLOAT"), r.Get(target::kParamFloat));
        engine_.BindAddress<std::int32_t(std::uint32_t)>(EngineHash("PARAM_GET_INT"), r.Get(target::kParamInt));
        scriptStore_ = r.Get(target::kScriptStore);
        scriptMgrUpdate_ = r.Get(target::kScriptMgrUpdate);
        // The guard signal needs both the global and proof of the field layout.
        if (r.Get(target::kModeLayout)) modeGlobal_ = r.Get(target::kGameModeObj);
        ResolveScriptRuntime(r, host, exe);
        entityResolve_ = reinterpret_cast<EntityResolveFn>(r.Get(target::kEntityResolve));
        entityMakeRef_ = reinterpret_cast<EntityMakeRefFn>(r.Get(target::kEntityMakeRef));
        poolPeds_ = r.Get(target::kPoolPeds);
        poolVehicles_ = r.Get(target::kPoolVehicles);
        poolObjects_ = r.Get(target::kPoolObjects);
        playerPed_ = r.Get(target::kPlayerPedObj);
        versionString_ = r.Get(target::kVersionString);
        instances_ = r.Get(target::kInstances);
        LoadNativeTable(host, exe);
        ConfigureThreadContext(host, r);
        SHVI_LOG("gta6: game-ready signal: player ped global %p%s", reinterpret_cast<void*>(playerPed_),
                 playerPed_ ? "" : " NOT FOUND (plugins stay off)");
        SHVI_LOG("gta6: entity api: resolve %p, make_ref %p, pool globals peds %p vehicles %p objects %p "
                 "(no pickup pool in this build: pickups are objects)",
                 reinterpret_cast<void*>(entityResolve_), reinterpret_cast<void*>(entityMakeRef_),
                 reinterpret_cast<void*>(poolPeds_), reinterpret_cast<void*>(poolVehicles_),
                 reinterpret_cast<void*>(poolObjects_));
        SHVI_LOG("gta6: %s resolved, %zu engine functions bound, ScriptStore at %p (host %ls)",
                 Name(), engine_.Size(), reinterpret_cast<void*>(scriptStore_), host.exeName.c_str());
        return ResolveResult::Ok;
    }

    NativeTarget FindTarget(std::uint64_t hash) override { return engine_.Find(hash); }

    // Tick source: detour the script manager's per-frame update (registered with
    // the game skeleton under 0x0B3A5867). Statically derived; per-frame and
    // single-thread behaviour are enforced by TickGate rather than assumed.
    bool InstallTick(TickFn tick) override {
        // The script_call detour is installed at Resolve time (ResolveScriptRuntime).
        if (!g_call.target || !modeGlobal_) {
            SHVI_LOG("gta6: tick needs the script_call detour and the game-mode signal; plugins stay off");
            return false;
        }
        g_vi.tick = tick;
        SHVI_LOG("gta6: tick after script_call %p (script_mgr_update %p is an init function, not hooked)",
                 g_call.target, reinterpret_cast<void*>(scriptMgrUpdate_));
        return true;
    }
    void RemoveTick() override {
        if (g_link.target) MH_DisableHook(g_link.target);
        if (g_call.target) MH_DisableHook(g_call.target);
        g_vi.tick = nullptr;
    }
    // Peds, vehicles and objects: the three pools named by the pool config (T1.8).
    // The build has no pickup pool (no pool config is named for pickups; the
    // pickup natives return objects), so SHVI_ENTITY_PICKUP stays unsupported.
    int WorldGetAll(std::uint32_t kind, std::uint64_t* refs, int max) override {
        std::uintptr_t pool = kind == 0 ? poolPeds_ : kind == 1 ? poolVehicles_ : kind == 2 ? poolObjects_ : 0;
        if (!pool || !entityMakeRef_) return -4;  // unsupported
        std::vector<std::uintptr_t> objs(static_cast<std::size_t>(max));
        bool ok = false;
        std::size_t live = SehLiveItems(pool, objs.data(), objs.size(), &ok);
        if (!ok) return -2;  // not ready
        std::size_t n = live < objs.size() ? live : objs.size();
        for (std::size_t i = 0; i < n; ++i) SehMakeRef(entityMakeRef_, objs[i], refs + 2 * i);
        return static_cast<int>(live > 0x7FFFFFFF ? 0x7FFFFFFF : live);
    }
    Readiness GameReady() override {
        // Once per session: log the title version next to the PE timestamp
        // that game_build() reports and the native table is keyed by (T1.13).
        if (versionString_ && !versionLogged_) {
            char v[65];
            if (SehReadVersion(versionString_, v, sizeof v)) {
                versionLogged_ = true;
                SHVI_LOG("gta6: title version \"%s\", build key (PE timestamp) %u", v, build_);
            }
        }
        if (!playerPed_) return Readiness::Unknown;
        int v = SehReadPlayerPed(playerPed_);
        return v < 0 ? Readiness::Unknown : v ? Readiness::Ready : Readiness::NotReady;
    }
    void* EntityPtr(const std::uint64_t* ref) override {
        if (!entityResolve_ || !ref || ref[0] == 0) return nullptr;  // the engine skips null refs too
        return SehResolve(entityResolve_, ref);
    }

    GameMode QueryMode(Invoker&) override {
        if (!modeGlobal_) return GameMode::Unknown;  // fail closed
        std::uint32_t cur = 0, req = 0;
        bool have = ReadModeFields(modeGlobal_, &cur, &req);
        return gta6::ClassifyMode(have, cur, req);
    }

    // ABI 1.6: a script function, run on the thread whose outermost script_call just returned.
    int CallScriptFunction(std::uint32_t program, std::uint32_t offset, const std::uint64_t* args, std::uint32_t argc,
                           std::uint64_t* ret, std::uint32_t retc) override {
        if (!g_call.original || !t_tickThread) return SHVI_E_NOT_READY;
        std::uintptr_t prog = 0;
        {
            std::lock_guard lock(g_programsMu);
            if (auto it = g_programs.find(program); it != g_programs.end()) prog = it->second;
        }
        if (!prog) return SHVI_E_NOT_READY;
        ProgramInfo pi{};
        if (!ReadProgram(prog, &pi) || !pi.valid || pi.name != program) return SHVI_E_NOT_READY;
        if (offset == gta6::vehicle_experiment::kSentinel) {
            SHVI_LOG("vehicle experiment: gate enabled=%d build=%u program=%08X argc=%u retc=%u",
                     ExperimentalVehicleEnabled(), build_, program, argc, retc);
            if (!ExperimentalVehicleEnabled() || build_ != 1794606036u ||
                program != gta6::vehicle_experiment::kProgram || argc != 13 || retc != 1 ||
                !VehicleStubOffset(prog, &offset)) {
                SHVI_LOG("vehicle experiment: validation or allocation refused; no native was called");
                return SHVI_E_UNSUPPORTED;
            }
            SHVI_LOG("vehicle experiment: calling standalone VM wrapper, 13 arguments; engine acceptance unverified");
        }
        ScriptFnRef ref{offset, 0, prog};
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 16)
            SHVI_LOG("gta6: script_function_call program 0x%08X (%p) offset 0x%X argc %u retc %u on thread %p", program,
                     reinterpret_cast<void*>(prog), offset, argc, retc, t_tickThread);
        int heap = -2;
        if (g_heaps.imageBase) {
            HeapProbe hp{};
            bool probed = SehProbeHeaps(g_heaps.imageBase, EngineTlsBlock(), &hp);
            if (probed && g_heaps.mode == -1) {
                int best = 0;
                for (int i = 1; i < 3; ++i)
                    if (hp.poolHits[i] > hp.poolHits[best]) best = i;
                if (hp.poolHits[best] && static_cast<std::uint32_t>(best) != hp.index) heap = best;
            } else if (g_heaps.mode >= 0) {
                heap = g_heaps.mode;
            }
            static std::atomic<int> heapLogged{0};
            if (heapLogged.fetch_add(1) < 16) {
                if (!probed) SHVI_LOG("gta6: script heaps unreadable");
                else
                    SHVI_LOG("gta6: script heap of this thread: index %u (%p); %u pools; heaps [0] %p %u/%u slots, %u pool ids "
                             "[1] %p %u/%u, %u [2] %p %u/%u, %u; call runs under %s%d",
                             hp.index, reinterpret_cast<void*>(hp.current), hp.pools, reinterpret_cast<void*>(hp.heap[0]),
                             hp.keys[0], hp.count[0], hp.poolHits[0], reinterpret_cast<void*>(hp.heap[1]), hp.keys[1],
                             hp.count[1], hp.poolHits[1], reinterpret_cast<void*>(hp.heap[2]), hp.keys[2], hp.count[2],
                             hp.poolHits[2], heap >= 0 ? "heap " : "the current heap ", heap >= 0 ? heap : -1);
            }
        }
        static std::atomic<int> gatesLogged{0};
        if (g_heaps.imageBase && program == 0x48391B87u && offset == 0x18247u && argc >= 3 &&
            gatesLogged.fetch_add(1) < 8) {  // the ped wrapper: why a creation would be refused
            SpawnDiag d{};
            bool okd = SehSpawnDiag(g_heaps.imageBase, args[0], &d);
            float x = 0, y = 0, z = 0;
            std::uint32_t xb = static_cast<std::uint32_t>(args[1]), yb = static_cast<std::uint32_t>(args[1] >> 32),
                          zb = static_cast<std::uint32_t>(args[2]);
            std::memcpy(&x, &xb, 4); std::memcpy(&y, &yb, 4); std::memcpy(&z, &zb, 4);
            SHVI_LOG("gta6: ped creation gates%s: owner instance %u, model info %p type %u (without deny list %p type %u), "
                     "ped pool %u / %u, at %.1f %.1f %.1f", okd ? "" : " (FAULT)", d.owner, reinterpret_cast<void*>(d.info),
                     d.typeChecked, reinterpret_cast<void*>(d.infoAny), d.typeAny, d.pool24, d.pool28, x, y, z);
            SHVI_LOG("gta6: ped creation owner check: TLS object %p, instance id %u -> %p, state %u, may-create rva 0x%llX -> %d",
                     reinterpret_cast<void*>(d.tlsObj), d.id, reinterpret_cast<void*>(d.inst), d.state,
                     static_cast<unsigned long long>(d.mayCreate ? d.mayCreate - g_heaps.imageBase : 0), d.mayCreateResult);
        }
        std::uint32_t snapN[3] = {};
        HeapProbe hpPin{};
        if (g_pinMode && g_heaps.imageBase && SehProbeHeaps(g_heaps.imageBase, EngineTlsBlock(), &hpPin))
            for (int hi = 0; hi < 3; ++hi)
                if (hpPin.heap[hi]) snapN[hi] = SehSnapshotHandles(hpPin.heap[hi], g_snapBefore[hi], 8192);
        bool ok = heap >= 0 ? SehScriptCallInHeap(g_call.original, t_tickThread, &ref, argc, args, ret, retc,
                                                  g_heaps.imageBase, heap)
                            : SehScriptCall(g_call.original, t_tickThread, &ref, argc, args, ret, retc);
        if (ok && g_pinMode && heap >= 0 && retc == 1 && ret && ret[0] > 0x10000) {
            int changedSlots = 0;
            for (int hi = 0; hi < 3; ++hi) {
                if (!snapN[hi]) continue;
                std::uint32_t n = SehSnapshotHandles(hpPin.heap[hi], g_snapAfter[hi], snapN[hi]);
                for (std::uint32_t k = 0; k < n && changedSlots < 24; ++k) {
                    std::uint64_t b = g_snapBefore[hi][k], a2 = g_snapAfter[hi][k];
                    if (a2 == b) continue;
                    ++changedSlots;
                    bool pinned = false;
                    // A slot that was a small free-list link and now holds a big value is a new handle.
                    if (g_pinMode >= 2 && (a2 >> 56) == 1 && (b >> 32) == 0)
                        pinned = SehWriteSlot(hpPin.heap[hi], k, (a2 & 0x00FFFFFFFFFFFFFFull) | (2ull << 56));
                    SHVI_LOG("gta6: handle slot changed by the call: heap %d slot %u: %llX -> %llX%s (result %p)", hi, k,
                             static_cast<unsigned long long>(b), static_cast<unsigned long long>(a2), pinned ? " [type 1 -> 2]" : "",
                             reinterpret_cast<void*>(ret[0]));
                }
            }
            if (!changedSlots) SHVI_LOG("gta6: no handle slot changed during the call (result %p)", reinterpret_cast<void*>(ret[0]));
        }
        if (!ok) {
            SHVI_LOG("gta6: script_function_call 0x%08X+0x%X faulted", program, offset);
            return SHVI_E_INVALID_ARG;
        }
        return SHVI_OK;
    }

    // ABI 1.7: a table-2 (strong) handle through the engine's own helper, owned by the current script instance.
    int PinScriptObject(std::uint64_t object, std::uint64_t* pin) override {
        if (!g_heaps.imageBase) return SHVI_E_UNSUPPORTED;
        if (!t_tickThread) return SHVI_E_NOT_READY;
        std::uintptr_t instance = 0;
        if (!SehCurrentScriptInstance(EngineTlsBlock(), &instance) || !instance) return SHVI_E_NOT_READY;
        std::lock_guard lock(g_pins.mu);
        std::uint32_t i = 0;
        while (i < ScriptPins::kSlots && g_pins.used[i]) ++i;
        if (i == ScriptPins::kSlots) return SHVI_E_INVALID_ARG;
        ScriptPins::Holder& h = g_pins.holders[i];
        h = {};
        bool ok = SehPinObject(g_heaps.imageBase, &h, object);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32)
            SHVI_LOG("gta6: pin %p -> %s handle %08X instance %u reg %08X (slot %u)", reinterpret_cast<void*>(object),
                     ok ? "ok" : "FAULT", h.w[0], h.w[1], h.w[2], i);
        if (!ok || !h.w[0] || (h.w[0] & 0xF) != 2) {
            if (ok && h.w[0]) SehUnpinObject(g_heaps.imageBase, &h);
            return SHVI_E_INVALID_ARG;
        }
        g_pins.used[i] = true;
        *pin = i + 1;
        return SHVI_OK;
    }
    int UnpinScriptObject(std::uint64_t pin) override {
        if (!g_heaps.imageBase) return SHVI_E_UNSUPPORTED;
        if (!t_tickThread) return SHVI_E_NOT_READY;
        if (pin == 0 || pin > ScriptPins::kSlots) return SHVI_E_INVALID_ARG;
        std::lock_guard lock(g_pins.mu);
        std::uint32_t i = static_cast<std::uint32_t>(pin - 1);
        if (!g_pins.used[i]) return SHVI_E_INVALID_ARG;
        bool ok = SehUnpinObject(g_heaps.imageBase, &g_pins.holders[i]);
        g_pins.used[i] = false;
        g_pins.holders[i] = {};
        if (!ok) SHVI_LOG("gta6: unpin slot %u faulted", i);
        return ok ? SHVI_OK : SHVI_E_INVALID_ARG;
    }

    // ABI 1.3 static_ptr (T2.4).
    void* StaticPtr(std::uint32_t program, std::uint32_t module, std::uint32_t field) override {
        if (!instances_ || !program) return nullptr;
        return reinterpret_cast<void*>(SehStaticPtr(instances_, program, module, field));
    }

private:
    // T2.3: <game>\shvi\natives\<PE timestamp>.json, e.g. the repo's
    // crossmap/gta6_xbox_1.0.2.1.json installed as natives\1794606036.json.
    void LoadNativeTable(const HostInfo& host, const pe::Module& exe) {
        // T7.1: the draw names go to the core's overlay draw list whether or not a native table loads.
        engine_.Bind<void(float, float, float, float, std::uint32_t)>(EngineHash("UI_DRAW_RECT"), &gta6::UiDrawRect);
        engine_.Bind<void(const char*, float, float, float, std::int32_t, std::uint32_t, std::int32_t)>(
            EngineHash("UI_DRAW_TEXT"), &gta6::UiDrawText);
        auto logUi = [] {
            SHVI_LOG("gta6: UI_DISABLE_MENU_CONTROLS %s; UI_DRAW_RECT/UI_DRAW_TEXT bound to the overlay draw list",
                     g_disableControlAction ? "bound to DISABLE_CONTROL_ACTION" : "unbound");
        };
        std::wstring path = host.exeDir + L"\\shvi\\natives\\" + std::to_wstring(build_) + L".json";
        NativeTable t;
        std::string err;
        if (!LoadNativeTableFile(path, &t, &err)) {
            SHVI_LOG("gta6: no native table for build %u (%s)", build_, err.c_str());
            logUi();
            return;
        }
        auto readId = [](std::uintptr_t fn) -> std::optional<std::uint64_t> {
            std::uint64_t id = 0;
            if (!SehHandlerId(fn, &id)) return std::nullopt;
            return id;
        };
        NativeBindReport rep = BindNativeTable(t, build_, exe.image.begin, exe.image.size, readId, engine_);
        SHVI_LOG("gta6: %s", rep.log.c_str());
        NativeTarget dca = engine_.Find(EngineHash("DISABLE_CONTROL_ACTION"));
        if (dca.thunk == &detail::ViNativeThunk && dca.fn) {
            g_disableControlAction = reinterpret_cast<rage::ViNativeHandler>(dca.fn);
            engine_.Bind<void()>(EngineHash("UI_DISABLE_MENU_CONTROLS"), &UiDisableMenuControls);
        }
        logUi();
    }

    // T2.7: one switch, read once. Unknown or missing file = none.
    void ConfigureThreadContext(const HostInfo& host, const scan::Report& r) {
        auto mode = gta6::ParseThreadContextMode(ReadSmallFile(host.exeDir + L"\\shvi\\thread_context.txt"));
        g_vi.threadEngine.enter = reinterpret_cast<gta6::ThreadContextEngine::ScopeEnterFn>(r.Get(target::kScopeEnter));
        g_vi.threadEngine.leave = reinterpret_cast<gta6::ThreadContextEngine::ScopeLeaveFn>(r.Get(target::kScopeLeave));
        if (g_vi.threadEngine.leave) ResolveTlsThunk(r.Get(target::kScopeLeave) + 10);  // push x5, sub rsp: 10 bytes
        SHVI_LOG("gta6: title TLS array: %s", g_tlsArrayThunk ? "through the host's thunk" : "gs:[58h]");
        g_vi.threadEngine.tlsBlock = &EngineTlsBlock;
        if (std::uintptr_t off = r.Get(target::kTlsScriptThread)) g_vi.threadEngine.tlsThreadOffset = off;
        else if (mode == gta6::ThreadContextMode::TlsSwap) mode = gta6::ThreadContextMode::None;
        if (mode == gta6::ThreadContextMode::EngineScope && (!g_vi.threadEngine.enter || !g_vi.threadEngine.leave))
            mode = gta6::ThreadContextMode::None;
        g_vi.threadMode = mode;
        SHVI_LOG("gta6: plugin thread context: %s",
                 mode == gta6::ThreadContextMode::EngineScope ? "engine scope"
                 : mode == gta6::ThreadContextMode::TlsSwap   ? "TLS swap"
                                                              : "none (default; T3.5 decides)");
    }

    // Reads the operand keys back from the image and compares them with the
    // ones vi_script.h was written against; installs the script inventory.
    void ResolveScriptRuntime(const scan::Report& r, const HostInfo& host, const pe::Module& exe) {
        namespace sc = gta6::script;
        std::uintptr_t keyA = r.Get(target::kOperandKeyA), mul = r.Get(target::kOperandMul);
        if (keyA && mul) {
            sc::OperandKeys k{};
            std::memcpy(k.a.data(), reinterpret_cast<const void*>(keyA), 8);
            std::memcpy(k.b.data(), reinterpret_cast<const void*>(keyA + 16), 8);  // adjacent in this build
            k.mul = *reinterpret_cast<const std::uint8_t*>(mul);
            keysMatch_ = k.a == sc::kXboxKeys.a && k.b == sc::kXboxKeys.b && k.mul == sc::kXboxKeys.mul;
        }
        std::uintptr_t link = r.Get(target::kProgramLink);
        // T2.2: the census rides on the link detour; it needs the native base global.
        std::wstring flag = host.exeDir + L"\\shvi\\native_census.on";
        if (r.Get(target::kNativeBase) && keysMatch_ && GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_census.nativeBaseGlobal = r.Get(target::kNativeBase);
            g_census.imageBase = exe.image.begin;
            g_census.imageSize = exe.image.size;
            g_census.out = _wfopen((host.exeDir + L"\\shvi\\native_census.tsv").c_str(), L"w");
            if (g_census.out)
                std::fprintf(g_census.out, "program\tsite\thandler_rva\targc\tretc\thandler_id\n");
        }
        SHVI_LOG("gta6: native census %s", g_census.out ? "on (shvi\\native_census.tsv)" : "off");
        // Script heaps: this build only (fixed RVAs). shvi\\script_heap.txt = auto | 0 | 1 | 2 switches the heap
        // around script_function_call; without the file the heaps are only logged.
        if (build_ == 1794606036u) {
            g_heaps.imageBase = exe.image.begin;
            // Default heap 2: the pooled types the async creators allocate have regions in heaps 1 and 2, and
            // heap 1 crashed the game in run 17; ped and object creation completed under heap 2 (2026-11-26).
            std::string m = ReadSmallFile(host.exeDir + L"\\shvi\\script_heap.txt");
            g_heaps.mode = 2;
            if (m.rfind("auto", 0) == 0) g_heaps.mode = -1;
            else if (m.rfind("off", 0) == 0) g_heaps.mode = -2;
            else if (!m.empty() && m[0] >= '0' && m[0] <= '2') g_heaps.mode = m[0] - '0';
            std::string pm = ReadSmallFile(host.exeDir + L"\\shvi\\pin_task.txt");
            if (!pm.empty() && (pm[0] == '1' || pm[0] == '2')) g_pinMode = pm[0] - '0';
            SHVI_LOG("gta6: script heap for script_function_call: %s", g_heaps.mode == -2 ? "unchanged (log only)"
                                                                       : g_heaps.mode == -1 ? "auto" : "fixed index");
        }
        bool inventory = InstallLinkHook(link);
        bool threadCheck = InstallCallHook(r.Get(target::kScriptCall));
        SHVI_LOG("gta6: thread layout check on script_call %s", threadCheck ? "on" : "off");
        SHVI_LOG("gta6: script runtime: link %p (inventory %s), builtin types %p, native base global %p, "
                 "operand keys %s",
                 reinterpret_cast<void*>(link), inventory ? "on" : "off",
                 reinterpret_cast<void*>(r.Get(target::kBuiltinTypes)),
                 reinterpret_cast<void*>(r.Get(target::kNativeBase)),
                 keyA ? (keysMatch_ ? "match vi_script.h" : "DIFFER from vi_script.h (new build?)") : "not found");
    }

    std::uint32_t build_ = 0;
    bool keysMatch_ = false;
    std::uintptr_t scriptStore_ = 0;
    std::uintptr_t scriptMgrUpdate_ = 0;
    std::uintptr_t modeGlobal_ = 0;
    EntityResolveFn entityResolve_ = nullptr;
    EntityMakeRefFn entityMakeRef_ = nullptr;
    std::uintptr_t poolPeds_ = 0;
    std::uintptr_t poolVehicles_ = 0;
    std::uintptr_t poolObjects_ = 0;
    std::uintptr_t playerPed_ = 0;
    std::uintptr_t versionString_ = 0;
    std::uintptr_t instances_ = 0;
    bool versionLogged_ = false;
    EngineTable engine_;
};

class Gta6PcPlaceholder final : public GameProfile {
public:
    const char* Name() const override { return "GTA VI PC (placeholder)"; }
    bool Matches(const HostInfo&) const override { return false; }  // exe name unknown until release
    std::uint32_t Build() const override { return 0; }
    std::vector<std::wstring_view> AntiCheatModules() const override { return {}; }  // fill from Q0
    std::vector<std::wstring_view> OnlineCommandLineFlags() const override { return {}; }
    // Start from the Xbox set: same engine, same compiler family.
    std::vector<scan::Target> ScanTargets() const override { return XboxTargets(); }
    ResolveResult Resolve(const HostInfo&, const pe::Module&, std::string* log) override {
        if (log) *log = "GTA VI PC profile is a placeholder";
        return ResolveResult::Unsupported;
    }
    NativeTarget FindTarget(std::uint64_t) override { return {}; }
    bool InstallTick(TickFn) override { return false; }
    void RemoveTick() override {}
    GameMode QueryMode(Invoker&) override { return GameMode::Unknown; }  // fail closed
};

}  // namespace

std::vector<std::unique_ptr<GameProfile>> CreateGta6Profiles() {
    std::vector<std::unique_ptr<GameProfile>> v;
    v.push_back(std::make_unique<Gta6Xbox>());
    v.push_back(std::make_unique<Gta6PcPlaceholder>());
    return v;
}

}  // namespace shvi
