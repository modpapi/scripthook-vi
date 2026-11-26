#include "pe.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace shvi::pe {

std::optional<Module> Module::FromLoaded(HMODULE mod) {
    if (!mod) return std::nullopt;
    auto base = reinterpret_cast<std::uintptr_t>(mod);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return std::nullopt;

    Module m;
    m.image = {base, nt->OptionalHeader.SizeOfImage};
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char name[9] = {};
        std::memcpy(name, s->Name, 8);
        std::size_t size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        m.sections.push_back({name, {base + s->VirtualAddress, size}, s->Characteristics});
    }
    return m;
}

std::vector<Region> Module::ExecutableRegions() const {
    std::vector<Region> out;
    for (const auto& s : sections)
        if (s.Executable()) out.push_back(s.range);
    return out;
}

std::string FileVersion::ToString() const {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", major, minor, build, revision);
    return buf;
}

std::optional<FileVersion> GetModuleVersion(HMODULE mod) {
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /*RT_VERSION*/);
    if (!res) return std::nullopt;
    HGLOBAL h = LoadResource(mod, res);
    DWORD size = SizeofResource(mod, res);
    auto p = static_cast<const std::uint8_t*>(LockResource(h));
    if (!p || size < sizeof(VS_FIXEDFILEINFO)) return std::nullopt;
    // VS_FIXEDFILEINFO starts with its signature, DWORD-aligned inside VS_VERSIONINFO.
    for (DWORD off = 0; off + sizeof(VS_FIXEDFILEINFO) <= size; off += 4) {
        DWORD sig;
        std::memcpy(&sig, p + off, 4);
        if (sig != 0xFEEF04BD) continue;
        VS_FIXEDFILEINFO fi;
        std::memcpy(&fi, p + off, sizeof(fi));
        return FileVersion{HIWORD(fi.dwFileVersionMS), LOWORD(fi.dwFileVersionMS),
                           HIWORD(fi.dwFileVersionLS), LOWORD(fi.dwFileVersionLS)};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- ImageFile

bool ImageFile::Load(const std::wstring& path, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open file");
    data_.assign(std::istreambuf_iterator<char>(f), {});
    if (data_.size() < sizeof(IMAGE_DOS_HEADER)) return fail("file too small");

    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, data_.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return fail("no MZ header");
    std::size_t nt = static_cast<std::size_t>(dos.e_lfanew);
    if (nt + 4 + sizeof(IMAGE_FILE_HEADER) + 2 > data_.size()) return fail("bad e_lfanew");
    if (std::memcmp(data_.data() + nt, "PE\0\0", 4) != 0) return fail("no PE signature");

    IMAGE_FILE_HEADER fh;
    std::memcpy(&fh, data_.data() + nt + 4, sizeof(fh));
    machine_ = fh.Machine;
    std::size_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic;
    std::memcpy(&magic, data_.data() + opt, 2);
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        dirOffset_ = opt + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
        std::memcpy(&dirCount_, data_.data() + opt + offsetof(IMAGE_OPTIONAL_HEADER64, NumberOfRvaAndSizes), 4);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        dirOffset_ = opt + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
        std::memcpy(&dirCount_, data_.data() + opt + offsetof(IMAGE_OPTIONAL_HEADER32, NumberOfRvaAndSizes), 4);
    } else {
        return fail("unknown optional header magic");
    }
    sectionsOffset_ = opt + fh.SizeOfOptionalHeader;
    sectionCount_ = fh.NumberOfSections;
    if (sectionsOffset_ + sectionCount_ * sizeof(IMAGE_SECTION_HEADER) > data_.size())
        return fail("section table out of file");
    return true;
}

std::optional<std::size_t> ImageFile::RvaToOffset(std::uint32_t rva) const {
    for (std::uint16_t i = 0; i < sectionCount_; ++i) {
        IMAGE_SECTION_HEADER s;
        std::memcpy(&s, data_.data() + sectionsOffset_ + i * sizeof(s), sizeof(s));
        std::uint32_t span = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
        if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) {
            std::size_t off = s.PointerToRawData + (rva - s.VirtualAddress);
            if (off < data_.size()) return off;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string ImageFile::StringAtRva(std::uint32_t rva) const {
    auto off = RvaToOffset(rva);
    if (!off) return {};
    std::string s;
    for (std::size_t i = *off; i < data_.size() && data_[i] && s.size() < 512; ++i)
        s.push_back(static_cast<char>(data_[i]));
    return s;
}

const IMAGE_DATA_DIRECTORY* ImageFile::Directory(int index) const {
    if (static_cast<std::uint32_t>(index) >= dirCount_) return nullptr;
    std::size_t off = dirOffset_ + index * sizeof(IMAGE_DATA_DIRECTORY);
    if (off + sizeof(IMAGE_DATA_DIRECTORY) > data_.size()) return nullptr;
    auto d = reinterpret_cast<const IMAGE_DATA_DIRECTORY*>(data_.data() + off);
    return d->VirtualAddress ? d : nullptr;
}

std::vector<std::string> ImageFile::Imports() const {
    std::vector<std::string> out;
    auto dir = Directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!dir) return out;
    auto off = RvaToOffset(dir->VirtualAddress);
    for (; off && *off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= data_.size();
         *off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR d;
        std::memcpy(&d, data_.data() + *off, sizeof(d));
        if (!d.Name) break;
        out.push_back(StringAtRva(d.Name));
    }
    return out;
}

std::vector<std::string> ImageFile::DelayImports() const {
    std::vector<std::string> out;
    auto dir = Directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (!dir) return out;
    auto off = RvaToOffset(dir->VirtualAddress);
    for (; off && *off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= data_.size();
         *off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
        IMAGE_DELAYLOAD_DESCRIPTOR d;
        std::memcpy(&d, data_.data() + *off, sizeof(d));
        if (!d.DllNameRVA) break;
        out.push_back(StringAtRva(d.DllNameRVA));
    }
    return out;
}

std::vector<std::string> ImageFile::ExportNames() const {
    std::vector<std::string> out;
    auto dir = Directory(IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!dir) return out;
    auto off = RvaToOffset(dir->VirtualAddress);
    if (!off || *off + sizeof(IMAGE_EXPORT_DIRECTORY) > data_.size()) return out;
    IMAGE_EXPORT_DIRECTORY e;
    std::memcpy(&e, data_.data() + *off, sizeof(e));
    auto names = RvaToOffset(e.AddressOfNames);
    if (!names) return out;
    for (DWORD i = 0; i < e.NumberOfNames && *names + (i + 1) * 4 <= data_.size(); ++i) {
        std::uint32_t rva;
        std::memcpy(&rva, data_.data() + *names + i * 4, 4);
        out.push_back(StringAtRva(rva));
    }
    return out;
}

}  // namespace shvi::pe

// ---------------------------------------------------------------- MappedImage

namespace shvi::pe {

void MappedImage::Free::operator()(void* p) const {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

bool MappedImage::MakeExecutable() {
    DWORD old = 0;
    return memory && VirtualProtect(memory.get(), module.image.size, PAGE_EXECUTE_READ, &old) != 0;
}

std::optional<MappedImage> MapImageFile(const std::wstring& path, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<MappedImage> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open file");
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(f)), {});

    auto in = [&](std::size_t off, std::size_t n) { return off <= file.size() && n <= file.size() - off; };
    if (!in(0, sizeof(IMAGE_DOS_HEADER))) return fail("file too small");
    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, file.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return fail("no MZ header");
    auto ntOff = static_cast<std::size_t>(dos.e_lfanew);
    if (!in(ntOff, sizeof(IMAGE_NT_HEADERS64))) return fail("bad e_lfanew");
    IMAGE_NT_HEADERS64 nt;
    std::memcpy(&nt, file.data() + ntOff, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("not a PE32+ image");

    const auto& opt = nt.OptionalHeader;
    std::size_t imageSize = opt.SizeOfImage;
    if (imageSize == 0 || opt.SizeOfHeaders > imageSize || !in(0, opt.SizeOfHeaders))
        return fail("bad image/header size");

    void* mem = VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return fail("VirtualAlloc failed");
    MappedImage m;
    m.memory.reset(mem);
    auto base = reinterpret_cast<std::uintptr_t>(mem);
    auto* dst = static_cast<std::uint8_t*>(mem);
    std::memcpy(dst, file.data(), opt.SizeOfHeaders);

    m.preferredBase = static_cast<std::uintptr_t>(opt.ImageBase);
    m.timeDateStamp = nt.FileHeader.TimeDateStamp;
    m.entryRva = opt.AddressOfEntryPoint;
    m.module.image = {base, imageSize};

    std::size_t secOff = ntOff + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh;
        if (!in(secOff + i * sizeof(sh), sizeof(sh))) return fail("truncated section table");
        std::memcpy(&sh, file.data() + secOff + i * sizeof(sh), sizeof(sh));
        std::size_t vsize = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        if (sh.VirtualAddress > imageSize || vsize > imageSize - sh.VirtualAddress)
            return fail("section outside image");
        std::size_t raw = sh.SizeOfRawData < vsize ? sh.SizeOfRawData : vsize;
        if (raw && !in(sh.PointerToRawData, raw)) return fail("section data outside file");
        if (raw) std::memcpy(dst + sh.VirtualAddress, file.data() + sh.PointerToRawData, raw);
        char name[9] = {};
        std::memcpy(name, sh.Name, 8);
        m.module.sections.push_back({name, {base + sh.VirtualAddress, vsize}, sh.Characteristics});
    }

    // Base relocations: make absolute pointers point into this mapping.
    const auto& rel = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    auto delta = static_cast<std::int64_t>(base - m.preferredBase);
    if (delta != 0 && rel.VirtualAddress && rel.Size && rel.VirtualAddress <= imageSize &&
        rel.Size <= imageSize - rel.VirtualAddress) {
        std::size_t p = rel.VirtualAddress, end = p + rel.Size;
        while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
            IMAGE_BASE_RELOCATION blk;
            std::memcpy(&blk, dst + p, sizeof(blk));
            if (blk.SizeOfBlock < sizeof(blk) || blk.SizeOfBlock > end - p) break;
            std::size_t n = (blk.SizeOfBlock - sizeof(blk)) / 2;
            for (std::size_t k = 0; k < n; ++k) {
                std::uint16_t e;
                std::memcpy(&e, dst + p + sizeof(blk) + 2 * k, 2);
                std::size_t at = static_cast<std::size_t>(blk.VirtualAddress) + (e & 0x0FFF);
                if ((e >> 12) == IMAGE_REL_BASED_DIR64 && at + 8 <= imageSize) {
                    std::uint64_t v;
                    std::memcpy(&v, dst + at, 8);
                    v += static_cast<std::uint64_t>(delta);
                    std::memcpy(dst + at, &v, 8);
                }
            }
            p += blk.SizeOfBlock;
        }
        m.relocated = true;
    }
    return m;
}

}  // namespace shvi::pe
