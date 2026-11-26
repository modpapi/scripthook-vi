// shvi_nativedb: builds the native database for one exe without IDA.
//   shvi_nativedb census --exe <exe> --out <file.tsv>
//   shvi_nativedb build  --exe <exe> --out <dir> [--ida <tsv> --dump <tsv>]
//                        -> <dir>\<PE timestamp>\{census.tsv, census_vs_ida.md, manifest.json}
//   shvi_nativedb compare-census --census <ours.tsv> --ida <tsv> --dump <tsv> --out <md>
//   shvi_nativedb disasm --exe <exe> --rva <hex> [--len <hex>]   (Zydis listing, for reading a row's bytes)
// Exit: 0 ok, 1 analysis failure (or unexplained / id-set / rva-vs-dump rows), 2 usage / I/O.
// Outputs are deterministic: same inputs, same bytes (spec §7.2).
#include "census.h"
#include "compare.h"
#include "core/sha256.h"
#include "disasm.h"
#include "image.h"
#include "manifest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace shvi;
using namespace shvi::nativedb;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: shvi_nativedb census --exe <exe> --out <file.tsv>\n"
                 "       shvi_nativedb build --exe <exe> --out <dir> [--ida <natives_census.tsv> --dump <natives_xbox.tsv>]\n"
                 "       shvi_nativedb compare-census --census <ours.tsv> --ida <tsv> --dump <tsv> --out <md>\n");
    return 2;
}

int Fail(int code, const std::string& why) {
    std::fprintf(stderr, "shvi_nativedb: %s\n", why.c_str());
    return code;
}

bool ReadAll(const std::wstring& path, std::string* out) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), {});
    return !f.bad();
}

struct Analysis {
    Census census;
    BuildInfo build;
    InputRec exe;
};

// 0 ok; else the exit code, with the reason already printed.
int Analyse(const std::wstring& exePath, Analysis* a) {
    std::string bytes;
    if (!ReadAll(exePath, &bytes)) return Fail(2, "cannot read the exe");
    a->exe = {"exe", Utf8Basename(exePath), sha256::Hex(bytes.data(), bytes.size()), bytes.size(), ""};
    bytes.clear();
    bytes.shrink_to_fit();

    std::string err;
    auto m = pe::MapImageFile(exePath, &err);
    if (!m) return Fail(err == "cannot open file" ? 2 : 1, "cannot map the exe: " + err);
    ImageView v = ViewOf(*m);
    auto c = RunCensus(v, &err);
    if (!c) return Fail(1, "census failed: " + err);
    a->census = std::move(*c);
    a->build.key = v.timeDateStamp;
    a->build.machine = v.machine;
    a->build.imageSize = v.size;
    a->build.handlers = static_cast<std::uint32_t>(a->census.rows.size());
    a->build.lock = a->census.lock;
    return 0;
}

void Summary(const Analysis& a) {
    const LockInfo& l = a.census.lock;
    std::printf("census: %zu handlers, lock global 0x%08X acquire 0x%08X release 0x%08X (%u of %u), %zu notes\n",
                a.census.rows.size(), l.global, l.acquire, l.release, l.votes, l.voters, a.census.notes.size());
    for (const auto& n : a.census.notes) std::printf("  note: %s\n", n.c_str());
}

int CmdCensus(const std::map<std::string, std::wstring>& opt) {
    if (!opt.count("--exe") || !opt.count("--out")) return Usage();
    Analysis a;
    if (int rc = Analyse(opt.at("--exe"), &a)) return rc;
    if (!WriteFileAtomic(opt.at("--out"), FormatCensusTsv(a.census))) return Fail(2, "cannot write the census");
    Summary(a);
    return 0;
}

// The IDA census and dump table, read and parsed; each also becomes a manifest input.
struct RefTables {
    std::vector<IdaRow> ida;
    std::vector<DumpRow> dump;
    std::vector<InputRec> inputs;
};

int ReadRefTables(const std::wstring& idaPath, const std::wstring& dumpPath, RefTables* t) {
    std::string ida, dump, err;
    if (!ReadAll(idaPath, &ida) || !ReadAll(dumpPath, &dump)) return Fail(2, "cannot read an input table");
    auto i = ParseIdaCensus(ida, &err);
    if (!i) return Fail(2, "bad IDA census: " + err);
    auto d = ParseDumpTable(dump, &err);
    if (!d) return Fail(2, "bad dump table: " + err);
    t->ida = std::move(*i);
    t->dump = std::move(*d);
    t->inputs.push_back({"census", Utf8Basename(idaPath), sha256::Hex(ida.data(), ida.size()), ida.size(), ""});
    t->inputs.push_back({"dump", Utf8Basename(dumpPath), sha256::Hex(dump.data(), dump.size()), dump.size(), ""});
    return 0;
}

void PrintCompare(const CompareResult& r) {
    for (const auto& cat : CompareCategories()) std::printf("%-18s %zu\n", cat.c_str(), r.byCategory.at(cat).size());
}

// build --exe <exe> --out <dir> [--ida <natives_census.tsv> --dump <natives_xbox.tsv>]
// With --ida/--dump the folder also gets census_vs_ida.md, and the run fails (exit 1, files kept)
// when it has unexplained, id-set or rva-vs-dump rows.
int CmdBuild(const std::map<std::string, std::wstring>& opt) {
    if (!opt.count("--exe") || !opt.count("--out") || opt.count("--ida") != opt.count("--dump")) return Usage();
    RefTables refs;
    bool compare = opt.count("--ida") != 0;
    if (compare)
        if (int rc = ReadRefTables(opt.at("--ida"), opt.at("--dump"), &refs)) return rc;
    Analysis a;
    if (int rc = Analyse(opt.at("--exe"), &a)) return rc;  // nothing is written on failure

    std::filesystem::path dir = std::filesystem::path(opt.at("--out")) / std::to_wstring(a.build.key);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Fail(2, "cannot create the output folder");

    std::vector<OutputRec> outs;
    auto emit = [&](const char* name, const std::string& bytes) {
        if (!WriteFileAtomic((dir / name).wstring(), bytes)) return false;
        outs.push_back({name, sha256::Hex(bytes.data(), bytes.size()), bytes.size()});
        return true;
    };
    if (!emit("census.tsv", FormatCensusTsv(a.census))) return Fail(2, "cannot write census.tsv");
    std::optional<CompareResult> cmp;
    if (compare) {
        cmp = CompareCensus(a.census, refs.ida, refs.dump);
        if (!emit("census_vs_ida.md", FormatCompareMd(*cmp))) return Fail(2, "cannot write census_vs_ida.md");
    }
    std::vector<InputRec> inputs = refs.inputs;
    inputs.push_back(a.exe);
    // The manifest is written last and does not list itself.
    if (!WriteFileAtomic((dir / "manifest.json").wstring(), FormatManifest(a.build, inputs, outs)))
        return Fail(2, "cannot write manifest.json");
    Summary(a);
    if (cmp) {
        PrintCompare(*cmp);
        if (cmp->failures) return Fail(1, "census_vs_ida.md has unexplained, id-set or rva-vs-dump rows");
    }
    return 0;
}

int CmdCompare(const std::map<std::string, std::wstring>& opt) {
    for (const char* k : {"--census", "--ida", "--dump", "--out"})
        if (!opt.count(k)) return Usage();
    std::string ours, err;
    if (!ReadAll(opt.at("--census"), &ours)) return Fail(2, "cannot read the census");
    auto oursRows = ParseCensusTsv(ours, &err);
    if (!oursRows) return Fail(2, "bad census: " + err);
    RefTables refs;
    if (int rc = ReadRefTables(opt.at("--ida"), opt.at("--dump"), &refs)) return rc;
    CompareResult r = CompareCensus(*oursRows, refs.ida, refs.dump);
    if (!WriteFileAtomic(opt.at("--out"), FormatCompareMd(r))) return Fail(2, "cannot write the report");
    PrintCompare(r);
    return r.failures ? 1 : 0;  // unexplained, id-set or rva-vs-dump rows
}

// disasm --exe <exe> --rva <hex> [--len <hex>]: linear Zydis listing, for reading the bytes behind a census row.
int CmdDisasm(const std::map<std::string, std::wstring>& opt) {
    if (!opt.count("--exe") || !opt.count("--rva")) return Usage();
    std::string err;
    auto m = pe::MapImageFile(opt.at("--exe"), &err);
    if (!m) return Fail(2, "cannot map the exe: " + err);
    ImageView v = ViewOf(*m);
    std::uint32_t rva = static_cast<std::uint32_t>(std::wcstoul(opt.at("--rva").c_str(), nullptr, 16));
    std::uint32_t len = opt.count("--len") ? static_cast<std::uint32_t>(std::wcstoul(opt.at("--len").c_str(), nullptr, 16)) : 0x40;
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    for (std::uint32_t p = rva; p < rva + len;) {
        std::uint32_t avail = v.Avail(p);
        ZydisDecodedInstruction zi;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!avail || !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, v.At(p), avail < 15 ? avail : 15, &zi, ops))) {
            std::printf("%08X  ?? %02X\n", p, avail ? *v.At(p) : 0);
            ++p;
            continue;
        }
        char text[256];
        // Runtime address = RVA, so targets print as RVAs.
        ZydisFormatterFormatInstruction(&fmt, &zi, ops, zi.operand_count_visible, text, sizeof(text), p, nullptr);
        std::printf("%08X  %s\n", p, text);
        p += zi.length;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return Usage();
    std::wstring cmd = argv[1];
    std::map<std::string, std::wstring> opt;
    for (int i = 2; i < argc; i += 2) {
        std::wstring k = argv[i];
        if (k.size() < 3 || k.compare(0, 2, L"--") != 0 || i + 1 >= argc) return Usage();
        std::string name;
        for (wchar_t ch : k) {
            if (ch > 0x7F) return Usage();  // option names are ASCII
            name += static_cast<char>(ch);
        }
        opt[name] = argv[i + 1];
    }
    if (cmd == L"census") return CmdCensus(opt);
    if (cmd == L"build") return CmdBuild(opt);
    if (cmd == L"compare-census") return CmdCompare(opt);
    if (cmd == L"disasm") return CmdDisasm(opt);
    return Usage();
}
