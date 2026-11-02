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

/*
 * Revision 1.29 (pending), Tools of Destruction's console menu (menu.c's
 * native renderer, docs/tod-menu.md), whose pause-menu half is the Lua
 * helper's.
 *
 * lua_core_helper_ready: the helper in the game, for the menu. On a console
 * it is installed now if it is not yet (the menu's install, a few seconds
 * into play, is the user's choice of menu.enabled as for every game's
 * renderer); under RPCS3 it is the patch's: ST_BUSY until qwark has looked,
 * ST_UNSUPPORTED without it. ST_OK when the helper is in.
 *
 * lua_core_menu_glue: the menu's Lua glue sent to the pause menu's VM (a
 * request of target MENU), which the helper asked for (lh_menu.h, LM_GLUE
 * WANTED). ST_OK when it went out, ST_BUSY while another request is out
 * (try again next tick). Its answer is the core's: logged when it failed,
 * never a LUA_EXEC's.
 *
 * lua_core_menu_diag (the third console test, docs/tod-menu.md 11): the
 * helper's look at the pause (lh_diag.lua) sent the same way, which menu.c
 * asks for once when the glue's own look failed; its answer, a line of
 * text, goes into qwark.log ("menu: what the glue saw ...").
 */
#if QWARK_HAS_TOD
int  lua_core_helper_ready(void);
int  lua_core_menu_glue(void);
int  lua_core_menu_diag(void);

/*
 * Revision 1.29 (pending), the game's own planet picker (menu.c, struct
 * menu_picker; Tools of Destruction's galactic map): a chunk of the core's
 * own for the gameplay VM, the way the glue goes to the pause menu's.
 *
 * lua_core_gameplay_ready: ST_OK when one could go now, the helper in and a
 * level's lua_State up (ST_NOT_FOUND without one). Nothing is written, and
 * on a console nothing is installed (the menu's install did that).
 *
 * lua_core_gameplay_send: the chunk sent (ST_OK, its sequence number in
 * *seq), ST_BUSY while another request is out, else why not.
 *
 * lua_core_gameplay_result: 0 while request `seq` is out, 1 once it ran
 * (status OK), 2 once it failed or was lost. A failure is logged.
 */
int  lua_core_gameplay_ready(void);
int  lua_core_gameplay_send(const char *chunk, u32 len, u32 *seq);
int  lua_core_gameplay_result(u32 seq);
#endif

#ifdef QWARK_EMULATOR
/* LUA_PATCH's reply. Under the core lock, on a network thread: it reads no memory. */
int  lua_core_patch(u8 *reply, u32 cap, u32 *len);
#endif

#endif /* QWARK_LUACORE_H */
