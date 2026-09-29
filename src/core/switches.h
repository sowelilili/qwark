/*
 * The code switches, build 50: qwark's instruction-patch toggles on a platform
 * that cannot patch code.
 *
 * On a console a WRITES_CODE toggle writes the patched word over the game's own
 * instruction and puts the original back to turn it off. RPCS3 cannot take
 * that: it recompiles PPU code ahead of running it, so a rewritten instruction
 * changes memory and nothing else, and once it has loaded the executable it
 * takes write permission away from the code segment altogether. What RPCS3 does
 * apply is its own patch files, as it loads the executable, before it compiles
 * a single block.
 *
 * So the game is given both forms of every patched instruction at load. An
 * RPCS3 patch the client writes from SWITCH_PATCH replaces each site with a
 * branch to a trampoline in a code cave, and the trampoline reads one flag byte
 * - in data memory, which RPCS3 leaves writable - and runs the patched form or
 * the original before branching back. src/games/switches/ holds them, one
 * assembly source per game, built into the committed src/games/switches_bins.c.
 *
 * This file is the core's half, and it mirrors what savefile.c does for the
 * savefile helper since build 47:
 *
 *   - SWITCH_PATCH hands out the running game's caves and site words, and the
 *     flag bytes at 0, on either platform.
 *   - Where code cannot be patched the tick thread looks, once a session, for
 *     every byte of every cave and every site word in the game, a few reads a
 *     tick, and keeps the answer until the session ends.
 *   - While the answer is yes, SessionInfo flags bit4 is set and every
 *     WRITES_CODE toggle of the game is a write of its flag byte instead of a
 *     patch (classic.h, classic_code_toggle), read back out of memory like a
 *     LIVE toggle's byte.
 *
 * On a console none of it happens: the look never runs, switches_present() is
 * always 0, and every toggle patches code exactly as it always has.
 */
#ifndef QWARK_SWITCHES_H
#define QWARK_SWITCHES_H

#include "proto.h"

/*
 * Forget the answer. The session calls this where it calls savefile_forget, on
 * the way into INGAME and on the way out of it: a patch goes in at load or not
 * at all, so every new process is looked at afresh.
 */
void switches_forget(void);

/*
 * The look, a few reads a tick, on the tick thread while INGAME. It does
 * nothing on a platform that patches code and nothing once the answer is known.
 * Returns 1 on the one tick the answer arrives, which is the session's cue to
 * apply the WRITES_CODE toggles that were waiting for it (features.h).
 */
int  switches_tick(void);

/*
 * -1 while qwark has not looked yet this session, 0 when the running game's
 * switches are not all in it (or it has none), 1 when they are. Always 0 on a
 * platform that patches code.
 */
int  switches_state(void);

/* switches_state() == 1: flip WRITES_CODE toggles by their flag bytes. */
int  switches_present(void);

/*
 * SWITCH_PATCH, revision 1.17, network thread under the core lock: the running
 * game's switches as what an RPCS3 patch writes, in SAVEFILE_PATCH's layout.
 * It reads the generated table and nothing else, so it answers on either
 * platform. NOT_INGAME outside a game, UNSUPPORTED for a game without switches,
 * FULL for a reply that would not fit `cap`.
 */
int  switches_patch(u8 *out, u32 cap, u32 *len);

#endif /* QWARK_SWITCHES_H */
