// shvi_scan <exe> [--all] [--no-probe]
// shvi_scan <exe> --hash <va> <string>   call a pure u32(const char*, u32 seed) at an IDA VA
// shvi_scan <exe> --crossmap <json>       check that every {rva, id} in a VI crossmap matches the exe
//
// Offline signature check. Maps the exe from disk (pe::MapImageFile: sections at
// their RVAs, relocations applied), resolves the scan targets of every profile
// whose Matches() accepts the file name (or of every profile with --all), and
// prints each hit both as a mapped address and as the VA IDA shows. Then runs
// the functional probes that are safe on a cold image (pure functions only),
// e.g. string_hash against Joaat(). --no-probe skips them (and validators
// that call code will then reject their candidates).
//
// Nothing is hooked or written into the game; the exe is never started.
// Exit code: 0 = every required target of every checked profile resolved
// (and its probes passed), 1 = something missing, 2 = usage or I/O error.
#include "core/game_profile.h"
#include "core/joaat.h"
#include "core/native_table.h"
#include "core/pattern.h"
#include "core/pe.h"
#include "core/probes.h"
#include "games/gta6/vi_script.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstring>
#include <string>
#include <vector>

using namespace shvi;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: shvi_scan <exe> [--all] [--no-probe]\n");
        return 2;
    }
    std::wstring path = argv[1];
    bool all = false, probe = true;
    unsigned long long hashVa = 0;
    std::string hashArg;
    std::wstring crossmapPath;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--all")) all = true;
        else if (!wcscmp(argv[i], L"--no-probe")) probe = false;
        else if (!wcscmp(argv[i], L"--crossmap") && i + 1 < argc) crossmapPath = argv[++i];
        else if (!wcscmp(argv[i], L"--hash") && i + 2 < argc) {
            hashVa = wcstoull(argv[i + 1], nullptr, 16);
            for (const wchar_t* c = argv[i + 2]; *c; ++c) hashArg.push_back(static_cast<char>(*c));
            i += 2;
        }
    }

    std::string err;
    auto img = pe::MapImageFile(path, &err);
    if (!img) {
        fprintf(stderr, "cannot map image: %s\n", err.c_str());
        return 2;
    }
    printf("image: %ls\n  size 0x%zX, preferred base 0x%llX, timestamp %u, %zu sections, relocated %s\n",
           path.c_str(), img->module.image.size, static_cast<unsigned long long>(img->preferredBase),
           img->timeDateStamp, img->module.sections.size(), img->relocated ? "yes" : "no");

    if (hashVa) {
        std::uintptr_t fn = img->FromPreferred(static_cast<std::uintptr_t>(hashVa));
        if (!img->module.image.Contains(fn) || !img->MakeExecutable()) {
            fprintf(stderr, "bad address or cannot make image executable\n");
            return 2;
        }
        std::uint32_t h = 0;
        if (!probes::CallStringHash(fn, hashArg.c_str(), &h)) {
            fprintf(stderr, "call faulted\n");
            return 1;
        }
        printf("0x%llX(\"%s\", 0) = 0x%08X   (Joaat 0x%08X)\n", hashVa, hashArg.c_str(), h, Joaat(hashArg));
        return 0;
    }

    // VI crossmap check (TASKS T1.6): each entry's handler must carry the id the table says.
    if (!crossmapPath.empty()) {
        std::ifstream f(crossmapPath, std::ios::binary);
        if (!f) {
            fprintf(stderr, "cannot read crossmap\n");
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        // The same parser the core uses (core/native_table.h), so this checks what the game would load.
        NativeTable table;
        std::string perr;
        std::size_t skipped = 0;
        if (!ParseNativeTable(text, &table, &perr, &skipped)) {
            fprintf(stderr, "crossmap does not parse: %s\n", perr.c_str());
            return 2;
        }
        if (skipped) printf("  warning: %zu entries without a valid rva/id were skipped\n", skipped);
        if (table.peTimestamp != img->timeDateStamp)
            printf("  warning: the table is for build %u, this exe is %u (the core would refuse it)\n",
                   table.peTimestamp, img->timeDateStamp);
        int ok = 0, bad = 0;
        for (const auto& n : table.natives) {
            std::uintptr_t fn = img->module.image.begin + n.rva;
            std::optional<std::uint64_t> got;
            if (img->module.image.Contains(fn, 64))
                got = shvi::gta6::script::HandlerId(reinterpret_cast<const std::uint8_t*>(fn),
                                                    img->module.image.begin + img->module.image.size - fn);
            bool match = got && *got == n.id;
            match ? ++ok : ++bad;
            if (!match)
                printf("  crossmap %-28s rva 0x%X: id %s0x%016llX, table says 0x%016llX\n", n.name.c_str(), n.rva,
                       got ? "" : "missing ", got ? static_cast<unsigned long long>(*got) : 0ull,
                       static_cast<unsigned long long>(n.id));
        }
        printf("crossmap: %d entries match the exe, %d do not\n", ok, bad);
        return (bad || !ok) ? 1 : 0;
    }

    HostInfo host;
    host.exePath = path;
    auto slash = path.find_last_of(L"/\\");
    host.exeName = slash == std::wstring::npos ? path : path.substr(slash + 1);

    std::vector<std::unique_ptr<GameProfile>> profiles = CreateGta6Profiles();

    // Validators may call pure functions in the image (e.g. string_hash), so map
    // it executable unless probes are off. Nothing in it runs otherwise.
    bool executable = probe && img->MakeExecutable();
    if (probe && !executable) printf("  warning: could not make the image executable; probes will fail\n");
    scan::Scanner scanner(img->module);
    int checked = 0, failed = 0;
    for (auto& p : profiles) {
        auto targets = p->ScanTargets();
        if (targets.empty() || (!all && !p->Matches(host))) continue;
        ++checked;
        printf("\nprofile: %s\n", p->Name());
        scan::Report r = scan::ResolveAll(scanner, targets);
        for (const auto& t : targets) {
            auto it = r.hits.find(std::string(t.name));
            if (it == r.hits.end()) {
                printf("  %-20.*s %s\n", int(t.name.size()), t.name.data(), t.required ? "MISSING (required)" : "missing");
                continue;
            }
            std::uintptr_t v = it->second.value;
            bool isAddr = img->module.image.Contains(v);
            unsigned long long va = isAddr ? v - img->module.image.begin + img->preferredBase : v;
            const auto& c = t.candidates[static_cast<std::size_t>(it->second.candidate)];
            printf("  %-20.*s %s 0x%llX  [candidate %d: %.*s]\n", int(t.name.size()), t.name.data(),
                   isAddr ? "va" : "value", va, it->second.candidate, int(c.note.size()), c.note.data());
        }
        if (!r.Ok()) {
            ++failed;
            printf("  scan log:\n%s", r.log.c_str());
            continue;
        }
        std::uintptr_t hashFn = r.Get("string_hash");
        if (probe && hashFn) {
            std::string why;
            bool ok = executable && probes::StringHashMatchesJoaat(hashFn, &why);
            printf("  probe string_hash == Joaat: %s%s%s\n", ok ? "PASS" : "FAIL", why.empty() ? "" : " - ",
                   why.c_str());
            if (!ok) ++failed;
        }
        // GTA VI operand codec: print what the image holds and whether it is
        // what games/gta6/vi_script.h was written against (a new build may differ).
        std::uintptr_t keyA = r.Get("script_operand_key"), mul = r.Get("script_operand_mul");
        if (keyA && mul && img->module.image.Contains(keyA + 24) && img->module.image.Contains(mul)) {
            namespace sc = shvi::gta6::script;
            sc::OperandKeys k{};
            std::memcpy(k.a.data(), reinterpret_cast<const void*>(keyA), 8);
            std::memcpy(k.b.data(), reinterpret_cast<const void*>(keyA + 16), 8);
            k.mul = *reinterpret_cast<const std::uint8_t*>(mul);
            printf("  operand keys: a");
            for (auto b : k.a) printf(" %02X", b);
            printf(" / b");
            for (auto b : k.b) printf(" %02X", b);
            bool same = k.a == sc::kXboxKeys.a && k.b == sc::kXboxKeys.b && k.mul == sc::kXboxKeys.mul;
            printf(" / mul 0x%02X: %s\n", k.mul, same ? "match vi_script.h" : "DIFFER from vi_script.h");
            if (!same) ++failed;
        }
        // GTA VI native handlers: every one carries a self-identification
        // prologue (vi_script.h HandlerId). Count them; T1.5 found 6,805 on the
        // Xbox build. Zero means the prologue changed and HandlerId needs work.
        if (keyA) {
            namespace sc = shvi::gta6::script;
            std::size_t hits = 0;
            std::vector<std::uint64_t> ids;
            for (const auto& reg : img->module.ExecutableRegions()) {
                auto* b = reinterpret_cast<const std::uint8_t*>(reg.begin);
                for (std::size_t i = 0; i + 4 <= reg.size; ++i) {
                    if (b[i] != 0x83 || b[i + 1] != 0x79 || b[i + 2] != 0x10 || b[i + 3] != 0x00) continue;
                    if (auto id = sc::HandlerId(b + i, reg.size - i)) ids.push_back(*id), ++hits;
                }
            }
            std::sort(ids.begin(), ids.end());
            std::size_t distinct = static_cast<std::size_t>(std::unique(ids.begin(), ids.end()) - ids.begin());
            printf("  native handler ids: %zu prologues, %zu distinct ids\n", hits, distinct);
            if (distinct == 0) ++failed;
        }
    }
    if (!checked) {
        printf("\nno profile matched (use --all to try every profile's targets)\n");
        return 1;
    }
    printf("\n%d profile(s) checked, %d with problems\n", checked, failed);
    return failed ? 1 : 0;
}
