// GENERIC. PE helpers: sections of a loaded module, imports/exports of an
// image on disk (for the proxy picker), and the version resource.
#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shvi::pe {

struct Region {
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    bool Contains(std::uintptr_t p, std::size_t n = 1) const {
        return p >= begin && n <= size && p - begin <= size - n;
    }
};

struct Section {
    std::string name;
    Region range;
    std::uint32_t characteristics = 0;
    bool Executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
};

struct Module {
    Region image;
    std::vector<Section> sections;

    static std::optional<Module> FromLoaded(HMODULE mod);
    std::vector<Region> ExecutableRegions() const;
};

// A PE file laid out the way the loader would (headers and sections at their
// RVAs, base relocations applied to the new address), without running it.
// Lets the signature scanner, including Rip/Deref ops, work on an exe from disk
// (the offline scan tool, and console builds that can't run on PC).
// The memory is read/write; MakeExecutable() is for probing pure functions only.
struct MappedImage {
    Module module;                  // image/sections point into the mapping
    std::uintptr_t preferredBase = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t entryRva = 0;
    bool relocated = false;         // base relocations applied (delta != 0)

    bool MakeExecutable();          // PAGE_EXECUTE_READ for the whole image
    std::uintptr_t FromPreferred(std::uintptr_t va) const {  // VA as IDA shows it -> mapped address
        return va - preferredBase + module.image.begin;
    }

    struct Free { void operator()(void* p) const; };
    std::unique_ptr<void, Free> memory;
};
std::optional<MappedImage> MapImageFile(const std::wstring& path, std::string* error);

struct FileVersion {
    std::uint16_t major = 0, minor = 0, build = 0, revision = 0;
    std::string ToString() const;
};
std::optional<FileVersion> GetModuleVersion(HMODULE mod);

// A PE file read from disk (not mapped by the loader). PE32 and PE32+.
class ImageFile {
public:
    bool Load(const std::wstring& path, std::string* error);
    std::uint16_t Machine() const { return machine_; }
    std::vector<std::string> Imports() const;       // DLL names, as written
    std::vector<std::string> DelayImports() const;
    std::vector<std::string> ExportNames() const;

private:
    std::optional<std::size_t> RvaToOffset(std::uint32_t rva) const;
    std::string StringAtRva(std::uint32_t rva) const;
    const IMAGE_DATA_DIRECTORY* Directory(int index) const;

    std::vector<std::uint8_t> data_;
    std::uint16_t machine_ = 0;
    std::size_t sectionsOffset_ = 0;
    std::uint16_t sectionCount_ = 0;
    std::size_t dirOffset_ = 0;
    std::uint32_t dirCount_ = 0;
};

}  // namespace shvi::pe
