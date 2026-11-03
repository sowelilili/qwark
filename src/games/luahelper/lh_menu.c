/*
 * Tools of Destruction's console menu (revision 1.29, pending): the Lua
 * helper's bridge between qwark's menu mailbox (src/core/menubox.h) and the
 * Lua glue in the pause menu's VM (lh_glue.lua). lh_menu.h has the block it
 * keeps, docs/tod-menu.md the design.
 *
 * What it does is what the four renderers' shared core does with the same
 * mailbox (src/games/menuhelper/mh_core.c), and no more: it copies a page qwark
 * finished writing into a block of its own (page_seq a seqlock), so the glue
 * never reads a page half written; it posts the glue's presses back as events,
 * one at a time, with the shown page's key and the row's id; it keeps hstate
 * and the frame count; and it hands the glue qwark's open and close requests.
 * The rows, their labels and values, what a row does: all qwark's, in C, as
 * for every game. The glue only shows them with the game's widgets.
 *
 * Plain C over 32-bit addresses with no global, no string and no call, so it
 * runs in the game (luahelper.c includes it) and on a PC: under LH_HOST
 * (test/test_tod_menu.c) every access goes through lh_host_ptr(), which the
 * test supplies, big-endian byte by byte, as test/test_menu.c runs mh_core.c.
 */
#include "lh_menu.h"
#include "lh_tod.h"
#include "../../core/menubox.h"

#ifdef LH_HOST

static u8 *lh_host_ptr(u32 addr);

static u32 lm8(u32 a)  { return *lh_host_ptr(a); }
static u32 lm16(u32 a) { return ((u32)*lh_host_ptr(a) << 8) | *lh_host_ptr(a + 1); }
static u32 lm32(u32 a)
{
	return ((u32)*lh_host_ptr(a) << 24) | ((u32)*lh_host_ptr(a + 1) << 16) |
	       ((u32)*lh_host_ptr(a + 2) << 8) | *lh_host_ptr(a + 3);
}
static void lmw8(u32 a, u32 v)  { *lh_host_ptr(a) = (u8)v; }
static void lmw32(u32 a, u32 v) { lmw8(a, v >> 24); lmw8(a + 1, v >> 16); lmw8(a + 2, v >> 8); lmw8(a + 3, v); }
#define lm_sync() __sync_synchronize()
typedef u32 lm_u32;

#else

typedef unsigned int lm_u32;
#define lm8(a)      ((lm_u32)*(volatile unsigned char *)(unsigned long)(lm_u32)(a))
#define lm16(a)     ((lm_u32)*(volatile unsigned short *)(unsigned long)(lm_u32)(a))
#define lm32(a)     (*(volatile lm_u32 *)(unsigned long)(lm_u32)(a))
#define lmw8(a, v)  (*(volatile unsigned char *)(unsigned long)(lm_u32)(a) = (unsigned char)(v))
#define lmw32(a, v) (*(volatile lm_u32 *)(unsigned long)(lm_u32)(a) = (lm_u32)(v))
#define lm_sync()   __asm__ __volatile__("sync" ::: "memory")

#endif

#define LMB(o)  ((lm_u32)LM_BOX + (lm_u32)(o))
#define LMS(o)  ((lm_u32)LM_STATE + (lm_u32)(o))
/* The taken page's field at the mailbox offset `o` (MBOX_OFF_PAGE_KEY and on). */
#define LMP(o)  (LMS(LM_PAGE) + (lm_u32)(o) - (lm_u32)MBOX_OFF_PAGE_KEY)

/* Whether qwark serves the mailbox: its identity is there (menu.c clears it when the menu is switched off). */
static int lm_on(void)
{
	return lm32(LMB(MBOX_OFF_MAGIC)) == MBOX_MAGIC && lm16(LMB(MBOX_OFF_VERSION)) == MBOX_VERSION;
}

static lm_u32 lm_nrows(void)
{
	lm_u32 n;

	if (lm8(LMS(LM_HAVE)) == 0) return 0;
	n = lm8(LMP(MBOX_OFF_NROWS));
	return n > LM_ROWS ? LM_ROWS : n;
}

/* Row i's bytes in the taken page (0-based), as the mailbox lays a row out. */
static lm_u32 lm_row(lm_u32 i)
{
	return LMP(MBOX_OFF_ROWS) + i * MBOX_ROW_SIZE;
}

/* A string field of `len` bytes in capitals, up to its NUL (a RANGE row keeps words past its label's). */
static void lm_caps(lm_u32 a, lm_u32 len)
{
	lm_u32 c;

	for (; len > 0 && (c = lm8(a)) != 0; a++, len--)
		if (c >= 'a' && c <= 'z') lmw8(a, c - 0x20);
}

/*
 * Revision 1.29, the fourth console test (docs/tod-menu.md 12): the taken
 * page's title and its rows' labels and values in capitals, as the pause
 * menu's own lists and screens write theirs (qwark's hint, a sentence, is
 * left as it is). Only the copy changes, which only the glue reads.
 */
static void lm_upper(lm_u32 n)
{
	lm_u32 i;

	lm_caps(LMP(MBOX_OFF_TITLE), MBOX_TITLE_LEN);
	for (i = 0; i < n; i++) {
		lm_caps(lm_row(i) + MROW_OFF_LABEL, MROW_LABEL_LEN);
		lm_caps(lm_row(i) + MROW_OFF_VALUE, MROW_VALUE_LEN);
	}
}

/*
 * Takes the page qwark last posted, if it has posted one not taken yet:
 * page_seq even and the same before and after the copy, or nothing is kept
 * and the next frame tries again. A new key (another page, window or visit)
 * moves LM_GEN, which the glue reads as "start the list again, the cursor
 * where qwark says"; any page taken moves LM_VGEN, "the values changed".
 */
static void lm_take(void)
{
	lm_u32 s1 = lm32(LMB(MBOX_OFF_PAGE_SEQ));
	lm_u32 n, len, k, key;

	if (s1 == 0 || (s1 & 1) != 0) return;
	if (lm8(LMS(LM_HAVE)) != 0 && s1 == lm32(LMS(LM_SEQ))) return;

	lm_sync();
	n = lm8(LMB(MBOX_OFF_NROWS));
	if (n > LM_ROWS) n = LM_ROWS;
	len = MBOX_OFF_ROWS - MBOX_OFF_PAGE_KEY + n * MBOX_ROW_SIZE;
	for (k = 0; k < len; k += 4) lmw32(LMS(LM_PAGE) + k, lm32(LMB(MBOX_OFF_PAGE_KEY) + k));
	lm_sync();
	if (lm32(LMB(MBOX_OFF_PAGE_SEQ)) != s1) return;

	lm_upper(n);
	key = lm32(LMP(MBOX_OFF_PAGE_KEY));
	if (lm8(LMS(LM_HAVE)) == 0 || key != lm32(LMS(LM_KEY)))
		lmw32(LMS(LM_GEN), lm32(LMS(LM_GEN)) + 1);
	lmw32(LMS(LM_VGEN), lm32(LMS(LM_VGEN)) + 1);
	lmw32(LMS(LM_SEQ), s1);
	lmw32(LMS(LM_KEY), key);
	lmw8(LMS(LM_HAVE), 1);
}

/*
 * A press on row `row` (0-based) of the page shown, or on none (a BACK): the
 * record, then ev_seq. One in flight at a time, as every renderer does: a
 * press while qwark has not answered the last is dropped (0), so a slow answer
 * never becomes a queue of stale presses.
 */
static int lm_post(lm_u32 kind, lm_u32 row)
{
	lm_u32 seq = lm32(LMB(MBOX_OFF_EV_SEQ));

	if (lm8(LMS(LM_HAVE)) == 0 || !lm_on()) return 0;
	if (seq != lm32(LMB(MBOX_OFF_EV_ACK))) return 0;

	lmw8(LMB(MBOX_OFF_EV_KIND), kind);
	lmw8(LMB(MBOX_OFF_EV_ROW), row < lm_nrows() ? row : 0);
	lmw8(LMB(MBOX_OFF_EV_ID), row < lm_nrows() ? lm8(lm_row(row) + MROW_OFF_ID) : 0);
	lmw8(LMB(MBOX_OFF_EV_ID) + 1, row < lm_nrows() ? lm8(lm_row(row) + MROW_OFF_ID + 1) : 0);
	lmw32(LMB(MBOX_OFF_EV_KEY), lm32(LMS(LM_KEY)));
	lm_sync();
	lmw32(LMB(MBOX_OFF_EV_SEQ), seq + 1);
	return 1;
}

/* Row r's flags (0-based) in the taken page, 0 past its end. */
static lm_u32 lm_flags(lm_u32 r)
{
	return r < lm_nrows() ? lm8(lm_row(r) + MROW_OFF_FLAGS) : 0;
}

/*
 * Revision 1.29, the fourth console test (docs/tod-menu.md 12): a press on
 * row `row` (0-based), posted only where the row takes it, as every
 * renderer's core decides: the cross on a row the cursor may stand on (an
 * INFO row takes none), Left and Right on a row that steps; anything else
 * (BACK) as it is. 1 posted, 0 not.
 */
static int lm_press(lm_u32 kind, lm_u32 row)
{
	lm_u32 f = lm_flags(row);

	if (kind == MBOX_EV_ACTIVATE && (f & MROWF_ENABLED) == 0) return 0;
	if ((kind == MBOX_EV_LEFT || kind == MBOX_EV_RIGHT) && (f & MROWF_STEPS) == 0) return 0;
	return lm_post(kind, row);
}

/*
 * The cursor came to row `row` (0-based) of a window the page goes on past:
 * at the window's last row the cursor can stand on, with more below, qwark is
 * asked for the window that goes on (SCROLL_DOWN); at its first, with more
 * above, for the one before (SCROLL_UP); as the renderers' core asks. 1
 * posted, 0 not.
 */
static int lm_edge(lm_u32 row)
{
	lm_u32 pf = lm8(LMP(MBOX_OFF_PFLAGS)), n = lm_nrows(), k, before = 0, after = 0;

	for (k = 0; k < n; k++) {
		if ((lm_flags(k) & MROWF_ENABLED) == 0) continue;
		if (k < row) before = 1;
		if (k > row) after = 1;
	}
	if ((pf & MBOX_PAGE_MORE_DOWN) != 0 && !after) return lm_post(MBOX_EV_SCROLL_DOWN, row);
	if ((pf & MBOX_PAGE_MORE_UP) != 0 && !before) return lm_post(MBOX_EV_SCROLL_UP, row);
	return 0;
}

/*
 * From row `row` (0-based) of the first `m` rows, `d` rows on (+1 down, -1
 * up), wrapping, to the next the cursor may stand on, as the game's lists
 * move; `row` itself when no other is (Game Options' changeSelection moves
 * by the difference). Revision 1.29, the fourth console test.
 *
 * Revision 1.29, the polish (docs/tod-menu.md 15): with the page a page at a
 * time (MBOX_CAP_PAGED in the header) and longer than the window, a move
 * that finds no row the cursor may stand on before the window's end asks
 * qwark for the next window that way (SCROLL_DOWN or SCROLL_UP: qwark puts
 * the cursor on its first or last row it may stand on, and wraps past the
 * page's ends), and the cursor stays where it is until that window comes.
 * Tools of Destruction's mailbox always says so (LM_CAPS); without the bit
 * the move wraps inside the window as before and asks nothing.
 */
static lm_u32 lm_next(lm_u32 row, int d, lm_u32 m)
{
	lm_u32 j = row, k;

	if (m > lm_nrows()) m = lm_nrows();
	if (m == 0 || row >= m) return row;
	if ((lm8(LMB(MBOX_OFF_CAPS)) & MBOX_CAP_PAGED) != 0 &&
	    (lm8(LMP(MBOX_OFF_PFLAGS)) & (MBOX_PAGE_MORE_UP | MBOX_PAGE_MORE_DOWN)) != 0) {
		while (d < 0 ? j > 0 : j + 1 < m) {
			j = d < 0 ? j - 1 : j + 1;
			if ((lm_flags(j) & MROWF_ENABLED) != 0) return j;
		}
		lm_post(d < 0 ? MBOX_EV_SCROLL_UP : MBOX_EV_SCROLL_DOWN, row);
		return row;
	}
	for (k = 0; k < m; k++) {
		j = d < 0 ? (j == 0 ? m - 1 : j - 1) : (j + 1 >= m ? 0 : j + 1);
		if (j == row || (lm_flags(j) & MROWF_ENABLED) != 0) return j;
	}
	return row;
}

/*
 * The menu's screen up (1) or down (0). Up: the frames count from 0 first,
 * which tells qwark it opened again even if it never saw it close, and a
 * request still waiting is taken with it.
 */
static void lm_hstate(lm_u32 up)
{
	if (!lm_on()) return;
	if (up) {
		lmw32(LMB(MBOX_OFF_FRAMES), 0);
		lm_sync();
		lmw8(LMB(MBOX_OFF_HSTATE), MBOX_HSTATE_OPEN);
		lmw8(LMB(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	} else {
		lmw8(LMB(MBOX_OFF_HSTATE), MBOX_HSTATE_CLOSED);
	}
}

/*
 * qwark's request, taken (the renderer clears the byte), or the open a pause
 * the combo brought up is waiting to make: 0 none, 1 open, 2 close.
 */
static lm_u32 lm_take_request(void)
{
	lm_u32 req;

	if (!lm_on()) return 0;
	req = lm8(LMB(MBOX_OFF_REQUEST));
	if (req != MBOX_REQ_NONE) lmw8(LMB(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	if (lm8(LMS(LM_PENDING)) != 0) {
		lmw8(LMS(LM_PENDING), 0);
		if (req == MBOX_REQ_NONE) req = MBOX_REQ_OPEN;
	}
	return req == MBOX_REQ_OPEN || req == MBOX_REQ_CLOSE ? req : 0;
}

/* Every frame of the pause menu, after the glue's: the frame count while the menu is up. */
static void lm_menu_frame(void)
{
	if (lm_on() && lm8(LMB(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN)
		lmw32(LMB(MBOX_OFF_FRAMES), lm32(LMB(MBOX_OFF_FRAMES)) + 1);
}

/*
 * Revision 1.29, the crash fix (lh_tod.h, LH_PAUSE_STATE and on; docs/
 * tod-menu.md 9): whether the pause menu's system `ms` is one the helper may
 * enter, with the presentation pushed around what it does there. 0 when it
 * is; else the LM_WHY_* bits of every reason it is not. The game's own words
 * only, read before anything is called: the pause is up and this is its menu
 * system, the system has its Lua state, runtime, presentation and Lua bridge,
 * the presentation's first update is behind it, the system is not stopping
 * and has updated LM_READY_FRAMES times, and nothing is pushed (the runtime's
 * count and the bridge's presentation both 0), as after every update of it.
 *
 * Revision 1.29, the galactic map (docs/tod-menu.md 17): with `map`, `ms`
 * is the galactic map's menu system (lh_menu_tick saw g_galMapSystem hold
 * it), and the first test is the map's instead: g_gameState 6, the map up.
 * The rest is the same, for the map's menu system is an AnarkMenuSystem too.
 */
static lm_u32 lm_ready(lm_u32 ms, int map)
{
	lm_u32 why = 0, st, rt, br;

	st = lm32(LH_PAUSE_STATE);
	if (map ? lm32(LH_GAME_STATE) != LH_STATE_MAP : ms == 0 || lm32(LH_PAUSE_MS) != ms || (st != 2 && st != 3))
		return LM_WHY_SYSTEM;
	rt = lm32(ms + LH_MS_RUNTIME);
	br = lm32(ms + LH_MS_BRIDGE);
	if (lm32(ms + LH_MS_STATE) == 0 || rt == 0 || br == 0 || lm32(ms + LH_MS_PRES) == 0) return LM_WHY_PARTS;
	if (lm8(ms + LH_MS_FIRST) != 0) why |= LM_WHY_FIRST;
	if (lm8(ms + LH_MS_STOPPED) != 0) why |= LM_WHY_STOPPED;
	if (lm32(ms + LH_MS_FRAMES) < LM_READY_FRAMES) why |= LM_WHY_YOUNG;
	if (lm32(rt + LH_RT_PUSHED) != 0 || lm32(br + LH_BR_PRES) != 0) why |= LM_WHY_PUSHED;
	return why;
}

/*
 * qw(LM_OP_PICK, level), from the map's glue (lh_gmap.lua): the level a pick
 * on qwark's galactic map loads, into g_galMap.selectedLevel, where the
 * game's own pick (GalMap_selectLevel) leaves it; the glue then closes the
 * map (exit_galactic_map), and GalMapState_exit loads it as for any pick.
 * Only a level (a negative one, made unsigned, is past them). Nothing but
 * the map's glue asks for it, in the map's VM, which is there only while the
 * map is up; written at any other time it would do nothing, for
 * GalMapState_enter sets the word to -1 at every map's start.
 */
static void lm_pick(lm_u32 level)
{
	if (level < LH_LEVELS) lmw32(LH_MAP_PICK, level);
}

/* Whether the push took: the bridge holds the menu system's presentation. */
static int lm_pushed(lm_u32 ms)
{
	return lm32(lm32(ms + LH_MS_BRIDGE) + LH_BR_PRES) == lm32(ms + LH_MS_PRES);
}

/*
 * Revision 1.29, the second crash (lh_tod.h LH_ANARK_POOL, docs/tod-menu.md
 * 10): the Anark VMs' Lua pool faults when a block is asked of it and every
 * class from the block's own up is used up, so the helper counts what is
 * left before it lets the menu's Lua run. Reads only, of the pool's own
 * words, every one checked before it is followed.
 *
 * Blocks chunk `c` has never handed out.
 */
static lm_u32 lm_fresh(lm_u32 c)
{
	lm_u32 bytes = lm32(c + LH_PC_BYTES), used = lm32(c + LH_PC_USED), stride = lm32(c + LH_PC_STRIDE);

	return stride != 0 && used < bytes ? (bytes - used) / stride : 0;
}

/* Blocks on chunk c's free list, counted to `cap`; a link that is not one of its blocks ends the count. */
static lm_u32 lm_freed(lm_u32 c, lm_u32 cap)
{
	lm_u32 base = lm32(c + LH_PC_BASE), bytes = lm32(c + LH_PC_BYTES), stride = lm32(c + LH_PC_STRIDE);
	lm_u32 p = lm32(c + LH_PC_FREE), n = 0;

	if (stride == 0) return 0;
	while (p != 0 && n < cap && p >= base && p - base < bytes && (p - base) % stride == 0) {
		n++;
		p = lm32(p);
	}
	return n;
}

/*
 * The pool's room (LM_ROOM's form, kept there too): the blocks its largest
 * class has left, counted to `big`, and the blocks an LM_ROOM_BLOCK-byte
 * block can still come from (its class and every larger one), counted to
 * `small`; the blocks never handed out first, the free lists only as far as
 * needed, from the largest class down. 0 for a pool not set up (between
 * pauses its table is 0) or one whose words make no sense.
 */
static lm_u32 lm_room(lm_u32 pool, lm_u32 small, lm_u32 big)
{
	lm_u32 desc = lm32(pool + LH_PA_DESC), ch = lm32(pool + LH_PA_CHUNKS), n = 0, first, k, top = 0, sum = 0;

	if (desc != 0) n = lm32(desc);
	first = lm32(pool + LH_PA_INDEX + (LM_ROOM_BLOCK - 1) / 4 * 4);
	if (ch != 0 && n != 0 && n <= LH_PA_CLASSES && first < n) {
		k = ch + (n - 1) * LH_PC_SIZE;
		top = lm_fresh(k);
		if (top < big) top += lm_freed(k, big - top);
		for (k = first; k < n; k++) sum += lm_fresh(ch + k * LH_PC_SIZE);
		for (k = n; k > first && sum < small; k--) sum += lm_freed(ch + (k - 1) * LH_PC_SIZE, small - sum);
	}
	if (top > 0xFFFF) top = 0xFFFF;
	if (sum > 0xFFFF) sum = 0xFFFF;
	lmw32(LMS(LM_ROOM), (top << 16) | sum);
	return (top << 16) | sum;
}

static int lm_room_ok(lm_u32 room, lm_u32 small, lm_u32 big)
{
	return (room & 0xFFFF) >= small && (room >> 16) >= big;
}

/*
 * Whether a full collection may be made, the pool at `room`: not while one
 * that left it short holds the next off (LM_HOLD, counted down here), nor
 * under the floor, where the collection itself might find no block.
 */
static int lm_may_collect(lm_u32 room)
{
	lm_u32 h = lm32(LMS(LM_HOLD));

	if (h != 0) {
		lmw32(LMS(LM_HOLD), h - 1);
		return 0;
	}
	return lm_room_ok(room, LM_ROOM_FLOOR_SMALL, LM_ROOM_FLOOR_BIG);
}

/* A collection left the pool short: counted for qwark's log, and the next one held off. */
static void lm_short(void)
{
	lmw32(LMS(LM_SHORT), lm32(LMS(LM_SHORT)) + 1);
	lmw32(LMS(LM_HOLD), LM_ROOM_HOLD);
}

/* A pause menu frame kept out of the menu's VM for `why`, or (0) let in: LM_WHY and LM_WAITS. */
static void lm_kept_out(lm_u32 why)
{
	lmw32(LMS(LM_WHY), why);
	lmw32(LMS(LM_WAITS), why != 0 ? lm32(LMS(LM_WAITS)) + 1 : 0);
}

/*
 * Every gameplay frame. No pause menu is up, so the menu is not either,
 * whatever closed it (Start in the Cheats screen, a load from the pause): the
 * mailbox says closed, and the next pause is a new one for the glue. A pause
 * the combo asked for that has not come up in LM_PENDING_FRAMES frames is
 * forgotten, so it never opens the menu at some later pause.
 */
static void lm_gameplay_frame(void)
{
	lm_u32 p = lm8(LMS(LM_PENDING));

	lmw32(LMS(LM_L), 0);
	lmw8(LMS(LM_TRIED), 0);
	if (lm32(LMS(LM_WAITS)) != 0) lmw32(LMS(LM_WAITS), 0);
	if (lm32(LMS(LM_HOLD)) != 0) lmw32(LMS(LM_HOLD), 0);
	if (p != 0) lmw8(LMS(LM_PENDING), p - 1);
	if (lm_on() && lm8(LMB(MBOX_OFF_HSTATE)) != MBOX_HSTATE_CLOSED) lm_hstate(0);
}

/*
 * Game_UpdateFrame's Start test, the one that opens the pause menu (the
 * pause hook: its own answer, `pressed`, comes in). qwark's open request in
 * play is a press of Start: the game opens its pause exactly as for the
 * button, past every check of its own that comes before (no load waiting, no
 * cinematic, the first seconds of a level over), and the glue opens the menu
 * in it once it is up. The request is taken here, so qwark sees it go.
 */
int lm_pause_check(int pressed)
{
	if (!lm_on() || lm8(LMB(MBOX_OFF_REQUEST)) != MBOX_REQ_OPEN) return pressed;
	lmw8(LMB(MBOX_OFF_REQUEST), MBOX_REQ_NONE);
	lmw8(LMS(LM_PENDING), LM_PENDING_FRAMES);
	return 1;
}
