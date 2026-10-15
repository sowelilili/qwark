/*
 * Revision 1.21, build 53: manip categories.
 *
 * The PS3 ports keep a lot of RAM globals across files and planets that the PS2
 * originals reset, and speedrunners call the ones that matter manips. In a
 * challenge-mode (NG+) category a runner may set them before a run; in a
 * new-game (any%) category they must be as a freshly booted game has them. A
 * game describes its groups of writes in a struct game_manips (game.h); this
 * file applies them:
 *
 *   setup    once per game boot: at the first compatible file load after the
 *            boot or after the selection, or straight away when the group is
 *            selected while a compatible file is in play. The first time per
 *            boot the console says "<group> manips activated".
 *   per-run  on every file load (a save loaded, a new game started) while
 *            auto-reset is on, and by Apply now.
 *
 * Nothing is written unless the challenge-mode check passes: an NG+ group
 * needs the counter at 1 or more, a new-game group needs it at 0, and a group
 * flagged MANIP_FLAG_NO_CHALLENGE (RaC2 NG+ No Specials, which runs on an
 * overflowed counter) skips it. A refusal is an event the client toasts, and a
 * console notification too, since the PC may not be connected.
 *
 * The options are per game in config.txt, so they work without a PC:
 * auto_reset (default on), remember (default off) and category. With remember
 * off a new game process starts at None; with it on the category comes back.
 *
 * Everything here runs under the core lock: the tick thread's on_tick and the
 * ring handlers hold it, and MANIP_LIST takes it on its network thread. Only
 * the tick thread writes game memory.
 */
#ifndef QWARK_MANIPS_H
#define QWARK_MANIPS_H

#include "proto.h"
#include "../games/game.h"

/*
 * Every time the session reaches INGAME, with the running game (NULL for a
 * process no game claims): the game's groups, a new boot's setup state, and
 * the selection from config.txt when remember is on, None otherwise.
 */
void manips_enter(const struct game_api *game);

/*
 * The running game's trigger saw a save loaded or a new game started. Applies
 * whatever is due. Tick thread, under the core lock.
 */
void manips_file_loaded(void);

/* 1 while the running game has manip groups. */
int  manips_supported(void);

/* MANIP_LIST's reply. ST_UNSUPPORTED without groups, ST_FULL past `cap`. */
int  manips_list(u8 *out, u32 cap, u32 *len);

/* MANIP_SELECT, MANIP_OPTIONS and MANIP_APPLY. Tick thread. */
int  manips_select(u8 id);
int  manips_options(u8 auto_reset, u8 remember);
int  manips_apply(void);

/* For the tests: the selection and whether this boot's setup has been applied. */
u8   manips_selected(void);
int  manips_setup_done(void);
u16  manips_event_seq(void);
u8   manips_event_kind(void);
u8   manips_event_group(void);

#endif /* QWARK_MANIPS_H */
