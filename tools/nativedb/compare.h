// shvi_nativedb compare-census: our census against the hand-merged IDA census
// (analysis/gta6_xbox/natives_census.tsv) and the dump table
// (docs/reference/natives_xbox_1.0.2.1.tsv). Every difference lands in a named
// category (spec §7.4); `unexplained` must be empty.
#pragma once
#include "census.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shvi::nativedb {

struct IdaRow {
    std::uint32_t rva = 0;
    std::uint64_t id = 0;
    std::optional<std::uint32_t> size;  // `?` -> nullopt
    std::optional<bool> lock;
    std::vector<std::optional<std::uint32_t>> callees;  // `ind` -> nullopt
    bool x = false;                     // last column `x: ...` (no IDA function)
};

struct DumpRow {
    std::uint64_t id = 0;
    std::uint32_t rva = 0;
};

struct CompareResult {
    std::map<std::string, std::vector<std::string>> byCategory;  // every category present; lines sorted
    std::size_t unexplained = 0;
    std::size_t failures = 0;  // unexplained + id-set + rva-vs-dump: any of them fails the run
};

// Categories in report order.
const std::vector<std::string>& CompareCategories();

std::optional<Census> ParseCensusTsv(std::string_view text, std::string* err);
std::optional<std::vector<IdaRow>> ParseIdaCensus(std::string_view text, std::string* err);
std::optional<std::vector<DumpRow>> ParseDumpTable(std::string_view text, std::string* err);

// Rows joined by id64; a row may land in several categories.
CompareResult CompareCensus(const Census& ours, const std::vector<IdaRow>& ida, const std::vector<DumpRow>& dump);
std::string FormatCompareMd(const CompareResult& r);

}  // namespace shvi::nativedb
