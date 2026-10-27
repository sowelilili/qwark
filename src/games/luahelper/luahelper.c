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
 * The rules are the savefile helper's (src/games/sfhelper/sfhelper.c): no
 * global or static, no string constant, no C pointer holding a game address;
 * every game function an undefined symbol bound to its address. The one thing
 * that is new is r2: the Lua functions want LH_LUA_TOC, which lh_entry.S sets
 * before it calls lh_tick and takes back afterwards. Nothing in this file
 * reads r2, and every function it calls is in the Lua range, whose own calls
 * leave r2 as they found it (each call there is followed by a `nop`, not a
 * reload), so it stays LH_LUA_TOC throughout.
 */
#include "luahelper.h"

#define MB LH_MAILBOX

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

/* The chunk's name, "=qwark", which lh_entry.S keeps beside the code. */
static inline lh_u32 lh_chunk_name(void)
{
	lh_u32 a;

	__asm__("lis %0,lh_chunkname@ha\n\taddi %0,%0,lh_chunkname@l" : "=r"(a));
	return a;
}

void lh_tick(lh_u32 vm)
{
	lh_u32 r, L, len, s;
	lh_u32 out_len = 0, out_type = LUAMB_T_NONE, out_flags = 0;
	int st, cut;

	if (LH_WORD(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;

	LH_WORD(MB + LUAMB_OFF_HEARTBEAT) = LH_WORD(MB + LUAMB_OFF_HEARTBEAT) + 1;
	L = LH_WORD(vm + LH_VM_STATE);
	LH_WORD(MB + LUAMB_OFF_LAST_STATE) = L;

	r = LH_WORD(MB + LUAMB_OFF_REQ);
	if (r == LH_WORD(MB + LUAMB_OFF_ACK)) return;

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
	} else {
		lh_u32 prev = LH_WORD(LH_CUR_ALLOC);
		int top, t;

		lh_alloc_set_current(vm + LH_VM_ALLOC);
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
}

/*
 * reportError(vm, text): a leaf, reached by a plain branch, returning to the
 * game's caller. r2 is whichever TOC the caller had; nothing here uses it.
 */
void lh_err_tap(lh_u32 vm, lh_u32 msg)
{
	lh_u32 n;
	int cut;

	if (LH_WORD(MB + LUAMB_OFF_MAGIC) != LUAMB_MAGIC) return;

	n = lh_copy_str(MB + LUAMB_OFF_ERR, msg, LUAMB_ERR_CAP, &cut);
	lh_copy_str(MB + LUAMB_OFF_ERR_SCRIPT, LH_SCRIPT_NAME, LUAMB_ERR_SCRIPT_CAP, &cut);
	LH_WORD(MB + LUAMB_OFF_ERR_VM) = vm;
	LH_WORD(MB + LUAMB_OFF_ERR_LEN) = n;
	lh_sync();
	LH_WORD(MB + LUAMB_OFF_ERR_SEQ) = LH_WORD(MB + LUAMB_OFF_ERR_SEQ) + 1;
}
