// GENERIC. Direct engine calls behind the SDK's hash-based invoke API.
//
// GTA VI has a script VM but no native registry with names or hashes
// (research plan Q4, outcome (b)): script natives are addressed by code
// offset from each compiled program. What a plugin calls instead is a named
// function: a typed engine function found by signature (Bind), or a VI
// script native once its handler is identified (BindViNative). To keep the
// SDK unchanged (shvi_api still invokes by 64-bit hash with 8-byte argument
// slots), each engine function is bound with its C++ signature, and a
// generated thunk unpacks the slots, calls the function and writes the result
// back the way a script would read it:
//   * integers and enums: sign- or zero-extended to 64 bits
//   * float: low 32 bits of the slot
//   * bool: 0 or 1
//   * pointer: the address
//   * rage::scrVec3N (engine Vector3) is widened to rage::scrVector (24 bytes, 3 slots)
//
// The names live in their own hash space: EngineHash(name) = "VI" tag in the top
// 16 bits | case-folded joaat (the SDK's shvi_engine_hash). A crossmap can
// still route one name's hash to another.
#pragma once
#include "invoker.h"
#include "joaat.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace shvi {

inline constexpr std::uint64_t kEngineHashTag = 0x5649'0000'0000'0000ull;  // "VI"

constexpr std::uint64_t EngineHash(std::string_view name) {
    return kEngineHashTag | Joaat(name);
}

namespace detail {

template <class T>
T FromSlot(std::uint64_t s) {
    if constexpr (std::is_same_v<T, float>) {
        auto b = static_cast<std::uint32_t>(s);
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    } else if constexpr (std::is_same_v<T, bool>) {
        return (s & 0xFF) != 0;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(s));
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported engine argument type");
        return static_cast<T>(s);
    }
}

template <class R>
void ToRet(void* ret, R v) {
    if constexpr (std::is_same_v<R, rage::scrVec3N>) {
        rage::scrVector out{v.x, 0, v.y, 0, v.z, 0};
        std::memcpy(ret, &out, sizeof(out));
    } else {
        std::uint64_t slot = 0;
        if constexpr (std::is_same_v<R, float>) {
            std::memcpy(&slot, &v, 4);
        } else if constexpr (std::is_same_v<R, bool>) {
            slot = v ? 1u : 0u;
        } else if constexpr (std::is_pointer_v<R>) {
            slot = reinterpret_cast<std::uintptr_t>(v);
        } else if constexpr (std::is_enum_v<R>) {
            slot = static_cast<std::uint64_t>(static_cast<std::underlying_type_t<R>>(v));
        } else {
            static_assert(std::is_integral_v<R>, "unsupported engine return type");
            if constexpr (std::is_signed_v<R>)
                slot = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
            else
                slot = static_cast<std::uint64_t>(v);
        }
        std::memcpy(ret, &slot, sizeof(slot));
    }
}

template <class Sig>
struct Thunk;

template <class R, class... A>
struct Thunk<R(A...)> {
    static_assert(sizeof...(A) <= Invoker::kMaxArgs);

    static void Call(void* fn, rage::NativeCallContext* c) {
        Run(reinterpret_cast<R (*)(A...)>(fn), c, std::index_sequence_for<A...>{});
    }

private:
    template <std::size_t... I>
    static void Run(R (*f)(A...), rage::NativeCallContext* c, std::index_sequence<I...>) {
        const auto* s = static_cast<const std::uint64_t*>(c->args);
        if constexpr (std::is_void_v<R>) {
            f(FromSlot<A>(I < c->argCount ? s[I] : 0)...);
        } else {
            ToRet<R>(c->ret, f(FromSlot<A>(I < c->argCount ? s[I] : 0)...));
        }
    }
};

// A GTA VI native handler behind the generic invoker: rebuild the VM's
// 24-byte context from ours. The caller doesn't say how many results it
// expects, so retCount = 3 (enough for a Vector3; the invoker's return buffer
// has 4 slots).
inline void ViNativeThunk(void* fn, rage::NativeCallContext* c) {
    rage::ViNativeContext v{static_cast<std::uint64_t*>(c->args), static_cast<std::uint64_t*>(c->ret),
                            c->argCount, 3};
    reinterpret_cast<rage::ViNativeHandler>(fn)(&v);
}

}  // namespace detail

// hash -> typed engine function. Fill once at Resolve time; read on the game thread.
class EngineTable {
public:
    template <class Sig>
    void Bind(std::uint64_t hash, Sig* fn) {
        map_[hash] = NativeTarget(&detail::Thunk<Sig>::Call, reinterpret_cast<void*>(fn));
    }
    // For addresses that came out of the signature scanner.
    template <class Sig>
    void BindAddress(std::uint64_t hash, std::uintptr_t address) {
        if (address) Bind<Sig>(hash, reinterpret_cast<Sig*>(address));
    }

    // A VI script native (void(ViNativeContext*)), e.g. one whose address was
    // recovered from a script program's native-call operands.
    void BindViNative(std::uint64_t hash, rage::ViNativeHandler fn) {
        if (fn) map_[hash] = NativeTarget(&detail::ViNativeThunk, reinterpret_cast<void*>(fn));
    }

    NativeTarget Find(std::uint64_t hash) const {
        auto it = map_.find(hash);
        return it == map_.end() ? NativeTarget{} : it->second;
    }
    std::size_t Size() const { return map_.size(); }

private:
    std::unordered_map<std::uint64_t, NativeTarget> map_;
};

}  // namespace shvi
