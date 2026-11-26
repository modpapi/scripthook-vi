#include "synth_pe.h"

#include "pe_layout.h"

#include <cstring>

namespace shvi::nativedb::test {

namespace {

constexpr std::uint32_t kText = 0x1000, kTextSize = 0x2040;
constexpr std::uint32_t kRdata = 0x4000, kPdata = 0x5000, kData = 0x6000;
constexpr std::uint32_t kIndirectSlot = 0x4800;  // qword the H5 indirect call goes through
constexpr std::uint64_t kId0 = 0x5A00000000000000ull;  // H<n> has id kId0 + n

struct Rf { std::uint32_t begin, end, unwind; };

// Hand assembler over the .text bytes, addressed by RVA.
struct Asm {
    std::vector<std::uint8_t>& t;
    std::uint32_t pc = kText;

    void Org(std::uint32_t rva) { pc = rva; }
    void B(std::initializer_list<std::uint8_t> bytes) {
        for (auto x : bytes) t[pc++ - kText] = x;
    }
    void D32(std::uint32_t v) { std::memcpy(&t[pc - kText], &v, 4); pc += 4; }
    void D64(std::uint64_t v) { std::memcpy(&t[pc - kText], &v, 8); pc += 8; }
    void Rel32To(std::uint32_t target) { D32(target - (pc + 4)); }  // rel32 at pc, insn ends after it

    void Call(std::uint32_t target) { B({0xE8}); Rel32To(target); }
    void Jmp(std::uint32_t target) { B({0xE9}); Rel32To(target); }
    void CallRipMem(std::uint32_t slot) { B({0xFF, 0x15}); Rel32To(slot); }
    void LeaRsiRip(std::uint32_t x) { B({0x48, 0x8D, 0x35}); Rel32To(x); }
    void LeaRcxRip(std::uint32_t x) { B({0x48, 0x8D, 0x0D}); Rel32To(x); }
    void MovRcxRsi() { B({0x48, 0x8B, 0xCE}); }
    void CmpArgc() { B({0x83, 0x79, 0x10, 0x00}); }
    // js rel8 with the displacement patched later; returns the patch position.
    std::uint32_t Js8() { B({0x78, 0x00}); return pc - 1; }
    void Patch8(std::uint32_t at, std::uint32_t target) {
        t[at - kText] = static_cast<std::uint8_t>(static_cast<std::int8_t>(target - (at + 1)));
    }
    void Js32(std::uint32_t target) { B({0x0F, 0x88}); Rel32To(target); }
    // cold block: mov rax,[rcx] ; mov rdx, id ; mov [rax], rdx
    void ColdId(std::uint64_t id) {
        B({0x48, 0x8B, 0x01});
        B({0x48, 0xBA});
        D64(id);
        B({0x48, 0x89, 0x10});
    }
};

struct Built {
    std::vector<std::uint8_t> file;
    std::vector<Rf> pdata;
};

Built Build() {
    std::vector<std::uint8_t> text(kTextSize, 0xCC);
    std::vector<std::uint8_t> rdata(0x810, 0);
    Asm a{text};
    std::vector<Rf> pd;
    std::uint32_t unwindNext = kRdata;
    auto plainUnwind = [&] {
        std::uint32_t u = unwindNext;
        rdata[u - kRdata] = 0x01;  // version 1, no flags, no codes
        unwindNext += 0x10;
        return u;
    };

    // Lock acquire / release and two command functions: plain `ret`.
    a.Org(kSynthAcquire); a.B({0xC3});
    pd.push_back({kSynthAcquire, kSynthAcquire + 1, plainUnwind()});
    a.Org(kSynthRelease); a.B({0xC3});
    a.Org(kSynthC1); a.B({0xC3});
    a.Org(kSynthC2); a.B({0xC3});

    // H1 0x1100: plain handler, cold block inside the same .pdata range.
    {
        a.Org(0x1100);
        a.B({0x56});                    // push rsi
        a.B({0x48, 0x83, 0xEC, 0x20});  // sub rsp, 20h
        a.CmpArgc();
        std::uint32_t js = a.Js8();
        a.LeaRsiRip(kSynthLockGlobal);
        a.MovRcxRsi();
        a.Call(kSynthAcquire);
        a.Call(kSynthC1);
        a.MovRcxRsi();
        std::uint32_t epi = a.pc;
        a.B({0x48, 0x83, 0xC4, 0x20});  // add rsp, 20h
        a.B({0x5E});                    // pop rsi
        a.Jmp(kSynthRelease);
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 1);
        a.Jmp(epi);
        pd.push_back({0x1100, a.pc, plainUnwind()});
    }

    // H2 0x1200: as H1 but calls C1 then C2, and the cold block is a chunk at 0x3000
    // with its own .pdata entry chained to 0x1200.
    std::uint32_t h2Unwind = plainUnwind();
    {
        a.Org(0x1200);
        a.B({0x56});
        a.B({0x48, 0x83, 0xEC, 0x20});
        a.CmpArgc();
        a.Js32(0x3000);
        a.LeaRsiRip(kSynthLockGlobal);
        a.MovRcxRsi();
        a.Call(kSynthAcquire);
        a.Call(kSynthC1);
        a.Call(kSynthC2);
        a.MovRcxRsi();
        std::uint32_t epi = a.pc;
        a.B({0x48, 0x83, 0xC4, 0x20});
        a.B({0x5E});
        a.Jmp(kSynthRelease);
        Rf primary{0x1200, a.pc, h2Unwind};
        pd.push_back(primary);

        a.Org(0x3000);
        a.ColdId(kId0 + 2);
        a.Jmp(epi);
        std::uint32_t cu = unwindNext;
        unwindNext += 0x10;
        rdata[cu - kRdata] = 0x01 | (0x4 << 3);  // UNW_FLAG_CHAININFO, 0 codes
        std::memcpy(&rdata[cu - kRdata + 4], &primary, 12);
        pd.push_back({0x3000, a.pc, cu});
    }

    // H3 0x1300: leaf, no .pdata, int3 padding before it.
    {
        a.Org(0x1300);
        a.B({0x48, 0x8B, 0xC1});        // mov rax, rcx
        a.CmpArgc();                    // 0x1303
        std::uint32_t js = a.Js8();
        a.B({0x33, 0xC0});              // xor eax, eax
        a.B({0xC3});                    // ret
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 3);
        a.B({0xC3});
    }

    // H4 0x1400: long prologue, the cmp at offset 25; lock via lea rcx directly.
    {
        a.Org(0x1400);
        a.B({0x53});                    // push rbx
        a.B({0x41, 0x54});              // push r12
        a.B({0x41, 0x55});              // push r13
        a.B({0x41, 0x56});              // push r14
        a.B({0x41, 0x57});              // push r15
        a.B({0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00});  // sub rsp, 100h
        a.B({0x48, 0x8B, 0xD9});        // mov rbx, rcx
        a.B({0x4C, 0x8B, 0xE2});        // mov r12, rdx
        a.B({0x4D, 0x8B, 0xE8});        // mov r13, r8
        a.CmpArgc();                    // 0x1419
        std::uint32_t js = a.Js8();
        a.LeaRcxRip(kSynthLockGlobal);
        a.Call(kSynthAcquire);
        a.Call(kSynthC1);
        a.LeaRcxRip(kSynthLockGlobal);
        std::uint32_t epi = a.pc;
        a.B({0x48, 0x81, 0xC4, 0x00, 0x01, 0x00, 0x00});  // add rsp, 100h
        a.B({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B});  // pop r15..r12, rbx
        a.Jmp(kSynthRelease);
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 4);
        a.Jmp(epi);
        pd.push_back({0x1400, a.pc, plainUnwind()});
    }

    // H5 0x1500: lock + an indirect call through a qword in .rdata.
    {
        a.Org(0x1500);
        a.B({0x56});
        a.B({0x48, 0x83, 0xEC, 0x20});
        a.CmpArgc();
        std::uint32_t js = a.Js8();
        a.LeaRsiRip(kSynthLockGlobal);
        a.MovRcxRsi();
        a.Call(kSynthAcquire);
        a.CallRipMem(kIndirectSlot);
        a.MovRcxRsi();
        std::uint32_t epi = a.pc;
        a.B({0x48, 0x83, 0xC4, 0x20});
        a.B({0x5E});
        a.Jmp(kSynthRelease);
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 5);
        a.Jmp(epi);
        pd.push_back({0x1500, a.pc, plainUnwind()});
    }

    // H6 0x1600: lock + call C1, release by `call`, then a tail call `jmp C2`.
    {
        a.Org(0x1600);
        a.B({0x56});
        a.B({0x48, 0x83, 0xEC, 0x20});
        a.CmpArgc();
        std::uint32_t js = a.Js8();
        a.LeaRsiRip(kSynthLockGlobal);
        a.MovRcxRsi();
        a.Call(kSynthAcquire);
        a.Call(kSynthC1);
        a.MovRcxRsi();
        a.Call(kSynthRelease);
        std::uint32_t epi = a.pc;
        a.B({0x48, 0x83, 0xC4, 0x20});
        a.B({0x5E});
        a.Jmp(kSynthC2);
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 6);
        a.Jmp(epi);
        pd.push_back({0x1600, a.pc, plainUnwind()});
    }

    // H7: no .pdata; `mov rax, imm64` at 0x1700 swallows the cmp's first bytes when
    // decoded from 0x1700, so no start candidate decodes onto the cmp at 0x1708.
    {
        a.Org(0x1700);
        a.B({0x48, 0xB8, 0, 0, 0, 0, 0, 0});  // mov rax, imm64 (imm continues into the cmp)
        a.CmpArgc();                    // 0x1708
        std::uint32_t js = a.Js8();
        a.B({0x33, 0xC0});
        a.B({0xC3});
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 7);
        a.B({0xC3});
    }

    // H8 0x1800: leaf that tail-calls far forward (`jmp 0x1900`) with its cold block after the
    // jmp, as on the Xbox exe at 0x3182960. The extent must end at the int3 padding after the
    // cold block, not run on to the jmp target.
    {
        a.Org(kSynthC3); a.B({0xC3});
        a.Org(0x1800);
        a.CmpArgc();
        std::uint32_t js = a.Js8();
        std::uint32_t body = a.pc;
        a.B({0x48, 0x8B, 0x01});        // mov rax, [rcx]
        a.Jmp(kSynthC3);
        a.Patch8(js, a.pc);
        a.ColdId(kId0 + 8);
        a.Jmp(body);
    }

    std::vector<std::uint8_t> pdata;
    // .pdata is sorted by begin, as the linker writes it.
    std::vector<Rf> sorted = pd;
    for (std::size_t i = 1; i < sorted.size(); ++i)
        for (std::size_t j = i; j > 0 && sorted[j].begin < sorted[j - 1].begin; --j) std::swap(sorted[j], sorted[j - 1]);
    pdata.resize(sorted.size() * 12);
    std::memcpy(pdata.data(), sorted.data(), pdata.size());

    PeLayout L;
    L.timeDateStamp = kSynthTimeDateStamp;
    L.sections.push_back({".text", kText, text, kRX});
    L.sections.push_back({".rdata", kRdata, rdata, kR});
    L.sections.push_back({".pdata", kPdata, pdata, kR});
    L.sections.push_back({".data", kData, std::vector<std::uint8_t>(0x10, 0), kRW});
    L.pdataRva = kPdata;
    L.pdataSize = static_cast<std::uint32_t>(pdata.size());
    return {LayoutPe(L), sorted};
}

const Built& Cached() {
    static const Built b = Build();
    return b;
}

}  // namespace

std::vector<std::uint8_t> BuildSynthPe() { return Build().file; }

const std::vector<SynthExpect>& SynthExpected() {
    static const std::vector<SynthExpect> e = {
        {0x1100, kId0 + 1, true, {kSynthC1}, ""},
        {0x1200, kId0 + 2, true, {kSynthC1, kSynthC2}, ""},
        {0x1300, kId0 + 3, false, {}, "L"},
        {0x1400, kId0 + 4, true, {kSynthC1}, "M"},
        {0x1500, kId0 + 5, true, {std::nullopt}, ""},
        {0x1600, kId0 + 6, true, {kSynthC1, kSynthC2}, "T"},
        {0x1708, kId0 + 7, false, {}, "LS"},
        {0x1800, kId0 + 8, false, {kSynthC3}, "LT"},
    };
    return e;
}

std::uint32_t SynthSizeOf(std::uint32_t rva) {
    for (const auto& f : Cached().pdata)
        if (f.begin == rva) return f.end - f.begin;
    return 0;
}

}  // namespace shvi::nativedb::test
