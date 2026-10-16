/*
 * The descriptor registry: DESCRIBE, FEATURE_SET / TRIGGER / SET_AUTO / OPTIONS,
 * the live toggle state and the per-toggle auto flags.
 *
 * Everything that writes game memory runs on the tick thread; DESCRIBE and
 * OPTIONS are read-only and answer on the network thread under the core lock.
 */
#ifndef QWARK_FEATURES_H
#define QWARK_FEATURES_H

#include "proto.h"
#include "../games/game.h"

/* Called when the session picks up (or drops) a game. NULL clears everything. */
void features_set_game(const struct game_api *game, const char *title);

/* Drops the toggle state without writing anything to the game. */
void features_forget_state(void);

u64  features_toggle_state(void);
u64  features_toggle_auto(void);

int  features_set(u8 id, u32 value);
int  features_trigger(u8 id);
int  features_set_auto(u8 id, int on);
int  features_options(u8 id, const char * const **options, u8 *count);

/* Encodes the DESCRIBE reply. */
int  features_describe(u8 *out, u32 cap, u32 *len);

/*
 * Turns on every toggle in `mask` that the current game has.
 *
 * Build 50. On a platform that cannot patch code a WRITES_CODE toggle works
 * only through the game's code switches (switches.h), and until the tick has
 * looked for them nobody knows whether it will: such a toggle waits, rather
 * than being skipped, and features_apply_waiting puts it on once the answer is
 * in. That is what lets the auto-apply at INGAME and PREVIOUS_REAPPLY, both of
 * which can come before the first look, turn the switches on.
 */
void features_apply_mask(u64 mask);

/*
 * Build 50. The WRITES_CODE toggles features_apply_mask set aside before the
 * code switches were looked for: on now, through their flag bytes, if the look
 * found the switches, and skipped as they always were if it did not. The
 * session calls this on the tick the answer arrives. Tick thread only.
 *
 * Build 51: the emulator builds only (QWARK_EMULATOR, plat.h). A console has no
 * look to wait for, so nothing ever waits there.
 */
#ifdef QWARK_EMULATOR
void features_apply_waiting(void);
#else
static __inline void features_apply_waiting(void) { }
#endif

/* The mask of toggles flagged auto in config for the current game. */
u64  features_auto_mask(void);

/*
 * Protocol 1.3. The mask of TOGGLEs the current game marks FEATURE_FLAG_LIVE:
 * their state lives in game memory, so they are never re-applied and never go
 * into the previous-session record.
 */
u64  features_live_mask(void);

/*
 * Re-reads every LIVE toggle through the game's toggle_read and makes
 * toggle_state agree with what memory says. Reads game memory, so the tick
 * thread is the only caller; it never writes and never fires a hook.
 *
 * Build 50: while the running game's code switches are in (switches.h), its
 * WRITES_CODE toggles are read back the same way, out of their flag bytes. They
 * stay what they are in every other respect - not LIVE, so they keep their auto
 * flag and go into the previous-session record - but the checkbox follows the
 * byte that decides what the game runs.
 */
void features_poll_live(void);

#endif /* QWARK_FEATURES_H */
