#include "probes.h"

#include "joaat.h"

#include <windows.h>

#include <cstdio>

namespace shvi::probes {
namespace {

using StringHashFn = std::uint32_t (*)(const char*, std::uint32_t);

// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool CallSeh(StringHashFn f, const char* s, std::uint32_t* out) {
    __try {
        *out = f(s, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool CallStringHash(std::uintptr_t fn, const char* s, std::uint32_t* out) {
    return fn && CallSeh(reinterpret_cast<StringHashFn>(fn), s, out);
}

bool StringHashMatchesJoaat(std::uintptr_t fn, std::string* why) {
    auto set = [&](const char* m) { if (why) *why = m; return false; };
    if (!fn) return set("no address");
    static const char* const kNames[] = {"adder", "ADDER", "player_story000_00", "Cs_Mandy",
                                         "MOBILE_PRERING", "AMBIENT_SCRIPT_SLOT_0",
                                         "Data\\Path/Mixed", "\"Quoted\"tail"};
    auto f = reinterpret_cast<StringHashFn>(fn);
    for (const char* n : kNames) {
        std::uint32_t got = 0;
        if (!CallSeh(f, n, &got)) return set("probe call faulted");
        std::uint32_t want = Joaat(n);
        if (got != want) {
            char buf[128];
            snprintf(buf, sizeof(buf), "hash(\"%s\") = 0x%08X, joaat = 0x%08X", n, got, want);
            if (why) *why = buf;
            return false;
        }
    }
    if (why) why->clear();
    return true;
}

}  // namespace shvi::probes
