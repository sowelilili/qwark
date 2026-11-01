/*
 * The Lua mailbox: how qwark hands a chunk of Lua source to a game's own Lua VM
 * and gets the result back (docs/tod-poc.md, REPORT-tod-lua section 4, helper A).
 *
 * The mailbox is a block of the game's writable memory. qwark writes a chunk
 * into it and raises a sequence number; a helper qwark put into the game (on
 * RPCS3, an RPCS3 patch put it there) sees the number on its next gameplay
 * frame, runs the chunk under lua_pcall on the game's own thread, writes the
 * status and a text result back and answers with the same number. Nothing else
 * travels: once the helper is in, everything is a data write, which PINE can
 * do as well as PS3MAPI.
 *
 * This header is shared by two compilers. The helper (src/games/luahelper/) is
 * built by ppu-lv2-gcc with no qwark header and no libc, so everything here is
 * a plain #define: offsets into the block, sizes, the magic and the status
 * codes. The host-side codec and the request state machine are in luamb.c.
 *
 * Every field is big-endian, as the game stores it. Each word has exactly one
 * writer, which is what makes the protocol safe without a lock:
 *
 *   qwark  magic, version, size (once, at install), req, cancel, chunk_len, chunk,
 *          target
 *   helper ack, busy, status, out_len, out_type, out_flags, heartbeat, last_state,
 *          menu_beat, out, and the error tap's err_seq, err_vm, err_len,
 *          err_script, err
 *
 * The protocol (docs/tod-poc.md section 1 has the reasoning):
 *
 *   qwark sends    only while req == ack: chunk, chunk_len, target, then
 *                  req = req + 1
 *   helper, every gameplay frame, after the VM's own update (and, version 2,
 *   every frame of the pause menu, after the menu's own update, for target
 *   MENU; menu_beat for heartbeat there):
 *                  heartbeat += 1
 *                  r = req; if r == ack, or target is the other VM's, nothing to do
 *                  busy = r; sync
 *                  if cancel == r: status = CANCELLED
 *                  else if the state is 0: status = NO_STATE
 *                  else if chunk_len is 0 or too long: status = BAD_LEN
 *                  else (MENU, revision 1.29) if its Lua pool is short: NO_ROOM
 *                  else run it: luaL_loadbuffer + lua_pcall(L, 0, 1, 0)
 *                  out, out_len, out_type, out_flags, status; sync
 *                  ack = r; busy = 0
 *   qwark waits    for ack == req. On a timeout it writes cancel = req, and then
 *                  reads busy: busy != req means the helper is not inside that
 *                  request and never will be (it checks cancel after raising
 *                  busy), so the chunk buffer is free again; busy == req means
 *                  it is running now, and qwark waits for its ack.
 */
#ifndef QWARK_LUAMB_H
#define QWARK_LUAMB_H

/* 'QLUA'. Written by the installer, not by the helper, which runs only on it. */
#define LUAMB_MAGIC   0x514C5541
/*
 * Version 2 (revision 1.29, pending): the request's target, gameplay VM or the
 * pause menu's (`target`), and the pause menu's heartbeat. A version 1 helper
 * would run a request meant for the menu in the gameplay VM, so the two never
 * meet: the identity words differ, and qwark looks for its own.
 */
#define LUAMB_VERSION 2

/* ------------------------------------------------------------------ layout */

#define LUAMB_OFF_MAGIC      0x000  /* u32 LUAMB_MAGIC                          qwark  */
#define LUAMB_OFF_VERSION    0x004  /* u16 LUAMB_VERSION                        qwark  */
#define LUAMB_OFF_SIZE       0x006  /* u16 LUAMB_SIZE, the block's size         qwark  */
#define LUAMB_OFF_REQ        0x008  /* u32 sequence number of the request       qwark  */
#define LUAMB_OFF_ACK        0x00C  /* u32 sequence number last answered        helper */
#define LUAMB_OFF_CANCEL     0x010  /* u32 a request qwark withdrew             qwark  */
#define LUAMB_OFF_BUSY       0x014  /* u32 the request being run, else 0        helper */
#define LUAMB_OFF_STATUS     0x018  /* s32 LUAMB_ST_*                           helper */
#define LUAMB_OFF_CHUNK_LEN  0x01C  /* u32 bytes of source in chunk             qwark  */
#define LUAMB_OFF_OUT_LEN    0x020  /* u32 bytes in out, before its NUL         helper */
#define LUAMB_OFF_OUT_TYPE   0x024  /* u8  lua_type of the result, LUAMB_T_*    helper */
#define LUAMB_OFF_OUT_FLAGS  0x025  /* u8  LUAMB_OUT_*                          helper */
#define LUAMB_OFF_TARGET     0x026  /* u8  LUAMB_TARGET_*, the VM the request is for  qwark */
                                    /* u8  0                                           */
#define LUAMB_OFF_HEARTBEAT  0x028  /* u32 +1 every gameplay frame              helper */
#define LUAMB_OFF_LAST_STATE 0x02C  /* u32 the lua_State* of the last frame     helper */
#define LUAMB_OFF_ERR_SEQ    0x030  /* u32 +1 every error the game reports      helper */
#define LUAMB_OFF_ERR_VM     0x034  /* u32 the CLuaVM* that reported it         helper */
#define LUAMB_OFF_ERR_LEN    0x038  /* u32 bytes in err, before its NUL         helper */
#define LUAMB_OFF_MENU_BEAT  0x03C  /* u32 +1 every frame of the pause menu     helper */
#define LUAMB_OFF_ERR_SCRIPT 0x040  /* char[64] the script running, NUL-ended   helper */
#define LUAMB_OFF_ERR        0x100  /* char[256] the error text, NUL-ended      helper */
#define LUAMB_OFF_OUT        0x200  /* char[1024] the result or the error       helper */
#define LUAMB_OFF_CHUNK      0x600  /* char[8192] the source                    qwark  */

#define LUAMB_HDR_SIZE    0x040     /* the words qwark polls, one read           */
#define LUAMB_ERR_SCRIPT_CAP 0x40
#define LUAMB_ERR_CAP     0x100
#define LUAMB_OUT_CAP     0x400     /* out_len is at most LUAMB_OUT_CAP - 1      */
#define LUAMB_CHUNK_CAP   0x2000
#define LUAMB_SIZE        0x2600    /* LUAMB_OFF_CHUNK + LUAMB_CHUNK_CAP         */

/* ------------------------------------------------------------------ status */

/*
 * What `status` holds once `ack` has moved. 0..5 are Lua 5.0's own lua_pcall /
 * lua_load codes, passed through; 0x100.. are the helper's.
 */
#define LUAMB_ST_OK         0      /* ran; out holds the first result as text  */
#define LUAMB_ST_ERRRUN     1      /* LUA_ERRRUN: out holds the message        */
#define LUAMB_ST_ERRFILE    2      /* LUA_ERRFILE (never from a buffer)        */
#define LUAMB_ST_ERRSYNTAX  3      /* LUA_ERRSYNTAX: out holds the message     */
#define LUAMB_ST_ERRMEM     4      /* LUA_ERRMEM                               */
#define LUAMB_ST_ERRERR     5      /* LUA_ERRERR                               */
#define LUAMB_ST_NO_STATE   0x100  /* the game had no lua_State: between levels */
#define LUAMB_ST_BAD_LEN    0x101  /* chunk_len 0 or over LUAMB_CHUNK_CAP      */
#define LUAMB_ST_CANCELLED  0x102  /* qwark withdrew it before it ran          */
/*
 * Revision 1.29 (pending), the second crash (docs/tod-menu.md 10): a request
 * for the pause menu's VM not run, its Lua pool too short for it even after
 * a full collection. Only target MENU answers it.
 */
#define LUAMB_ST_NO_ROOM    0x103

/*
 * Never in the mailbox: what qwark's side reports when the helper did not
 * answer (luamb.c, and LUA_EXEC's reply, PROTOCOL.md 5.15).
 */
#define LUAMB_ST_TIMEOUT    0x200  /* frames ran, but no answer in time        */
#define LUAMB_ST_NOT_RUNNING 0x201 /* no gameplay frame ran: paused, a menu, a load */
#define LUAMB_ST_HUNG       0x202  /* the helper is inside the chunk, past the hang limit */

/*
 * target (version 2). The gameplay VM's tick serves only GAMEPLAY requests and
 * the pause menu's tick only MENU ones, each on its own lua_State; a request
 * for the other one waits, and times out as any request no frame answers.
 */
#define LUAMB_TARGET_GAMEPLAY 0
#define LUAMB_TARGET_MENU     1

/* out_type: Lua 5.0's lua_type, plus NONE for an empty stack. */
#define LUAMB_T_NONE     0xFF
#define LUAMB_T_NIL      0
#define LUAMB_T_BOOLEAN  1
#define LUAMB_T_LIGHTUD  2
#define LUAMB_T_NUMBER   3
#define LUAMB_T_STRING   4
#define LUAMB_T_TABLE    5
#define LUAMB_T_FUNCTION 6
#define LUAMB_T_USERDATA 7
#define LUAMB_T_THREAD   8

/* out_flags */
#define LUAMB_OUT_TRUNCATED 0x01   /* the text was longer than out             */
#define LUAMB_OUT_TRUE      0x02   /* a boolean result was true                */

#endif /* QWARK_LUAMB_H */
