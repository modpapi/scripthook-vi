// GENERIC (RAGE-wide). Jenkins one-at-a-time hash with RAGE's string rules,
// as used for script names, model names, asset names, state names.
//
// Behaviour pinned against the GTA VI binary's own atStringHash
// (0x144FC21D0 in the Xbox build, ts 1794606036, called through
// `shvi_scan --hash`), not against documentation:
//   * ASCII letters fold to lower case;
//   * '\' hashes as '/';
//   * if the string starts with '"', the quote is skipped and hashing stops at
//     the next '"'.
// Also confirmed for GTA VI asset names (Leonida/docs/modding.md:
// player_story000_00 -> 0xE472B687) and for the PSIN state keys
// (Leonida/docs/mission_logic.md: case folding is required).
#pragma once
#include <cstdint>
#include <string_view>

namespace shvi {

constexpr std::uint32_t Joaat(std::string_view s) {
    std::size_t i = 0;
    bool quoted = !s.empty() && s[0] == '"';
    if (quoted) i = 1;
    std::uint32_t h = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (quoted && c == '"') break;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c == '\\') c = '/';
        h += static_cast<std::uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

static_assert(Joaat("adder") == 0xB779A091u);  // well-known joaat test value
static_assert(Joaat("a\\b") == Joaat("a/b"));

}  // namespace shvi
