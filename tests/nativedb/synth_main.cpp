// shvi_nativedb_synth <out.exe>: writes the synthetic PE the nativedb ctests run on.
#include "synth_pe.h"

#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shvi_nativedb_synth <out.exe>\n");
        return 2;
    }
    auto bytes = shvi::nativedb::test::BuildSynthPe();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path(), ec);
    FILE* f = _wfopen(argv[1], L"wb");
    if (!f) {
        std::fprintf(stderr, "shvi_nativedb_synth: cannot open output\n");
        return 2;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        std::fprintf(stderr, "shvi_nativedb_synth: write failed\n");
        return 2;
    }
    return 0;
}
