#include "disasm.h"

namespace shvi::nativedb {

namespace {
const ZydisDecoder& Decoder() {
    static const ZydisDecoder d = [] {
        ZydisDecoder x;
        ZydisDecoderInit(&x, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return x;
    }();
    return d;
}
}  // namespace

std::optional<Insn> Decode(const ImageView& img, std::uint32_t rva) {
    std::uint32_t avail = img.Avail(rva);
    if (avail == 0) return std::nullopt;
    if (avail > ZYDIS_MAX_INSTRUCTION_LENGTH) avail = ZYDIS_MAX_INSTRUCTION_LENGTH;
    ZydisDecodedInstruction zi;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), img.At(rva), avail, &zi, ops))) return std::nullopt;

    Insn r;
    r.rva = rva;
    r.len = zi.length;
    r.mnem = zi.mnemonic;
    const std::uint32_t next = rva + zi.length;
    switch (zi.mnemonic) {
        case ZYDIS_MNEMONIC_CALL: r.flow = Insn::Flow::Call; break;
        case ZYDIS_MNEMONIC_JMP: r.flow = Insn::Flow::Jmp; break;
        case ZYDIS_MNEMONIC_RET: r.flow = Insn::Flow::Ret; break;
        case ZYDIS_MNEMONIC_INT3: r.flow = Insn::Flow::Int3; break;
        default:
            if (zi.meta.category == ZYDIS_CATEGORY_COND_BR) r.flow = Insn::Flow::Jcc;
    }
    for (ZyanU8 k = 0; k < zi.operand_count_visible; ++k) {
        const ZydisDecodedOperand& op = ops[k];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative && r.flow != Insn::Flow::None) {
            r.direct = true;
            r.target = next + static_cast<std::uint32_t>(op.imm.value.s);
        } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) {
            r.ripMem = true;
            r.memTarget = next + static_cast<std::uint32_t>(op.mem.disp.value);
        }
    }
    for (ZyanU8 k = 0; k < zi.operand_count; ++k) {
        const ZydisDecodedOperand& op = ops[k];
        if (op.type != ZYDIS_OPERAND_TYPE_REGISTER || !(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
        int g = Gpr64Index(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, op.reg.value));
        if (g >= 0) r.writes |= static_cast<std::uint16_t>(1u << g);
    }
    if (zi.operand_count_visible > 0 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) r.dst = ops[0].reg.value;
    if (zi.operand_count_visible > 1 && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) r.src = ops[1].reg.value;
    return r;
}

}  // namespace shvi::nativedb
