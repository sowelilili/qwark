/*
 * Revision 1.31: the VSH pad probe. The input display's pad at the XMB.
 *
 * qwark runs inside the VSH, so it can read the controller the way webMAN
 * MOD's XMB combos do, through the VSH's own libpad (plat_vsh_pad_read),
 * with no game running. SessionInfo's pad_mask and analog[] then come from
 * whichever of two views of pad 1 has the pad:
 *
 *   the VSH's view, while the VSH has the pad (the owner, below), when it has
 *     a button held (the buttons) or a stick out of its dead zone (the sticks);
 *   otherwise the running game's own pad, read out of game memory by the
 *     game's hot_decode as always, while a game is INGAME with its code in;
 *   otherwise nothing: 0 and four 0.0 sticks.
 *
 * The owner. At the XMB, with no game, the VSH always has the pad, and
 * libpad's len 0 ("same as before") is just that: a button held still stays
 * lit for as long as it is held. In a game the owner is the game, except
 * while the PS button's overlay has the pad, which is decided by
 *
 *   GetInfo2's system_info bit0 (INTERCEPTED), once it has shown it follows
 *     the overlay: its value is learnt from the game's own pad (the bit's
 *     value whenever the game takes input with the bit steady), and once it
 *     has been seen to differ from that in a game, the VSH has the pad exactly
 *     while it differs. Either sense works, so the VSH's libpad may set the
 *     bit while it has the pad or while it has not;
 *   until then, by who last took input: the game when its pad shows a change
 *     with something in it, the VSH when its libpad hands over a change with
 *     something in it while the game's pad is empty, and the game again once
 *     the VSH has handed over nothing new for VSHPAD_QUIET_US.
 *
 * Whenever the game takes the pad back the VSH's view is dropped, so its last
 * report (the press that closed the overlay, say) is never left lit and the
 * game's pad shows at once. The buttons and the sticks are decided apart. The
 * combos and the console menu keep the game's own pad: only what goes out to a
 * client changes. qwark.log gets a line at every change of owner saying what
 * drove it (rewrite/docs/REPORT-vsh-pad-probe.md).
 *
 * Read on the tick thread every tick (120 Hz, the rate the games' own pad is
 * read out of their memory), outside the core lock, and only while a client
 * is connected; merged under the core lock as SessionInfo is encoded, at the
 * telemetry rate. An error, or no pad API at all (the host and RPCS3), leaves
 * the game's pad alone, as before this revision.
 */
#ifndef QWARK_VSHPAD_H
#define QWARK_VSHPAD_H

#include "../plat/plat.h"

/*
 * A stick counts as moved past this far from 0x80 (of 128): a resting stick
 * wanders a few counts, and a stick held still must read as blank so the
 * other view's sticks are not hidden behind it. The game's float sticks the
 * same, as VSHPAD_DEADZONE / 128.
 */
#define VSHPAD_DEADZONE        32
/*
 * In a game, while system_info has not shown it follows the overlay, the VSH
 * gives the pad back to the game after this long with nothing new from its
 * libpad, so a press its last report holds is not left lit while the game's
 * pad stays empty. Never at the XMB, and never once system_info decides.
 */
#define VSHPAD_QUIET_US        1000000ull
/* GetInfo2, which picks the port and gives system_info: every 33 ms, a 30 Hz telemetry frame whatever the rate (revision 1.32). */
#define VSHPAD_INFO_EVERY_US   33333ull
/* system_info's bit is learnt from the game's input only once it has held still this long. */
#define VSHPAD_BIT_STEADY_US   250000ull
/* The probe's change lines: at most one per half second, at most this many per module load. */
#define VSHPAD_LOG_GAP_US      500000ull
#define VSHPAD_LOG_MAX         1000u
/* The owner's lines, each one said: at most this many per module load, apart from the rest. */
#define VSHPAD_SWITCH_LOG_MAX  500u

/* The OG layout (PROTOCOL.md section 10) out of libpad's two digital bytes. */
u32  vshpad_og_mask(u8 digital1, u8 digital2);

/* libpad's 0..255 stick byte as the games' -1..1 float: (v - 128) / 128. */
f32  vshpad_axis(u8 v);

/*
 * One poll, tick thread, every tick, outside the core lock. `wanted` 0 (no
 * client connected) reads nothing and blanks the view. `game_running` is the
 * same test vshpad_merge is given; `game_mask` and `game_analog` the game's
 * own pad, which says when the game takes input; `state` and `game_id`
 * SessionInfo's, for the log.
 */
void vshpad_poll(int wanted, int game_running, u32 game_mask, const f32 game_analog[4],
                 u8 state, u8 game_id, u64 now_us);

/*
 * The defer rule: what SessionInfo carries at +44 and +48. `game_running`
 * is 1 while a game is INGAME with its code in; the game's pad is ignored
 * otherwise, whatever g_hot still holds from the last game. The VSH's view
 * is the last poll's, owner and dead zone included.
 */
void vshpad_merge(int game_running, u32 game_mask, const f32 game_analog[4],
                  u32 *mask, f32 analog[4]);

#ifdef QWARK_TEST
/* Back to a module that has just loaded: no view, nothing learnt or logged, the counters at 0. */
void vshpad_reset(void);
#endif

#endif /* QWARK_VSHPAD_H */
