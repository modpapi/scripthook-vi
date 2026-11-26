/* PS5 pad -> SHVI key codes (pure, host-testable).
 *
 * Mods see the same virtual-key codes on every platform (shvi/ui.hpp, namespace vk). On PS5 the
 * runtime turns pad button edges into those codes:
 *
 *   L1 held + R3 press  -> F4        open / close the menu
 *   D-pad               -> Up, Down, Left, Right
 *   Cross               -> Return    select
 *   Circle              -> Back      back
 *   Square              -> Numpad5   (alternate select)
 *   Triangle            -> Numpad0   (alternate back)
 *
 * While the menu combo's L1 is held, the d-pad and face buttons are still reported, so a mod can
 * also read raw combos. Bits follow the ScePadData button mask used by PS4/PS5 homebrew.
 */
#ifndef SHVI_PS5_PAD_MAP_H
#define SHVI_PS5_PAD_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHVI_PAD_L3       0x00000002u
#define SHVI_PAD_R3       0x00000004u
#define SHVI_PAD_OPTIONS  0x00000008u
#define SHVI_PAD_UP       0x00000010u
#define SHVI_PAD_RIGHT    0x00000020u
#define SHVI_PAD_DOWN     0x00000040u
#define SHVI_PAD_LEFT     0x00000080u
#define SHVI_PAD_L2       0x00000100u
#define SHVI_PAD_R2       0x00000200u
#define SHVI_PAD_L1       0x00000400u
#define SHVI_PAD_R1       0x00000800u
#define SHVI_PAD_TRIANGLE 0x00001000u
#define SHVI_PAD_CIRCLE   0x00002000u
#define SHVI_PAD_CROSS    0x00004000u
#define SHVI_PAD_SQUARE   0x00008000u

typedef void (*shvi_pad_emit_fn)(uint32_t vk, int is_down, void* user);

/* Compares two button masks and emits a key event per mapped edge. Returns the number emitted. */
int shvi_ps5_pad_edges(uint32_t prev, uint32_t now, shvi_pad_emit_fn emit, void* user);

#ifdef __cplusplus
}
#endif
#endif /* SHVI_PS5_PAD_MAP_H */
