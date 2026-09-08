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

/* Turns on every toggle in `mask` that the current game has. */
void features_apply_mask(u64 mask);

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
 */
void features_poll_live(void);

#endif /* QWARK_FEATURES_H */
