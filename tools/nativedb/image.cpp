#include "image.h"

#include <cstring>

namespace shvi::nativedb {

const pe::Section* ImageView::SectionOf(std::uint32_t rva) const {
    for (const auto& s : sections)
        if (rva >= s.range.begin && rva - s.range.begin < s.range.size) return &s;
    return nullptr;
}

bool ImageView::Readable(std::uint32_t rva, std::uint32_t n) const {
    const pe::Section* s = SectionOf(rva);
    return s && n <= s->range.size - (rva - s->range.begin);
}

bool ImageView::Executable(std::uint32_t rva) const {
    const pe::Section* s = SectionOf(rva);
    return s && s->Executable();
}

std::uint32_t ImageView::Avail(std::uint32_t rva) const {
    const pe::Section* s = SectionOf(rva);
    return s ? static_cast<std::uint32_t>(s->range.size - (rva - s->range.begin)) : 0;
}

ImageView ViewOfBytes(const std::uint8_t* mapped, std::uint32_t size) {
    ImageView v;
    v.base = mapped;
    v.size = size;
    auto in = [&](std::size_t off, std::size_t n) { return off <= size && n <= size - off; };
    if (!mapped || !in(0, sizeof(IMAGE_DOS_HEADER))) return v;
    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, mapped, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return v;
    auto ntOff = static_cast<std::size_t>(dos.e_lfanew);
    if (!in(ntOff, sizeof(IMAGE_NT_HEADERS64))) return v;
    IMAGE_NT_HEADERS64 nt;
    std::memcpy(&nt, mapped + ntOff, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE) return v;
    v.machine = nt.FileHeader.Machine;
    v.timeDateStamp = nt.FileHeader.TimeDateStamp;
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return v;
    v.pe32plus = true;
    if (nt.OptionalHeader.SizeOfImage < v.size) v.size = nt.OptionalHeader.SizeOfImage;
    DWORD ndirs = nt.OptionalHeader.NumberOfRvaAndSizes;
    for (DWORD i = 0; i < ndirs && i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i)
        v.dirs[i] = nt.OptionalHeader.DataDirectory[i];

    std::size_t secOff = ntOff + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh;
        if (!in(secOff + i * sizeof(sh), sizeof(sh))) break;
        std::memcpy(&sh, mapped + secOff + i * sizeof(sh), sizeof(sh));
        std::uint32_t vsize = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        if (sh.VirtualAddress >= v.size) continue;
        if (vsize > v.size - sh.VirtualAddress) vsize = v.size - sh.VirtualAddress;
        char name[9] = {};
        std::memcpy(name, sh.Name, 8);
        v.sections.push_back({name, {sh.VirtualAddress, vsize}, sh.Characteristics});
    }
    return v;
}

ImageView ViewOf(const pe::MappedImage& m) {
    return ViewOfBytes(reinterpret_cast<const std::uint8_t*>(m.module.image.begin),
                       static_cast<std::uint32_t>(m.module.image.size));
}

}  // namespace shvi::nativedb
