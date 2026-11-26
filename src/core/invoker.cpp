#include "invoker.h"

#include "log.h"

#include <windows.h>

namespace shvi {
namespace {

// No C++ objects in here: __try can't coexist with unwinding in one frame.
bool CallTargetSeh(const NativeTarget* t, rage::NativeCallContext* ctx, rage::VectorFixupFn fix) {
    __try {
        if (t->handler) {
            t->handler(ctx);
            fix(ctx);
        } else {
            t->thunk(t->fn, ctx);  // writes script-layout results itself
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

NativeTarget Invoker::Lookup(std::uint64_t hash) {
    if (auto it = cache_.find(hash); it != cache_.end()) return it->second;
    NativeTarget h = resolver_ ? resolver_(hash) : NativeTarget{};
    cache_.emplace(hash, h);  // negative results are cached too
    if (!h.Valid()) SHVI_LOG("invoker: native 0x%016llX is not registered in this build", hash);
    return h;
}

void Invoker::Begin(std::uint64_t hash) {
    requestedHash_ = hash;
    hash_ = crossmap_ ? crossmap_->Map(hash) : hash;
    ctx_ = {};
    ctx_.ret = ret_;
    ctx_.args = args_;
    std::memset(ret_, 0, sizeof(ret_));
    std::memset(args_, 0, sizeof(args_));  // engine thunks read missing args as 0
}

void Invoker::Push64(std::uint64_t v) {
    if (ctx_.argCount < kMaxArgs) args_[ctx_.argCount++] = v;
}

std::uint64_t* Invoker::CallImpl(bool gated) {
    lastFailed_ = true;
    if (gated && gate_ && !gate_()) return ret_;
    if (gated && denylist_ && (denylist_->Contains(hash_) || denylist_->Contains(requestedHash_))) {
        ++denied_;
        return ret_;
    }
    NativeTarget t = Lookup(hash_);
    if (!t.Valid()) {
        ++unresolved_;
        return ret_;
    }
    if (!CallTargetSeh(&t, &ctx_, fixup_)) {
        ++faults_;
        std::memset(ret_, 0, sizeof(ret_));
        SHVI_LOG("invoker: native 0x%016llX faulted (bad arguments?)", hash_);
        return ret_;
    }
    lastFailed_ = false;
    return ret_;
}

}  // namespace shvi
