/*
 * Revision 1.30: the run-start warnings.
 *
 * With the in-game trainer it is easy to leave a cheat on, so as a run starts
 * the console says what is on that the run should not have:
 *
 *   Warning: N cheats enabled                    the running game's TOGGLEs its
 *                                                table counts as cheats
 *                                                (game_describe.cheats) that
 *                                                are on
 *   Warning: N mods enabled                      mods in the game whose
 *                                                patch.txt says `#- legal: false`
 *   Warning: N mods of unknown legality enabled  mods in the game with no
 *                                                `#- legal:` line, or one that
 *                                                is neither true nor false
 *
 * as one notification, a line each, the lines with nothing to say left out
 * and nothing at all when all three are 0. A mod with `#- legal: true` is
 * never counted.
 *
 * The run start is the autosplitter's: autosplit_emit calls runwarn_run_start
 * for every START, which each game's watcher emits once per run (RaC1 to
 * Deadlocked with the RESET of the same tick, Tools of Destruction as the
 * run's Metropolis fades in) and never for a death, a checkpoint or another
 * load. So all five games warn at the same moment, the moment a timer starts.
 *
 * The unknown-legality line is said once: a mod is named in it, counted, at
 * the first run start it is in the game for, and after that only counted
 * again on a run start that has another unknown mod not yet warned about (for
 * the rest of the module's life, which is the console's power-on: the XMB,
 * every game boot and every game in it). The cheats and the illegal mods are
 * said at every run start they are there for.
 *
 * config.txt's `run.warnings` (RUN_WARN, 1 by default) switches all three
 * off; while it is off nothing is marked as warned about.
 *
 * Tick thread, under the core lock (autosplit_emit runs inside on_tick).
 */
#ifndef QWARK_RUNWARN_H
#define QWARK_RUNWARN_H

#include "proto.h"

void runwarn_run_start(void);

/*
 * The notification's text for these counts into `out`, the lines joined by
 * '\n'; "" when all three are 0. Returns the number of lines.
 */
u32  runwarn_compose(char *out, u32 cap, u32 cheats, u32 illegal, u32 unknown);

#ifdef QWARK_TEST
/* Forgets which unknown-legality mods were warned about, as a new power-on would. */
void runwarn_forget(void);
/* The counts the last run start found (whatever was shown), and how many run starts there were. */
u32  runwarn_starts(void);
u32  runwarn_last_cheats(void);
u32  runwarn_last_illegal(void);
u32  runwarn_last_unknown(void);
#endif

#endif /* QWARK_RUNWARN_H */
