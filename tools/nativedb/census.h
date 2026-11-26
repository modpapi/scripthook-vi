// shvi_nativedb Stage 1: the native handler census, read from the exe alone
// (byte sweep + HandlerId, .pdata, linear Zydis decoding). Spec §7.4.
#pragma once
#include "image.h"
#include "pdata.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shvi::nativedb {

struct Range { std::uint32_t begin = 0, end = 0; };

struct Handler {
    std::uint32_t rva = 0;     // function start
    std::uint32_t cmpRva = 0;  // the `cmp dword [rcx+10h], 0`
    std::uint64_t id = 0;
    std::uint32_t size = 0;    // primary .pdata range, or the decoded leaf extent
    std::vector<Range> code;   // primary first, then chained chunks by begin
    std::string flags;         // sorted letters: L leaf, M cmp > 24 bytes in, S start unresolved, T tail call
};

struct HandlerScan {
    std::vector<Handler> handlers;   // sorted by rva
    std::string error;               // fatal (duplicate id)
    std::vector<std::string> notes;  // sorted; "skipped 0x%08X: <reason>"
};

HandlerScan FindHandlers(const ImageView& img, const std::vector<RuntimeFn>& pdata);

}  // namespace shvi::nativedb

namespace shvi::nativedb {

// The script lock every locking handler takes: acquire(&global) ... release(&global). Found by vote.
struct LockInfo {
    std::uint32_t global = 0, acquire = 0, release = 0;  // RVAs
    std::uint32_t votes = 0, voters = 0;                 // acquire vote
};

struct CensusRow {
    Handler h;
    bool lock = false;                                    // calls acquire
    std::vector<std::optional<std::uint32_t>> callees;    // address order; nullopt = indirect call
};

struct Census {
    std::vector<CensusRow> rows;  // sorted by rva
    LockInfo lock;
    std::vector<std::string> notes;  // sorted
};

// nullopt + *err (one line) when the image has no usable .pdata, no handlers, or no clear lock.
std::optional<Census> RunCensus(const ImageView& img, std::string* err);
// census.tsv, schema 1 (spec §7.4); rows in the order given.
std::string FormatCensusTsv(const Census& c);

}  // namespace shvi::nativedb
