// Test helper: lay out a PE32+ image whose file offsets equal its RVAs
// (FileAlignment == SectionAlignment == 0x1000). The same bytes are then both
// a valid file for pe::MapImageFile and an already-mapped image for
// nativedb::ViewOfBytes, so in-memory tests and the synthetic exe share one writer.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace shvi::nativedb::test {

struct PeSection {
    std::string name;
    std::uint32_t rva = 0;
    std::vector<std::uint8_t> bytes;  // VirtualSize = bytes.size()
    std::uint32_t characteristics = 0;
};

struct PeLayout {
    std::uint32_t timeDateStamp = 0;
    std::uint16_t machine = IMAGE_FILE_MACHINE_AMD64;
    std::uint64_t imageBase = 0x140000000ull;
    std::vector<PeSection> sections;
    std::uint32_t pdataRva = 0, pdataSize = 0;
};

inline constexpr std::uint32_t kRX = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
inline constexpr std::uint32_t kR = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
inline constexpr std::uint32_t kRW = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

inline std::vector<std::uint8_t> LayoutPe(const PeLayout& L) {
    constexpr std::uint32_t kAlign = 0x1000;
    auto up = [](std::uint32_t x) { return (x + kAlign - 1) & ~(kAlign - 1); };
    std::uint32_t imageSize = kAlign;
    for (const auto& s : L.sections) {
        std::uint32_t end = up(s.rva + static_cast<std::uint32_t>(s.bytes.size()));
        if (end > imageSize) imageSize = end;
    }
    std::vector<std::uint8_t> img(imageSize, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(img.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = L.machine;
    nt.FileHeader.NumberOfSections = static_cast<WORD>(L.sections.size());
    nt.FileHeader.TimeDateStamp = L.timeDateStamp;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    auto& o = nt.OptionalHeader;
    o.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    o.AddressOfEntryPoint = L.sections.empty() ? 0 : L.sections[0].rva;
    o.ImageBase = L.imageBase;
    o.SectionAlignment = kAlign;
    o.FileAlignment = kAlign;
    o.MajorOperatingSystemVersion = 6;
    o.MajorSubsystemVersion = 6;
    o.SizeOfImage = imageSize;
    o.SizeOfHeaders = kAlign;
    o.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    o.SizeOfStackReserve = 0x100000;
    o.SizeOfStackCommit = 0x1000;
    o.SizeOfHeapReserve = 0x100000;
    o.SizeOfHeapCommit = 0x1000;
    o.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    o.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {L.pdataRva, L.pdataSize};
    std::memcpy(img.data() + 0x80, &nt, sizeof(nt));

    std::size_t sh = 0x80 + sizeof(nt);
    for (const auto& s : L.sections) {
        IMAGE_SECTION_HEADER h{};
        std::memcpy(h.Name, s.name.data(), s.name.size() < 8 ? s.name.size() : 8);
        h.Misc.VirtualSize = static_cast<DWORD>(s.bytes.size());
        h.VirtualAddress = s.rva;
        h.SizeOfRawData = up(static_cast<std::uint32_t>(s.bytes.size()));
        h.PointerToRawData = s.rva;
        h.Characteristics = s.characteristics;
        std::memcpy(img.data() + sh, &h, sizeof(h));
        sh += sizeof(h);
        if (!s.bytes.empty()) std::memcpy(img.data() + s.rva, s.bytes.data(), s.bytes.size());
    }
    return img;
}

}  // namespace shvi::nativedb::test
