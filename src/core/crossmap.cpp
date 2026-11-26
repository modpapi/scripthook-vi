#include "crossmap.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace shvi {
namespace {

bool ParseHash(std::string_view s, std::uint64_t* out) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), *out, 16);
    return ec == std::errc() && p == s.data() + s.size();
}

}  // namespace

std::size_t Crossmap::Parse(std::string_view text, std::size_t* bad) {
    std::size_t good = 0, badCount = 0;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (auto c = line.find_first_of(";#"); c != std::string_view::npos) line = line.substr(0, c);
        auto trim = [](std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
            return v;
        };
        line = trim(line);
        if (line.empty()) continue;

        std::size_t sp = line.find_first_of(" \t");
        std::uint64_t from = 0, to = 0;
        if (sp == std::string_view::npos || !ParseHash(line.substr(0, sp), &from) ||
            !ParseHash(trim(line.substr(sp)), &to)) {
            ++badCount;
            continue;
        }
        map_[from] = to;
        ++good;
    }
    if (bad) *bad = badCount;
    return good;
}

bool Crossmap::LoadFile(const std::wstring& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open crossmap file";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    Parse(ss.str());
    return true;
}

}  // namespace shvi
