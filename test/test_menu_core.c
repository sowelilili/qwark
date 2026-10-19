/*
 * The console menu's renderer core (src/games/menuhelper/mh_core.c), once,
 * against a fake shim: what every game's renderer shares - the mailbox's
 * seqlock, composing and casing the text, the presses into events with one in
 * flight, the waiting title, the description line, switching between the two
 * widgets and the fallback for a shim with one, the requests and the counters.
 * Each game's shim is tested against its own game's mocks (test_menu.c for
 * Deadlocked's).
 *
 * The fake game header is variables where a real one has numbers, so one
 * build of the core runs every shape a shim can declare.
 */
#include "../src/core/proto.h"
#include "../src/core/util.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define MH_HOST 1

static u8 *mh_host_ptr(u32 addr)
{
	return host_mem_ptr(addr);
}

/* ------------------------------------------------------- the fake game header */

static u32 g_cfg_rows = 6;
static u32 g_case_main = 1, g_case_head = 1, g_case_label = 2, g_case_value = 1;
static u32 g_style = 0x0F;
static u32 g_restart_main, g_restart_cfg;
static u32 g_hold_main, g_hold_cfg;

#define MH_GAME_ID      0x7F
#define MH_BOX          0x01F00000u
#define MH_STATE        0x01F01000u
#define MH_ROWS         6
#define MH_ROW_BYTES    0x80
#define MH_MAIN_ROWS    6
#define MH_CFG_ROWS     g_cfg_rows
#define MH_CAPS         0x0F
#define MH_CASE_MAIN    g_case_main
#define MH_CASE_HEAD    g_case_head
#define MH_CASE_LABEL   g_case_label
#define MH_CASE_VALUE   g_case_value
#define MH_STYLE        g_style
#define MH_MAIN_SEP     ": "
#define MH_WAIT_FRAMES  30
#define MH_MAGIC        0x514D4846u
#define MH_RESTART_MAIN g_restart_main
#define MH_RESTART_CFG  g_restart_cfg
#define MH_HOLD_MAIN    g_hold_main
#define MH_HOLD_CFG     g_hold_cfg

#include "../src/games/menuhelper/mh_core.c"

/* -------------------------------------------------------------- the fake shim */

static struct {
	int shows, last_how, switches, closes, last_close, titles, descs, sound, sounds;
	u32 cursor;
	int hold;                 /* >= 0: shim_switch calls mh_core_hold(hold) */
} F;

MH_API void shim_show(int p, u32 how)     { (void)p; F.shows++; F.last_how = (int)how; }
MH_API void shim_switch(int p)            { (void)p; F.switches++; if (F.hold >= 0) mh_core_hold((u32)F.hold); }
MH_API void shim_close(int p, u32 how)    { (void)p; F.closes++; F.last_close = (int)how; }
MH_API void shim_title(int p)             { (void)p; F.titles++; }
MH_API void shim_desc(int p)              { (void)p; F.descs++; }
MH_API u32  shim_cursor(int p)            { (void)p; return F.cursor; }
MH_API void shim_sound(u32 snd)           { F.sound = (int)snd; F.sounds++; }

/* ----------------------------------------------------------------- helpers */

static void zero(u32 a, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++) w8(a + i, 0);
}

static void put_str(u32 a, const char *s, u32 cap)
{
	u32 i;
	for (i = 0; i < cap; i++) w8(a + i, i < strlen(s) ? (u8)s[i] : 0);
}

static void get_str(u32 a, char *out, u32 cap)
{
	u32 i;
	for (i = 0; i + 1 < cap; i++) {
		out[i] = (char)r8(a + i);
		if (out[i] == 0) return;
	}
	out[i] = 0;
}

static int str_is(u32 a, const char *want)
{
	char buf[96];
	get_str(a, buf, sizeof(buf));
	return strcmp(buf, want) == 0;
}

struct prow {
	u8 kind, flags;
	u16 id;
	const char *label, *value, *desc;
};

/* Row i's four range words, for a MROWF_RANGE row the next post_page writes (round 3). */
static const u32 *g_range[MH_ROWS];

/* What qwark does to post a page: page_seq odd, the page, page_seq even. */
static void post_page(u32 key, u8 ptype, u8 pflags, u8 cursor, const char *title, const char *hint,
                      const struct prow *rows, u8 n)
{
	u32 seq = r32(BOX(MBOX_OFF_PAGE_SEQ));
	u8 i;

	w32(BOX(MBOX_OFF_PAGE_SEQ), seq + 1);
	w32(BOX(MBOX_OFF_PAGE_KEY), key);
	w8(BOX(MBOX_OFF_NROWS), n);
	w8(BOX(MBOX_OFF_CURSOR), cursor);
	w8(BOX(MBOX_OFF_PFLAGS), pflags);
	w8(BOX(MBOX_OFF_PTYPE), ptype);
	put_str(BOX(MBOX_OFF_TITLE), title, MBOX_TITLE_LEN);
	put_str(BOX(MBOX_OFF_HINT), hint, MBOX_HINT_LEN);
	for (i = 0; i < n; i++) {
		u32 r = BOX(MBOX_OFF_ROWS) + i * MH_ROW_BYTES;
		w8(r + MROW_OFF_KIND, rows[i].kind);
		w8(r + MROW_OFF_FLAGS, rows[i].flags);
		w16(r + MROW_OFF_ID, rows[i].id);
		put_str(r + MROW_OFF_LABEL, rows[i].label, MROW_LABEL_LEN);
		put_str(r + MROW_OFF_VALUE, rows[i].value, MROW_VALUE_LEN);
		put_str(r + MROW_OFF_DESC, rows[i].desc != NULL ? rows[i].desc : "", MROW_DESC_LEN);
		if (g_range[i] != NULL) {
			u32 k;
			for (k = 0; k < 4; k++) w32(r + MROW_OFF_RANGE + 4 * k, g_range[i][k]);
		}
	}
	w32(BOX(MBOX_OFF_PAGE_SEQ), seq + 2);
}

static void ack(void)
{
	w32(BOX(MBOX_OFF_EV_ACK), r32(BOX(MBOX_OFF_EV_SEQ)));
}

static void fresh(void)
{
	zero(MH_BOX, 0x80 + MH_ROWS * MH_ROW_BYTES);
	zero(MH_STATE, MH_CORE_SIZE);
	w32(MH_STATE + ST_MAGIC, MH_MAGIC);
	memset(&F, 0, sizeof(F));
	F.hold = -1;
	g_cfg_rows = 6;
	g_case_main = 1; g_case_head = 1; g_case_label = 2; g_case_value = 1;
	g_style = 0x0F;
	g_restart_main = 0;
	g_restart_cfg = 0;
	g_hold_main = 0;
	g_hold_cfg = 0;
	memset(g_range, 0, sizeof(g_range));
}

/*
 * Round 3: the two text sets, the hold, the reset on close, the range words.
 * The pointers a widget was given are game addresses; the old screen is the
 * text at the addresses it had before the switch.
 */
static void test_menu_core_sets(void)
{
	static const struct prow main_a[3] = {
		{ MROW_LINK, MROWF_ENABLED, 0x0700, "game", "", NULL },
		{ MROW_LINK, MROWF_ENABLED, 0x0701, "cheats", "", NULL },
		{ MROW_INFO, 0, 0x0500, "qwark build 54", "", NULL }
	};
	static const struct prow main_b[2] = {
		{ MROW_ACTION, MROWF_ENABLED, 0x0901, "veldin", "", NULL },
		{ MROW_ACTION, MROWF_ENABLED, 0x0902, "florana", "", NULL }
	};
	static const struct prow cfg_a[3] = {
		{ MROW_TOGGLE, MROWF_ENABLED | MROWF_STEPS, 0x0205, "ghost ratchet", "Off", "Ghost." },
		{ MROW_VALUE, MROWF_ENABLED | MROWF_STEPS | MROWF_RANGE | MROWF_SLIDER, 0x0207, "challenge mode", "3 [10]",
		  "Steps." },
		{ MROW_VALUE, MROWF_ENABLED | MROWF_STEPS, 0x0206, "bolts", "12", "Bolts." }
	};
	static const struct prow cfg_b[3] = {
		{ MROW_TOGGLE, MROWF_ENABLED | MROWF_STEPS | MROWF_ON, 0x0205, "ghost ratchet", "On", "Ghost." },
		{ MROW_VALUE, MROWF_ENABLED | MROWF_STEPS | MROWF_RANGE | MROWF_SLIDER, 0x0207, "challenge mode", "4 [10]",
		  "Steps." },
		{ MROW_VALUE, MROWF_ENABLED | MROWF_STEPS, 0x0206, "bolts", "12", "Bolts." }
	};
	static const u32 challenge[4] = { 3, 0, 255, 10 };
	u32 old_text, old_title, old_label, set0, i, front0;
	char buf[96];

	group("console menu: the renderer core's two text sets, the hold and the close (round 3)");

	/* A widget's pointers stay put for a page taken in place, and show its text at once. */
	fresh();
	post_page(0x00000101, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_a, 3);
	mh_core_start(0);
	set0 = mh_set();
	old_text = mh_text(1);
	old_title = mh_title();
	check(str_is(old_text, "CHEATS: >") && r8(MH_STATE + ST_FRONT) == 0, "the root on show, in text set 0");
	post_page(0x00000101, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_b, 2);
	check(mh_core_frame(0) == 0 && F.shows == 1 && mh_text(1) == old_text && mh_set() == set0 &&
	      str_is(old_text, "FLORANA") && mh_nrows() == 2,
	      "a page taken in place: the same addresses, the new text in them at once");

	/* A switch: the old screen keeps its text until the start, and after it. */
	post_page(0x07000102, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	check(mh_core_frame(0) == MH_GONE && F.switches == 1 && r8(MH_STATE + ST_SWITCHING) == 1,
	      "a CONFIG page: the shim is asked to start the screen again");
	check(str_is(old_text, "FLORANA") && str_is(old_title, "TRAINER") && mh_nrows() == 2 && mh_text(1) == old_text,
	      "and until it does, the screen on show keeps its own text: no frame of the next page on it");
	F.cursor = 1;
	mh_core_press(0, MH_BTN_CROSS, 1);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 0, "a press on the screen going does nothing");
	check(mh_core_frame(0) == 0 && F.shows == 1 && str_is(old_text, "FLORANA"),
	      "a frame more of it changes nothing");
	mh_core_stop(0);
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN, "its stop is not a close");
	check(mh_core_start(0) == MBOX_PTYPE_CONFIG && r8(MH_STATE + ST_FRONT) == 1 && mh_set() != set0 &&
	      str_is(mh_label(1), "Challenge Mode") && str_is(old_text, "FLORANA") && str_is(old_title, "TRAINER"),
	      "the start: text set 1 is on show with the new page, set 0 still the old screen's to its end");

	/* The range words of a VALUE row, where a native slider finds them. */
	check((mh_row_flags(1) & (MROWF_RANGE | MROWF_SLIDER)) == (MROWF_RANGE | MROWF_SLIDER) &&
	      (mh_row_flags(2) & MROWF_RANGE) == 0, "a slider row is flagged, Bolts not");
	g_range[1] = challenge;
	post_page(0x07000102, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	check(r32(mh_row_range(1)) == 3 && r32(mh_row_range(1) + 4) == 0 && r32(mh_row_range(1) + 8) == 255 &&
	      r32(mh_row_range(1) + 12) == 10 && mh_row_range(1) == mh_label(1) + MH_RANGE_CFG &&
	      str_is(mh_label(1), "Challenge Mode") && str_is(mh_value(1), "3 [10]"),
	      "its words: value 3, 0..255, step 10, past the label in the row's text, label and value as before");
	g_cfg_rows = 0;
	post_page(0x07000103, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	mh_core_stop(0);
	mh_core_start(0);
	check(r32(mh_row_range(1) + 8) == 255 && mh_row_range(1) == mh_text(1) + MH_RANGE_MAIN &&
	      str_is(mh_text(1), "CHALLENGE MODE: 3 [10]"),
	      "on a MAIN page the same words, further on, past the one string");
	g_range[1] = NULL;
	g_cfg_rows = 6;

	/* A page taken in place while the old screen fades: straight into the set on show. */
	fresh();
	g_hold_main = 3;
	post_page(0x00000201, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_a, 3);
	mh_core_start(0);
	old_text = mh_text(0);
	post_page(0x07000202, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	mh_core_stop(0);
	mh_core_start(0);
	check(r8(MH_STATE + ST_HOLD) == 3 && str_is(old_text, "GAME: >"),
	      "MAIN to CONFIG with MH_HOLD_MAIN 3: the list's set held three frames");
	post_page(0x07000202, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "Ghost Ratchet: done", cfg_b, 3);
	check(mh_core_frame(0) == 0 && F.shows == 1 && str_is(mh_value(0), "ON") && str_is(old_text, "GAME: >"),
	      "a value changing while it fades shows at once, the fading list's text untouched");
	post_page(0x00000203, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "Trainer", "", main_a, 3);
	check(mh_core_frame(0) == 0 && F.switches == 1 && str_is(old_text, "GAME: >") && r8(MH_STATE + ST_HOLD) == 1,
	      "a quick page back to the list while it still fades waits in the mailbox");
	check(mh_core_frame(0) == MH_GONE && F.switches == 2 && str_is(old_text, "GAME: >"),
	      "and is taken the frame the hold runs out, into the list's old set, for the screen started again");
	mh_core_stop(0);
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && mh_text(0) == old_text && mh_core_cursor(0) == 1,
	      "which is the set on show again, with the page's own cursor");

	/* A shim that knows its screen going is gone overrides the hold for that switch. */
	fresh();
	g_hold_main = 3;
	F.hold = 0;
	post_page(0x00000211, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_a, 3);
	mh_core_start(0);
	post_page(0x07000212, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	mh_core_stop(0);
	mh_core_start(0);
	check(r8(MH_STATE + ST_HOLD) == 0, "mh_core_hold(0) in shim_switch: no hold for this switch, MH_HOLD_MAIN or not");
	post_page(0x00000213, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "Trainer", "", main_a, 3);
	check(mh_core_frame(0) == MH_GONE && F.switches == 2, "so a quick page back switches at once");
	mh_core_stop(0);
	mh_core_start(0);
	F.hold = -1;

	/* A page half written never reaches the screen. */
	fresh();
	post_page(0x07000301, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_start(0);
	old_label = mh_label(0);
	post_page(0x07000301, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_b, 3);
	w32(BOX(MBOX_OFF_PAGE_SEQ), r32(BOX(MBOX_OFF_PAGE_SEQ)) + 1);      /* qwark in the middle of the next */
	put_str(BOX(MBOX_OFF_ROWS) + MROW_OFF_LABEL, "half written", MROW_LABEL_LEN);
	check(mh_core_frame(0) == 0 && F.shows == 0 && str_is(old_label, "Ghost Ratchet") && str_is(mh_value(0), "OFF"),
	      "page_seq odd: nothing taken, the page on show as it was");
	w32(BOX(MBOX_OFF_PAGE_SEQ), r32(BOX(MBOX_OFF_PAGE_SEQ)) + 1);
	check(mh_core_frame(0) == 0 && F.shows == 1 && mh_label(0) == old_label && str_is(old_label, "Half Written"),
	      "whole again: taken in place, at the same address");

	/* A close forgets the cursor; the next open counts its frames from 0; a switch does not. */
	fresh();
	post_page(0x00000401, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_a, 3);
	mh_core_start(0);
	F.cursor = 1;
	for (i = 0; i < 5; i++) mh_core_frame(0);
	check(r32(BOX(MBOX_OFF_FRAMES)) == 5, "five frames up");
	mh_core_press(0, MH_BTN_CLOSE, 1);
	mh_core_stop(0);
	check(F.last_close == MH_CLOSE_EXIT && r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_CLOSED &&
	      r8(MH_STATE + ST_LAST_CURSOR) == 0, "Start closes; the stop forgets where the cursor was");
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && r32(BOX(MBOX_OFF_FRAMES)) == 0 &&
	      mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR)) == 0,
	      "the next open, the page unchanged: frames from 0, the cursor on the first row");
	mh_core_frame(0);
	post_page(0x07000402, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	mh_core_stop(0);
	mh_core_start(0);
	check(r32(BOX(MBOX_OFF_FRAMES)) == 2, "a switch keeps counting");

	/* Start halfway through a switch: the stop is a close, and the set the switch was for dropped. */
	post_page(0x00000403, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", main_a, 3);
	check(mh_core_frame(0) == MH_GONE && r8(MH_STATE + ST_SWITCHING) == 1, "a switch under way");
	front0 = r8(MH_STATE + ST_FRONT);
	mh_core_press(0, MH_BTN_CLOSE, 0);
	mh_core_stop(0);
	check(r8(MH_STATE + ST_SWITCHING) == 0 && r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_CLOSED &&
	      r8(MH_STATE + ST_FRONT) == (front0 ^ 1),
	      "Start then: a close, the menu down, the page last taken's set the one on show for the next open");
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && mh_nrows() == 3, "the next open shows the page in the mailbox");

	/* A title-case game: the label of its row and its waiting title in its own case. */
	fresh();
	g_case_main = MH_CASE_TITLE;
	g_case_head = MH_CASE_TITLE;
	mh_core_start(0);
	get_str(mh_title(), buf, sizeof(buf));
	check(strcmp(buf, "Trainer") == 0 && str_is(mh_text(0), "Waiting For Qwark"),
	      "before any page in a title-case game: \"Trainer\", \"Waiting For Qwark\"");
	post_page(0x07000501, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cfg_a, 3);
	mh_core_frame(0);
	mh_core_stop(0);
	mh_core_start(0);
	mh_core_press(0, MH_BTN_RIGHT, 0);
	for (i = 0; i < 30; i++) mh_core_frame(0);
	check(str_is(mh_title(), "Waiting For Qwark..."), "and its waiting title in its own case");
}

void test_menu_core(void)
{
	static const struct prow root[4] = {
		{ MROW_LINK, MROWF_ENABLED, 0x0700, "game", "", NULL },
		{ MROW_ACTION, MROWF_ENABLED, 0x0201, "die", "done", NULL },
		{ MROW_OPTION, MROWF_ENABLED | MROWF_ON, 0x0302, "Marauder", "", NULL },
		{ MROW_INFO, 0, 0x0500, "qwark build 54", "", NULL }
	};
	static const struct prow cheats[5] = {
		{ MROW_TOGGLE, MROWF_ENABLED | MROWF_STEPS | MROWF_ON, 0x0205, "ghost ratchet", "Auto",
		  "Auto also turns it on whenever the game starts." },
		{ MROW_ACTION, MROWF_ENABLED, 0x020B, "unlock all planets", "", "Runs it." },
		{ MROW_CHOICE, MROWF_ENABLED | MROWF_STEPS, 0x08F0, "position slot", "3", "The slot." },
		{ MROW_INFO, 0, 0x0207, "colour", "#\x01\xC3", NULL },
		{ MROW_MORE, MROWF_ENABLED, 0x0401, "More", "", NULL }
	};
	char buf[96];
	u32 ev;

	group("console menu: the renderer core, against a fake shim");

	/* No page yet: one grey row, cased as the game cases its lists. */
	fresh();
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && mh_nrows() == 1 && str_is(mh_text(0), "WAITING FOR QWARK") &&
	      str_is(mh_title(), "TRAINER") && mh_root() && (mh_row_flags(0) & MROWF_ENABLED) == 0,
	      "before any page: one grey row, \"WAITING FOR QWARK\", under TRAINER (MH_ENTRY_LABEL), at the root");
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN && r32(MH_STATE + ST_STARTS) == 1,
	      "the start marks the mailbox open and counts itself");
	mh_core_press(0, MH_BTN_BACK, 0);
	check(F.closes == 1 && F.last_close == MH_CLOSE_BACK && r32(BOX(MBOX_OFF_EV_SEQ)) == 0,
	      "and back there is the game's own back, no event");

	/* A MAIN page: one string a row, in the game's capitals, the decorations on. */
	post_page(0x00000101, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "Trainer", "", root, 4);
	F.cursor = 0;
	check(mh_core_frame(0) == 0 && F.shows == 1 && (F.last_how & MH_SHOW_NEW) != 0 &&
	      (F.last_how & MH_SHOW_ROWS) != 0 && mh_nrows() == 4,
	      "the next frame takes the page and shows it: new, with another number of rows");
	check(str_is(mh_text(0), "GAME: >") && str_is(mh_text(1), "DIE: DONE") &&
	      str_is(mh_text(2), "MARAUDER *") && str_is(mh_text(3), "QWARK BUILD 54"),
	      "rows read \"GAME: >\", \"DIE: DONE\", \"MARAUDER *\", \"QWARK BUILD 54\"");
	check(str_is(mh_title(), "TRAINER") && mh_core_cursor(0) == 1, "the title in capitals, the cursor on the page's row");
	check(mh_core_cursor(3) == 0, "a cursor kept on a grey row moves to the first it may land on");
	check(mh_core_frame(0) == 0 && F.shows == 1, "the same page is not shown again");

	g_case_main = MH_CASE_AS_IS;
	g_style = 0;
	post_page(0x00000102, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", root, 4);
	mh_core_frame(0);
	check(str_is(mh_text(0), "game") && str_is(mh_text(1), "die: done") && str_is(mh_text(2), "Marauder") &&
	      (F.last_how & MH_SHOW_ROWS) == 0,
	      "another game's casing and no decorations: \"game\", \"die: done\", \"Marauder\"");
	g_case_main = MH_CASE_UPPER;
	g_style = 0x0F;

	/* Presses. */
	mh_core_press(0, MH_BTN_CROSS, 3);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 0, "the cross on a grey row posts nothing");
	mh_core_press(0, MH_BTN_CROSS, 0);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 1 && r8(BOX(MBOX_OFF_EV_KIND)) == MBOX_EV_ACTIVATE &&
	      r8(BOX(MBOX_OFF_EV_ROW)) == 0 && r16(BOX(MBOX_OFF_EV_ID)) == 0x0700 &&
	      r32(BOX(MBOX_OFF_EV_KEY)) == 0x00000102 && F.sound == MH_SND_FORWARD,
	      "the cross on a link posts ACTIVATE with its row, id and the page's key, and the forward sound");
	mh_core_press(0, MH_BTN_CROSS, 1);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 1 && r32(MH_STATE + ST_DROPPED) == 1,
	      "a press while qwark has not answered is dropped, and counted");
	mh_core_press(0, MH_BTN_LEFT, 0);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 1, "so is anything else");

	/* Waiting, then the answer. */
	for (ev = 0; ev < 29; ev++) mh_core_frame(0);
	check(F.titles == 0, "29 frames unanswered: nothing yet");
	mh_core_frame(0);
	check(F.titles == 1 && str_is(mh_title(), "WAITING FOR QWARK..."), "the 30th: the title says it is waiting");
	ack();
	mh_core_frame(0);
	check(F.titles == 2 && str_is(mh_title(), "TRAINER"), "the answer puts the page's title back");

	/* The other widget: the core asks the shim to start again with it. */
	post_page(0x07000103, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cheats, 5);
	check(mh_core_frame(0) == MH_GONE && F.switches == 1 && r8(MH_STATE + ST_SWITCHING) == 1,
	      "a CONFIG page while the list is up: the shim is asked to switch, and the frame stops there");
	mh_core_stop(0);
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN, "the stop on the way does not close the mailbox");
	check(mh_core_start(0) == MBOX_PTYPE_CONFIG && r8(MH_STATE + ST_SWITCHING) == 0 &&
	      r32(MH_STATE + ST_STARTS) == 2, "and the start answers CONFIG");
	check(str_is(mh_label(0), "Ghost Ratchet") && str_is(mh_value(0), "AUTO") &&
	      str_is(mh_label(1), "Unlock All Planets") && str_is(mh_value(1), "...") &&
	      str_is(mh_value(2), "3") && str_is(mh_title(), "CHEATS") && str_is(mh_value(3), "#??"),
	      "two columns: labels in title case, values in capitals, an action's \"...\", a choice its bare "
	      "value \"3\" (round 3: no brackets, MH_STYLE_ARROWS or not), a title in capitals, a byte outside "
	      "ASCII a question mark");
	check(mh_core_cursor(9) == 0, "the cursor on the new page's row");

	/* The description line follows the cursor. */
	F.cursor = 0;
	F.descs = 0;
	mh_core_frame(0);
	check(F.descs == 1 && str_is(mh_desc(), "Auto also turns it on whenever the game starts."),
	      "the description line is the cursor's row's, read out of the mailbox as written");
	mh_core_frame(0);
	check(F.descs == 1, "and only read again when the cursor moves");
	F.cursor = 2;
	mh_core_frame(0);
	check(F.descs == 2 && str_is(mh_desc(), "The slot."), "which it does");
	post_page(0x07000103, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "Ghost Ratchet: done", cheats, 5);
	mh_core_frame(0);
	check(F.shows == 3 && (F.last_how & (MH_SHOW_NEW | MH_SHOW_ROWS)) == 0 && str_is(mh_desc(), "Ghost Ratchet: done"),
	      "the same page refreshed: shown again, neither new nor resized, its hint on the description line");

	/* Steps and pages. */
	mh_core_press(0, MH_BTN_RIGHT, 1);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 1, "Right on a row that does not step posts nothing");
	mh_core_press(0, MH_BTN_RIGHT, 0);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 2 && r8(BOX(MBOX_OFF_EV_KIND)) == MBOX_EV_RIGHT && F.sound == MH_SND_STEP,
	      "Right on one that does posts RIGHT, with the step sound");
	ack();
	mh_core_press(0, MH_BTN_L1, 0);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 2, "L1 on a page with no Previous row posts nothing");
	mh_core_press(0, MH_BTN_R1, 0);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 3 && r8(BOX(MBOX_OFF_EV_KIND)) == MBOX_EV_PAGE_NEXT,
	      "R1 with a More row posts PAGE_NEXT");
	ack();
	mh_core_press(0, MH_BTN_BACK, 2);
	check(r32(BOX(MBOX_OFF_EV_SEQ)) == 4 && r8(BOX(MBOX_OFF_EV_KIND)) == MBOX_EV_BACK && F.sound == MH_SND_BACK &&
	      F.closes == 1, "back on a page that is not the root posts BACK");
	ack();
	mh_core_press(0, MH_BTN_CLOSE, 0);
	check(F.closes == 2 && F.last_close == MH_CLOSE_EXIT, "Start or Select is the shim's close, always");

	/* Requests. */
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_OPEN);
	check(mh_core_frame(0) == 0 && r8(BOX(MBOX_OFF_REQUEST)) == MBOX_REQ_NONE,
	      "an open request while open is taken and nothing more");
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_CLOSE);
	check(mh_core_frame(0) == MH_GONE && F.closes == 3 && F.last_close == MH_CLOSE_EXIT &&
	      r8(BOX(MBOX_OFF_REQUEST)) == MBOX_REQ_NONE, "a close request is taken and the shim closes");
	mh_core_stop(0);
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_CLOSED && r8(MH_STATE + ST_SHOWN) == MH_NONE,
	      "a stop that is not a switch closes the mailbox");
	check(mh_core_open_request() == 0, "no open request, none taken");
	w8(BOX(MBOX_OFF_REQUEST), MBOX_REQ_OPEN);
	check(mh_core_open_request() == 1 && r8(BOX(MBOX_OFF_REQUEST)) == MBOX_REQ_NONE &&
	      r32(MH_STATE + ST_OPEN_REQS) == 1, "a shim that opens from C takes the request, counted");

	/* A page half written is left; a shim with one widget draws CONFIG pages as MAIN. */
	w32(BOX(MBOX_OFF_PAGE_SEQ), r32(BOX(MBOX_OFF_PAGE_SEQ)) + 1);
	ev = r32(MH_STATE + ST_PAGES);
	mh_core_frame(0);
	check(r32(MH_STATE + ST_PAGES) == ev, "page_seq odd: the page is not taken");
	w32(BOX(MBOX_OFF_PAGE_SEQ), r32(BOX(MBOX_OFF_PAGE_SEQ)) + 1);

	fresh();
	g_cfg_rows = 0;
	post_page(0x07000201, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cheats, 5);
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && str_is(mh_text(0), "GHOST RATCHET: AUTO") &&
	      str_is(mh_text(1), "UNLOCK ALL PLANETS"),
	      "a shim with no CONFIG widget is handed a CONFIG page as MAIN, one string a row");
	post_page(0x07000202, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cheats, 5);
	check(mh_core_frame(0) == 0 && F.switches == 0, "and never asked to switch");
	get_str(mh_title(), buf, sizeof(buf));
	check(strcmp(buf, "CHEATS") == 0 && r32(MH_STATE + ST_PAGES) == 2, "the pages counted");

	/*
	 * A widget that takes no new page in place (UYA's: MH_RESTART_* say which
	 * of shim_show's reasons it cannot take): the core starts the screen again.
	 */
	fresh();
	g_restart_main = MH_SHOW_NEW | MH_SHOW_ROWS;
	post_page(0x00000301, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", root, 4);
	check(mh_core_start(0) == MBOX_PTYPE_MAIN && mh_nrows() == 4, "a restarting shim starts on a MAIN page");
	F.cursor = 2;
	check(mh_core_cursor(0) == 0 && mh_core_frame(0) == 0 && F.shows == 0, "and has nothing to show again");
	post_page(0x00000301, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "Trainer", "", root, 4);
	check(mh_core_frame(0) == 0 && F.shows == 1 && F.last_how == 0 && F.switches == 0,
	      "the same page refreshed is shown in place");
	post_page(0x00000302, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "Trainer", "", root, 3);
	check(mh_core_frame(0) == MH_GONE && F.switches == 1 && F.shows == 1 && r8(MH_STATE + ST_SWITCHING) == 1,
	      "a new page: the shim is asked to start its screen again, not to show it");
	mh_core_stop(0);
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN && mh_core_start(0) == MBOX_PTYPE_MAIN &&
	      mh_nrows() == 3 && mh_core_cursor(0) == 1,
	      "the stop on the way keeps the menu open, and the start takes the page with its own cursor");
	post_page(0x00000302, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "Trainer", "", root, 4);
	check(mh_core_frame(0) == MH_GONE && F.switches == 2, "another number of rows on the same page: again");
	g_restart_cfg = MH_SHOW_ROWS;
	post_page(0x07000303, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cheats, 5);
	check(mh_core_frame(0) == 0 && F.switches == 2 && F.shows == 1 && mh_nrows() == 3,
	      "a frame more of the screen going (round 3): no page taken, the old page still on show");
	mh_core_stop(0);
	check(mh_core_start(0) == MBOX_PTYPE_CONFIG && mh_nrows() == 5 && str_is(mh_label(0), "Ghost Ratchet"),
	      "the start takes over, with the newest page in the mailbox, the other type");
	post_page(0x07000304, MBOX_PTYPE_CONFIG, 0, 0, "cheats", "", cheats, 5);
	check(mh_core_frame(0) == 0 && F.switches == 2 && (F.last_how & MH_SHOW_NEW) != 0,
	      "a CONFIG widget that only restarts for another number of rows takes a new key in place");

	test_menu_core_sets();
}
