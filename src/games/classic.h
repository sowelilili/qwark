/*
 * The handful of things the four PS3 originals genuinely share (DESIGN.md 3.6).
 * Anything that is true of only some of them stays in the game file.
 */
#ifndef QWARK_CLASSIC_H
#define QWARK_CLASSIC_H

#include "game.h"
#include "../core/mem.h"

/* The four analog floats: rx, ry, lx, ly at +0, +4, +8, +12. */
void classic_decode_analogs(const u8 *block, f32 out[4]);

/*
 * The RaC1 to RaC3 planet request: "00000001 000000XX" written as two words at
 * the request address. Deadlocked does it differently and keeps its own.
 */
int  classic_planet_request(u32 addr, u8 planet);

/* Ratchet dies by having his Z driven far below the level: -50.0f. */
#define CLASSIC_DEATH_Z_BITS 0xC2480000u
int  classic_die_set_z(u32 coords_addr);

/* Ghost Ratchet is a frame countdown; all four games freeze it to 10. */
#define CLASSIC_GHOST_VALUE 10
int  classic_ghost(u32 timer_addr, int on);

/* Turning a patch off that is already off is not an error. */
int  classic_patch_toggle(const struct patch_def *def, int on);

/*
 * Chargeboot colours, RaC2 and RaC3. Both store one colour per word as the four
 * bytes { alpha, B, G, R }, and ChargebootColorPicker wrote 0x40 as the alpha
 * for the front and tint words and 0x25 for the primary back one. The wire
 * carries 0x00RRGGBB, so these two translate.
 */
#define CLASSIC_CB_ALPHA_FRONT 0x40u
#define CLASSIC_CB_ALPHA_BACK  0x25u
int  classic_cb_write(u32 addr, u8 alpha, u32 rgb);
u32  classic_cb_to_rgb(u32 word);

#endif /* QWARK_CLASSIC_H */
