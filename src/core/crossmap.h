// GENERIC. Native hash crossmap: the hash a plugin was written against ->
// the hash the running build registers. Text format, one pair per line:
//     0xD80958FC74E988A6 0xD80958FC74E988A6   ; comment
// Produced per build by the dumping pipeline (docs/ARCHITECTURE.md, layer C).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace shvi {

class Crossmap {
public:
    // Returns the number of pairs read; malformed lines are skipped and counted in *bad.
    std::size_t Parse(std::string_view text, std::size_t* bad = nullptr);
    bool LoadFile(const std::wstring& path, std::string* error = nullptr);

    std::uint64_t Map(std::uint64_t hash) const {
        auto it = map_.find(hash);
        return it == map_.end() ? hash : it->second;
    }
    std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> map_;
};

}  // namespace shvi
