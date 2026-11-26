// GENERIC. Functional checks for resolved engine functions: a signature that
// matched is a guess; a function that computes the right answers is a fact.
// Only for PURE functions (no game state needed). Each call is SEH-guarded.
#pragma once
#include <cstdint>
#include <string>

namespace shvi::probes {

// fn: std::uint32_t(const char* str, std::uint32_t seed), RAGE's
// atPartialStringHash shape. Passes if it reproduces Joaat() (case-folded
// one-at-a-time) for a fixed set of names, mixed case included.
bool StringHashMatchesJoaat(std::uintptr_t fn, std::string* why);

// One SEH-guarded call of such a function with seed 0. False if it faulted.
bool CallStringHash(std::uintptr_t fn, const char* s, std::uint32_t* out);

}  // namespace shvi::probes
