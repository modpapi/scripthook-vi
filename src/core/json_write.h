// GENERIC. Deterministic JSON writer: object keys sorted bytewise (std::map),
// 2-space indent, LF line ends, a trailing LF at top level, integers via
// std::to_chars, no floats. Same tree in, same bytes out. The reader is
// core/json.h; this is only for files tools write (shvi_nativedb).
#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace shvi::jsonw {

struct Node;
using Array = std::vector<Node>;
using Object = std::map<std::string, Node>;

struct Node {
    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, std::string, Array, Object> v;

    Node() : v(nullptr) {}
    Node(std::nullptr_t) : v(nullptr) {}
    Node(bool b) : v(b) {}
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Node(T x) {
        if constexpr (std::is_signed_v<T>) v = static_cast<std::int64_t>(x);
        else v = static_cast<std::uint64_t>(x);
    }
    Node(const char* s) : v(std::string(s)) {}
    Node(std::string s) : v(std::move(s)) {}
    Node(Array a) : v(std::move(a)) {}
    Node(Object o) : v(std::move(o)) {}
};

namespace detail {
template <class T>
inline void Int(std::string& out, T x) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), x);
    out.append(buf, r.ptr);
}

inline void Str(std::string& out, const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);  // bytes >= 0x80 (UTF-8) pass through
                }
        }
    }
    out += '"';
}

inline void Write(std::string& out, const Node& n, int depth) {
    auto indent = [&](int d) { out.append(static_cast<std::size_t>(2 * d), ' '); };
    switch (n.v.index()) {
        case 0: out += "null"; break;
        case 1: out += std::get<bool>(n.v) ? "true" : "false"; break;
        case 2: Int(out, std::get<std::int64_t>(n.v)); break;
        case 3: Int(out, std::get<std::uint64_t>(n.v)); break;
        case 4: Str(out, std::get<std::string>(n.v)); break;
        case 5: {
            const auto& a = std::get<Array>(n.v);
            if (a.empty()) { out += "[]"; break; }
            out += "[\n";
            for (std::size_t i = 0; i < a.size(); ++i) {
                indent(depth + 1);
                Write(out, a[i], depth + 1);
                out += i + 1 < a.size() ? ",\n" : "\n";
            }
            indent(depth);
            out += ']';
            break;
        }
        default: {
            const auto& o = std::get<Object>(n.v);
            if (o.empty()) { out += "{}"; break; }
            out += "{\n";
            std::size_t i = 0;
            for (const auto& [k, val] : o) {
                indent(depth + 1);
                Str(out, k);
                out += ": ";
                Write(out, val, depth + 1);
                out += ++i < o.size() ? ",\n" : "\n";
            }
            indent(depth);
            out += '}';
        }
    }
}

inline std::string HexN(std::uint64_t x, int digits) {
    static const char* kHex = "0123456789ABCDEF";
    std::string s = "0x";
    for (int i = digits - 1; i >= 0; --i) s += kHex[(x >> (4 * i)) & 0xF];
    return s;
}
}  // namespace detail

inline std::string Dump(const Node& n) {
    std::string out;
    detail::Write(out, n, 0);
    out += '\n';
    return out;
}

inline std::string Hex32(std::uint32_t x) { return detail::HexN(x, 8); }    // "0x%08X"
inline std::string Hex64(std::uint64_t x) { return detail::HexN(x, 16); }   // "0x%016X"

}  // namespace shvi::jsonw
