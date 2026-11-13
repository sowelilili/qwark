/*
 * Revision 1.31: the VSH pad probe. The input display's pad at the XMB.
 *
 * qwark runs inside the VSH, so it can read the controller the way webMAN
 * MOD's XMB combos do, through the VSH's own libpad (plat_vsh_pad_read),
 * with no game running. SessionInfo's pad_mask and analog[] then come from
 * whichever of two views of pad 1 has something to show:
 *
 *   the VSH's view, when it has a button held (the buttons) or a stick out
 *     of its dead zone (the sticks);
 *   otherwise the running game's own pad, read out of game memory by the
 *     game's hot_decode as always, while a game is INGAME with its code in;
 *   otherwise nothing: 0 and four 0.0 sticks.
 *
 * The XMB overlay blocks a game's input and the VSH sees nothing while a
 * game has the pad, so between them the two views cover the XMB, a game, and
 * a game with the PS button's overlay open. The buttons and the sticks are
 * decided apart. The combos and the console menu keep the game's own pad: only
 * what goes out to a client changes.
 *
 * This first step is a probe for the hardware: it logs to qwark.log what the
 * VSH sees and what the game sees whenever either changes (rate-limited and
 * capped), and libpad's answers at the first call and at every error, so a
 * test on the console says whether reading the pad from the VSH works there,
 * what it sees in a game, and whether it gets in anything's way
 * (rewrite/docs/REPORT-vsh-pad-probe.md).
 *
 * Read on the tick thread once per telemetry frame, outside the core lock,
 * and only while a client is connected; merged under the core lock as
 * SessionInfo is encoded. An error, or no pad API at all (the host and
 * RPCS3), leaves the game's pad alone, as before this revision.
 */
#ifndef QWARK_VSHPAD_H
#define QWARK_VSHPAD_H

#include "../plat/plat.h"

/*
 * A stick counts as moved past this far from 0x80 (of 128): a resting stick
 * wanders a few counts, and a stick held still at the XMB must read as blank
 * so the game's sticks are not hidden behind it.
 */
#define VSHPAD_DEADZONE        32
/*
 * The VSH's view goes blank when libpad has handed over nothing new for this
 * long (its len 0, "same as before"). A press or release webMAN's own poll
 * took first never reaches qwark, and without this a button would stay lit
 * until the next change.
 */
#define VSHPAD_STALE_US        2000000ull
/* GetInfo2, which picks the port, once a second. */
#define VSHPAD_INFO_EVERY_US   1000000ull
/* The probe's change lines: at most one per half second, at most this many per module load. */
#define VSHPAD_LOG_GAP_US      500000ull
#define VSHPAD_LOG_MAX         1000u

/* The OG layout (PROTOCOL.md section 10) out of libpad's two digital bytes. */
u32  vshpad_og_mask(u8 digital1, u8 digital2);

/* libpad's 0..255 stick byte as the games' -1..1 float: (v - 128) / 128. */
f32  vshpad_axis(u8 v);

/*
 * One poll, tick thread, outside the core lock. `wanted` 0 (no client
 * connected) reads nothing and blanks the view. `game_running` is the same
 * test vshpad_merge is given; `game_mask` the game's own pad_mask, for the
 * log; `state` and `game_id` SessionInfo's, for the log.
 */
void vshpad_poll(int wanted, int game_running, u32 game_mask, u8 state, u8 game_id, u64 now_us);

/*
 * The defer rule: what SessionInfo carries at +44 and +48. `game_running`
 * is 1 while a game is INGAME with its code in; the game's pad is ignored
 * otherwise, whatever g_hot still holds from the last game. The VSH's view
 * is the last poll's, dead zone and staleness included.
 */
void vshpad_merge(int game_running, u32 game_mask, const f32 game_analog[4],
                  u32 *mask, f32 analog[4]);

#ifdef QWARK_TEST
/* Back to a module that has just loaded: no view, nothing logged, the counters at 0. */
void vshpad_reset(void);
#endif

#endif /* QWARK_VSHPAD_H */
