/*
 * The Lua helper: runs a chunk of Lua source qwark left in the game's memory,
 * in the game's own gameplay VM, on the game's own thread.
 *
 * lh_entry.S replaces the game's per-frame call to its gameplay VM's update
 * with a call to itself: it runs the update, exactly as the game did, and
 * then calls lh_tick below, which looks at the mailbox (src/core/luamb.h has
 * the layout and the protocol). That is the safe point REPORT-tod-lua 2.3
 * describes: after the update no level-script coroutine is running, the
 * gameplay allocator is current, the level is up, and only gameplay frames
 * get here, never a paused one, a menu or a load.
 *
 * The second entry, lh_err_tap, replaces the game's empty
 * CLuaVM_reportError, so every error a level script raises (which the game
 * otherwise drops) lands in the mailbox's err area with the script's name.
 * A chunk qwark sends does not go through reportError: its error comes back in
 * `out`, from lua_pcall's own message.
 *
 * Revision 1.29 (pending), the console menu (docs/tod-menu.md): two entries
 * more. The pause menu's frame calls lh_menu_tick after the Anark menu's own
 * update, on the main thread, where the pause menu's VM is as safe to enter
 * as the gameplay VM is after its update. It gives each pause menu's
 * lua_State the glue's C function and asks qwark for the menu's Lua glue
 * (lh_glue.lua, which qwark keeps and sends as a request for the menu's VM,
 * target MENU, too big for the RPCS3 patch the cave is), serves the
 * mailbox's requests for the menu's VM, and calls the glue's qwark_frame
 * every frame. The glue reaches qwark's menu through one C
 * function, qw() (lh_qw below, over lh_menu.c). And Game_UpdateFrame's test of
 * the pause button goes through lm_pause_check, which presses it for qwark's
 * open request.
 *
 * The rules are the savefile helper's (src/games/sfhelper/sfhelper.c): no
 * global or static, no string constant, no C pointer holding a game address;
 * every game function an undefined symbol bound to its address. The one thing
 * that is new is r2: the Lua functions want LH_LUA_TOC, which lh_entry.S sets
 * before it calls lh_tick and takes back afterwards. Nothing in this file
 * reads r2, and every function it calls is in the Lua range, whose own calls
 * leave r2 as they found it (each call there is followed by a `nop`, not a
 * reload), so it stays LH_LUA_TOC throughout. qw() is entered from the game's
 * Lua through its descriptor (lh_entry.S), which gives it the same r2.
 */
#include "luahelper.h"

#define MB LH_MAILBOX

#include "lh_menu.c"

static inline void lh_sync(void)
{
	__asm__ __volatile__("sync" ::: "memory");
}

/*
 * A NUL-ended string from `src` into `dst`, which holds `cap` bytes; the copy
 * is cut at cap - 1 and always ended. Returns the bytes copied, and sets
 * *cut when the source went on.
 */
static lh_u32 lh_copy_str(lh_u32 dst, lh_u32 src, lh_u32 cap, int *cut)
{
	lh_u32 n = 0;

	*cut = 0;
	if (src != 0) {
		while (LH_BYTE(src + n) != 0) {
			if (n == cap - 1) {
				*cut = 1;
				break;
			}
			LH_BYTE(dst + n) = LH_BYTE(src + n);
			n++;
		}
	}
	LH_BYTE(dst + n) = 0;
	return n;
}

/* Strings and data lh_entry.S keeps beside the code, by their labels. */
#define LH_TEXT_ADDR(label, out) \
	__asm__("lis %0," #label "@ha\n\taddi %0,%0," #label "@l" : "=r"(out))

/* The chunk's name, "=qwark". */
static inline lh_u32 lh_chunk_name(void)
{
	lh_u32 a;

	LH_TEXT_ADDR(lh_chunkname, a);
	return a;
}

/*
 * Revision 1.29, the second crash (docs/tod-menu.md 10, lh_menu.c lm_room):
 * whether the Anark Lua pool has room for `small` and `big` blocks, with a
 * full collection of the pause menu's VM first if it has not and one may be
 * made (the game's own, which AnarkMenuSystem_update makes once a second):
 * 2 room; 1 short after a collection made now; 0 short and none made (held
 * off after one that did not help, or the pool under the floor). The Anark
 * allocator is current.
 */
static int lh_room(lh_u32 L, lh_u32 pool, lh_u32 small, lh_u32 big)
{
	lh_u32 r = lm_room(pool, small, big);

	if (lm_room_ok(r, small, big)) return 2;
	if (!lm_may_collect(r)) return 0;
	lh_setgcthreshold(L, 0);
	if (lm_room_ok(lm_room(pool, small, big), small, big)) return 2;
	lm_short();
	return 1;
}

/*
 * The request in the mailbox, if there is one for `target` (luamb.h), run on
 * `L` with `alloc` current; the answer written back. The gameplay VM's frame
 * and the pause menu's both come here, each for its own target. Answers
 * whether there was one. Revision 1.29: one for the pause menu's VM (the glue
 * above all) runs only with LM_ROOM_LOAD_* left in its pool, NO_ROOM
 * otherwise, and the parser's garbage is collected after it if it left the
 * pool short of that.
 */
static int lh_serve(lh_u32 L, lh_u32 alloc, lh_u32 target)
{
	lh_u32 r, len, s;
	lh_u32 out_len = 0, out_type = LUAMB_T_NONE, out_flags = 0;
	int st, cut;

	r = LH_WORD(MB + LUAMB_OFF_REQ);
	if (r == LH_WORD(MB + LUAMB_OFF_ACK)) return 0;
	if (LH_BYTE(MB + LUAMB_OFF_TARGET) != target) return 0;

	/*
	 * Claim it, then look for a withdrawal: qwark writes cancel and then
	 * reads busy, so one side or the other always sees the other's word.
	 */
	LH_WORD(MB + LUAMB_OFF_BUSY) = r;
	lh_sync();

	LH_BYTE(MB + LUAMB_OFF_OUT) = 0;
	len = LH_WORD(MB + LUAMB_OFF_CHUNK_LEN);

	if (LH_WORD(MB + LUAMB_OFF_CANCEL) == r) {
		st = LUAMB_ST_CANCELLED;
	} else if (L == 0) {
		st = LUAMB_ST_NO_STATE;
	} else if (len == 0 || len > LUAMB_CHUNK_CAP) {
		st = LUAMB_ST_BAD_LEN;
	} else if (target == LUAMB_TARGET_MENU && lh_room(L, alloc, LM_ROOM_LOAD_SMALL, LM_ROOM_LOAD_BIG) != 2) {
		st = LUAMB_ST_NO_ROOM;
	} else {
		lh_u32 prev = LH_WORD(LH_CUR_ALLOC);
		int top, t;

		lh_alloc_set_current(alloc);
		top = lh_gettop(L);

		st = lh_loadbuffer(L, MB + LUAMB_OFF_CHUNK, len, lh_chunk_name());
		if (st == 0) st = lh_pcall(L, 0, 1, 0);

		/* The first result, or the error message either call left. */
		if (lh_gettop(L) > top) {
			t = lh_type(L, -1);
			if (t >= 0) out_type = (lh_u32)t;
			if (t == LUAMB_T_BOOLEAN && lh_toboolean(L, -1)) out_flags |= LUAMB_OUT_TRUE;
			if (t == LUAMB_T_STRING || t == LUAMB_T_NUMBER) {
				s = lh_tostring(L, -1);
				out_len = lh_copy_str(MB + LUAMB_OFF_OUT, s, LUAMB_OUT_CAP, &cut);
				if (cut) out_flags |= LUAMB_OUT_TRUNCATED;
			}
		}
		lh_settop(L, top);
		if (target == LUAMB_TARGET_MENU) lh_room(L, alloc, LM_ROOM_LOAD_SMALL, LM_ROOM_LOAD_BIG);
		lh_alloc_set_current(prev);
	}

	LH_WORD(MB + LUAMB_OFF_OUT_LEN) = out_len;
	LH_BYTE(MB + LUAMB_OFF_OUT_TYPE) = (lh_u8)out_type;
	LH_BYTE(MB + LUAMB_OFF_OUT_FLAGS) = (lh_u8)out_flags;
	LH_WORD(MB + LUAMB_OFF_STATUS) = (lh_u32)st;
	lh_sync();
	LH_WORD(MB + LUAMB_OFF_ACK) = r;
	lh_sync();
	LH_WORD(MB + LUAMB_OFF_BUSY) = 0;
	return 1;
}

void lh_tick(lh_u32 vm)
{
	lh_u32 L;

	if (LH_WORD(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;

	LH_WORD(MB + LUAMB_OFF_HEARTBEAT) = LH_WORD(MB + LUAMB_OFF_HEARTBEAT) + 1;
	L = LH_WORD(vm + LH_VM_STATE);
	LH_WORD(MB + LUAMB_OFF_LAST_STATE) = L;

	/* Revision 1.29: a gameplay frame, so no pause menu, and no console menu in one. */
	lm_gameplay_frame();

	lh_serve(L, vm + LH_VM_ALLOC, LUAMB_TARGET_GAMEPLAY);
}

/*
 * An error text into the mailbox's err area, as the error tap writes the
 * game's own: `msg` the text, `name` the script's, `vm` who raised it.
 */
static void lh_report(lh_u32 vm, lh_u32 msg, lh_u32 name)
{
	lh_u32 n;
	int cut;

	n = lh_copy_str(MB + LUAMB_OFF_ERR, msg, LUAMB_ERR_CAP, &cut);
	lh_copy_str(MB + LUAMB_OFF_ERR_SCRIPT, name, LUAMB_ERR_SCRIPT_CAP, &cut);
	LH_WORD(MB + LUAMB_OFF_ERR_VM) = vm;
	LH_WORD(MB + LUAMB_OFF_ERR_LEN) = n;
	lh_sync();
	LH_WORD(MB + LUAMB_OFF_ERR_SEQ) = LH_WORD(MB + LUAMB_OFF_ERR_SEQ) + 1;
}

/*
 * reportError(vm, text): a leaf, reached by a plain branch, returning to the
 * game's caller. r2 is whichever TOC the caller had; nothing here uses it.
 */
void lh_err_tap(lh_u32 vm, lh_u32 msg)
{
	if (LH_WORD(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;
	lh_report(vm, msg, LH_SCRIPT_NAME);
}

/* ------------------------------------------------------------ the menu */

/* A string field of `cap` bytes, NUL-padded and not always ended, onto the Lua stack. */
static void lh_push_field(lh_u32 L, lh_u32 a, lh_u32 cap)
{
	lh_u32 n = 0;

	while (n < cap && LH_BYTE(a + n) != 0) n++;
	lh_pushlstring(L, a, n);
}

static void lh_push_int(lh_u32 L, int v)
{
	lh_pushnumber(L, (float)v);
}

static int lh_int_arg(lh_u32 L, int i)
{
	return (int)lh_tonumber(L, i);
}

/*
 * qw(op, a, b): the glue's one way to qwark's menu, lh_menu.h's LM_OP_*.
 * Numbers are small integers both ways (row indices, kinds, flags, counters
 * modulo 2^24 at worst), which a float holds exactly.
 */
int lh_qw(lh_u32 L)
{
	int op = lh_int_arg(L, 1), a = lh_int_arg(L, 2), b = lh_int_arg(L, 3);
	lh_u32 r;

	/* An if chain, not a switch: a switch is a jump table in .toc, which the helper has none of. */
	if (op == LM_OP_POLL) {
		lm_take();
		lh_push_int(L, lm_on());
		lh_push_int(L, (int)LH_BYTE(LMS(LM_HELP)));
		lh_push_int(L, (int)(LH_WORD(LMS(LM_GEN)) & 0xFFFFFF));
		lh_push_int(L, (int)(LH_WORD(LMS(LM_VGEN)) & 0xFFFFFF));
		lh_push_int(L, (int)lm_nrows());
		lh_push_int(L, (int)LH_BYTE(LMP(MBOX_OFF_CURSOR)));
		lh_push_int(L, (int)LH_BYTE(LMP(MBOX_OFF_PTYPE)));
		lh_push_int(L, (int)LH_BYTE(LMP(MBOX_OFF_PFLAGS)));
		return 8;
	}
	if (op == LM_OP_ROW) {
		if (a < 1 || (lh_u32)a > lm_nrows()) {
			lh_pushnil(L);
			return 1;
		}
		r = lm_row((lh_u32)a - 1);
		lh_push_field(L, r + MROW_OFF_LABEL, MROW_LABEL_LEN);
		lh_push_field(L, r + MROW_OFF_VALUE, MROW_VALUE_LEN);
		lh_push_int(L, (int)LH_BYTE(r + MROW_OFF_KIND));
		lh_push_int(L, (int)LH_BYTE(r + MROW_OFF_FLAGS));
		return 4;
	}
	if (op == LM_OP_TEXT) {
		lh_push_field(L, LMP(MBOX_OFF_TITLE), LH_BYTE(LMS(LM_HAVE)) ? MBOX_TITLE_LEN : 0);
		lh_push_field(L, LMP(MBOX_OFF_HINT), LH_BYTE(LMS(LM_HAVE)) ? MBOX_HINT_LEN : 0);
		return 2;
	}
	if (op == LM_OP_EVENT) {
		lh_push_int(L, lm_post((lh_u32)a, b >= 1 ? (lh_u32)b - 1 : 0xFFu));
		return 1;
	}
	if (op == LM_OP_STATE) {
		lm_hstate(a != 0);
		return 0;
	}
	if (op == LM_OP_TAKE) {
		lh_push_int(L, (int)lm_take_request());
		return 1;
	}
	if (op == LM_OP_NOTE) {
		LH_WORD(LMS(LM_NOTE)) = (lh_u32)a;
		return 0;
	}
	return 0;
}

/* The glue's global `qwark_frame` on top of the stack; answers whether it is a function. */
static int lh_get_frame_fn(lh_u32 L)
{
	lh_u32 a;

	LH_TEXT_ADDR(lh_framename, a);
	lh_pushstring(L, a);
	lh_gettable(L, LH_GLOBALSINDEX);
	return lh_type(L, -1) == LH_T_FUNCTION;
}

/*
 * The glue stops in this VM: `msg` (a text, or 0) into the err area as
 * "qwark-menu", qwark_frame gone until the next pause, the menu down, and
 * LM_GLUE says so (qwark logs it once). The stack back at `top` after the
 * message is copied, for it may be the message's own slot.
 */
static void lh_menu_stop(lh_u32 L, lh_u32 ms, int top, lh_u32 msg)
{
	lh_u32 a;

	LH_TEXT_ADDR(lh_menuname, a);
	lh_report(ms, msg, a + 1);
	lh_settop(L, top);
	LH_TEXT_ADDR(lh_framename, a);
	lh_pushstring(L, a);
	lh_pushnil(L);
	lh_settable(L, LH_GLOBALSINDEX);
	LH_WORD(LMS(LM_GLUE)) = LM_GLUE_STOPPED;
	lm_hstate(0);
}

/*
 * Revision 1.29, the second crash: the pool short even after a collection,
 * with the glue in. The glue's qwark_quit puts back everything it wrapped and
 * the Help row, so what it holds in the VM is garbage the game's next
 * collection gives back to the game, which needs the room more; then the glue
 * is stopped as on an error. qwark_quit allocates next to nothing (its name
 * is already a string of the VM's, and it makes no closure).
 */
static void lh_menu_quit(lh_u32 L, lh_u32 ms, int top)
{
	lh_u32 a;

	LH_TEXT_ADDR(lh_quitname, a);
	lh_pushstring(L, a);
	lh_gettable(L, LH_GLOBALSINDEX);
	if (lh_type(L, -1) == LH_T_FUNCTION) lh_pcall(L, 0, 0, 0);
	lh_settop(L, top);
	LH_TEXT_ADDR(lh_shortmsg, a);
	lh_menu_stop(L, ms, top, a);
}

/*
 * A new pause menu's VM: the global qw, and a word that asks qwark for the
 * glue (lh_glue.lua), which qwark sends as a request for the menu's VM
 * (target MENU) and lh_serve runs like any other; it wraps what it needs of
 * the pause menu and defines qwark_frame. Once a lua_State.
 */
static void lh_menu_load(lh_u32 L)
{
	lh_u32 a;

	LH_BYTE(LMS(LM_TRIED)) = 1;
	LH_WORD(LMS(LM_NOTE)) = 0;
	lm_hstate(0);

	LH_TEXT_ADDR(lh_qwname, a);
	lh_pushstring(L, a);
	LH_TEXT_ADDR(lh_qw_desc, a);
	lh_pushcclosure(L, a, 0);
	lh_settable(L, LH_GLOBALSINDEX);

	LH_WORD(LMS(LM_LOADS)) = LH_WORD(LMS(LM_LOADS)) + 1;
	lh_sync();
	LH_WORD(LMS(LM_GLUE)) = LM_GLUE_WANTED;
}

/*
 * The pause menu's frame, after AnarkMenuSystem_update (lh_entry.S). Every
 * Anark object the menu's Lua touches is found through the presentation the
 * runtime has pushed, and here, after the update, none is: so nothing goes
 * into the VM unless the game's own words say the presentation is up
 * (lm_ready), and then only with it pushed, as AnarkMenuSystem_reset calls
 * into it from outside an update, and popped again before the game goes on.
 * Every Lua call in between is a pcall or cannot fail; the push is checked to
 * have taken (the bridge holds the presentation) before any of it.
 *
 * Revision 1.29, the second crash (docs/tod-menu.md 10): the pause menu's
 * Lua pool faults when it runs out, so before anything (the push included,
 * whose listener sets a Lua global) the helper counts its room, and with too
 * little makes the full collection the game's own update makes once a
 * second, here where the game makes it (after the update, nothing pushed).
 * Still short: nothing goes into the VM this frame, and a glue already in
 * takes itself out (lh_menu_quit). A frame that ran a request does not also
 * run qwark_frame: the glue's load and its first look at the pause are never
 * one frame's garbage.
 */
void lh_menu_tick(lh_u32 ms)
{
	lh_u32 L, prev, alloc, rt, why;
	int top, room;

	if (LH_WORD(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;
	LH_WORD(MB + LUAMB_OFF_MENU_BEAT) = LH_WORD(MB + LUAMB_OFF_MENU_BEAT) + 1;
	why = lm_ready(ms);
	if (why != 0) {
		lm_kept_out(why);
		return;
	}
	L = LH_WORD(ms + LH_MS_STATE);
	rt = LH_WORD(ms + LH_MS_RUNTIME);

	prev = LH_WORD(LH_CUR_ALLOC);
	alloc = lh_anark_alloc();
	lh_alloc_set_current(alloc);
	top = lh_gettop(L);

	room = lh_room(L, alloc, LM_ROOM_SMALL, LM_ROOM_BIG);
	if (room != 2) why = LM_WHY_MEMORY;
	if (room == 2 || (room == 1 && LH_WORD(LMS(LM_GLUE)) == LM_GLUE_LOADED)) {
		lh_pres_push(rt, LH_WORD(ms + LH_MS_PRES));

		if (!lm_pushed(ms)) {
			why |= LM_WHY_PUSH;
		} else if (room == 1) {
			lh_menu_quit(L, ms, top);
		} else {
			/* Another pause, or another menu system: the glue has to go in again. */
			if (LH_WORD(LMS(LM_L)) != L || LH_WORD(LMS(LM_MS)) != ms) {
				LH_WORD(LMS(LM_L)) = L;
				LH_WORD(LMS(LM_MS)) = ms;
				LH_BYTE(LMS(LM_TRIED)) = 0;
			}
			if (LH_BYTE(LMS(LM_TRIED)) == 0 && !lh_get_frame_fn(L)) lh_menu_load(L);
			lh_settop(L, top);

			/* qwark's requests for this VM, the glue first of all; or else the glue's frame. */
			if (!lh_serve(L, alloc, LUAMB_TARGET_MENU) && lh_get_frame_fn(L)) {
				if (LH_WORD(LMS(LM_GLUE)) == LM_GLUE_WANTED) LH_WORD(LMS(LM_GLUE)) = LM_GLUE_LOADED;
				/* The glue broke (it put Help back first): said once, and qwark_frame gone until the next pause. */
				if (lh_pcall(L, 0, 0, 0) != 0)
					lh_menu_stop(L, ms, top, lh_type(L, -1) == LUAMB_T_STRING ? lh_tostring(L, -1) : 0);
			}
		}
		lh_pres_pop(rt);
	}

	lh_settop(L, top);
	lh_alloc_set_current(prev);
	lm_kept_out(why);
	if (why == 0) lm_menu_frame();
}
