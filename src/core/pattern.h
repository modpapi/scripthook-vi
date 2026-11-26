// GENERIC. Patch-tolerant signature scanner.
//
// A Target ("native_registration_table") has several Candidates, tried in
// order. Each candidate is an IDA-style pattern plus post-processing ops
// (Add / Rip / Deref / ReadI32). A candidate only wins if exactly one of its
// matches survives the ops and the optional validator; an ambiguous or empty
// candidate falls through to the next one. That is what makes a signature set
// survive patches: when Rockstar's compiler shuffles one function, an older or
// alternative anchor (a different instruction, a caller, a string xref)
// still resolves, and the log says which one did.
#pragma once
#include "pe.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shvi::scan {

using pe::Region;

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;  // 1 = byte must match, 0 = wildcard
    static std::optional<Pattern> Parse(std::string_view ida);  // "48 8D 0D ? ? ? ?"
    std::size_t size() const { return bytes.size(); }
};

// Returns at most maxHits match addresses, in address order.
std::vector<std::uintptr_t> FindAll(const Pattern& p, std::span<const Region> regions,
                                    std::size_t maxHits = 8);

enum class OpKind { Add, Rip, Deref, ReadI32 };
struct Op {
    OpKind kind;
    std::int32_t value = 0;
};
constexpr Op Add(std::int32_t n) { return {OpKind::Add, n}; }
// Current address points at a rel32: result = cur + 4 + trailing + rel32.
// trailing = immediate bytes after the displacement (e.g. 1 for "cmp [rip+x], imm8").
constexpr Op Rip(std::int32_t trailing = 0) { return {OpKind::Rip, trailing}; }
constexpr Op Deref() { return {OpKind::Deref}; }     // read pointer at cur (must be in image)
constexpr Op ReadI32() { return {OpKind::ReadI32}; } // result is a VALUE (struct offset), not an address

struct Candidate {
    std::string_view pattern;
    std::vector<Op> ops;
    std::string_view note;  // where it came from / which builds it is known for
};

struct Target {
    std::string_view name;
    std::vector<Candidate> candidates;
    bool required = true;
    std::function<bool(std::uintptr_t)> validate;  // optional sanity check on the result
};

struct Hit {
    std::uintptr_t value = 0;  // address, or value for ReadI32 targets
    int candidate = -1;        // index of the winning candidate
};

class Scanner {
public:
    Scanner(Region image, std::vector<Region> code) : image_(image), code_(std::move(code)) {}
    explicit Scanner(const pe::Module& m) : Scanner(m.image, m.ExecutableRegions()) {}

    // On failure returns nullopt and appends a human-readable reason per candidate.
    std::optional<Hit> Resolve(const Target& t, std::string* log = nullptr) const;

private:
    std::optional<std::uintptr_t> Apply(std::uintptr_t at, const std::vector<Op>& ops, bool* isValue) const;
    Region image_;
    std::vector<Region> code_;
};

struct Report {
    std::unordered_map<std::string, Hit> hits;
    std::vector<std::string> missingRequired;
    std::string log;
    bool Ok() const { return missingRequired.empty(); }
    std::uintptr_t Get(std::string_view name) const {
        auto it = hits.find(std::string(name));
        return it == hits.end() ? 0 : it->second.value;
    }
};

Report ResolveAll(const Scanner& s, std::span<const Target> targets);

}  // namespace shvi::scan
