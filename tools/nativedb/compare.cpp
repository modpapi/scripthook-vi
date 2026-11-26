#include "compare.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace shvi::nativedb {

namespace {

constexpr std::uint32_t kMidWindow = 0x40;  // the dump's rva is at most this far before IDA's cmp rva

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t p = 0;
    for (;;) {
        std::size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
        if (q == std::string_view::npos) return out;
        p = q + 1;
    }
}

std::vector<std::string_view> Lines(std::string_view t) {
    std::vector<std::string_view> out;
    for (auto l : Split(t, '\n')) {
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

template <class T>
bool Hex(std::string_view s, T* v) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v, 16);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool Dec(std::string_view s, std::uint32_t* v) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v, 10);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool Callees(std::string_view s, std::vector<std::optional<std::uint32_t>>* out) {
    out->clear();
    if (s.empty()) return true;
    for (auto c : Split(s, ',')) {
        if (c == "ind") { out->push_back(std::nullopt); continue; }
        std::uint32_t v;
        if (!Hex(c, &v)) return false;
        out->push_back(v);
    }
    return true;
}

std::string Bad(std::size_t line, const char* what) {
    return "line " + std::to_string(line) + ": " + what;
}

std::string H32(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}
std::string H64(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llX", static_cast<unsigned long long>(v));
    return b;
}
std::string List(const std::vector<std::optional<std::uint32_t>>& c) {
    if (c.empty()) return "(none)";
    std::string s;
    for (std::size_t i = 0; i < c.size(); ++i) s += (i ? "," : "") + (c[i] ? H32(*c[i]) : std::string("ind"));
    return s;
}

// `small` is `big` with some elements deleted (order kept).
bool Subsequence(const std::vector<std::optional<std::uint32_t>>& small, const std::vector<std::optional<std::uint32_t>>& big) {
    std::size_t j = 0;
    for (const auto& x : big)
        if (j < small.size() && small[j] == x) ++j;
    return j == small.size();
}

bool SameMultiset(std::vector<std::optional<std::uint32_t>> a, std::vector<std::optional<std::uint32_t>> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

struct Explanation { const char* name; const char* text; };
const Explanation kExplain[] = {
    {"ida-missing", "One of the rows IDA has no function for (`x` in the IDA census, every field `?`). Ours has "
                    "values read from `.pdata` or, for a leaf, from linear decoding; the dump's rva agrees with ours "
                    "where the dump has the native (uncalled natives have no dump row)."},
    {"ida-mid-function", "The IDA census rva for an `x` row is the `cmp dword [rcx+10h], 0`, not the function start. "
                         "Ours is the start (the `.pdata` begin, or the leaf start found by decoding), at most 0x40 "
                         "bytes earlier, and equals the dump's rva where it has one."},
    {"size-chunking", "Same function, different size: IDA's main chunk (`end_ea - start_ea`) was split or merged "
                      "differently from the primary `.pdata` range ours uses."},
    {"callees-truncated", "IDA's list (q26) keeps the first 6 command callees; ours is not truncated and starts "
                          "with the same 6."},
    {"callees-tailcall", "Ours also counts a `jmp` out of the function as a call (flag `T`); IDA's list is ours "
                         "without those tail-call targets."},
    {"callees-order", "Same callees, different order: IDA lists the head chunk before the tails; ours is in address "
                      "order."},
    {"start-unresolved", "Leaf rows whose start could not be decoded back from the cmp (flag `S`); ours uses the cmp "
                         "as the start. Listed for review (spec §8), not a difference by itself."},
    {"id-set", "An id present on one side only. Must be empty."},
    {"rva-vs-dump", "Our rva differs from the dump table's handler rva. Must be empty."},
    {"unexplained", "A difference none of the categories above explains. Must be empty."},
};

}  // namespace

const std::vector<std::string>& CompareCategories() {
    static const std::vector<std::string> c = [] {
        std::vector<std::string> v;
        for (const auto& e : kExplain) v.push_back(e.name);
        return v;
    }();
    return c;
}

std::optional<Census> ParseCensusTsv(std::string_view text, std::string* err) {
    Census c;
    auto lines = Lines(text);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        auto l = lines[n];
        if (l[0] == '#' || l.substr(0, 4) == "rva\t") continue;
        auto f = Split(l, '\t');
        CensusRow r;
        std::uint32_t lock = 0;
        if (f.size() != 6 || !Hex(f[0], &r.h.rva) || !Hex(f[1], &r.h.id) || !Dec(f[2], &r.h.size) || !Dec(f[3], &lock) ||
            lock > 1 || !Callees(f[4], &r.callees)) {
            if (err) *err = Bad(n + 1, "not a census schema 1 row");
            return std::nullopt;
        }
        r.lock = lock == 1;
        r.h.flags = std::string(f[5]);
        c.rows.push_back(std::move(r));
    }
    return c;
}

std::optional<std::vector<IdaRow>> ParseIdaCensus(std::string_view text, std::string* err) {
    std::vector<IdaRow> out;
    auto lines = Lines(text);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        auto l = lines[n];
        if (l[0] == '#') continue;
        auto f = Split(l, '\t');
        IdaRow r;
        bool ok = f.size() == 7 && Hex(f[0], &r.rva) && Hex(f[1], &r.id);
        if (ok && f[2] != "?") {
            std::uint32_t s;
            ok = Dec(f[2], &s);
            r.size = s;
        }
        if (ok && f[3] != "?") {
            ok = f[3] == "0" || f[3] == "1";
            r.lock = f[3] == "1";
        }
        if (ok) {
            if (f[6].substr(0, 2) == "x:") r.x = true;
            else ok = Callees(f[6], &r.callees);
        }
        if (!ok) {
            if (err) *err = Bad(n + 1, "not an IDA census row (rva id size lock argc retc callees)");
            return std::nullopt;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<std::vector<DumpRow>> ParseDumpTable(std::string_view text, std::string* err) {
    std::vector<DumpRow> out;
    auto lines = Lines(text);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        auto f = Split(lines[n], '\t');
        if (n == 0 && !f.empty() && f[0] == "namespace") continue;
        DumpRow r;
        if (f.size() < 4 || !Hex(f[2], &r.id) || !Hex(f[3], &r.rva)) {
            if (err) *err = Bad(n + 1, "not a dump row (namespace name hash64 handler_rva ...)");
            return std::nullopt;
        }
        out.push_back(r);
    }
    return out;
}

CompareResult CompareCensus(const Census& ours, const std::vector<IdaRow>& ida, const std::vector<DumpRow>& dump) {
    CompareResult r;
    for (const auto& c : CompareCategories()) r.byCategory[c];
    auto add = [&](const char* cat, std::string line) { r.byCategory[cat].push_back(std::move(line)); };

    std::map<std::uint64_t, const IdaRow*> byIda;
    for (const auto& i : ida) byIda[i.id] = &i;
    std::map<std::uint64_t, std::uint32_t> byDump;
    for (const auto& d : dump) byDump[d.id] = d.rva;
    std::map<std::uint64_t, bool> seen;

    for (const CensusRow& o : ours.rows) {
        const std::string tag = H64(o.h.id) + " " + H32(o.h.rva);
        seen[o.h.id] = true;
        if (o.h.flags.find('S') != std::string::npos) add("start-unresolved", tag + ": start = cmp, flags " + o.h.flags);
        auto d = byDump.find(o.h.id);
        bool dumpOk = d == byDump.end() || d->second == o.h.rva;
        if (!dumpOk) add("rva-vs-dump", tag + ": dump rva " + H32(d->second));

        auto it = byIda.find(o.h.id);
        if (it == byIda.end()) {
            add("id-set", H64(o.h.id) + ": only in ours (rva " + H32(o.h.rva) + ")");
            continue;
        }
        const IdaRow& i = *it->second;
        if (i.x) {
            std::string vals = "size " + std::to_string(o.h.size) + ", lock " + (o.lock ? "1" : "0") + ", callees " +
                               List(o.callees) + (o.h.flags.empty() ? "" : ", flags " + o.h.flags) +
                               (d == byDump.end() ? ", uncalled (no dump row)" : ", dump agrees");
            if (dumpOk) add("ida-missing", tag + ": " + vals);
            if (i.rva != o.h.rva) {
                if (o.h.rva < i.rva && i.rva - o.h.rva <= kMidWindow && dumpOk)
                    add("ida-mid-function", tag + ": IDA rva " + H32(i.rva) + " (+0x" +
                                                [&] { char b[16]; std::snprintf(b, sizeof(b), "%X", i.rva - o.h.rva); return std::string(b); }() + ")");
                else
                    add("unexplained", tag + ": x row, IDA rva " + H32(i.rva));
            }
            continue;
        }

        std::vector<std::string> why;
        if (i.rva != o.h.rva) why.push_back("IDA rva " + H32(i.rva));
        if (i.lock && *i.lock != o.lock) why.push_back(std::string("lock ours ") + (o.lock ? "1" : "0") + ", IDA " + (*i.lock ? "1" : "0"));
        if (i.size && *i.size != o.h.size && i.rva == o.h.rva)
            add("size-chunking", tag + ": ours " + std::to_string(o.h.size) + ", IDA " + std::to_string(*i.size));
        if (i.callees != o.callees) {
            const auto& oc = o.callees;
            const auto& ic = i.callees;
            bool tail = o.h.flags.find('T') != std::string::npos;
            if (ic.size() == 6 && oc.size() > 6 && std::equal(ic.begin(), ic.end(), oc.begin()))
                add("callees-truncated", tag + ": ours " + std::to_string(oc.size()) + " callees, IDA the first 6");
            else if (tail && oc.size() > ic.size() && Subsequence(ic, oc))
                add("callees-tailcall", tag + ": ours " + List(oc) + ", IDA " + List(ic));
            else if (SameMultiset(oc, ic))
                add("callees-order", tag + ": ours " + List(oc) + ", IDA " + List(ic));
            else
                why.push_back("callees ours " + List(oc) + ", IDA " + List(ic));
        }
        if (!why.empty()) {
            std::string line = tag + ":";
            for (std::size_t k = 0; k < why.size(); ++k) line += (k ? "; " : " ") + why[k];
            add("unexplained", line);
        }
    }
    for (const auto& i : ida)
        if (!seen.count(i.id)) add("id-set", H64(i.id) + ": only in IDA (rva " + H32(i.rva) + ")");

    for (auto& [cat, lines] : r.byCategory) std::sort(lines.begin(), lines.end());
    r.unexplained = r.byCategory["unexplained"].size();
    r.failures = r.unexplained + r.byCategory["id-set"].size() + r.byCategory["rva-vs-dump"].size();
    return r;
}

std::string FormatCompareMd(const CompareResult& r) {
    std::string s =
        "# Census vs IDA census\n\n"
        "Written by `shvi_nativedb` (`build --ida --dump`, or `compare-census`): our census (`census.tsv`) against the hand-merged IDA census and "
        "the dump table, joined by id64. A row may appear in several categories. `id-set`, `rva-vs-dump` and "
        "`unexplained` must be empty.\n\n"
        "| category | rows |\n|---|---|\n";
    auto count = [&](const std::string& c) {
        auto it = r.byCategory.find(c);
        return it == r.byCategory.end() ? std::size_t{0} : it->second.size();
    };
    for (const auto& e : kExplain) s += std::string("| ") + e.name + " | " + std::to_string(count(e.name)) + " |\n";
    for (const auto& e : kExplain) {
        s += std::string("\n## ") + e.name + " (" + std::to_string(count(e.name)) + ")\n\n" + e.text + "\n\n";
        auto it = r.byCategory.find(e.name);
        if (it == r.byCategory.end() || it->second.empty()) {
            s += "None.\n";
            continue;
        }
        s += "```\n";
        for (const auto& l : it->second) s += l + "\n";
        s += "```\n";
    }
    return s;
}

}  // namespace shvi::nativedb
