// GENERIC. Executable memory within reach of a 32-bit unsigned offset above a
// base address (TASKS T2.9).
//
// GTA VI's native call (VM case 139) jumps to [script_native_base] + u32, so
// any C++ code a script detour calls (route C2, SCRIPT_MODDING.md §4) must sit
// in [base, base + 4 GiB). This allocator reserves and commits pages in that
// window, and never anything outside it: an allocation that cannot be placed
// fails instead of landing out of range.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace shvi {

class TrampolineAllocator {
public:
    static constexpr std::uint64_t kWindow = 0x1'0000'0000ull;  // a u32 offset reaches [base, base + 4 GiB)

    explicit TrampolineAllocator(std::uintptr_t base) : base_(base) {}
    TrampolineAllocator(const TrampolineAllocator&) = delete;
    TrampolineAllocator& operator=(const TrampolineAllocator&) = delete;
    ~TrampolineAllocator();

    // `size` bytes, 16-aligned, PAGE_EXECUTE_READWRITE, inside the window; nullptr if none is free.
    void* Allocate(std::size_t size);
    // The u32 the VM operand needs for `p`, or nullopt when p is out of range.
    std::optional<std::uint32_t> OffsetOf(const void* p) const;
    bool InRange(const void* p, std::size_t size = 1) const;
    std::uintptr_t Base() const { return base_; }
    std::size_t BlockCount() const { return blocks_.size(); }

private:
    struct Block {
        std::uintptr_t begin;
        std::size_t size, used;
    };
    void* NewBlock(std::size_t minSize);

    std::uintptr_t base_;
    std::vector<Block> blocks_;
};

}  // namespace shvi
