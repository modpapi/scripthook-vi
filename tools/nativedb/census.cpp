#include "census.h"

#include "disasm.h"
#include "games/gta6/vi_script.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <map>

namespace shvi::nativedb {

namespace {

constexpr std::uint32_t kLeafBack = 0x40;     // how furthest before the cmp a leaf start may be
constexpr std::uint32_t kLeafMax = 0x1000;    // leaf extent bound
constexpr std::uint32_t kIdWindow = 24;       // HandlerId's prologue window (flag M)
// Bytes HandlerId may read from the cmp: cmp(4) + js search (8 + 6) + cold search (8 + 10).
constexpr std::uint32_t kIdReach = 4 + 8 + 6 + 8 + 10;

std::string Note(std::uint32_t rva, const char* why) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "skipped 0x%08X: %s", rva, why);
    return buf;
}

bool IsPad(std::uint8_t b) { return b == 0xCC || b == 0x90 || b == 0xC3; }

// Linear decode from a to exactly `cmp`, with no ret/jmp/int3 on the way.
bool DecodesOnto(const ImageView& img, std::uint32_t a, std::uint32_t cmp) {
    std::uint32_t p = a;
    while (p < cmp) {
        auto i = Decode(img, p);
        if (!i) return false;
        if (i->flow == Insn::Flow::Ret || i->flow == Insn::Flow::Jmp || i->flow == Insn::Flow::Int3) return false;
        p += i->len;
    }
    return p == cmp;
}

// Leaf extent: stop after a ret / unconditional jmp once past the furthest forward conditional
// branch target, or when its fall-through is int3/nop padding (spec §7.4 item 3). Unconditional
// jmp targets don't extend the leaf: a forward one is a tail call (Xbox 0x3182960 `jmp 0x3182CA0`),
// and code it would skip is only reachable through an earlier jcc, which already counted.
std::optional<std::uint32_t> LeafEnd(const ImageView& img, std::uint32_t rva, const char** why) {
    std::uint32_t p = rva, furthest = rva;
    while (p - rva < kLeafMax) {
        auto i = Decode(img, p);
        if (!i) { *why = "leaf decode failed"; return std::nullopt; }
        p += i->len;
        if (i->flow == Insn::Flow::Jcc && i->direct && i->target >= rva && i->target - rva < kLeafMax &&
            i->target > furthest)
            furthest = i->target;
        if (i->flow == Insn::Flow::Ret || i->flow == Insn::Flow::Jmp) {
            bool padNext = img.Readable(p, 1) && (*img.At(p) == 0xCC || *img.At(p) == 0x90);
            if (p > furthest || padNext) return p;
        }
    }
    *why = "leaf extent over 0x1000 bytes";
    return std::nullopt;
}

std::string SortedFlags(std::string f) {
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end()), f.end());
    return f;
}

}  // namespace

HandlerScan FindHandlers(const ImageView& img, const std::vector<RuntimeFn>& pdata) {
    HandlerScan out;

    // Chained chunks by primary, and every .pdata end (leaf start candidates), built once.
    std::map<std::uint32_t, std::vector<Range>> chunks;
    std::vector<std::uint32_t> ends;
    ends.reserve(pdata.size());
    for (const auto& f : pdata) {
        if (f.primary != f.begin) chunks[f.primary].push_back({f.begin, f.end});  // pdata is sorted by begin
        ends.push_back(f.end);
    }
    std::sort(ends.begin(), ends.end());
    auto isEnd = [&](std::uint32_t a) { return std::binary_search(ends.begin(), ends.end(), a); };

    std::map<std::uint64_t, std::uint32_t> idAt;  // id -> cmp rva
    for (const auto& sec : img.sections) {
        if (!sec.Executable() || sec.range.size < 4) continue;
        const std::uint32_t sb = static_cast<std::uint32_t>(sec.range.begin);
        const std::uint32_t se = sb + static_cast<std::uint32_t>(sec.range.size);
        for (std::uint32_t c = sb; c + 4 <= se; ++c) {
            const std::uint8_t* p = img.At(c);
            if (!(p[0] == 0x83 && p[1] == 0x79 && p[2] == 0x10 && p[3] == 0x00)) continue;
            const std::uint32_t avail = se - c;
            auto id = shvi::gta6::script::HandlerId(p, avail);
            if (!id) {
                if (avail < kIdReach && avail >= 6 && (p[4] == 0x78 || (p[4] == 0x0F && p[5] == 0x88)))
                    out.notes.push_back(Note(c, "handler bytes run past the section end"));
                continue;
            }

            Handler h;
            h.cmpRva = c;
            h.id = *id;
            std::string flags;
            if (const RuntimeFn* f = FindContaining(pdata, c)) {
                h.rva = f->primary;
                const RuntimeFn* prim = FindContaining(pdata, f->primary);
                if (!prim || prim->begin != f->primary) {
                    out.notes.push_back(Note(c, "chained unwind points at no .pdata entry"));
                    continue;
                }
                h.size = prim->end - prim->begin;
                h.code.push_back({prim->begin, prim->end});
                if (auto it = chunks.find(prim->begin); it != chunks.end())
                    h.code.insert(h.code.end(), it->second.begin(), it->second.end());
            } else {
                flags += 'L';
                std::uint32_t start = 0;
                bool found = false;
                for (std::uint32_t a = c & ~0xFu; a + kLeafBack >= c && a > sb; a -= 0x10) {
                    if ((isEnd(a) || IsPad(*img.At(a - 1))) && DecodesOnto(img, a, c)) {
                        start = a;
                        found = true;
                        break;
                    }
                    if (a < 0x10) break;
                }
                if (!found) {
                    start = c;
                    flags += 'S';
                }
                const char* why = "";
                auto end = LeafEnd(img, start, &why);
                if (!end) {
                    out.notes.push_back(Note(c, why));
                    continue;
                }
                h.rva = start;
                h.size = *end - start;
                h.code.push_back({start, *end});
            }
            if (h.cmpRva - h.rva > kIdWindow) flags += 'M';
            h.flags = SortedFlags(flags);

            auto [it, fresh] = idAt.emplace(h.id, c);
            if (!fresh && out.error.empty()) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "duplicate id 0x%016llX at 0x%08X and 0x%08X",
                              static_cast<unsigned long long>(h.id), it->second, c);
                out.error = buf;
            }
            out.handlers.push_back(std::move(h));
        }
    }
    std::sort(out.handlers.begin(), out.handlers.end(),
              [](const Handler& a, const Handler& b) { return a.rva != b.rva ? a.rva < b.rva : a.cmpRva < b.cmpRva; });
    std::sort(out.notes.begin(), out.notes.end());
    return out;
}

namespace {

// A call or jmp seen while walking a handler, with what rcx held at that point.
struct Flow {
    std::uint32_t rva = 0;
    bool call = false;  // else jmp
    bool direct = false;
    std::uint32_t target = 0;
    std::optional<std::uint32_t> rcx;  // rip-relative address rcx holds, if known
};

constexpr std::uint16_t kVolatile = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);
constexpr int kRcx = 1;

// Linear walk over the handler's code ranges in order, tracking which GPRs hold a
// [rip + X] address (set by lea, copied by mov r64,r64, cleared by any other write,
// by a call for volatile registers, and after a ret / unconditional jmp).
std::vector<Flow> Walk(const ImageView& img, const Handler& h, std::vector<std::string>* notes) {
    std::vector<Flow> out;
    std::array<std::optional<std::uint32_t>, 16> holds{};
    for (const Range& r : h.code) {
        holds = {};
        for (std::uint32_t p = r.begin; p < r.end;) {
            auto i = Decode(img, p);
            if (!i) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "decode stopped 0x%08X in handler 0x%08X", p, h.rva);
                notes->push_back(buf);
                break;
            }
            if ((i->flow == Insn::Flow::Call || i->flow == Insn::Flow::Jmp))
                out.push_back({i->rva, i->flow == Insn::Flow::Call, i->direct, i->target, holds[kRcx]});

            int d = Gpr64Index(i->dst), s = Gpr64Index(i->src);
            if (i->mnem == ZYDIS_MNEMONIC_LEA && d >= 0 && i->ripMem) {
                holds[d] = i->memTarget;
            } else if (i->mnem == ZYDIS_MNEMONIC_MOV && d >= 0 && s >= 0) {
                holds[d] = holds[s];
            } else {
                for (int g = 0; g < 16; ++g)
                    if (i->writes & (1u << g)) holds[g].reset();
            }
            if (i->flow == Insn::Flow::Call)
                for (int g = 0; g < 16; ++g)
                    if (kVolatile & (1u << g)) holds[g].reset();
            if (i->flow == Insn::Flow::Ret || i->flow == Insn::Flow::Jmp) holds = {};
            p += i->len;
        }
    }
    std::sort(out.begin(), out.end(), [](const Flow& a, const Flow& b) { return a.rva < b.rva; });
    return out;
}

bool InCode(const Handler& h, std::uint32_t rva) {
    for (const Range& r : h.code)
        if (rva >= r.begin && rva < r.end) return true;
    return false;
}

}  // namespace

std::optional<Census> RunCensus(const ImageView& img, std::string* err) {
    auto fail = [&](std::string why) -> std::optional<Census> {
        if (err) *err = std::move(why);
        return std::nullopt;
    };
    if (!img.pe32plus) return fail("not a PE32+ image");
    PdataResult pd = ReadPdata(img);
    if (!pd.error.empty()) return fail(pd.error);
    HandlerScan scan = FindHandlers(img, pd.fns);
    if (!scan.error.empty()) return fail(scan.error);
    if (scan.handlers.empty()) return fail("no handlers found");

    Census c;
    c.notes = std::move(scan.notes);
    std::vector<std::vector<Flow>> flows;
    flows.reserve(scan.handlers.size());
    for (const Handler& h : scan.handlers) flows.push_back(Walk(img, h, &c.notes));

    // Acquire: each handler's first direct call made while rcx holds some X votes (X, target).
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> acq;
    std::uint32_t voters = 0;
    for (const auto& fl : flows) {
        for (const Flow& f : fl) {
            if (f.call && f.direct && f.rcx) {
                ++acq[{*f.rcx, f.target}];
                ++voters;
                break;
            }
        }
    }
    if (voters == 0) return fail("lock: no voters");
    auto best = acq.begin();  // map order = (X, target) ascending, so the first max wins ties
    for (auto it = acq.begin(); it != acq.end(); ++it)
        if (it->second > best->second) best = it;
    if (static_cast<std::uint64_t>(best->second) * 10 < static_cast<std::uint64_t>(voters) * 9) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "lock: no 90%% majority (%u of %u)", best->second, voters);
        return fail(buf);
    }
    c.lock.global = best->first.first;
    c.lock.acquire = best->first.second;
    c.lock.votes = best->second;
    c.lock.voters = voters;

    // Release: among handlers that call acquire, every other direct call/jmp made while rcx holds X.
    auto callsAcquire = [&](const std::vector<Flow>& fl) {
        for (const Flow& f : fl)
            if (f.call && f.direct && f.target == c.lock.acquire) return true;
        return false;
    };
    std::map<std::uint32_t, std::uint32_t> rel;
    for (const auto& fl : flows) {
        if (!callsAcquire(fl)) continue;
        for (const Flow& f : fl)
            if (f.direct && f.target != c.lock.acquire && f.rcx == c.lock.global) ++rel[f.target];
    }
    std::uint32_t relVotes = 0;
    for (const auto& [t, n] : rel)
        if (n > relVotes) { relVotes = n; c.lock.release = t; }

    for (std::size_t k = 0; k < scan.handlers.size(); ++k) {
        CensusRow row;
        row.h = std::move(scan.handlers[k]);
        row.lock = callsAcquire(flows[k]);
        bool tail = false;
        for (const Flow& f : flows[k]) {
            if (f.direct && (f.target == c.lock.acquire || (c.lock.release && f.target == c.lock.release))) continue;
            if (f.call) {
                row.callees.push_back(f.direct ? std::optional<std::uint32_t>(f.target) : std::nullopt);
            } else if (f.direct && !InCode(row.h, f.target)) {
                row.callees.push_back(f.target);
                tail = true;
            }
        }
        if (tail) row.h.flags = SortedFlags(row.h.flags + 'T');
        c.rows.push_back(std::move(row));
    }
    std::sort(c.notes.begin(), c.notes.end());
    return c;
}

std::string FormatCensusTsv(const Census& c) {
    std::string s = "# shvi_nativedb census schema 1\nrva\tid64\tsize\tlock\tcallees\tflags\n";
    char buf[64];
    for (const CensusRow& r : c.rows) {
        std::snprintf(buf, sizeof(buf), "%08X\t%016llX\t", r.h.rva, static_cast<unsigned long long>(r.h.id));
        s += buf;
        auto d = std::to_chars(buf, buf + sizeof(buf), r.h.size);
        s.append(buf, d.ptr);
        s += r.lock ? "\t1\t" : "\t0\t";
        for (std::size_t i = 0; i < r.callees.size(); ++i) {
            if (i) s += ',';
            if (r.callees[i]) {
                std::snprintf(buf, sizeof(buf), "%08X", *r.callees[i]);
                s += buf;
            } else {
                s += "ind";
            }
        }
        s += '\t';
        s += r.h.flags;
        s += '\n';
    }
    return s;
}

}  // namespace shvi::nativedb
