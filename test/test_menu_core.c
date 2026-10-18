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

#include "../src/games/menuhelper/mh_core.c"

/* -------------------------------------------------------------- the fake shim */

static struct {
	int shows, last_how, switches, closes, last_close, titles, descs, sound, sounds;
	u32 cursor;
} F;

MH_API void shim_show(int p, u32 how)     { (void)p; F.shows++; F.last_how = (int)how; }
MH_API void shim_switch(int p)            { (void)p; F.switches++; }
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
	g_cfg_rows = 6;
	g_case_main = 1; g_case_head = 1; g_case_label = 2; g_case_value = 1;
	g_style = 0x0F;
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
	      str_is(mh_title(), "RACMAN") && mh_root() && (mh_row_flags(0) & MROWF_ENABLED) == 0,
	      "before any page: one grey row, \"WAITING FOR QWARK\", under RACMAN, at the root");
	check(r8(BOX(MBOX_OFF_HSTATE)) == MBOX_HSTATE_OPEN && r32(MH_STATE + ST_STARTS) == 1,
	      "the start marks the mailbox open and counts itself");
	mh_core_press(0, MH_BTN_BACK, 0);
	check(F.closes == 1 && F.last_close == MH_CLOSE_BACK && r32(BOX(MBOX_OFF_EV_SEQ)) == 0,
	      "and back there is the game's own back, no event");

	/* A MAIN page: one string a row, in the game's capitals, the decorations on. */
	post_page(0x00000101, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 1, "RaCMAN", "", root, 4);
	F.cursor = 0;
	check(mh_core_frame(0) == 0 && F.shows == 1 && (F.last_how & MH_SHOW_NEW) != 0 &&
	      (F.last_how & MH_SHOW_ROWS) != 0 && mh_nrows() == 4,
	      "the next frame takes the page and shows it: new, with another number of rows");
	check(str_is(mh_text(0), "GAME: >") && str_is(mh_text(1), "DIE: DONE") &&
	      str_is(mh_text(2), "MARAUDER *") && str_is(mh_text(3), "QWARK BUILD 54"),
	      "rows read \"GAME: >\", \"DIE: DONE\", \"MARAUDER *\", \"QWARK BUILD 54\"");
	check(str_is(mh_title(), "RACMAN") && mh_core_cursor(0) == 1, "the title in capitals, the cursor on the page's row");
	check(mh_core_cursor(3) == 0, "a cursor kept on a grey row moves to the first it may land on");
	check(mh_core_frame(0) == 0 && F.shows == 1, "the same page is not shown again");

	g_case_main = MH_CASE_AS_IS;
	g_style = 0;
	post_page(0x00000102, MBOX_PTYPE_MAIN, MBOX_PAGE_ROOT, 0, "RaCMAN", "", root, 4);
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
	check(F.titles == 2 && str_is(mh_title(), "RACMAN"), "the answer puts the page's title back");

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
	      str_is(mh_value(2), "< 3 >") && str_is(mh_title(), "CHEATS") && str_is(mh_value(3), "#??"),
	      "two columns: labels in title case, values in capitals, an action's \"...\", a choice's \"< 3 >\", "
	      "a title in capitals, a byte outside ASCII a question mark");
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
}
