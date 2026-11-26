#include "pdata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace shvi::nativedb {

namespace {
constexpr int kMaxHops = 32;
constexpr std::uint8_t kChainInfo = 0x4;  // UNW_FLAG_CHAININFO

std::uint32_t Read32(const ImageView& img, std::uint32_t rva) {
    std::uint32_t v;
    std::memcpy(&v, img.At(rva), 4);
    return v;
}

// The parent RUNTIME_FUNCTION's RVA for an entry with this unwind RVA, or 0 when it is a root.
// Sets *bad when the unwind data can't be read.
std::uint32_t ParentOf(const ImageView& img, std::uint32_t unwind, bool* bad) {
    if (unwind & 1) return unwind & ~1u;  // chained by reference: points at a RUNTIME_FUNCTION
    if (!img.Readable(unwind, 4)) { *bad = true; return 0; }
    std::uint8_t flags = img.At(unwind)[0] >> 3;
    if (!(flags & kChainInfo)) return 0;
    std::uint8_t count = img.At(unwind)[2];
    return unwind + 4 + 2 * ((count + 1u) & ~1u);
}
}  // namespace

PdataResult ReadPdata(const ImageView& img) {
    PdataResult r;
    const auto& dir = img.dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.VirtualAddress || !dir.Size) { r.error = "no .pdata (exception directory empty)"; return r; }
    if (!img.Readable(dir.VirtualAddress, dir.Size)) { r.error = "exception directory outside the image"; return r; }
    std::uint32_t n = dir.Size / 12;
    r.fns.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t at = dir.VirtualAddress + 12 * i;
        RuntimeFn f{Read32(img, at), Read32(img, at + 4), Read32(img, at + 8), 0};
        if (f.begin == 0 && f.end == 0) continue;  // zero padding at the table end
        f.primary = f.begin;
        r.fns.push_back(f);
    }
    std::sort(r.fns.begin(), r.fns.end(), [](const RuntimeFn& a, const RuntimeFn& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    for (auto& f : r.fns) {
        std::uint32_t unwind = f.unwind, begin = f.begin;
        std::set<std::uint32_t> seen{f.begin};
        bool bad = false, ok = true;
        for (int hop = 0;; ++hop) {
            std::uint32_t parent = ParentOf(img, unwind, &bad);
            if (bad || parent == 0) break;
            if (hop >= kMaxHops || !img.Readable(parent, 12)) { ok = false; break; }
            begin = Read32(img, parent);
            unwind = Read32(img, parent + 8);
            if (!seen.insert(begin).second) { ok = false; break; }
        }
        if (!ok) {
            if (r.error.empty()) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unwind chain too deep at 0x%08X", f.begin);
                r.error = buf;
            }
            continue;
        }
        f.primary = begin;
    }
    return r;
}

const RuntimeFn* FindContaining(const std::vector<RuntimeFn>& fns, std::uint32_t rva) {
    auto it = std::upper_bound(fns.begin(), fns.end(), rva,
                               [](std::uint32_t x, const RuntimeFn& f) { return x < f.begin; });
    if (it == fns.begin()) return nullptr;
    --it;
    return rva < it->end ? &*it : nullptr;
}

std::vector<const RuntimeFn*> ChunksOf(const std::vector<RuntimeFn>& fns, std::uint32_t primary) {
    std::vector<const RuntimeFn*> out;
    for (const auto& f : fns)
        if (f.primary == primary) out.push_back(&f);  // fns is sorted by begin already
    return out;
}

}  // namespace shvi::nativedb
