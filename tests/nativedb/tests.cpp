// Unit tests for shvi_nativedb (tools/nativedb) and the helpers it shares with the core
// (core/sha256.h, core/json_write.h). No game exe needed: the census tests run on a
// synthetic PE written by tests/nativedb/synth_pe.cpp.
#include "core/json_write.h"
#include "core/sha256.h"
#include "census.h"
#include "compare.h"
#include "disasm.h"
#include "manifest.h"
#include "pdata.h"
#include "image.h"
#include "pe_layout.h"
#include "synth_pe.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) { ++g_failed; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& Tests() { static std::vector<TestCase> t; return t; }
#define TEST(n) static void n(); static const bool n##_reg = (Tests().push_back({#n, n}), true); static void n()

using namespace shvi;
using namespace shvi::nativedb;
using namespace shvi::nativedb::test;

// Bytes helpers for hand-laid code.
static void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) { std::memcpy(&b[at], &v, 4); }
static std::uint32_t Rel32(std::uint32_t insnEnd, std::uint32_t target) { return target - insnEnd; }

// ------------------------------------------------------------------ sha256
TEST(sha256_known_vectors) {
    CHECK(sha256::Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256::Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(std::strlen(m56) == 56);
    CHECK(sha256::Hex(m56, 56) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    CHECK(sha256::Hex(million.data(), million.size()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ------------------------------------------------------------------ json writer
TEST(jsonw_sorted_and_formatted) {
    using namespace jsonw;
    std::string s = Dump(Object{{"b", 1}, {"a", Array{true, nullptr, "x\n"}}});
    CHECK(s == "{\n  \"a\": [\n    true,\n    null,\n    \"x\\n\"\n  ],\n  \"b\": 1\n}\n");
    CHECK(Dump(Array{}) == "[]\n");
    CHECK(Dump(Object{}) == "{}\n");
    CHECK(Dump(Object{{"e", Array{}}, {"o", Object{}}}) == "{\n  \"e\": [],\n  \"o\": {}\n}\n");
    CHECK(Dump(Node(std::int64_t{-5})) == "-5\n");
    CHECK(Dump(Node(std::uint64_t{18446744073709551615ull})) == "18446744073709551615\n");
}

TEST(jsonw_hex_and_escapes) {
    using namespace jsonw;
    CHECK(Hex32(0x3CD0DC0) == "0x03CD0DC0");
    CHECK(Hex64(0xBE0F39BFDF49C015ull) == "0xBE0F39BFDF49C015");
    CHECK(Dump(Node(std::string("a\x01" "b"))) == "\"a\\u0001b\"\n");
    CHECK(Dump(Node(std::string("q\"\\\b\f\r\t"))) == "\"q\\\"\\\\\\b\\f\\r\\t\"\n");
    CHECK(Dump(Node(std::string("\xC3\xA9"))) == "\"\xC3\xA9\"\n");  // é passes through
}

// ------------------------------------------------------------------ decoder
TEST(disasm_basic_forms) {
    // One executable section at RVA 0x1000, 0x40 bytes long.
    std::vector<std::uint8_t> t(0x40, 0xCC);
    const std::uint8_t cmp[] = {0x83, 0x79, 0x10, 0x00};
    std::memcpy(&t[0x00], cmp, 4);                         // 0x1000 cmp dword [rcx+10h],0
    t[0x04] = 0x78; t[0x05] = 0x30;                        // 0x1004 js 0x1036
    t[0x06] = 0xE8; Put32(t, 0x07, Rel32(0x100B, 0x1020)); // 0x1006 call 0x1020
    t[0x0B] = 0xFF; t[0x0C] = 0x15; Put32(t, 0x0D, Rel32(0x1011, 0x2000));  // 0x100B call [rip->0x2000]
    t[0x11] = 0x48; t[0x12] = 0x8D; t[0x13] = 0x0D; Put32(t, 0x14, Rel32(0x1018, 0x3000));  // lea rcx,[0x3000]
    t[0x18] = 0xC3;                                        // 0x1018 ret
    t[0x19] = 0xCC;                                        // 0x1019 int3
    t[0x1A] = 0xE9; Put32(t, 0x1B, Rel32(0x101F, 0x1000)); // 0x101A jmp 0x1000
    t[0x3E] = 0xE8;                                        // 0x103E: call rel32 cut by the section end
    PeLayout L;
    L.sections.push_back({".text", 0x1000, t, kRX});
    auto img = LayoutPe(L);
    ImageView v = ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size()));
    CHECK(v.sections.size() == 1);
    CHECK(v.Executable(0x1000) && !v.Executable(0x1040));
    CHECK(v.Readable(0x1000, 0x40) && !v.Readable(0x1000, 0x41));

    auto i = Decode(v, 0x1000);
    CHECK(i && i->mnem == ZYDIS_MNEMONIC_CMP && i->len == 4 && i->flow == Insn::Flow::None);
    i = Decode(v, 0x1004);
    CHECK(i && i->flow == Insn::Flow::Jcc && i->direct && i->target == 0x1036);
    i = Decode(v, 0x1006);
    CHECK(i && i->flow == Insn::Flow::Call && i->direct && i->target == 0x1020);
    i = Decode(v, 0x100B);
    CHECK(i && i->flow == Insn::Flow::Call && !i->direct && i->ripMem && i->memTarget == 0x2000);
    i = Decode(v, 0x1011);
    CHECK(i && i->mnem == ZYDIS_MNEMONIC_LEA && i->dst == ZYDIS_REGISTER_RCX && i->ripMem && i->memTarget == 0x3000);
    i = Decode(v, 0x1018);
    CHECK(i && i->flow == Insn::Flow::Ret);
    i = Decode(v, 0x1019);
    CHECK(i && i->flow == Insn::Flow::Int3);
    i = Decode(v, 0x101A);
    CHECK(i && i->flow == Insn::Flow::Jmp && i->direct && i->target == 0x1000);
    CHECK(!Decode(v, 0x103E));
    CHECK(!Decode(v, 0x5000));
}

// ------------------------------------------------------------------ .pdata
// Unwind info at `at` in `b`: chained (CHAININFO + parent RUNTIME_FUNCTION) when parent != nullptr.
struct RF { std::uint32_t begin, end, unwind; };
static void PutUnwind(std::vector<std::uint8_t>& b, std::size_t at, std::uint8_t codes, const RF* parent) {
    b[at] = static_cast<std::uint8_t>(1 | ((parent ? 0x4 : 0) << 3));
    b[at + 2] = codes;
    if (parent) {
        std::size_t p = at + 4 + 2 * ((codes + 1u) & ~1u);
        Put32(b, p, parent->begin); Put32(b, p + 4, parent->end); Put32(b, p + 8, parent->unwind);
    }
}
static std::vector<std::uint8_t> PdataImage(const std::vector<RF>& table, const std::vector<std::uint8_t>& rdata) {
    std::vector<std::uint8_t> text(0x1000, 0xCC), pdata(table.size() * 12);
    for (std::size_t i = 0; i < table.size(); ++i) {
        Put32(pdata, 12 * i, table[i].begin); Put32(pdata, 12 * i + 4, table[i].end); Put32(pdata, 12 * i + 8, table[i].unwind);
    }
    PeLayout L;
    L.sections.push_back({".text", 0x1000, text, kRX});
    L.sections.push_back({".rdata", 0x2000, rdata, kR});
    L.sections.push_back({".pdata", 0x3000, pdata, kR});
    L.pdataRva = 0x3000;
    L.pdataSize = static_cast<std::uint32_t>(pdata.size());
    return LayoutPe(L);
}

TEST(pdata_reads_and_sorts) {
    // A 0x1000, B 0x1100, C 0x1200, D 0x1300, plain unwind infos, given out of order.
    std::vector<std::uint8_t> rd(0x100, 0);
    for (int i = 0; i < 4; ++i) PutUnwind(rd, 0x10 * i, 2, nullptr);
    auto img = PdataImage({{0x1300, 0x1340, 0x2030}, {0x1000, 0x1080, 0x2000}, {0x1200, 0x1220, 0x2020},
                           {0x1100, 0x1110, 0x2010}}, rd);
    auto r = ReadPdata(ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size())));
    CHECK(r.error.empty());
    CHECK(r.fns.size() == 4);
    for (std::size_t i = 0; i + 1 < r.fns.size(); ++i) CHECK(r.fns[i].begin < r.fns[i + 1].begin);
    for (const auto& f : r.fns) CHECK(f.primary == f.begin);
    auto* hit = FindContaining(r.fns, 0x1210);
    CHECK(hit && hit->begin == 0x1200);
    CHECK(FindContaining(r.fns, 0x1000) && FindContaining(r.fns, 0x1000)->begin == 0x1000);
    CHECK(FindContaining(r.fns, 0x1090) == nullptr);   // gap between A and B
    CHECK(FindContaining(r.fns, 0x1220) == nullptr);   // end is exclusive
    CHECK(FindContaining(r.fns, 0x0FFF) == nullptr);
}

TEST(pdata_resolves_chain) {
    RF A{0x1000, 0x1080, 0x2000}, B{0x1400, 0x1420, 0x2010}, C{0x1600, 0x1610, 0x2030};
    std::vector<std::uint8_t> rd(0x100, 0);
    PutUnwind(rd, 0x00, 2, nullptr);
    PutUnwind(rd, 0x10, 0, &A);     // B -> A
    PutUnwind(rd, 0x30, 3, &B);     // C -> B (odd code count: padded to 4)
    auto img = PdataImage({C, A, B, {0x1200, 0x1240, 0x2000}}, rd);
    auto r = ReadPdata(ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size())));
    CHECK(r.error.empty());
    CHECK(FindContaining(r.fns, 0x1605) && FindContaining(r.fns, 0x1605)->primary == 0x1000);
    CHECK(FindContaining(r.fns, 0x1405)->primary == 0x1000);
    CHECK(FindContaining(r.fns, 0x1205)->primary == 0x1200);
    auto ch = ChunksOf(r.fns, 0x1000);
    CHECK(ch.size() == 3);
    if (ch.size() == 3) CHECK(ch[0]->begin == 0x1000 && ch[1]->begin == 0x1400 && ch[2]->begin == 0x1600);
    CHECK(ChunksOf(r.fns, 0x1200).size() == 1);
}

TEST(pdata_indirect_entry) {
    // Unwind RVA with the low bit set points straight at the parent RUNTIME_FUNCTION.
    std::vector<std::uint8_t> rd(0x100, 0);
    PutUnwind(rd, 0x00, 2, nullptr);
    Put32(rd, 0x40, 0x1000); Put32(rd, 0x44, 0x1080); Put32(rd, 0x48, 0x2000);
    auto img = PdataImage({{0x1000, 0x1080, 0x2000}, {0x1800, 0x1810, 0x2041}}, rd);
    auto r = ReadPdata(ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size())));
    CHECK(r.error.empty());
    CHECK(FindContaining(r.fns, 0x1800) && FindContaining(r.fns, 0x1800)->primary == 0x1000);
}

TEST(pdata_chain_cycle_is_an_error) {
    RF A{0x1000, 0x1080, 0x2000}, B{0x1400, 0x1420, 0x2010};
    std::vector<std::uint8_t> rd(0x100, 0);
    PutUnwind(rd, 0x00, 0, &B);     // A -> B
    PutUnwind(rd, 0x10, 0, &A);     // B -> A
    auto img = PdataImage({A, B}, rd);
    auto r = ReadPdata(ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size())));
    CHECK(!r.error.empty());
    CHECK(r.error.rfind("unwind chain too deep at 0x", 0) == 0);
}

// ------------------------------------------------------------------ synthetic PE
// Writes BuildSynthPe() to a temp file and maps it the way the tool does.
static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"shvi_nativedb_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
}
static bool WriteFileBytes(const std::wstring& p, const std::vector<std::uint8_t>& b) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    return fclose(f) == 0 && ok;
}
static std::optional<pe::MappedImage> MapSynth() {
    auto p = TempPath(L"synth.exe");
    if (!WriteFileBytes(p, BuildSynthPe())) return std::nullopt;
    std::string err;
    auto m = pe::MapImageFile(p, &err);
    _wremove(p.c_str());
    return m;
}

TEST(synth_pe_maps) {
    auto m = MapSynth();
    CHECK(m.has_value());
    if (!m) return;
    CHECK(m->timeDateStamp == 0x5EED0001u);
    CHECK(m->module.sections.size() == 4);
    ImageView v = ViewOf(*m);
    CHECK(v.machine == IMAGE_FILE_MACHINE_AMD64);
    auto pd = ReadPdata(v);
    CHECK(pd.error.empty());
    auto* cold = FindContaining(pd.fns, 0x3000);
    CHECK(cold && cold->primary == 0x1200);
    CHECK(FindContaining(pd.fns, 0x1303) == nullptr);
    CHECK(BuildSynthPe() == BuildSynthPe());
    CHECK(SynthExpected().size() == 8);
    CHECK(SynthSizeOf(0x1200) > 0 && SynthSizeOf(0x1200) < 0x100);
}

// ------------------------------------------------------------------ census: handlers
static const Handler* HandlerAt(const HandlerScan& s, std::uint32_t rva) {
    for (const auto& h : s.handlers)
        if (h.rva == rva) return &h;
    return nullptr;
}

TEST(census_finds_synth_handlers) {
    auto m = MapSynth();
    CHECK(m.has_value());
    if (!m) return;
    ImageView v = ViewOf(*m);
    auto pd = ReadPdata(v);
    auto s = FindHandlers(v, pd.fns);
    CHECK(s.error.empty());
    CHECK(s.notes.empty());
    CHECK(s.handlers.size() == 8);
    for (std::size_t i = 0; i + 1 < s.handlers.size(); ++i) CHECK(s.handlers[i].rva < s.handlers[i + 1].rva);
    for (const auto& e : SynthExpected()) {
        const Handler* h = HandlerAt(s, e.rva);
        CHECK(h != nullptr);
        if (!h) continue;
        CHECK(h->id == e.id);
        if (e.flags.find('L') == std::string::npos) CHECK(h->size == SynthSizeOf(e.rva));
    }
}

TEST(census_flags) {
    auto m = MapSynth();
    if (!m) { CHECK(false); return; }
    ImageView v = ViewOf(*m);
    auto s = FindHandlers(v, ReadPdata(v).fns);
    const char* want[][2] = {{"1100", ""}, {"1200", ""}, {"1300", "L"}, {"1400", "M"},
                             {"1500", ""}, {"1600", ""}, {"1708", "LS"}, {"1800", "L"}};
    for (auto& w : want) {
        const Handler* h = HandlerAt(s, static_cast<std::uint32_t>(std::stoul(w[0], nullptr, 16)));
        CHECK(h && h->flags == w[1]);
    }
    const Handler* h3 = HandlerAt(s, 0x1300);
    CHECK(h3 && h3->cmpRva == 0x1303);
    // H3: mov(3) cmp(4) js(2) xor(2) ret(1) | cold: mov(3) mov imm64(10) mov(3) ret(1)
    CHECK(h3 && h3->size == 29 && h3->code.size() == 1 && h3->code[0].begin == 0x1300 && h3->code[0].end == 0x131D);
    // H8: cmp(4) js(2) mov(3) jmp far(5) | cold: mov(3) mov imm64(10) mov(3) jmp back(5) -> 35, then int3 padding.
    // The far `jmp 0x1900` is a tail call and must not stretch the extent to its target.
    const Handler* h8 = HandlerAt(s, 0x1800);
    CHECK(h8 && h8->size == 35 && h8->code.size() == 1 && h8->code[0].end == 0x1823);
    if (h8 && h8->size != 35) printf("  H8 size %u\n", h8->size);
}

TEST(census_cold_chunk_code_ranges) {
    auto m = MapSynth();
    if (!m) { CHECK(false); return; }
    ImageView v = ViewOf(*m);
    auto s = FindHandlers(v, ReadPdata(v).fns);
    const Handler* h2 = HandlerAt(s, 0x1200);
    CHECK(h2 && h2->code.size() == 2);
    if (h2 && h2->code.size() == 2) {
        CHECK(h2->code[0].begin == 0x1200 && h2->code[0].end == 0x1200 + SynthSizeOf(0x1200));
        CHECK(h2->code[1].begin == 0x3000);
    }
    const Handler* h1 = HandlerAt(s, 0x1100);
    CHECK(h1 && h1->code.size() == 1);
}

TEST(census_section_end_no_overread) {
    // .text ends 6 bytes after a cmp: the js target lies past the section end.
    std::vector<std::uint8_t> t(0x30, 0xCC);
    const std::uint8_t tail[] = {0x83, 0x79, 0x10, 0x00, 0x78, 0x10};
    std::memcpy(&t[0x2A], tail, 6);
    PeLayout L;
    L.sections.push_back({".text", 0x1000, t, kRX});
    auto img = LayoutPe(L);
    ImageView v = ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size()));
    auto s = FindHandlers(v, {});
    CHECK(s.handlers.empty());
    CHECK(s.error.empty());
    CHECK(s.notes.size() == 1);
    if (!s.notes.empty()) CHECK(s.notes[0].rfind("skipped 0x0000102A: ", 0) == 0);
}

// ------------------------------------------------------------------ census: lock, callees, TSV
static std::optional<Census> CensusOfSynth(std::string* err) {
    static auto m = MapSynth();
    if (!m) { *err = "map failed"; return std::nullopt; }
    return RunCensus(ViewOf(*m), err);
}

TEST(census_lock_detected) {
    std::string err;
    auto c = CensusOfSynth(&err);
    CHECK(c.has_value());
    if (!c) { printf("  err: %s\n", err.c_str()); return; }
    CHECK(c->lock.global == kSynthLockGlobal);
    CHECK(c->lock.acquire == kSynthAcquire);
    CHECK(c->lock.release == kSynthRelease);
    CHECK(c->lock.votes == 5 && c->lock.voters == 5);
}

TEST(census_callees_match_expectations) {
    std::string err;
    auto c = CensusOfSynth(&err);
    if (!c) { CHECK(false); return; }
    CHECK(c->rows.size() == SynthExpected().size());
    for (std::size_t i = 0; i < c->rows.size() && i < SynthExpected().size(); ++i) {
        const auto& r = c->rows[i];
        const auto& e = SynthExpected()[i];
        CHECK(r.h.rva == e.rva);
        CHECK(r.lock == e.lock);
        CHECK(r.callees == e.callees);
        CHECK(r.h.flags == e.flags);
        if (r.h.flags != e.flags) printf("  0x%08X flags '%s' want '%s'\n", r.h.rva, r.h.flags.c_str(), e.flags.c_str());
    }
}

// Four minimal handlers that all take the lock global in rcx; `acq[i]` is each one's first call.
static std::vector<std::uint8_t> LockVoteImage(const std::uint32_t acq[4]) {
    std::vector<std::uint8_t> t(0x400, 0xCC), rd(0x10, 0), pd;
    rd[0] = 0x01;
    for (std::uint32_t k = 0; k < 4; ++k) {
        std::uint32_t h = 0x1100 + 0x40 * k;
        std::size_t o = h - 0x1000;
        const std::uint8_t cmp[] = {0x83, 0x79, 0x10, 0x00, 0x78, 13};
        std::memcpy(&t[o], cmp, 6);                                                   // h+0 cmp ; js h+19
        t[o + 6] = 0x48; t[o + 7] = 0x8D; t[o + 8] = 0x0D; Put32(t, o + 9, Rel32(h + 13, 0x3000));  // lea rcx,[G]
        t[o + 13] = 0xE8; Put32(t, o + 14, Rel32(h + 18, acq[k]));                   // call acq
        t[o + 18] = 0xC3;
        const std::uint8_t cold[] = {0x48, 0x8B, 0x01, 0x48, 0xBA, static_cast<std::uint8_t>(k + 1), 0, 0, 0, 0, 0, 0, 0x5B,
                                     0x48, 0x89, 0x10, 0xC3};
        std::memcpy(&t[o + 19], cold, sizeof(cold));
        std::uint32_t e[3] = {h, h + 36, 0x2000};
        pd.insert(pd.end(), reinterpret_cast<std::uint8_t*>(e), reinterpret_cast<std::uint8_t*>(e) + 12);
    }
    t[0x000] = 0xC3;  // 0x1000
    t[0x010] = 0xC3;  // 0x1010
    PeLayout L;
    L.sections.push_back({".text", 0x1000, t, kRX});
    L.sections.push_back({".rdata", 0x2000, rd, kR});
    L.sections.push_back({".data", 0x3000, std::vector<std::uint8_t>(0x10, 0), kRW});
    L.sections.push_back({".pdata", 0x4000, pd, kR});
    L.pdataRva = 0x4000;
    L.pdataSize = static_cast<std::uint32_t>(pd.size());
    return LayoutPe(L);
}

TEST(census_lock_minority_is_error) {
    const std::uint32_t split[4] = {0x1000, 0x1000, 0x1010, 0x1010};
    auto img = LockVoteImage(split);
    std::string err;
    auto c = RunCensus(ViewOfBytes(img.data(), static_cast<std::uint32_t>(img.size())), &err);
    CHECK(!c.has_value());
    CHECK(err.rfind("lock: no 90% majority", 0) == 0);
    if (err.rfind("lock: no 90% majority", 0) != 0) printf("  err: %s\n", err.c_str());

    const std::uint32_t same[4] = {0x1000, 0x1000, 0x1000, 0x1000};
    auto img2 = LockVoteImage(same);
    auto c2 = RunCensus(ViewOfBytes(img2.data(), static_cast<std::uint32_t>(img2.size())), &err);
    CHECK(c2.has_value() && c2->lock.acquire == 0x1000 && c2->lock.global == 0x3000 && c2->rows.size() == 4);
}

TEST(census_tsv_format) {
    Census c;
    CensusRow a;
    a.h.rva = 0x22D3120; a.h.id = 0xBE0F39BFDF49C015ull; a.h.size = 74; a.lock = true;
    a.callees = {0x18343E0u, std::nullopt};
    CensusRow b;
    b.h.rva = 0x1300; b.h.id = 0x5A00000000000003ull; b.h.size = 29; b.lock = false; b.h.flags = "L";
    c.rows = {b, a};  // FormatCensusTsv writes rows in the order given; RunCensus sorts them
    std::string s = FormatCensusTsv(c);
    CHECK(s ==
          "# shvi_nativedb census schema 1\n"
          "rva\tid64\tsize\tlock\tcallees\tflags\n"
          "00001300\t5A00000000000003\t29\t0\t\tL\n"
          "022D3120\tBE0F39BFDF49C015\t74\t1\t018343E0,ind\t\n");
}

// ------------------------------------------------------------------ manifest
TEST(manifest_format) {
    BuildInfo b;
    b.key = 1592590337;
    b.machine = 0x8664;
    b.imageSize = 0x7000;
    b.handlers = 7;
    b.lock = LockInfo{0x6000, 0x1000, 0x1010, 5, 5};
    std::string A(64, 'a'), B(64, 'b'), C(64, 'c'), D(64, 'd'), E(64, 'e');
    std::vector<InputRec> in = {{"refdb", "natives.json", C, 20, "deadbeef"},
                                {"exe", "synth.exe", A, 28672, ""},
                                {"dump", "b.tsv", B, 10, ""}};
    std::vector<OutputRec> out = {{"z.md", D, 3}, {"census.tsv", E, 456}};
    std::string s = FormatManifest(b, in, out);
    std::string want =
        "{\n"
        "  \"build\": {\n"
        "    \"exe\": {\n"
        "      \"name\": \"synth.exe\",\n"
        "      \"sha256\": \"" + A + "\",\n"
        "      \"size\": 28672\n"
        "    },\n"
        "    \"image_size\": \"0x00007000\",\n"
        "    \"key\": 1592590337,\n"
        "    \"machine\": \"0x8664\"\n"
        "  },\n"
        "  \"census\": {\n"
        "    \"handlers\": 7,\n"
        "    \"lock\": {\n"
        "      \"acquire\": \"0x00001000\",\n"
        "      \"global\": \"0x00006000\",\n"
        "      \"release\": \"0x00001010\",\n"
        "      \"voters\": 5,\n"
        "      \"votes\": 5\n"
        "    }\n"
        "  },\n"
        "  \"inputs\": [\n"
        "    {\n"
        "      \"name\": \"b.tsv\",\n"
        "      \"role\": \"dump\",\n"
        "      \"sha256\": \"" + B + "\",\n"
        "      \"size\": 10\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"synth.exe\",\n"
        "      \"role\": \"exe\",\n"
        "      \"sha256\": \"" + A + "\",\n"
        "      \"size\": 28672\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"natives.json\",\n"
        "      \"pin\": \"deadbeef\",\n"
        "      \"role\": \"refdb\",\n"
        "      \"sha256\": \"" + C + "\",\n"
        "      \"size\": 20\n"
        "    }\n"
        "  ],\n"
        "  \"outputs\": [\n"
        "    {\n"
        "      \"path\": \"census.tsv\",\n"
        "      \"sha256\": \"" + E + "\",\n"
        "      \"size\": 456\n"
        "    },\n"
        "    {\n"
        "      \"path\": \"z.md\",\n"
        "      \"sha256\": \"" + D + "\",\n"
        "      \"size\": 3\n"
        "    }\n"
        "  ],\n"
        "  \"tool\": {\n"
        "    \"name\": \"shvi_nativedb\",\n"
        "    \"schema\": 1,\n"
        "    \"version\": \"" SHVI_TOOL_VERSION "\",\n"
        "    \"zycore\": \"" SHVI_ZYCORE_COMMIT "\",\n"
        "    \"zydis\": \"" SHVI_ZYDIS_COMMIT "\"\n"
        "  }\n"
        "}\n";
    CHECK(s == want);
    if (s != want) printf("%s", s.c_str());
}

TEST(manifest_utf8_basename) {
    CHECK(Utf8Basename(L"C:\\x y\\T\u00e9l\u00e9chargements\\jeu.exe") == "jeu.exe");
    CHECK(Utf8Basename(L"C:\\x y\\T\u00e9l\u00e9chargements\\j\u00e9u.exe") == "j\xC3\xA9u.exe");
    CHECK(Utf8Basename(L"D:/a/b/game.exe") == "game.exe");
    CHECK(Utf8Basename(L"game.exe") == "game.exe");
    BuildInfo b;
    b.key = 1;
    std::vector<InputRec> in = {{"exe", Utf8Basename(L"C:\\x y\\T\u00e9l\u00e9chargements\\j\u00e9u.exe"),
                                 std::string(64, '0'), 1, ""}};
    std::string s = FormatManifest(b, in, {});
    CHECK(s.find("\"name\": \"j\xC3\xA9u.exe\"") != std::string::npos);
    CHECK(s.find('\\') == std::string::npos);
    CHECK(s.find("C:") == std::string::npos);
    CHECK(s.find("\"census\"") == std::string::npos);  // no lock given: no census block
}

// ------------------------------------------------------------------ compare-census
static CensusRow Row(std::uint32_t rva, std::uint64_t id, std::uint32_t size, bool lock,
                     std::vector<std::optional<std::uint32_t>> callees, std::string flags = "") {
    CensusRow r;
    r.h.rva = rva; r.h.id = id; r.h.size = size; r.lock = lock; r.callees = std::move(callees); r.h.flags = std::move(flags);
    return r;
}
static IdaRow Ida(std::uint32_t rva, std::uint64_t id, std::optional<std::uint32_t> size, std::optional<bool> lock,
                  std::vector<std::optional<std::uint32_t>> callees, bool x = false) {
    return IdaRow{rva, id, size, lock, std::move(callees), x};
}

TEST(compare_categories) {
    Census ours;
    std::vector<IdaRow> ida;
    std::vector<DumpRow> dump;
    // 1 ida-missing: an x row, same rva, dump agrees
    ours.rows.push_back(Row(0x1000, 1, 10, true, {0x9000u}, "L"));
    ida.push_back(Ida(0x1000, 1, std::nullopt, std::nullopt, {}, true));
    dump.push_back({1, 0x1000});
    // 2 ida-mid-function (and ida-missing): IDA rva is the cmp, 0x20 in
    ours.rows.push_back(Row(0x2000, 2, 10, true, {}, "M"));
    ida.push_back(Ida(0x2020, 2, std::nullopt, std::nullopt, {}, true));
    dump.push_back({2, 0x2000});
    // 3 size-chunking
    ours.rows.push_back(Row(0x3000, 3, 100, true, {0x9000u}));
    ida.push_back(Ida(0x3000, 3, 60u, true, {0x9000u}));
    // 4 callees-truncated: IDA kept the first 6
    std::vector<std::optional<std::uint32_t>> seven = {0x9001u, 0x9002u, 0x9003u, 0x9004u, 0x9005u, 0x9006u, 0x9007u};
    ours.rows.push_back(Row(0x4000, 4, 10, true, seven));
    ida.push_back(Ida(0x4000, 4, 10u, true, {seven.begin(), seven.begin() + 6}));
    // 5 callees-tailcall: ours adds the jmp target
    ours.rows.push_back(Row(0x5000, 5, 10, true, {0x9001u, 0x9002u}, "T"));
    ida.push_back(Ida(0x5000, 5, 10u, true, {0x9001u}));
    // 6 callees-order
    ours.rows.push_back(Row(0x6000, 6, 10, true, {0x9001u, 0x9002u}));
    ida.push_back(Ida(0x6000, 6, 10u, true, {0x9002u, 0x9001u}));
    // 7 id-set: only in ours
    ours.rows.push_back(Row(0x7000, 7, 10, true, {}));
    // 8 rva-vs-dump
    ours.rows.push_back(Row(0x8000, 8, 10, true, {}));
    ida.push_back(Ida(0x8000, 8, 10u, true, {}));
    dump.push_back({8, 0x8010});
    // 9 unexplained: lock differs, nothing else
    ours.rows.push_back(Row(0x9000, 9, 10, true, {}));
    ida.push_back(Ida(0x9000, 9, 10u, false, {}));
    // 10 start-unresolved (and ida-missing)
    ours.rows.push_back(Row(0xA008, 10, 10, false, {}, "LS"));
    ida.push_back(Ida(0xA008, 10, std::nullopt, std::nullopt, {}, true));
    // 11 identical row: no category
    ours.rows.push_back(Row(0xB000, 11, 10, true, {0x9001u, std::nullopt}));
    ida.push_back(Ida(0xB000, 11, 10u, true, {0x9001u, std::nullopt}));
    dump.push_back({11, 0xB000});

    CompareResult r = CompareCensus(ours, ida, dump);
    auto n = [&](const char* cat) { return r.byCategory.count(cat) ? r.byCategory.at(cat).size() : 0; };
    auto has = [&](const char* cat, const char* idHex) {
        if (!r.byCategory.count(cat)) return false;
        for (const auto& l : r.byCategory.at(cat))
            if (l.find(idHex) != std::string::npos) return true;
        return false;
    };
    CHECK(n("ida-missing") == 3 && has("ida-missing", "0000000000000001") && has("ida-missing", "0000000000000002") &&
          has("ida-missing", "000000000000000A"));
    CHECK(n("ida-mid-function") == 1 && has("ida-mid-function", "0000000000000002"));
    CHECK(n("size-chunking") == 1 && has("size-chunking", "0000000000000003"));
    CHECK(n("callees-truncated") == 1 && has("callees-truncated", "0000000000000004"));
    CHECK(n("callees-tailcall") == 1 && has("callees-tailcall", "0000000000000005"));
    CHECK(n("callees-order") == 1 && has("callees-order", "0000000000000006"));
    CHECK(n("id-set") == 1 && has("id-set", "0000000000000007"));
    CHECK(n("rva-vs-dump") == 1 && has("rva-vs-dump", "0000000000000008"));
    CHECK(n("start-unresolved") == 1 && has("start-unresolved", "000000000000000A"));
    CHECK(n("unexplained") == 1 && has("unexplained", "0000000000000009"));
    CHECK(r.unexplained == 1);
    CHECK(r.failures == 3);  // unexplained + id-set + rva-vs-dump
    for (const auto& [cat, lines] : r.byCategory) CHECK(std::is_sorted(lines.begin(), lines.end()));
    std::string md = FormatCompareMd(r);
    CHECK(md == FormatCompareMd(CompareCensus(ours, ida, dump)));
    CHECK(md.find("unexplained") != std::string::npos && md.back() == '\n' && md.find('\r') == std::string::npos);
}

TEST(compare_parsers) {
    std::string err;
    auto c = ParseCensusTsv(FormatCensusTsv([] {
        Census x;
        x.rows.push_back(Row(0x22D3120, 0xBE0F39BFDF49C015ull, 74, true, {0x18343E0u, std::nullopt}, "T"));
        x.rows.push_back(Row(0x1300, 3, 29, false, {}, ""));
        return x;
    }()), &err);
    CHECK(c && c->rows.size() == 2);
    if (c && c->rows.size() == 2) {
        CHECK(c->rows[0].h.rva == 0x22D3120 && c->rows[0].h.id == 0xBE0F39BFDF49C015ull && c->rows[0].h.size == 74);
        CHECK(c->rows[0].lock && c->rows[0].callees.size() == 2 && !c->rows[0].callees[1] && c->rows[0].h.flags == "T");
        CHECK(c->rows[1].callees.empty() && c->rows[1].h.flags.empty());
    }
    // natives_census.tsv: CRLF, no leading zeros, `?` cells and an `x:` note in the last column.
    auto ida = ParseIdaCensus("# comment\r\n22D3120\tBE0F39BFDF49C015\t74\t1\t0\t1\t18343E0,ind\r\n"
                              "3182960\tC20A8681434F3F40\t?\t?\t?\t?\tx: not an IDA function\r\n"
                              "29CC170\t4566E7F729CA9638\t81\t0\t1\t1\t\r\n", &err);
    CHECK(ida && ida->size() == 3);
    if (ida && ida->size() == 3) {
        CHECK((*ida)[0].rva == 0x22D3120 && (*ida)[0].size == 74u && (*ida)[0].lock == true && !(*ida)[0].x);
        CHECK((*ida)[0].callees.size() == 2 && (*ida)[0].callees[0] == 0x18343E0u && !(*ida)[0].callees[1]);
        CHECK((*ida)[1].x && !(*ida)[1].size && !(*ida)[1].lock && (*ida)[1].callees.empty());
        CHECK((*ida)[2].lock == false && (*ida)[2].callees.empty());
    }
    auto d = ParseDumpTable("namespace\tname\thash64\thandler_rva\targc\tretc\tcall_sites\tscripts\n"
                            "\t_0x0027EED4D15D9B32\t0027EED4D15D9B32\t044608F0\t0\t0\t2\t1\n", &err);
    CHECK(d && d->size() == 1 && (*d)[0].id == 0x0027EED4D15D9B32ull && (*d)[0].rva == 0x044608F0);
    CHECK(!ParseIdaCensus("zz\tyy\n", &err) && !err.empty());
}

int main() {
    for (auto& t : Tests()) {
        int before = g_failed;
        t.fn();
        printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
    }
    printf("\n%zu tests, %d checks, %d failed\n", Tests().size(), g_checks, g_failed);
    return g_failed ? 1 : 0;
}
