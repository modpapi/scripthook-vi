// shvi_nativedb: an RVA-addressed, read-only view of a mapped PE32+ image
// (headers and sections at their RVAs, as pe::MapImageFile lays them out).
// Every read the pipeline makes goes through Readable(), so a malformed or
// truncated image can't make it read outside a section.
#pragma once
#include "core/pe.h"

#include <array>
#include <cstdint>
#include <vector>

namespace shvi::nativedb {

struct ImageView {
    const std::uint8_t* base = nullptr;
    std::uint32_t size = 0;            // SizeOfImage (bounded by the buffer)
    std::uint32_t timeDateStamp = 0;
    std::uint16_t machine = 0;
    std::vector<pe::Section> sections;  // range.begin is an RVA here, range.size the VirtualSize
    std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> dirs{};
    bool pe32plus = false;             // headers parsed as PE32+

    const pe::Section* SectionOf(std::uint32_t rva) const;
    // [rva, rva + n) lies inside one section.
    bool Readable(std::uint32_t rva, std::uint32_t n) const;
    bool Executable(std::uint32_t rva) const;
    // Bytes readable from rva to the end of its section (0 if outside every section).
    std::uint32_t Avail(std::uint32_t rva) const;
    const std::uint8_t* At(std::uint32_t rva) const { return base + rva; }
};

ImageView ViewOf(const pe::MappedImage& m);
// `mapped` holds an image laid out at its RVAs (headers at offset 0), `size` bytes long.
ImageView ViewOfBytes(const std::uint8_t* mapped, std::uint32_t size);

}  // namespace shvi::nativedb
