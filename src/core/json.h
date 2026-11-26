// GENERIC. A small JSON reader for the files the core loads itself (native
// tables). Objects keep their member order; numbers are kept as doubles and
// as their source text (so 64-bit integers written as numbers survive).
// No exceptions: Parse returns nullopt and an error with the byte offset.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shvi::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0;
    std::string str;  // String, or the source text of a Number
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    bool IsObject() const { return kind == Kind::Object; }
    bool IsString() const { return kind == Kind::String; }
    const Value* Get(std::string_view key) const {
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
    std::string_view StringOr(std::string_view def) const { return kind == Kind::String ? std::string_view(str) : def; }
};

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}
    std::optional<Value> Run(std::string* err) {
        Value v;
        Ws();
        if (!ParseValue(&v, 0) || (Ws(), i_ != t_.size())) {
            if (err) *err = (msg_.empty() ? std::string("trailing data") : msg_) + " at byte " + std::to_string(i_);
            return std::nullopt;
        }
        return v;
    }

private:
    bool Fail(const char* m) { if (msg_.empty()) msg_ = m; return false; }
    void Ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\r' || t_[i_] == '\n')) ++i_; }
    bool Lit(std::string_view w) {
        if (t_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    bool ParseString(std::string* out) {
        if (i_ >= t_.size() || t_[i_] != '"') return Fail("expected string");
        ++i_;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c != '\\') { out->push_back(c); continue; }
            if (i_ >= t_.size()) return Fail("bad escape");
            char e = t_[i_++];
            switch (e) {
                case '"': case '\\': case '/': out->push_back(e); break;
                case 'n': out->push_back('\n'); break;
                case 't': out->push_back('\t'); break;
                case 'r': out->push_back('\r'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'u': {
                    if (i_ + 4 > t_.size()) return Fail("bad \\u escape");
                    unsigned cp = static_cast<unsigned>(std::strtoul(std::string(t_.substr(i_, 4)).c_str(), nullptr, 16));
                    i_ += 4;
                    if (cp < 0x80) out->push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out->push_back(static_cast<char>(0xC0 | (cp >> 6))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    else { out->push_back(static_cast<char>(0xE0 | (cp >> 12))); out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        if (i_ >= t_.size()) return Fail("unterminated string");
        ++i_;
        return true;
    }
    bool ParseValue(Value* v, int depth) {
        if (depth > 64) return Fail("nesting too deep");
        if (i_ >= t_.size()) return Fail("unexpected end");
        char c = t_[i_];
        if (c == '{') {
            v->kind = Value::Kind::Object;
            ++i_;
            Ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!ParseString(&k)) return false;
                Ws();
                if (i_ >= t_.size() || t_[i_] != ':') return Fail("expected ':'");
                ++i_;
                Ws();
                Value m;
                if (!ParseValue(&m, depth + 1)) return false;
                v->members.emplace_back(std::move(k), std::move(m));
                Ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v->kind = Value::Kind::Array;
            ++i_;
            Ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
            for (;;) {
                Ws();
                Value m;
                if (!ParseValue(&m, depth + 1)) return false;
                v->items.push_back(std::move(m));
                Ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v->kind = Value::Kind::String; return ParseString(&v->str); }
        if (Lit("true")) { v->kind = Value::Kind::Bool; v->b = true; return true; }
        if (Lit("false")) { v->kind = Value::Kind::Bool; return true; }
        if (Lit("null")) { v->kind = Value::Kind::Null; return true; }
        std::size_t s = i_;
        while (i_ < t_.size() && (std::string_view("+-0123456789.eE").find(t_[i_]) != std::string_view::npos)) ++i_;
        if (s == i_) return Fail("unexpected character");
        v->kind = Value::Kind::Number;
        v->str = std::string(t_.substr(s, i_ - s));
        v->num = std::strtod(v->str.c_str(), nullptr);
        return true;
    }

    std::string_view t_;
    std::size_t i_ = 0;
    std::string msg_;
};

inline std::optional<Value> Parse(std::string_view text, std::string* err = nullptr) { return Parser(text).Run(err); }

// "0x1A2B" or a decimal number/string -> u64.
inline std::optional<std::uint64_t> ToU64(const Value* v) {
    if (!v) return std::nullopt;
    if (v->kind != Value::Kind::String && v->kind != Value::Kind::Number) return std::nullopt;
    const std::string& s = v->str;
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    std::uint64_t r = std::strtoull(s.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
    if (!end || *end != 0) return std::nullopt;
    return r;
}

}  // namespace shvi::json
