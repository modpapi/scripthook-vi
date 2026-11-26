// GENERIC. Natives that plugins may never call, even offline (architecture §G:
// the NETWORK_*, money and stat natives). Enforced by the gated invoker on both
// the hash a plugin passed and the crossmapped one. The guard's own probes
// (InvokeUnchecked) bypass it.
//
// File: <game>\shvi\denylist.txt (one hash per line, hex or decimal; ';' or '#'
// starts a comment). Generated from a NativeDB by namespace, never hand-guessed:
// a wrong hash would deny the wrong native.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace shvi {

class Denylist {
public:
    // Returns the number of hashes added; *badLines counts lines that weren't a hash.
    std::size_t Parse(std::string_view text, std::size_t* badLines = nullptr);
    bool LoadFile(const std::wstring& path, std::size_t* badLines = nullptr);
    void Add(std::uint64_t hash) { set_.insert(hash); }
    bool Contains(std::uint64_t hash) const { return set_.count(hash) != 0; }
    std::size_t size() const { return set_.size(); }

private:
    std::unordered_set<std::uint64_t> set_;
};

}  // namespace shvi
