#include "pad_map.h"

static const struct {
    uint32_t button, vk;
} kMap[] = {
    {SHVI_PAD_UP, 0x26},       {SHVI_PAD_DOWN, 0x28},   {SHVI_PAD_LEFT, 0x25},     {SHVI_PAD_RIGHT, 0x27},
    {SHVI_PAD_CROSS, 0x0D},    {SHVI_PAD_CIRCLE, 0x08}, {SHVI_PAD_SQUARE, 0x65},   {SHVI_PAD_TRIANGLE, 0x60},
};

#define VK_F4 0x73u

int shvi_ps5_pad_edges(uint32_t prev, uint32_t now, shvi_pad_emit_fn emit, void* user) {
    int n = 0;
    uint32_t pressed = now & ~prev, released = prev & ~now;
    unsigned i;
    if (!emit) return 0;
    /* The menu combo: R3 pressed while L1 is held. */
    if ((pressed & SHVI_PAD_R3) && (now & SHVI_PAD_L1)) {
        emit(VK_F4, 1, user);
        emit(VK_F4, 0, user);
        n += 2;
    }
    for (i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
        if (pressed & kMap[i].button) { emit(kMap[i].vk, 1, user); ++n; }
        if (released & kMap[i].button) { emit(kMap[i].vk, 0, user); ++n; }
    }
    return n;
}
