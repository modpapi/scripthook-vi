#include "denylist.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace shvi {

std::size_t Denylist::Parse(std::string_view text, std::size_t* badLines) {
    std::size_t added = 0, bad = 0;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string line(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (auto c = line.find_first_of(";#"); c != std::string::npos) line.resize(c);
        std::size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;  // blank or comment-only
        std::size_t e = line.find_last_not_of(" \t\r");
        std::string tok = line.substr(b, e - b + 1);

        char* end = nullptr;
        errno = 0;
        unsigned long long v = std::strtoull(tok.c_str(), &end, 0);
        if (errno != 0 || end == tok.c_str() || *end != '\0' || v == 0) {
            ++bad;
            continue;
        }
        set_.insert(v);
        ++added;
    }
    if (badLines) *badLines = bad;
    return added;
}

bool Denylist::LoadFile(const std::wstring& path, std::size_t* badLines) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    Parse(text, badLines);
    return true;
}

}  // namespace shvi
