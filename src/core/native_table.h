// GENERIC (VI table format). A per-build table of named native handlers
// (crossmap/gta6_xbox_1.0.2.1.json, TASKS T2.3):
//
//   {"build": {"pe_timestamp": 1794606036, ...},
//    "natives": {"NAME": {"rva": "0x...", "id": "0x...", "args": 3, "ret": "int",
//                          "params": ["entity", "int"], "deny": true, ...}, ...}}
//
// Binding rules, all fail-closed:
//   * the table must name the running build (pe_timestamp == profile Build());
//     a table for another build is refused whole and logged;
//   * each handler must carry, at image base + rva, the 64-bit id the table
//     gives (vi_script.h HandlerId): a moved or different function is skipped;
//   * "deny": true entries are never bound (a plugin call stays unresolved).
// Each bound name is callable as EngineHash(NAME) through BindViNative.
#pragma once
#include "engine_call.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shvi {

struct NativeEntry {
    std::string name;
    std::uint32_t rva = 0;
    std::uint64_t id = 0;
    int args = -1;                    // VM cells, -1 = not given
    std::string ret;                  // "int", "float", "bool", "entity", "vector3", "string", "" = none
    std::vector<std::string> params;  // optional parameter types, same vocabulary
    std::string confidence;
    bool deny = false;
};

struct NativeTable {
    std::uint32_t peTimestamp = 0;
    std::string exe, version;
    std::vector<NativeEntry> natives;
};

// False (with a reason) on malformed JSON or a table without build / natives.
// Entries without a valid rva or id are skipped and counted in *skipped.
bool ParseNativeTable(std::string_view json, NativeTable* out, std::string* error, std::size_t* skipped = nullptr);
bool LoadNativeTableFile(const std::wstring& path, NativeTable* out, std::string* error);

struct NativeBindReport {
    bool refused = false;  // wrong build: nothing bound
    std::size_t bound = 0, denied = 0, idMismatch = 0, outOfImage = 0;
    std::string log;
};

// Reads the handler id at an address, or nullopt (not a handler, unreadable).
using HandlerIdReader = std::function<std::optional<std::uint64_t>(std::uintptr_t)>;

NativeBindReport BindNativeTable(const NativeTable& t, std::uint32_t build, std::uintptr_t imageBase,
                                 std::size_t imageSize, const HandlerIdReader& readId, EngineTable& out);

}  // namespace shvi
