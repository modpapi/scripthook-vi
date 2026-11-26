#include "pattern.h"

#include <cstring>

namespace shvi::scan {

std::optional<Pattern> Pattern::Parse(std::string_view ida) {
    Pattern p;
    std::size_t i = 0;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (i < ida.size()) {
        char c = ida[i];
        if (c == ' ') { ++i; continue; }
        if (c == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            ++i;
            if (i < ida.size() && ida[i] == '?') ++i;  // accept "??"
            continue;
        }
        if (i + 1 >= ida.size()) return std::nullopt;
        int hi = hex(ida[i]), lo = hex(ida[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        p.bytes.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
        p.mask.push_back(1);
        i += 2;
    }
    if (p.bytes.empty()) return std::nullopt;
    return p;
}

std::vector<std::uintptr_t> FindAll(const Pattern& p, std::span<const Region> regions, std::size_t maxHits) {
    std::vector<std::uintptr_t> hits;
    // Anchor on the first exact byte and let memchr do the skipping.
    std::size_t anchor = 0;
    while (anchor < p.size() && !p.mask[anchor]) ++anchor;
    if (anchor == p.size()) return hits;  // all wildcards: refuse

    for (const Region& r : regions) {
        if (r.size < p.size()) continue;
        auto base = reinterpret_cast<const std::uint8_t*>(r.begin);
        const std::uint8_t* last = base + (r.size - p.size());
        const std::uint8_t* cur = base;
        while (cur <= last) {
            auto a = static_cast<const std::uint8_t*>(
                std::memchr(cur + anchor, p.bytes[anchor], static_cast<std::size_t>(last - cur) + 1));
            if (!a) break;
            const std::uint8_t* start = a - anchor;
            bool ok = true;
            for (std::size_t k = 0; k < p.size(); ++k) {
                if (p.mask[k] && start[k] != p.bytes[k]) { ok = false; break; }
            }
            if (ok) {
                hits.push_back(reinterpret_cast<std::uintptr_t>(start));
                if (hits.size() >= maxHits) return hits;
            }
            cur = start + 1;
        }
    }
    return hits;
}

std::optional<std::uintptr_t> Scanner::Apply(std::uintptr_t at, const std::vector<Op>& ops, bool* isValue) const {
    std::uintptr_t cur = at;
    *isValue = false;
    for (const Op& op : ops) {
        switch (op.kind) {
        case OpKind::Add:
            cur += static_cast<std::intptr_t>(op.value);
            break;
        case OpKind::Rip: {
            if (!image_.Contains(cur, 4)) return std::nullopt;
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(cur), 4);
            cur = cur + 4 + op.value + rel;
            break;
        }
        case OpKind::Deref:
            if (!image_.Contains(cur, 8)) return std::nullopt;
            std::memcpy(&cur, reinterpret_cast<const void*>(cur), 8);
            break;
        case OpKind::ReadI32: {
            if (!image_.Contains(cur, 4)) return std::nullopt;
            std::int32_t v;
            std::memcpy(&v, reinterpret_cast<const void*>(cur), 4);
            *isValue = true;
            return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(v));
        }
        }
    }
    return cur;
}

std::optional<Hit> Scanner::Resolve(const Target& t, std::string* log) const {
    auto note = [&](const std::string& s) {
        if (log) *log += "  [" + std::string(t.name) + "] " + s + "\n";
    };
    for (int ci = 0; ci < static_cast<int>(t.candidates.size()); ++ci) {
        const Candidate& c = t.candidates[ci];
        auto pat = Pattern::Parse(c.pattern);
        if (!pat) {
            note("candidate " + std::to_string(ci) + ": unparsable pattern");
            continue;
        }
        auto matches = FindAll(*pat, code_, 16);
        std::vector<std::uintptr_t> survivors;
        for (auto m : matches) {
            bool isValue = false;
            auto v = Apply(m, c.ops, &isValue);
            if (!v) continue;
            if (t.validate && !t.validate(*v)) continue;
            survivors.push_back(*v);
        }
        // Several raw matches that all resolve to the same target are fine
        // (e.g. many "lea rcx, [table]" sites).
        bool unique = !survivors.empty();
        for (auto v : survivors) unique = unique && v == survivors.front();
        if (unique) {
            note("candidate " + std::to_string(ci) + " matched (" + std::to_string(matches.size()) + " raw hits)");
            return Hit{survivors.front(), ci};
        }
        note("candidate " + std::to_string(ci) + ": " + std::to_string(matches.size()) + " raw hits, " +
             std::to_string(survivors.size()) + (survivors.empty() ? " valid -> next" : " valid but ambiguous -> next"));
    }
    return std::nullopt;
}

Report ResolveAll(const Scanner& s, std::span<const Target> targets) {
    Report r;
    for (const Target& t : targets) {
        if (auto h = s.Resolve(t, &r.log)) {
            r.hits.emplace(std::string(t.name), *h);
        } else if (t.required) {
            r.missingRequired.emplace_back(t.name);
        }
    }
    return r;
}

}  // namespace shvi::scan
