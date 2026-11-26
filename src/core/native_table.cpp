#include "native_table.h"

#include "json.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace shvi {

bool ParseNativeTable(std::string_view text, NativeTable* out, std::string* error, std::size_t* skipped) {
    std::string err;
    auto root = json::Parse(text, &err);
    if (!root || !root->IsObject()) {
        if (error) *error = root ? "top level is not an object" : err;
        return false;
    }
    const json::Value* build = root->Get("build");
    const json::Value* natives = root->Get("natives");
    auto ts = build ? json::ToU64(build->Get("pe_timestamp")) : std::nullopt;
    if (!ts || *ts > 0xFFFFFFFFull || !natives || !natives->IsObject()) {
        if (error) *error = "missing build.pe_timestamp or natives";
        return false;
    }
    NativeTable t;
    t.peTimestamp = static_cast<std::uint32_t>(*ts);
    if (auto* e = build->Get("exe")) t.exe = std::string(e->StringOr(""));
    if (auto* v = build->Get("version")) t.version = std::string(v->StringOr(""));
    std::size_t bad = 0;
    for (const auto& [name, v] : natives->members) {
        auto rva = json::ToU64(v.Get("rva"));
        auto id = json::ToU64(v.Get("id"));
        if (!v.IsObject() || !rva || !id || *rva == 0 || *rva > 0xFFFFFFFFull || name.empty()) {
            ++bad;
            continue;
        }
        NativeEntry n;
        n.name = name;
        n.rva = static_cast<std::uint32_t>(*rva);
        n.id = *id;
        if (auto* a = v.Get("args"); a && a->kind == json::Value::Kind::Number) n.args = static_cast<int>(a->num);
        if (auto* r = v.Get("ret")) n.ret = std::string(r->StringOr(""));
        if (auto* c = v.Get("confidence")) n.confidence = std::string(c->StringOr(""));
        if (auto* d = v.Get("deny")) n.deny = d->kind == json::Value::Kind::Bool && d->b;
        if (auto* p = v.Get("params"); p && p->kind == json::Value::Kind::Array)
            for (const auto& it : p->items) n.params.emplace_back(it.StringOr("?"));
        t.natives.push_back(std::move(n));
    }
    if (skipped) *skipped = bad;
    *out = std::move(t);
    return true;
}

bool LoadNativeTableFile(const std::wstring& path, NativeTable* out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseNativeTable(ss.str(), out, error);
}

NativeBindReport BindNativeTable(const NativeTable& t, std::uint32_t build, std::uintptr_t imageBase,
                                 std::size_t imageSize, const HandlerIdReader& readId, EngineTable& out) {
    NativeBindReport r;
    char line[256];
    if (t.peTimestamp != build) {
        r.refused = true;
        std::snprintf(line, sizeof line, "native table is for build %u, running build is %u: refused\n", t.peTimestamp,
                      build);
        r.log += line;
        return r;
    }
    for (const auto& n : t.natives) {
        if (n.deny) {
            ++r.denied;
            continue;
        }
        if (static_cast<std::size_t>(n.rva) + 32 > imageSize) {
            ++r.outOfImage;
            std::snprintf(line, sizeof line, "  %s: rva 0x%X outside the image\n", n.name.c_str(), n.rva);
            r.log += line;
            continue;
        }
        std::uintptr_t fn = imageBase + n.rva;
        auto id = readId(fn);
        if (!id || *id != n.id) {
            ++r.idMismatch;
            std::snprintf(line, sizeof line, "  %s: handler id at rva 0x%X is %s0x%016llX, table says 0x%016llX\n",
                          n.name.c_str(), n.rva, id ? "" : "missing ", id ? static_cast<unsigned long long>(*id) : 0ull,
                          static_cast<unsigned long long>(n.id));
            r.log += line;
            continue;
        }
        out.BindViNative(EngineHash(n.name), reinterpret_cast<rage::ViNativeHandler>(fn));
        ++r.bound;
    }
    std::snprintf(line, sizeof line, "native table: %zu bound, %zu denied, %zu id mismatches, %zu outside the image\n",
                  r.bound, r.denied, r.idMismatch, r.outOfImage);
    r.log += line;
    return r;
}

}  // namespace shvi
