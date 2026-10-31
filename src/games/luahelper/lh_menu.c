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
