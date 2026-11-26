// GENERIC (given a resolver). Calls game functions from C++ by hash: fill the
// call frame (NativeCallContext), call the target, run the vector fix-up.
// Game thread only.
//
// A hash resolves to a NativeTarget: a handler taking the call frame
// directly, or a thunk + function pointer. GTA VI binds typed engine
// functions and VI script natives that way (engine_call.h). Gate, crossmap
// and SEH isolation apply to all of them alike.
//
// Two entry points:
//   * Begin/Push/Call (and Invoke<R>) - the public path. Goes through the
//     gate (the online guard): when the gate says no, nothing is called and
//     the return slots read as zero.
//   * InvokeUnchecked<R> - for the guard's own probes (e.g.
//     NETWORK_IS_SESSION_STARTED), which must run while the gate is closed.
#pragma once
#include "crossmap.h"
#include "denylist.h"
#include "native_context.h"

#include <cstring>
#include <functional>
#include <type_traits>
#include <unordered_map>

#pragma warning(disable : 4324)  // padding from alignas buffers is intended

namespace shvi {

// Reads its arguments from ctx->args and writes its result to ctx->ret, the
// same contract as a RAGE handler, but calls the typed function `fn`.
using EngineThunk = void (*)(void* fn, rage::NativeCallContext* ctx);

struct NativeTarget {
    rage::NativeHandler handler = nullptr;
    EngineThunk thunk = nullptr;
    void* fn = nullptr;

    NativeTarget() = default;
    NativeTarget(rage::NativeHandler h) : handler(h) {}  // NOLINT: implicit on purpose
    NativeTarget(EngineThunk t, void* f) : thunk(t), fn(f) {}
    bool Valid() const { return handler != nullptr || (thunk != nullptr && fn != nullptr); }
};

class Invoker {
public:
    using Resolver = std::function<NativeTarget(std::uint64_t hash)>;
    using Gate = std::function<bool()>;
    static constexpr int kMaxArgs = 32;

    Invoker(Resolver resolver, rage::VectorFixupFn fixup = rage::GenericVectorFixup)
        : resolver_(std::move(resolver)), fixup_(fixup ? fixup : rage::GenericVectorFixup) {}

    void SetGate(Gate g) { gate_ = std::move(g); }
    void SetCrossmap(const Crossmap* m) { crossmap_ = m; }
    void SetDenylist(const Denylist* d) { denylist_ = d; }  // gated path only

    // --- SHV-style API -------------------------------------------------
    void Begin(std::uint64_t hash);
    void Push64(std::uint64_t v);
    std::uint64_t* Call() { return CallImpl(true); }

    template <class R = void, class... A>
    R Invoke(std::uint64_t hash, A... a) { return InvokeImpl<R>(true, hash, a...); }
    template <class R = void, class... A>
    R InvokeUnchecked(std::uint64_t hash, A... a) { return InvokeImpl<R>(false, hash, a...); }

    // Diagnostics
    bool LastCallFailed() const { return lastFailed_; }
    std::size_t UnresolvedCount() const { return unresolved_; }
    std::size_t FaultCount() const { return faults_; }
    std::size_t DeniedCount() const { return denied_; }
    bool Has(std::uint64_t hash) { return Lookup(hash).Valid(); }
    // As Begin() resolves it: through the crossmap first. Quiet (no "not registered" log line).
    bool HasMapped(std::uint64_t hash) {
        std::uint64_t h = crossmap_ ? crossmap_->Map(hash) : hash;
        if (auto it = cache_.find(h); it != cache_.end()) return it->second.Valid();
        return resolver_ ? resolver_(h).Valid() : false;
    }
    void ClearCache() { cache_.clear(); }

private:
    template <class T>
    static std::uint64_t Slot(T v) {
        static_assert(sizeof(T) <= 8);
        if constexpr (std::is_same_v<T, float>) {
            std::uint32_t b; std::memcpy(&b, &v, 4); return b;
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? 1u : 0u;
        } else if constexpr (std::is_pointer_v<T>) {
            return reinterpret_cast<std::uintptr_t>(v);
        } else if constexpr (std::is_signed_v<T>) {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
        } else {
            return static_cast<std::uint64_t>(v);
        }
    }

    template <class R, class... A>
    R InvokeImpl(bool gated, std::uint64_t hash, A... a) {
        Begin(hash);
        (Push64(Slot(a)), ...);
        std::uint64_t* r = CallImpl(gated);
        if constexpr (std::is_void_v<R>) {
            return;
        } else if constexpr (std::is_same_v<R, bool>) {
            return (r[0] & 0xFF) != 0;
        } else {
            static_assert(std::is_trivially_copyable_v<R> && sizeof(R) <= sizeof(ret_));
            R out; std::memcpy(&out, r, sizeof(R)); return out;
        }
    }

    std::uint64_t* CallImpl(bool gated);
    NativeTarget Lookup(std::uint64_t hash);

    Resolver resolver_;
    rage::VectorFixupFn fixup_;
    Gate gate_;
    const Crossmap* crossmap_ = nullptr;
    const Denylist* denylist_ = nullptr;
    std::unordered_map<std::uint64_t, NativeTarget> cache_;

    std::uint64_t hash_ = 0, requestedHash_ = 0;
    alignas(16) std::uint64_t args_[kMaxArgs] = {};
    alignas(16) std::uint64_t ret_[4] = {};  // 3 slots for a Vector3 return
    rage::NativeCallContext ctx_ = {};
    bool lastFailed_ = false;
    std::size_t unresolved_ = 0, faults_ = 0, denied_ = 0;
};

}  // namespace shvi
