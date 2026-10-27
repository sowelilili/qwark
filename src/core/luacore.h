/*
 * Revision 1.27: the core's half of a game's own Lua (PROTOCOL.md 5.15,
 * docs/tod-poc.md section 2): LUA_EXEC, LUA_STATUS and LUA_PATCH over the
 * mailbox of src/core/luamb.h, for whichever running game hands the core a Lua
 * helper (struct game_api's lua_desc; Tools of Destruction today).
 *
 * The state machine is the core's, not the game's: it outlives a game module's
 * load and unload, and what an earlier session left outstanding in the game is
 * withdrawn as the game arrives, whatever the module did.
 *
 * Only where QWARK_HAS_TOD (src/core/buildmodes.h): a release module has no
 * Lua game and none of this; its net.c answers the three ops UNSUPPORTED.
 *
 * Everything here but lua_core_patch runs on the tick thread under the core
 * lock, the only place that touches game memory.
 */
#ifndef QWARK_LUACORE_H
#define QWARK_LUACORE_H

#include "../plat/plat.h"
#include "proto.h"

/* The game arrived in a session (session.c's game_arrive): everything starts over. */
void lua_core_enter(void);

/* Every INGAME tick: the presence look, the attach, and an outstanding request's answer. */
void lua_core_tick(void);

/*
 * LUA_EXEC's first half: the request checked, the helper installed on a
 * console that has not got it yet, the chunk sent. ST_OK with the request's
 * sequence number in *seq; otherwise the frame status (PROTOCOL.md 5.15).
 */
int  lua_core_exec_start(const u8 *req, u32 reqlen, u32 *seq);

/*
 * LUA_EXEC's second half, asked until it is done: ST_OK with *done 1 and the
 * reply in `reply` once request `seq` is answered (or timed out and
 * withdrawn); ST_OK with *done 0 while it is still out; ST_BUSY when it was
 * lost (the game went in between).
 */
int  lua_core_exec_result(u32 seq, u8 *reply, u32 cap, u32 *len, int *done);

/* LUA_STATUS's reply. */
int  lua_core_status(u8 *reply, u32 cap, u32 *len);

#ifdef QWARK_EMULATOR
/* LUA_PATCH's reply. Under the core lock, on a network thread: it reads no memory. */
int  lua_core_patch(u8 *reply, u32 cap, u32 *len);
#endif

#endif /* QWARK_LUACORE_H */
