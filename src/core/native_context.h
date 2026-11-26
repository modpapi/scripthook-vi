// GENERIC. The invoker's call frame (NativeCallContext), which plugins fill
// slot by slot through the SDK, plus the VI script VM's native context.
// The frame keeps the classic RAGE scrNativeCallContext shape, so a handler
// written against it can be called directly.
//
// Vector fix-up: script vectors are three floats, each in its own 8-byte slot
// (scrVector, 24 bytes). Native handlers work on aligned 16-byte Vector3s, so
// when a handler gets a Vector3* OUT parameter it writes into inVectors[n],
// remembers the caller's pointer in outVectors[n] and bumps dataCount. After
// the handler returns, the VM (or an external invoker such as ours) must copy
// inVectors back to the script-layout pointers. Skip this and natives like
// GET_MODEL_DIMENSIONS leave garbage in your out params.
#pragma once
#include <cstddef>
#include <cstdint>

namespace shvi::rage {

struct scrVector {  // script layout
    float x; std::uint32_t _px;
    float y; std::uint32_t _py;
    float z; std::uint32_t _pz;
};
static_assert(sizeof(scrVector) == 24);

struct alignas(16) scrVec3N {  // native/engine layout
    float x, y, z;
    std::uint32_t w;
};

struct NativeCallContext {
    void* ret;                  // +0x00 return slots
    std::uint32_t argCount;     // +0x08
    void* args;                 // +0x10 argument slots (8 bytes each)
    std::uint32_t dataCount;    // +0x18 pending vector fix-ups
    scrVector* outVectors[4];   // +0x20
    scrVec3N inVectors[4];      // +0x40
    std::uint8_t pad[96];       // +0x80
};
static_assert(offsetof(NativeCallContext, argCount) == 0x08);
static_assert(offsetof(NativeCallContext, args) == 0x10);
static_assert(offsetof(NativeCallContext, dataCount) == 0x18);
static_assert(offsetof(NativeCallContext, outVectors) == 0x20);
static_assert(offsetof(NativeCallContext, inVectors) == 0x40);
static_assert(sizeof(NativeCallContext) == 0xE0);

using NativeHandler = void (*)(NativeCallContext*);

// GTA VI native call context, read off the Xbox build's VM (opcode 139 at
// 0x143DE7411, ts 1794606036): the VM builds it on its own stack and calls
// handler(&ctx). args points into the script stack; ret is the stack top
// when retCount != 0, else null. No vector fix-up fields: none were seen.
struct ViNativeContext {
    std::uint64_t* args;        // +0x00
    std::uint64_t* ret;         // +0x08
    std::uint32_t argCount;     // +0x10
    std::uint32_t retCount;     // +0x14
};
static_assert(offsetof(ViNativeContext, ret) == 0x08);
static_assert(offsetof(ViNativeContext, argCount) == 0x10);
static_assert(offsetof(ViNativeContext, retCount) == 0x14);
static_assert(sizeof(ViNativeContext) == 0x18);
using ViNativeHandler = void (*)(ViNativeContext*);
using VectorFixupFn = void (*)(NativeCallContext*);

// Same effect as the game's routine (which counts dataCount down to 0).
inline void GenericVectorFixup(NativeCallContext* c) {
    std::uint32_t n = c->dataCount > 4 ? 4 : c->dataCount;
    for (std::uint32_t i = 0; i < n; ++i) {
        scrVector* out = c->outVectors[i];
        if (!out) continue;
        out->x = c->inVectors[i].x;
        out->y = c->inVectors[i].y;
        out->z = c->inVectors[i].z;
    }
    c->dataCount = 0;
}

}  // namespace shvi::rage
