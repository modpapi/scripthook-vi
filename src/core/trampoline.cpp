#include "trampoline.h"

#include <windows.h>

namespace shvi {
namespace {

std::size_t Granularity() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity;  // 64 KiB on x64 Windows
}

}  // namespace

TrampolineAllocator::~TrampolineAllocator() {
    for (const auto& b : blocks_) VirtualFree(reinterpret_cast<void*>(b.begin), 0, MEM_RELEASE);
}

bool TrampolineAllocator::InRange(const void* p, std::size_t size) const {
    auto a = reinterpret_cast<std::uintptr_t>(p);
    if (a < base_ || size == 0) return false;
    std::uint64_t end = static_cast<std::uint64_t>(a - base_) + size;  // one past the last byte
    return end <= kWindow;
}

std::optional<std::uint32_t> TrampolineAllocator::OffsetOf(const void* p) const {
    if (!InRange(p)) return std::nullopt;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p) - base_);
}

void* TrampolineAllocator::NewBlock(std::size_t minSize) {
    const std::size_t g = Granularity();
    std::size_t size = (minSize + g - 1) / g * g;
    // Walk the free regions upward from the first granule at or above base.
    std::uintptr_t lo = (base_ + g - 1) / g * g;
    std::uint64_t limit = static_cast<std::uint64_t>(base_) + kWindow;  // exclusive
    for (std::uintptr_t at = lo; static_cast<std::uint64_t>(at) + size <= limit;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi)) return nullptr;  // past user space
        auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        std::uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            std::uintptr_t start = (at + g - 1) / g * g;
            if (start + size <= regionEnd && static_cast<std::uint64_t>(start) + size <= limit) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(start), size, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
                if (p && InRange(p, size)) {
                    blocks_.push_back({reinterpret_cast<std::uintptr_t>(p), size, 0});
                    return p;
                }
                if (p) VirtualFree(p, 0, MEM_RELEASE);  // the OS moved it: never keep an out-of-range block
            }
        }
        if (regionEnd <= at) return nullptr;
        at = regionEnd;
    }
    return nullptr;
}

void* TrampolineAllocator::Allocate(std::size_t size) {
    if (size == 0 || size > kWindow / 2) return nullptr;
    size = (size + 15) & ~std::size_t{15};
    for (auto& b : blocks_) {
        if (b.size - b.used >= size) {
            void* p = reinterpret_cast<void*>(b.begin + b.used);
            b.used += size;
            return p;
        }
    }
    if (!NewBlock(size)) return nullptr;
    Block& b = blocks_.back();
    b.used = size;
    return reinterpret_cast<void*>(b.begin);
}

}  // namespace shvi
