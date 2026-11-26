// ScriptHook VI - header-only C++ convenience layer over the C ABI (shvi.h).
// Nothing here crosses the DLL boundary, so it can evolve freely.
#pragma once
#include "shvi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace shvi {

struct Vector3 {  // script vector: each component sits in its own 8-byte slot
    float x; std::uint32_t _px;
    float y; std::uint32_t _py;
    float z; std::uint32_t _pz;
};
static_assert(sizeof(Vector3) == 24);

namespace sdk {
// constexpr twin of shvi_engine_hash(): engine function names -> invoke hash.
constexpr std::uint64_t EngineHash(std::string_view name) {
    std::uint32_t h = 0;
    std::size_t i = 0, n = name.size();
    bool quoted = n && name[0] == '"';
    if (quoted) ++i;
    for (; i < n && !(quoted && name[i] == '"'); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c == '\\') c = '/';
        h += static_cast<std::uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return SHVI_ENGINE_HASH_TAG | h;
}
}  // namespace sdk

inline const shvi_api*& api() { static const shvi_api* p = nullptr; return p; }

template <class T>
inline std::uint64_t to_slot(T v) {
    static_assert(sizeof(T) <= 8, "native arguments are at most 8 bytes");
    if constexpr (std::is_same_v<T, float>) {
        return shvi_f2u(v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return v ? 1u : 0u;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uintptr_t>(v);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<std::uint64_t>(static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_signed_v<T>) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));  // sign-extend like the VM
    } else {
        return static_cast<std::uint64_t>(v);
    }
}

template <class R>
inline R from_slots(const std::uint64_t* slots) {
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_same_v<R, bool>) {
        return (slots[0] & 0xFF) != 0;
    } else {
        static_assert(std::is_trivially_copyable_v<R> && sizeof(R) <= 32);
        R r;
        std::memcpy(&r, slots, sizeof(R));
        return r;
    }
}

// invoke<std::uint32_t>(sdk::EngineHash("STRING_HASH"), "name", 0u)
template <class R = void, class... Args>
inline R invoke(std::uint64_t hash, Args... args) {
    const shvi_api* a = api();
    a->native_init(hash);
    (a->native_push64(to_slot(args)), ...);
    return from_slots<R>(a->native_call());
}

inline void wait(std::uint32_t ms = 0) { api()->script_wait(ms); }

// ABI 1.2 entity calls. Older hosts report SHVI_E_UNSUPPORTED / nullptr.
inline int world_get_all(std::uint32_t kind, shvi_entity_ref* out, int max) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, world_get_all) ? a->world_get_all(kind, out, max) : SHVI_E_UNSUPPORTED;
}
inline void* entity_ptr(const shvi_entity_ref& ref) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, entity_ptr) ? a->entity_ptr(&ref) : nullptr;
}

// ABI 1.3 module statics (VI's getGlobalPtr). nullptr on older hosts.
template <class T = void>
inline T* static_ptr(std::uint32_t program, std::uint32_t module, std::uint32_t field) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, static_ptr) ? static_cast<T*>(a->static_ptr(program, module, field)) : nullptr;
}

// ABI 1.4 present callbacks. Older hosts report SHVI_E_UNSUPPORTED.
inline int present_register(shvi_present_fn fn, void* user = nullptr) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, present_register) ? a->present_register(fn, user) : SHVI_E_UNSUPPORTED;
}
inline int present_unregister(shvi_present_fn fn, void* user = nullptr) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, present_unregister) ? a->present_unregister(fn, user) : SHVI_E_UNSUPPORTED;
}

// ABI 1.5: is this native name bound in the running build? false on older hosts.
inline bool native_exists(std::uint64_t hash) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, native_exists) && a->native_exists(hash) != 0;
}
inline bool native_exists(std::string_view name) { return native_exists(sdk::EngineHash(name)); }

// ABI 1.6: call a function of a linked script program. SHVI_E_UNSUPPORTED on older hosts.
inline int script_function_call(std::uint32_t program, std::uint32_t offset, const std::uint64_t* args, std::uint32_t argc,
                                std::uint64_t* ret, std::uint32_t retc) {
    const shvi_api* a = api();
    return SHVI_API_HAS(a, script_function_call) ? a->script_function_call(program, offset, args, argc, ret, retc)
                                                 : SHVI_E_UNSUPPORTED;
}

}  // namespace shvi
