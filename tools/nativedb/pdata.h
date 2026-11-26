// shvi_nativedb: the exception directory (.pdata) as sorted RUNTIME_FUNCTION
// ranges, each resolved through chained unwind info to its primary entry.
#pragma once
#include "image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace shvi::nativedb {

struct RuntimeFn {
    std::uint32_t begin = 0, end = 0, unwind = 0;
    std::uint32_t primary = 0;  // begin of the root entry after following the chain; == begin when not chained
};

struct PdataResult {
    std::vector<RuntimeFn> fns;  // sorted by begin
    std::string error;
};

PdataResult ReadPdata(const ImageView& img);
// begin <= rva < end, or nullptr.
const RuntimeFn* FindContaining(const std::vector<RuntimeFn>& fns, std::uint32_t rva);
// All entries whose primary is `primary`, sorted by begin.
std::vector<const RuntimeFn*> ChunksOf(const std::vector<RuntimeFn>& fns, std::uint32_t primary);

}  // namespace shvi::nativedb
