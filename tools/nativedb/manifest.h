// shvi_nativedb Stage 0: manifest.json (spec §7.3) and the file helpers every
// stage writes through (UTF-8 basenames, atomic binary writes).
#pragma once
#include "census.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shvi::nativedb {

struct InputRec {
    std::string role, name, sha256;  // name = UTF-8 basename, never a path
    std::uint64_t size = 0;
    std::string pin;                 // refdb only; omitted when empty
};

struct OutputRec {
    std::string path, sha256;        // path relative to the build folder
    std::uint64_t size = 0;
};

struct BuildInfo {
    std::uint32_t key = 0;           // PE TimeDateStamp
    std::uint16_t machine = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t handlers = 0;      // census rows
    std::optional<LockInfo> lock;    // census lock (spec §7.4 item 5); no "census" block when unset
};

// Inputs sorted by (role, name), outputs by path; the exe input is repeated as build.exe.
std::string FormatManifest(const BuildInfo& b, std::vector<InputRec> inputs, std::vector<OutputRec> outputs);

// Last path component (either slash), as UTF-8.
std::string Utf8Basename(const std::wstring& path);

// Writes `<path>.tmp` in binary, then renames it over `path`. False on any failure (the tmp is removed).
bool WriteFileAtomic(const std::wstring& path, const std::string& bytes);

}  // namespace shvi::nativedb
