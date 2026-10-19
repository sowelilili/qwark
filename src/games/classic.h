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

/*
 * Build 53: Freeze health, in all four games. Each keeps the player's maximum
 * beside the hit points, and the game itself moves it as the health upgrades
 * come in (RaC1's two nanotech items, the experience levels and nanotech boosts
 * of the other three), so the toggle holds the hit points at whatever that
 * maximum is now rather than at a fixed number: a freeze entry carries it, and
 * classic_health_tick, which the game's on_tick calls, reads the maximum every
 * CLASSIC_HEALTH_EVERY ticks and moves the entry with it.
 *
 * The two words are the same type in every game, a signed word in RaC1 to RaC3
 * and a float in Deadlocked, so the four bytes are copied as they are. While the
 * maximum reads as nothing a game would hold (0 before a game sets it, a
 * negative or a non-finite float) no entry is kept, so the toggle never pins a
 * player at 0. Off removes the entry and leaves the hit points where they are.
 *
 * Build 53: the entry is only ever the toggle's own. The freeze table keys an
 * entry by its address and size, so a freeze the user sets on the hit points
 * in the Memory panel is the same entry, and the toggle used to write its value
 * over the user's, take it out while the maximum read as nothing, and take it
 * out when it went off. Now an entry the toggle did not put there, or whose
 * value someone else has changed since (`held`), is the user's: the toggle
 * leaves it as it is, its value and all, for as long as it is there, and puts
 * its own back once the user removes theirs.
 */
struct classic_health {
	u32 health;     /* the hit points */
	u32 max;        /* the game's own maximum, the same type */
	u8  is_float;   /* both are f32 (Deadlocked) rather than s32 */
	u8  on;         /* the toggle, qwark-side state */
	u8  wait;       /* ticks to the next look at the maximum */
	u8  owned;      /* build 53: the entry on `health` is the toggle's */
	u32 held;       /* build 53: the value the toggle last gave it */
};

#define CLASSIC_HEALTH_EVERY 8

int  classic_health_toggle(struct classic_health *h, int on);
void classic_health_tick(struct classic_health *h);
/* A new process: the toggle goes with it, as its bit in toggle_state does. */
void classic_health_forget(struct classic_health *h);

/* Turning a patch off that is already off is not an error. */
int  classic_patch_toggle(const struct patch_def *def, int on);

/*
 * The unlock snapshot. A game's unlock_list reads the arrays its table covers
 * into here, and its unlock_read serves each row from them: the core calls the
 * two back to back on the tick thread, and the menu reuses one list for the rest
 * of its tick. Only the running game ever fills it, so the four games share this
 * one buffer, where each used to keep its own (SPEC-footprint.md, cut C12). It is
 * sized by the largest, UYA's three reads, and each game checks its own fit at
 * build time.
 *
 * classic_snap_game is the game_id whose bytes it holds, 0 while it holds none.
 * It stands in for the flag each game kept beside its own buffer: a game sets it
 * to 0 before its reads and to its own id once they have all succeeded, and
 * serves nothing while it says another game, so no game ever reads another's.
 */
#define CLASSIC_SNAP_BYTES 2276u

extern u8 classic_snap[CLASSIC_SNAP_BYTES];
extern u8 classic_snap_game;

/*
 * Build 50, the code switches (src/core/switches.h). A WRITES_CODE toggle is
 * one of these three rather than a bare classic_patch_toggle, each naming the
 * flag byte its switch reads (switches_bins.h) beside the patch it makes on a
 * console. While the running game's switches are in - only ever under RPCS3 -
 * the flag is what they write and read; otherwise they are exactly the patch
 * calls they replace.
 *
 *   classic_code_toggle  classic_patch_toggle, or the flag at 1 or 0
 *   classic_code_force   patch_apply, or the flag at 1: the fast-load forcing
 *                        a planet load does behind the checkbox's back
 *   classic_code_read    the flag, for toggle_read; NOT_FOUND without the
 *                        switches, where the toggle is a patch and memory says
 *                        nothing qwark's own bit does not
 *
 * The PS3 module never has the switches (build 51, QWARK_EMULATOR in plat.h),
 * so there the three are the patch calls themselves and the flag is dropped.
 * They are macros there rather than inline functions because gcc 4.1 at -Os
 * kept an out-of-line copy of the toggle in every game that calls it often.
 */
#ifdef QWARK_EMULATOR
int  classic_code_toggle(const struct patch_def *def, u32 flag, int on);
int  classic_code_force(const struct patch_def *def, u32 flag);
int  classic_code_read(u32 flag, int *on);
#else
#define classic_code_toggle(def, flag, on) classic_patch_toggle((def), (on))
#define classic_code_force(def, flag)      patch_apply(def)
#define classic_code_read(flag, on)        ((void)(on), ST_NOT_FOUND)
#endif

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
