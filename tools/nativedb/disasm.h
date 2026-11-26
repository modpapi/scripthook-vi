// shvi_nativedb: one x64 instruction decoded by Zydis, reduced to what the
// census needs (flow kind, direct target, rip-relative memory target, the
// first two register operands). Linear decoding only; no analysis.
#pragma once
#include "image.h"

#include <Zydis/Zydis.h>

#include <cstdint>
#include <optional>

namespace shvi::nativedb {

struct Insn {
    std::uint32_t rva = 0;
    std::uint8_t len = 0;
    enum class Flow { None, Call, Jmp, Jcc, Ret, Int3 } flow = Flow::None;
    bool direct = false;          // flow target is an immediate (rel8/rel32)
    std::uint32_t target = 0;     // direct flow target (RVA)
    bool ripMem = false;          // has a [rip + disp] memory operand
    std::uint32_t memTarget = 0;  // its RVA (for lea, or an indirect call/jmp through memory)
    ZydisRegister dst = ZYDIS_REGISTER_NONE;  // operand 0, when it is a register
    ZydisRegister src = ZYDIS_REGISTER_NONE;  // operand 1, when it is a register
    ZydisMnemonic mnem = ZYDIS_MNEMONIC_INVALID;
    std::uint16_t writes = 0;     // GPRs written (any width, explicit or hidden), bit = Gpr64Index
};

// rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7, r8..r15 = 8..15 for a 64-bit GPR; -1 otherwise.
inline int Gpr64Index(ZydisRegister r) {
    return (r >= ZYDIS_REGISTER_RAX && r <= ZYDIS_REGISTER_R15) ? static_cast<int>(r - ZYDIS_REGISTER_RAX) : -1;
}

// nullopt on invalid bytes or when the instruction would run past rva's section.
std::optional<Insn> Decode(const ImageView& img, std::uint32_t rva);

}  // namespace shvi::nativedb
