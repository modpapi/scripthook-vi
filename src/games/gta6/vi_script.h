// GAME-SPECIFIC: GTA VI. The script runtime as the Xbox build (ts 1794606036)
// shows it statically: instruction codec, the engine's own tail-jump patch,
// type handles and the 102 built-in types, and the program layout. Pure
// functions and plain reads, so all of it is unit-tested without the game.
// Findings and addresses: docs/RESEARCH_PLAN.md, Q4 fourth pass;
// how to use them: docs/SCRIPT_MODDING.md.
//
// The model, in short:
//   * Scripts are compiled from a C# subset (System.Object, String, Delegate,
//     Task/async, generics, reflection, a GC). The engine registers 102
//     built-in types under joaat("System.Xxx") (0x143DECAF0).
//   * A program is a paged resource, version 111. Placement fixes up its
//     pointers (0x143E091C0) and then links it (0x143E09470): imports
//     resolved by name hash, types set up, engine patches applied.
//   * Instructions: u16 opcode, then operand blocks of 8 bytes. The VM
//     dispatches on (u16)(raw * 0x3315) over 530 cases and decodes each
//     operand byte as ((b ^ key[i]) * 0x6D) & 0xFF (0x143DE25A0).
//   * The engine can patch scripts itself (0x143E09610): a kind-1 patch
//     overwrites a function's first 14 bytes with a tail jump (case 505)
//     into another program's code and keeps the 18 original bytes to restore.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace shvi::gta6::script {

// ------------------------------------------------------------ instruction codec
inline constexpr std::uint16_t kXboxOpcodeMul = 0x3315;  // vm_opcode_multiplier
inline constexpr std::uint32_t kCaseCount = 530;
inline constexpr std::uint16_t kCaseCallFar = 133;         // module u8, code offset u32 at op byte 1; pushes a frame
inline constexpr std::uint16_t kCaseCallFarShort = 134;    // module u8, code offset u16 at op byte 1
inline constexpr std::uint16_t kCaseTypeMethodCall = 138;  // engine method of a built-in type (index byte 0, type byte 1)
inline constexpr std::uint16_t kCaseNativeCall = 139;      // handler = native_base + u32, argc byte 4, retc byte 5
inline constexpr std::uint16_t kCaseDelegateInvoke = 140;  // u16 argc, u16 retc
inline constexpr std::uint16_t kCaseCastProgramType = 141; // type check against a program type (i32 + flag byte)
inline constexpr std::uint16_t kCaseCastBuiltinType = 142; // type check against a built-in type (u8 + flag byte)
inline constexpr std::uint16_t kCaseReturn = 144;          // pops the 56-byte frame
inline constexpr std::uint16_t kCaseJump = 156;            // pc = (pc + 2) + i32
inline constexpr std::uint16_t kCaseJumpShort = 157;       // pc = (pc + 2) + i8
inline constexpr std::uint16_t kCaseSwitch = 158;          // u32 count, u32 offset of an i32 table in program +0xF0
inline constexpr std::uint16_t kCaseTailJump = 505;        // pc = program(u64 op A)->code + u32 op B

struct OperandKeys {
    std::array<std::uint8_t, 8> a;  // first block, at pc + 2
    std::array<std::uint8_t, 8> b;  // second block, at pc + 10
    std::uint8_t mul;               // per-byte multiplier (odd)
};
// xmmword_146178EE0 / 146178EF0, multiplier word_14615957C, Xbox ts 1794606036.
inline constexpr OperandKeys kXboxKeys{{0x85, 0x94, 0xA7, 0xB6, 0xC1, 0xD0, 0xE3, 0xF2},
                                       {0x0D, 0x1C, 0x2F, 0x3E, 0x49, 0x58, 0x6B, 0x7A},
                                       0x6D};

constexpr std::uint16_t OpcodeCase(std::uint16_t raw, std::uint16_t mul) {
    return static_cast<std::uint16_t>(raw * mul);
}

// Inverse of an odd number modulo 2^bits (Newton: x = x * (2 - m * x)).
constexpr std::uint32_t InverseModPow2(std::uint32_t odd) {
    std::uint32_t x = odd;  // correct to 3 bits for any odd number
    for (int i = 0; i < 5; ++i) x *= 2u - odd * x;
    return x;
}
constexpr std::uint8_t InverseMod256(std::uint8_t odd) { return static_cast<std::uint8_t>(InverseModPow2(odd)); }

constexpr std::uint16_t RawOpcode(std::uint16_t caseId, std::uint16_t mul) {
    return static_cast<std::uint16_t>(caseId * static_cast<std::uint16_t>(InverseModPow2(mul)));
}

constexpr std::uint64_t DecodeBlock(std::uint64_t raw, const std::array<std::uint8_t, 8>& key, std::uint8_t mul) {
    std::uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        auto b = static_cast<std::uint8_t>(raw >> (8 * i)) ^ key[static_cast<std::size_t>(i)];
        out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b * mul)) << (8 * i);
    }
    return out;
}

constexpr std::uint64_t EncodeBlock(std::uint64_t value, const std::array<std::uint8_t, 8>& key, std::uint8_t mul) {
    const std::uint8_t inv = InverseMod256(mul);
    std::uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        auto b = static_cast<std::uint8_t>(static_cast<std::uint8_t>(value >> (8 * i)) * inv) ^
                 key[static_cast<std::size_t>(i)];
        out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b)) << (8 * i);
    }
    return out;
}

// ------------------------------------------------------ instruction lengths
// Length in bytes (opcode included) and control-flow kind of every case.
// Derived from the VM's pseudocode (analysis/gta6_xbox/t11b_lengths.py over
// q17_vm.c): path-sensitive tracking of the pc from case entry to the next
// dispatch, cross-checked against the operand bytes each case reads (no case
// reads past its length). 14 cases whose advance is not on a straight path
// (calls, returns, jumps, switch, tail jump) are taken from the frame push or
// the operand layout (t11_gen.py, MANUAL). Xbox ts 1794606036.
//
// Branch and Jump targets are relative to the operand start:
// target = pc + 2 + imm (i32, or i8 for cases 157/160/163). A Call continues at
// pc + length when the callee returns. Native (139) also continues at
// pc + length unless the handler stops the thread (state byte != 1), in which
// case the VM leaves with the frame pc on the native opcode.
enum class CaseKind : std::uint8_t { Seq, Branch, Jump, Switch, Call, Return, Native, TailJump };

namespace detail {
inline constexpr CaseKind S = CaseKind::Seq, B = CaseKind::Branch, J = CaseKind::Jump, W = CaseKind::Switch,
                          C = CaseKind::Call, R = CaseKind::Return, N = CaseKind::Native, T = CaseKind::TailJump;
// generated by analysis/gta6_xbox/t11_gen.py
inline constexpr std::array<std::uint8_t, kCaseCount> kCaseLength{{
     2,  3,  2,  2,  2,  2,  2,  4,  4,  3,  4,  3,  3,  2,  2,  2,  // 0
     2,  2,  2,  2,  4,  4,  3,  3,  2,  2,  2,  2,  2,  2,  2,  4,  // 16
     3,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  3,  6,  6,  // 32
    10,  6, 18,  3,  6,  2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  // 48
     4,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  5,  // 64
     3,  3,  3,  3,  3,  3,  3,  3,  4,  3,  4,  4,  4,  4,  4,  4,  // 80
     5,  4,  3,  3,  3,  3,  3,  3,  4,  4,  4,  3,  3,  3,  3,  3,  // 96
     4,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  5,  7,  7,  // 112
     2,  3,  3,  3,  4,  7,  5,  8,  6,  5,  4,  8,  6,  7,  4,  3,  // 128
     2,  2,  3,  3,  2,  3,  6,  7,  2,  6,  2,  2,  6,  3, 10,  6,  // 144
     3,  6,  6,  3,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  // 160
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  // 176
     6,  6,  6,  6,  6,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 192
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 208
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  4, 10, 18,  2,  2,  2,  // 224
     2,  2,  2,  4,  2,  2,  2,  2,  2,  3,  2,  2,  2,  2,  2,  2,  // 240
     2,  2,  2,  3,  3,  3,  3,  3,  3,  2,  3,  3,  3,  3,  3,  3,  // 256
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 272
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 288
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 304
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 320
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  // 336
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  4,  // 352
     4,  4,  3,  6,  2,  4,  7,  6,  3,  4,  4,  6,  2,  2,  2,  2,  // 368
     2,  2,  2,  2,  2,  2,  2,  2,  3,  4,  6,  4,  4,  6,  6,  9,  // 384
     8,  4,  5,  4,  4,  4,  4,  4,  4,  6,  4,  6,  6,  6,  6,  6,  // 400
     6,  8,  4,  4,  4,  4,  4,  4,  6,  4,  7,  7,  7,  7,  7,  7,  // 416
     9,  7,  4,  4,  4,  4,  4,  6,  4,  4,  4,  4,  4,  6,  7,  7,  // 432
     7,  7,  9,  7,  7,  7,  7,  7,  7,  9,  7,  9,  2,  6,  6,  6,  // 448
     6,  4,  4,  2,  3,  2,  3,  3,  3,  2,  2,  2,  2,  2,  2,  2,  // 464
     2,  2,  2,  2,  2,  2,  2,  2,  2, 14,  3,  3,  3,  3,  3,  3,  // 480
     3,  3,  3,  3,  3,  7,  7,  4,  7, 14,  2,  7,  7,  8,  8,  4,  // 496
     4,  4,  4,  2,  6,  8,  7,  7,  6,  2,  6,  6,  2,  2,  2,  6,  // 512
     6,  7,  // 528
}};

inline constexpr std::array<CaseKind, kCaseCount> kCaseKind{{
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 0
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 16
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 32
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 48
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 64
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 80
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 96
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 112
    S, S, S, S, S, C, C, C, C, C, C, N, C, S, S, S,  // 128
    R, R, R, S, S, S, S, S, S, S, S, S, J, J, W, B,  // 144
    B, B, B, B, B, B, B, B, B, B, B, B, B, B, B, B,  // 160
    B, B, B, B, B, B, B, B, B, B, B, B, B, B, B, B,  // 176
    B, B, B, B, B, S, S, S, S, S, S, S, S, S, S, S,  // 192
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 208
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 224
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 240
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 256
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 272
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 288
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 304
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 320
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 336
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 352
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 368
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, C,  // 384
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 400
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 416
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 432
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 448
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 464
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 480
    S, S, S, S, S, S, S, S, S, T, S, S, S, S, S, S,  // 496
    S, S, S, S, S, S, S, S, S, S, S, S, S, S, S, S,  // 512
    S, S,  // 528
}};
}  // namespace detail

inline constexpr const auto& kCaseLength = detail::kCaseLength;
inline constexpr const auto& kCaseKind = detail::kCaseKind;
inline constexpr std::size_t kMaxInstructionLength = 18;

// Width of the relative offset of a Branch/Jump case (0 for other kinds).
constexpr std::uint8_t BranchImmBytes(std::uint16_t caseId) {
    if (caseId >= kCaseCount) return 0;
    CaseKind k = kCaseKind[caseId];
    if (k != CaseKind::Branch && k != CaseKind::Jump) return 0;
    return (caseId == 157 || caseId == 160 || caseId == 163) ? 1 : 4;
}

// Does execution continue at pc + length (always, or on one path)?
constexpr bool FallsThrough(CaseKind k) {
    return k == CaseKind::Seq || k == CaseKind::Branch || k == CaseKind::Call || k == CaseKind::Native ||
           k == CaseKind::Switch;  // Switch: the default target is pc + length
}

// One decoded instruction at `offset` in a program's code.
struct Instruction {
    std::uint32_t offset;
    std::uint16_t caseId;
    std::uint8_t length;
    CaseKind kind;
    std::uint64_t a;  // operand block A (decoded), bytes past the instruction zeroed
    std::uint64_t b;  // operand block B (decoded), bytes past the instruction zeroed
    // Branch/Jump target (offset in the same code), when kind is Branch or Jump.
    std::optional<std::int64_t> Target() const {
        std::uint8_t w = BranchImmBytes(caseId);
        if (w == 0) return std::nullopt;
        std::int64_t imm = (w == 1) ? static_cast<std::int8_t>(a) : static_cast<std::int32_t>(a);
        return static_cast<std::int64_t>(offset) + 2 + imm;
    }
};

// Decodes the instruction at code[offset]. `obfuscated` is the program flag
// (ProgramView::Obfuscated): clear means raw case numbers and plain operands,
// as the engine's patcher writes them. nullopt when the opcode is out of range
// or the instruction runs past `size`.
inline std::optional<Instruction> DecodeAt(const std::uint8_t* code, std::size_t size, std::uint32_t offset,
                                           bool obfuscated, const OperandKeys& k, std::uint16_t opcodeMul) {
    if (offset > size || size - offset < 2) return std::nullopt;
    std::uint16_t raw;
    std::memcpy(&raw, code + offset, 2);
    std::uint16_t c = obfuscated ? OpcodeCase(raw, opcodeMul) : raw;
    if (c >= kCaseCount) return std::nullopt;
    std::uint8_t len = kCaseLength[c];
    if (size - offset < len) return std::nullopt;
    std::uint8_t opb[16] = {};
    std::memcpy(opb, code + offset + 2, len - 2u);
    std::uint64_t a, b;
    std::memcpy(&a, opb, 8);
    std::memcpy(&b, opb + 8, 8);
    if (obfuscated) {
        a = DecodeBlock(a, k.a, k.mul);
        b = DecodeBlock(b, k.b, k.mul);
    }
    std::size_t n = len - 2u;  // operand bytes that belong to this instruction
    if (n < 8) a &= (n == 0) ? 0 : (~0ull >> (64 - 8 * n));
    if (n <= 8) b = 0;
    else if (n < 16) b &= ~0ull >> (64 - 8 * (n - 8));
    return Instruction{offset, c, len, kCaseKind[c], a, b};
}

// ------------------------------------------------------------- native ids
// Every native handler (6,805 on the Xbox build, T1.5) opens with
//     cmp dword ptr [rcx+10h], 0     ; ctx->argc
//     js  <cold>                     ; cold: mov rax,[rcx] ; mov r64, imm64 ; mov [rax], r64 ; jmp back
// so with argc < 0 it writes a 64-bit id into args[0]. The ids are unique,
// occur nowhere else in the exe and are not GTA V native hashes. They are a
// per-native key read straight from the handler, independent of its address.
// Whether they are stable across builds is checked on the PC build (TASKS T3.2).
// Returns the id, or nullopt if `fn` does not start like a handler.
// `avail` is how many bytes from `fn` may be read.
inline std::optional<std::uint64_t> HandlerId(const std::uint8_t* fn, std::size_t avail) {
    constexpr std::size_t kScan = 24;  // the cmp sits after at most a few pushes / sub rsp
    for (std::size_t i = 0; i + 4 <= avail && i < kScan; ++i) {
        if (!(fn[i] == 0x83 && fn[i + 1] == 0x79 && fn[i + 2] == 0x10 && fn[i + 3] == 0x00)) continue;
        // a js within the next few instructions (a register move may sit in between)
        for (std::size_t j = i + 4; j < i + 4 + 8 && j + 2 <= avail; ++j) {
            std::size_t target;
            if (fn[j] == 0x78) {
                target = j + 2 + static_cast<std::size_t>(static_cast<std::int8_t>(fn[j + 1]));
            } else if (fn[j] == 0x0F && j + 6 <= avail && fn[j + 1] == 0x88) {
                std::int32_t rel;
                std::memcpy(&rel, fn + j + 2, 4);
                if (rel < 0) continue;  // the cold block follows the body
                target = j + 6 + static_cast<std::size_t>(rel);
            } else {
                continue;
            }
            // cold block: mov r64,[rcx] (3 bytes) then mov r64, imm64 (REX.W B8+r)
            for (std::size_t k = target; k < target + 8 && k + 10 <= avail; ++k) {
                if ((fn[k] == 0x48 || fn[k] == 0x49) && fn[k + 1] >= 0xB8 && fn[k + 1] <= 0xBF) {
                    std::uint64_t id;
                    std::memcpy(&id, fn + k + 2, 8);
                    return id;
                }
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// --------------------------------------------------------------- tail jump
inline constexpr std::size_t kTailJumpSize = 14;   // bytes the patcher writes
inline constexpr std::size_t kTailJumpSaved = 18;  // bytes it saves (and restores on unload)

// What 0x143E09AB0 writes at the start of a patched function. `obfuscated` is
// the target program's flag (byte +0x111 bit 0): clear means raw opcode 505
// and plain operands.
inline std::array<std::uint8_t, kTailJumpSize> EncodeTailJump(std::uint64_t program, std::uint32_t codeOffset,
                                                              bool obfuscated, const OperandKeys& k,
                                                              std::uint16_t opcodeMul) {
    std::array<std::uint8_t, kTailJumpSize> out{};
    std::uint16_t op = obfuscated ? RawOpcode(kCaseTailJump, opcodeMul) : kCaseTailJump;
    std::uint64_t a = obfuscated ? EncodeBlock(program, k.a, k.mul) : program;
    std::uint64_t b = obfuscated ? EncodeBlock(codeOffset, k.b, k.mul) : codeOffset;
    std::memcpy(out.data(), &op, 2);
    std::memcpy(out.data() + 2, &a, 8);
    std::memcpy(out.data() + 10, &b, 4);
    return out;
}

struct TailJump {
    std::uint64_t program;
    std::uint32_t codeOffset;
};

inline std::optional<TailJump> DecodeTailJump(const std::uint8_t* p, bool obfuscated, const OperandKeys& k,
                                              std::uint16_t opcodeMul) {
    std::uint16_t op;
    std::uint64_t a = 0, b = 0;
    std::memcpy(&op, p, 2);
    std::memcpy(&a, p + 2, 8);
    std::memcpy(&b, p + 10, 4);
    if (obfuscated) {
        if (OpcodeCase(op, opcodeMul) != kCaseTailJump) return std::nullopt;
        a = DecodeBlock(a, k.a, k.mul);
        b = DecodeBlock(b, k.b, k.mul);  // upper 4 key bytes decode the zero padding; only 4 are used
    } else if (op != kCaseTailJump) {
        return std::nullopt;
    }
    return TailJump{a, static_cast<std::uint32_t>(b)};
}

// ------------------------------------------------------------- type handles
// Bit 31 set: an engine type; then bit 30 set means a dynamic type
// (*(xmmword_1498C4840+8))[h & 0x3FFFFFFF], else built-in kBuiltinTypes[h & 0x3FFFFFFF].
// Bit 31 clear: a type of some program, found through the import table.
enum class TypeKind { Builtin, Dynamic, ProgramLocal };
constexpr TypeKind Classify(std::uint32_t h) {
    if ((h & 0x80000000u) == 0) return TypeKind::ProgramLocal;
    return (h & 0x40000000u) ? TypeKind::Dynamic : TypeKind::Builtin;
}

struct BuiltinType {
    std::uint32_t fullNameHash;  // atStringHash of the full name, as registered
    const char* name;            // nullptr: not named yet (match fullNameHash against candidates)
};

// Indexed by handle & 0x3FFFFFFF. Names recovered by hashing .NET BCL names
// (TASKS T1.12): every name below hashes to its fullNameHash. Index 73
// (0x64DE629C, between System.Nullable`1 and Dictionary`2) is still unknown.
// Nested types use '+' (Dictionary`2+Entry). Two awaiter names are the
// engine's own: ConfiguredValueTaskAwaitable`1+ConfiguredTaskAwaiter, not .NET's
// ConfiguredValueTaskAwaiter. The table at 0x14ABC3CB0 holds 136-byte records.
inline constexpr std::array<BuiltinType, 102> kBuiltinTypes{{
    {0x06C25483u, "System.Object"},
    {0xA1AD8BC5u, "System.ValueType"},
    {0xCCDA825Fu, "System.Delegate"},
    {0xE2BF88D1u, "System.MulticastDelegate"},
    {0x042B8172u, "System.Reflection.MemberInfo"},
    {0x2191C299u, "System.Type"},
    {0x819C32AAu, "System.String"},
    {0xB806C3CDu, "System.SByte"},
    {0x9F18AEFEu, "System.Byte"},
    {0xF061D33Bu, "System.Int16"},
    {0x2B71AA3Bu, "System.UInt16"},
    {0xE528BF11u, "System.Int32"},
    {0xD48C0C19u, "System.UInt32"},
    {0xBB6E6905u, "System.Int64"},
    {0x1ADE9885u, "System.UInt64"},
    {0xC94FA866u, "System.Single"},
    {0x17A68C63u, "System.Double"},
    {0x7EDC69DBu, "System.Boolean"},
    {0x6F1B8D45u, "System.Char"},
    {0xA7204A6Cu, "System.Void"},
    {0xE4BF15E8u, "System.Array"},
    {0x4AE55570u, "System.Guid"},
    {0x14EE65ABu, "System.DateTime"},
    {0x989C616Du, "System.TimeSpan"},
    {0x214796A4u, "System.Activator"},
    {0x0D08E906u, "System.Reflection.MethodBase"},
    {0x2096CF91u, "System.Reflection.MethodInfo"},
    {0x5560E057u, "System.Reflection.FieldInfo"},
    {0xE6432DA2u, "System.Reflection.EventInfo"},
    {0xFB57FA49u, "System.Reflection.PropertyInfo"},
    {0x0310F4E8u, "System.Reflection.ParameterInfo"},
    {0x39DB7912u, "System.Collections.BitArray"},
    {0x19935804u, "System.Collections.Comparer"},
    {0x2BE3B1BFu, "System.Collections.DictionaryEntry"},
    {0xC6F3823Eu, "System.Collections.ArrayList"},
    {0x54476194u, "System.EventArgs"},
    {0xAC53F693u, "System.Convert"},
    {0x103EF860u, "System.Random"},
    {0x4211659Eu, "System.Attribute"},
    {0x9832D8FCu, "System.FlagsAttribute"},
    {0xA8FEBE2Cu, "System.Diagnostics.DebuggerDisplayAttribute"},
    {0xDE7F6300u, "System.Enum"},
    {0x413A4B49u, "System.Reflection.MemberTypes"},
    {0xB2FC53AFu, "System.Reflection.BindingFlags"},
    {0x0F8222C5u, "System.WeakReference"},
    {0x46E59347u, "System.GC"},
    {0x478E07D0u, "System.Threading.Tasks.Task"},
    {0xFFAA3A74u, "System.Threading.Tasks.Task`1"},
    {0x720BA976u, "System.Threading.Tasks.ValueTask`1"},
    {0xB512AFF4u, "System.Runtime.CompilerServices.IAsyncStateMachine"},
    {0x9B0CCC9Cu, "System.Runtime.CompilerServices.AsyncTaskMethodBuilder`1"},
    {0x0B3211F7u, "System.Runtime.CompilerServices.AsyncValueTaskMethodBuilder`1"},
    {0x040AED03u, "System.Runtime.CompilerServices.TaskAwaiter`1"},
    {0x5867285Du, "System.Runtime.CompilerServices.ConfiguredTaskAwaitable`1"},
    {0x0A5C5089u, "System.Runtime.CompilerServices.ConfiguredTaskAwaitable`1+ConfiguredTaskAwaiter"},
    {0x046F4806u, "System.Runtime.CompilerServices.ValueTaskAwaiter`1"},
    {0xA8AD2065u, "System.Runtime.CompilerServices.ConfiguredValueTaskAwaitable`1+ConfiguredTaskAwaiter"},
    {0x067E05ABu, "System.Runtime.CompilerServices.ConfiguredValueTaskAwaitable`1"},
    {0xB64D44B4u, "System.Threading.Tasks.TaskCompletionSource`1"},
    {0xA00AE351u, "System.Threading.CancellationToken"},
    {0x9A14255Du, "System.Threading.CancellationTokenSource"},
    {0xAB0B2A86u, "System.Collections.Generic.KeyValuePair`2"},
    {0x35CB79DAu, "System.ValueTuple`1"},
    {0x48051E4Du, "System.ValueTuple`2"},
    {0x52773331u, "System.ValueTuple`3"},
    {0x64B7D7B2u, "System.ValueTuple`4"},
    {0x74B677AFu, "System.ValueTuple`5"},
    {0x7F0A8C5Bu, "System.ValueTuple`6"},
    {0x8950A0E7u, "System.ValueTuple`7"},
    {0x9B93456Cu, "System.ValueTuple`8"},
    {0x373F5BBFu, "System.Span`1"},
    {0xEDFAA2A5u, "System.Rage"},
    {0xBA36382Fu, "System.Nullable`1"},
    {0x64DE629Cu, nullptr},
    {0xBD682A19u, "System.Collections.Generic.Dictionary`2"},
    {0xB76D8278u, "System.Collections.Generic.Dictionary`2+Entry"},
    {0x9B89108Fu, "System.Collections.Generic.Dictionary`2+Enumerator"},
    {0xB0C3600Eu, "System.Collections.Generic.Dictionary`2+KeyCollection"},
    {0x0CCA0070u, "System.Collections.Generic.Dictionary`2+KeyCollection+Enumerator"},
    {0xE458D83Fu, "System.Collections.Generic.Dictionary`2+ValueCollection"},
    {0x884A835Au, "System.Collections.Generic.Dictionary`2+ValueCollection+Enumerator"},
    {0xD837D23Au, "System.IDisposable"},
    {0x5E55DFFAu, "System.IComparable"},
    {0x2F091095u, "System.Collections.IEnumerable"},
    {0xA7DC029Cu, "System.Collections.IEnumerator"},
    {0x5F12DD8Eu, "System.Collections.IComparer"},
    {0x64398993u, "System.Collections.IEqualityComparer"},
    {0xC2DCD17Au, "System.Collections.ICollection"},
    {0x3A6CF10Cu, "System.Collections.IDictionary"},
    {0x6E45D8A3u, "System.Collections.IDictionaryEnumerator"},
    {0x7EB19F8Cu, "System.Collections.IList"},
    {0x3560E5FFu, "System.Collections.Generic.IEnumerable`1"},
    {0x9A94BA4Cu, "System.Collections.Generic.IEnumerator`1"},
    {0xD7682DE6u, "System.Collections.Generic.ICollection`1"},
    {0x3C33AF5Au, "System.Collections.Generic.IList`1"},
    {0x754D69A3u, "System.Collections.Generic.IReadOnlyCollection`1"},
    {0xBF03703Du, "System.Collections.Generic.IReadOnlyList`1"},
    {0x61C6D605u, "System.Collections.Generic.IDictionary`2"},
    {0x3D803FFCu, "System.Collections.Generic.IReadOnlyDictionary`2"},
    {0xAEC033EEu, "System.StringSplitOptions"},
    {0x04F9AC0Bu, "System.Threading.Tasks.TaskStatus"},
    {0xB6EE1733u, "System.DayOfWeek"},
}};

inline const char* BuiltinTypeName(std::uint32_t handle) {
    if (Classify(handle) != TypeKind::Builtin) return nullptr;
    std::uint32_t i = handle & 0x3FFFFFFFu;
    return i < kBuiltinTypes.size() ? kBuiltinTypes[i].name : nullptr;
}

// Built-in type record (136 bytes), as 0x143DF23D0 fills it.
namespace type {
inline constexpr std::size_t kStride = 136;
inline constexpr std::size_t kShortHash = 0x10;     // u64 (hash of the short name, function unknown)
inline constexpr std::size_t kFullNameHash = 0x18;  // u32 atStringHash("Namespace.Name")
inline constexpr std::size_t kHandle = 0x1C;        // u32
inline constexpr std::size_t kBase = 0x20;          // record* of the base type
inline constexpr std::size_t kMethods = 0x28;       // 32-byte method descriptors
inline constexpr std::size_t kMethodCount = 0x30;   // u32
inline constexpr std::size_t kFlags = 0x6C;         // u32; negative = forwarded
}  // namespace type

// ------------------------------------------------------------ program layout
// Offsets into a placed program (Xbox ts 1794606036; from 0x143E091C0,
// 0x143E09470, the VM and the patch code). Unverified at runtime.
namespace program {
inline constexpr std::size_t kNameHash = 0x10;              // u32, the key ScriptStore lookups use
inline constexpr std::size_t kRefCount = 0x14;              // u32
inline constexpr std::size_t kResourceVersionField = 0x18;  // u16
inline constexpr std::uint16_t kResourceVersion = 111;
inline constexpr std::size_t kNativeKey = 0x24;             // u32; base = anchor - (hash ^ ror32(this, 12))
inline constexpr std::size_t kTypeCount = 0x2C;             // u32
inline constexpr std::size_t kTypes = 0x30;                 // type records, kStride each
inline constexpr std::size_t kCodeBase = 0xE8;              // u8* bytecode; frames hold pc = code + offset
inline constexpr std::size_t kDataBase = 0xF0;              // u8* constants/strings
inline constexpr std::size_t kFlags = 0x110;                // byte +0x111 bit 0: operands obfuscated
inline constexpr std::size_t kPatchTableOffset = 0x134;     // u32 offset into data: this program's patches
inline constexpr std::size_t kPatchCount = 0x138;           // u32, 16-byte entries {kind, target hash, a, b}
inline constexpr std::size_t kImportCount = 0x144;          // u32 (entry 0 is the program itself)
inline constexpr std::size_t kImports = 0x148;              // import* (import:: below), kImportCount entries
}  // namespace program

// Import entry (24 bytes), TASKS T1.11. Confirmed by the link function
// 0x143E09470 and the static opcodes 90-97 / 121-125 / 426-433 / 446-450 / 501-502.
//   * Entry 0 is the program itself: link writes {this, static offset 0,
//     first type 0, type count = program +0x2C}.
//   * Entries 1..count-1 name an imported program by its name hash (+0x08);
//     link resolves each through ScriptStore (vtable slot 3) into +0x00.
//     They are sorted by that hash: the VM binary-searches them.
//   * +0x0C is where that module's statics start inside a thread's one
//     static block (thread +0x18). The compiler fixes it; link leaves it.
// A static access in program P, operand (import index i, field f), reads
//   thread.statics + E.staticOffset + f
// where M = P.imports[i].program and E = the ROOT program's (thread +0x20)
// entry for M: its entry 0 when M is the root, else the entry with
// nameHash == M.nameHash. Field f is a u8 (cases 90-97) or a u32 (433, 450).
namespace import {
inline constexpr std::size_t kStride = 24;
inline constexpr std::size_t kProgram = 0x00;       // program*
inline constexpr std::size_t kNameHash = 0x08;      // u32, the imported program's +0x10
inline constexpr std::size_t kStaticOffset = 0x0C;  // u32, into the thread's static block
inline constexpr std::size_t kFirstType = 0x10;     // u32, first type handle this module contributes
inline constexpr std::size_t kTypeCount = 0x14;     // u32
}  // namespace import

// Script thread (TASKS T1.2). Base constructor 0x141C180C0(this, owner, stack,
// stackSlots, locals, localSlots, frames, frameCount) sets every field below;
// the VM (0x143DE2550), script_call (0x143DE1F10, virtual slot 17), the frame
// push (0x143DE2390) and the reset (slot 10, 0x143DE1CF0) use them.
// Base vtable 0x14654FD20. Story threads (vtable 0x146560430, made by
// 0x141C17F00) are 0x3020 bytes with the three areas inline; a second kind
// (vtable 0x14654FDE0, 0x1458D0020) allocates them with sizes from globals.
namespace thread {
inline constexpr std::size_t kVtable = 0x00;
inline constexpr std::size_t kOwner = 0x08;         // script instance: +0x348 id, +0x380 program, +0x388 this thread
inline constexpr std::size_t kState = 0x10;         // u8, 1 = running, 2 = returned (script_call) / active (scope)
inline constexpr std::size_t kScopeDepth = 0x11;    // u8, nested run-as-script-thread scopes (0x140066080)
inline constexpr std::size_t kRunId = 0x14;         // u32, set when the first scope opens (counter dword_1498C1934)
inline constexpr std::size_t kStatics = 0x18;       // u8* one static block for all linked modules
inline constexpr std::size_t kRootProgram = 0x20;   // program*
inline constexpr std::size_t kStackBegin = 0x28;    // storage the stack area was given
inline constexpr std::size_t kLocalsBegin = 0x30;
inline constexpr std::size_t kFramesBegin = 0x38;
inline constexpr std::size_t kFramesBase = 0x40;    // frames[0]; an empty thread has kFrame = base - 56
inline constexpr std::size_t kFrame = 0x48;         // frame* current
inline constexpr std::size_t kFrameBottom = 0x50;   // a return to this frame ends the run
inline constexpr std::size_t kFramesEnd = 0x58;     // a push past it is __debugbreak
inline constexpr std::size_t kCallDepth = 0x60;     // u32, nested script_call / engine calls
inline constexpr std::size_t kLocalsBase = 0x68;
inline constexpr std::size_t kLocalsTop = 0x70;     // each frame takes 32+ bytes here
inline constexpr std::size_t kLocalsEnd = 0x78;
inline constexpr std::size_t kStackBase = 0x80;
inline constexpr std::size_t kStackEnd = 0x88;
inline constexpr std::size_t kStackTop = 0x90;      // u64* 8-byte slots
inline constexpr std::size_t kCommitCount = 0x98;   // u8
inline constexpr std::size_t kBaseSize = 0xA0;
inline constexpr std::size_t kFrameSize = 56;
inline constexpr std::uint8_t kStateRunning = 1;
inline constexpr std::uint8_t kStateReturned = 2;
inline constexpr std::size_t kVirtualScriptCall = 17;  // vtable slot of script_call
// Story thread: inline areas, as 0x141C17F00 lays them out.
inline constexpr std::size_t kStoryStackSlots = 0x80, kStoryLocalSlots = 0x500, kStoryFrames = 16;
inline constexpr std::size_t kStoryStack = kBaseSize;
inline constexpr std::size_t kStoryLocals = kStoryStack + 8 * kStoryStackSlots;
inline constexpr std::size_t kStoryFramesArea = kStoryLocals + 8 * kStoryLocalSlots;
inline constexpr std::size_t kStorySize = kStoryFramesArea + kFrameSize * kStoryFrames;
static_assert(kStoryStack == 20 * 8 && kStoryLocals == 148 * 8 && kStoryFramesArea == 1428 * 8,
              "0x141C17F00 passes this+20, this+148 and this+1428 qwords");
static_assert(kStorySize == 0x3020, "the story thread's deleting destructor frees 0x3020 bytes");
}  // namespace thread

// Call frame (56 bytes), as the frame push 0x143DE2390 fills it.
namespace frame {
inline constexpr std::size_t kPc = 0x00;          // u8* = program->code + offset
inline constexpr std::size_t kProgram = 0x08;     // program*
inline constexpr std::size_t kArgs = 0x10;        // u64* operand stack at entry: the arguments (cases 1-11)
inline constexpr std::size_t kLocals = 0x18;      // u64* locals in the local area (cases 12-32); 0 on engine frames
inline constexpr std::size_t kSavedLocalsTop = 0x20;
inline constexpr std::size_t kDelegateIndex = 0x2C;  // u32, case 140
inline constexpr std::size_t kDelegate = 0x30;       // object*, case 140
}  // namespace frame

// Script instance (912 bytes, vtable 0x14655EFE0, built by 0x140066170 and
// 0x141C17F00). One per OS thread and slot: script_instance_get
// (0x144EAC510(program, slot)) keeps it in the engine TLS block at
// +0xF80 + 8*slot and, when it creates one, appends that TLS slot's address
// to the global vector script_instances (TASKS T1.3).
namespace instance {
inline constexpr std::size_t kSize = 0x390;
inline constexpr std::size_t kGetThreadSlot = 12;  // virtual: returns +0x388
inline constexpr std::size_t kId = 0x348;          // u32 from dword_146C6A19C, copied to TLS +0xF38 by script_call
inline constexpr std::size_t kSlot = 0x378;        // u32, the TLS slot index it was made for
inline constexpr std::size_t kProgram = 0x380;     // program* it runs
inline constexpr std::size_t kThread = 0x388;      // the story thread (thread::kStorySize bytes)
inline constexpr std::size_t kTlsSlots = 0xF80;    // engine TLS block: instance* per slot
inline constexpr std::size_t kSlotKinds = 6;       // kind 0..5 -> slot via dword_146560818
}  // namespace instance

// The global vector of instance slots (script_instances), guarded by the
// reentrant lock qword_1498FADA8.
namespace instance_list {
inline constexpr std::size_t kCapacity = 0x0;  // u32
inline constexpr std::size_t kCount = 0x4;     // u32
inline constexpr std::size_t kItems = 0x8;     // instance** [count]: each item is a TLS slot address
}  // namespace instance_list

// The engine's run-as-script-thread scope: script_scope_enter (0x140066080)
// saves TLS +0x1478 and installs the instance's thread (marking it active),
// script_scope_leave (0x140065FD0) undoes it. 36 engine call sites use it.
// It is VI's changeScriptThread (TASKS T2.7). The caller owns the scope.
namespace scope {
inline constexpr std::size_t kTimestamp = 0x00;   // u64
inline constexpr std::size_t kInstance = 0x08;
inline constexpr std::size_t kPrevThread = 0x10;  // TLS +0x1478 before enter
inline constexpr std::size_t kThread = 0x18;
inline constexpr std::size_t kPrevState = 0x20;   // u8
inline constexpr std::size_t kPrevActive = 0x21;  // u8
inline constexpr std::size_t kBufferSize = 0x80;  // at least what 0x141C17F00 reserves on its stack
}  // namespace scope

// Calls `fn(instance, thread)` for every live instance in the vector at
// `list` (the script_instances global). Reads only; caller holds SEH.
template <class Fn>
std::size_t ForEachInstance(std::uintptr_t list, Fn&& fn, std::size_t maxCount = 4096) {
    auto at = [](std::uintptr_t a) { std::uintptr_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; };
    std::uint32_t count, cap;
    std::memcpy(&count, reinterpret_cast<const void*>(list + instance_list::kCount), 4);
    std::memcpy(&cap, reinterpret_cast<const void*>(list + instance_list::kCapacity), 4);
    if (count > cap || count > maxCount) return 0;
    std::uintptr_t items = at(list + instance_list::kItems);
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t slot = at(items + 8u * i);
        std::uintptr_t inst = slot ? at(slot) : 0;
        if (!inst) continue;
        fn(inst, at(inst + instance::kThread));
        ++n;
    }
    return n;
}

// Read-only view over a script thread. Same SEH rule as ProgramView.
class ThreadView {
public:
    explicit ThreadView(std::uintptr_t t) : t_(t) {}
    std::uint8_t State() const { return Read<std::uint8_t>(thread::kState); }
    std::uintptr_t Owner() const { return Read<std::uintptr_t>(thread::kOwner); }
    std::uintptr_t Statics() const { return Read<std::uintptr_t>(thread::kStatics); }
    std::uintptr_t RootProgram() const { return Read<std::uintptr_t>(thread::kRootProgram); }
    std::uintptr_t Frame() const { return Read<std::uintptr_t>(thread::kFrame); }
    std::uint32_t CallDepth() const { return Read<std::uint32_t>(thread::kCallDepth); }
    // Frames in use (0 when idle).
    std::size_t FrameCount() const {
        std::uintptr_t base = Read<std::uintptr_t>(thread::kFramesBase), cur = Frame();
        return cur + thread::kFrameSize < base ? 0 : (cur + thread::kFrameSize - base) / thread::kFrameSize;
    }
    // The ordering every thread keeps between its pointers (layout validator).
    bool LooksValid() const {
        auto p = [&](std::size_t o) { return Read<std::uintptr_t>(o); };
        std::uintptr_t fb = p(thread::kFramesBase), cur = p(thread::kFrame), fe = p(thread::kFramesEnd);
        std::uintptr_t lb = p(thread::kLocalsBase), lt = p(thread::kLocalsTop), le = p(thread::kLocalsEnd);
        std::uintptr_t sb = p(thread::kStackBase), st = p(thread::kStackTop), se = p(thread::kStackEnd);
        if (!fb || !lb || !sb) return false;
        if (!(lb <= lt && lt <= le && sb <= st && st <= se)) return false;
        if (cur + thread::kFrameSize < fb || cur + thread::kFrameSize > fe) return false;
        if ((cur + thread::kFrameSize - fb) % thread::kFrameSize) return false;
        if ((fe - fb) % thread::kFrameSize || (le - lb) % 8 || (se - sb) % 8) return false;
        std::uint8_t s = State();
        return s <= 8;  // small enum; 1 running, 2 returned
    }
    // Program of the innermost script frame (skips engine frames), like vtable slot 11 (0x1403DD2E0).
    std::uintptr_t CurrentProgram() const {
        std::uintptr_t fb = Read<std::uintptr_t>(thread::kFramesBase);
        for (std::uintptr_t f = Frame(); f + thread::kFrameSize > fb && f >= fb; f -= thread::kFrameSize)
            if (At<std::uintptr_t>(f + frame::kLocals)) return At<std::uintptr_t>(f + frame::kProgram);
        return 0;
    }
    // pc of the innermost frame as an offset into its program's code; nullopt when idle.
    std::optional<std::uint32_t> CurrentPcOffset() const {
        if (!FrameCount()) return std::nullopt;
        std::uintptr_t f = Frame();
        std::uintptr_t prog = At<std::uintptr_t>(f + frame::kProgram);
        std::uintptr_t pc = At<std::uintptr_t>(f + frame::kPc);
        if (!prog) return std::nullopt;
        std::uintptr_t code = At<std::uintptr_t>(prog + program::kCodeBase);
        if (pc < code) return std::nullopt;
        return static_cast<std::uint32_t>(pc - code);
    }
    std::uintptr_t Address() const { return t_; }

private:
    template <class T>
    static T At(std::uintptr_t a) {
        T v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
        return v;
    }
    template <class T>
    T Read(std::size_t off) const { return At<T>(t_ + off); }
    std::uintptr_t t_;
};

// Read-only view over a program in memory. The caller makes sure the address
// is readable (under SEH in the core).
class ProgramView {
public:
    explicit ProgramView(std::uintptr_t p) : p_(p) {}
    bool LooksValid() const { return p_ && Read<std::uint16_t>(program::kResourceVersionField) == program::kResourceVersion; }
    std::uint32_t NameHash() const { return Read<std::uint32_t>(program::kNameHash); }
    std::uint32_t TypeCount() const { return Read<std::uint32_t>(program::kTypeCount); }
    std::uint32_t ImportCount() const { return Read<std::uint32_t>(program::kImportCount); }
    std::uint32_t PatchCount() const { return Read<std::uint32_t>(program::kPatchCount); }
    std::uintptr_t Code() const { return Read<std::uintptr_t>(program::kCodeBase); }
    std::uintptr_t Data() const { return Read<std::uintptr_t>(program::kDataBase); }
    bool Obfuscated() const { return (Read<std::uint8_t>(program::kFlags + 1) & 1) != 0; }
    std::uintptr_t Address() const { return p_; }
    std::uintptr_t Imports() const { return Read<std::uintptr_t>(program::kImports); }

    // The import entry for program `nameHash`, as the VM finds it (see import::):
    // entry 0 when it is this program, else a binary search over entries
    // 1..count-1 by name hash. 0 when this program does not import it.
    std::uintptr_t FindImport(std::uint32_t nameHash) const {
        std::uint32_t n = ImportCount();
        std::uintptr_t t = Imports();
        if (!t || n == 0 || n > 4096) return 0;
        if (nameHash == NameHash()) return t;
        std::uint32_t lo = 1, hi = n;  // [lo, hi)
        while (lo < hi) {
            std::uint32_t mid = lo + (hi - lo) / 2;
            std::uintptr_t e = t + import::kStride * mid;
            std::uint32_t h = At<std::uint32_t>(e + import::kNameHash);
            if (h == nameHash) return e;
            if (h < nameHash) lo = mid + 1;
            else hi = mid;
        }
        return 0;
    }

private:
    template <class T>
    static T At(std::uintptr_t a) {
        T v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
        return v;
    }
    template <class T>
    T Read(std::size_t off) const { return At<T>(p_ + off); }
    std::uintptr_t p_;
};

// ----------------------------------------------------- statics (TASKS T2.4)
// Address of field `field` of module `moduleHash`'s statics in thread `t`
// (the formula of the VM's static opcodes, see import::). 0 when the thread
// has no static block or its root program does not import that module.
// Statics belong to a thread: each script instance has its own copy.
// Read-only memory walk; the caller holds SEH.
inline std::uintptr_t StaticAddress(std::uintptr_t t, std::uint32_t moduleHash, std::uint32_t field) {
    ThreadView tv(t);
    std::uintptr_t statics = tv.Statics(), root = tv.RootProgram();
    if (!statics || !root) return 0;
    std::uintptr_t e = ProgramView(root).FindImport(moduleHash);
    if (!e) return 0;
    std::uint32_t off;
    std::memcpy(&off, reinterpret_cast<const void*>(e + import::kStaticOffset), 4);
    return statics + off + field;
}

// ------------------------------------------------- bytecode walk (T2.1)
struct WalkStats {
    std::size_t instructions = 0;
    std::size_t resyncs = 0;  // undecodable 2-byte steps skipped
    std::uint32_t end = 0;    // offset where the walk stopped
};

// Decodes code[start, size) linearly with kCaseLength. An offset that does
// not decode (opcode out of range, or an instruction that runs past `size`)
// is skipped 2 bytes at a time (opcodes are u16) until decoding resumes;
// those steps count as resyncs. fn(const Instruction&) returns false to stop.
// Linear, so data inside code (none is known: switch tables live in +0xF0)
// would show up as resyncs.
template <class Fn>
WalkStats Walk(const std::uint8_t* code, std::size_t size, std::uint32_t start, bool obfuscated,
               const OperandKeys& k, std::uint16_t opcodeMul, Fn&& fn) {
    WalkStats s;
    std::uint32_t o = start;
    while (o < size) {
        auto i = DecodeAt(code, size, o, obfuscated, k, opcodeMul);
        if (!i) {
            ++s.resyncs;
            o += 2;
            continue;
        }
        ++s.instructions;
        if (!fn(*i)) {
            s.end = o;
            return s;
        }
        o += i->length;
    }
    s.end = o > size ? static_cast<std::uint32_t>(size) : o;
    return s;
}

// ---------------------------------------------- case-139 census (T2.2)
// Operand block A of a native call: u32 handler offset from [script_native_base],
// argc (byte 4) and retc (byte 5) in 8-byte cells (VM case 139, 0x143DE7411).
struct NativeCall {
    std::uint32_t site;           // offset of the opcode in the program's code
    std::uint32_t handlerOffset;  // handler = *script_native_base + handlerOffset
    std::uint8_t argc, retc;
};

constexpr NativeCall NativeCallOf(const Instruction& i) {
    return NativeCall{i.offset, static_cast<std::uint32_t>(i.a), static_cast<std::uint8_t>(i.a >> 32),
                      static_cast<std::uint8_t>(i.a >> 40)};
}

// Every native call in code[0, size), in order. fn(const NativeCall&).
template <class Fn>
WalkStats ForEachNativeCall(const std::uint8_t* code, std::size_t size, bool obfuscated, const OperandKeys& k,
                            std::uint16_t opcodeMul, Fn&& fn) {
    return Walk(code, size, 0, obfuscated, k, opcodeMul, [&](const Instruction& i) {
        if (i.kind == CaseKind::Native) fn(NativeCallOf(i));
        return true;
    });
}

}  // namespace shvi::gta6::script
