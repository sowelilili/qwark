/*
 * Ratchet & Clank's console menu shim: qwark's pages on the TV with the game's
 * own pause-menu framework. mh_rac1.h says where it lives, what it hooks and
 * what it calls; mh_shim.h is the interface to the core (mh_core.c), which
 * reads the mailbox, composes the text and posts the events. This file only
 * talks to the game, the way probe N1c did on hardware:
 *
 *   MAIN    a page of links and buttons on the main pause's own panes: one
 *           row per pane, each a one-row generic list whose text is a text id
 *           the get_ui_string hook answers with the shim's copy of the row;
 *   CONFIG  a page of labels and controls in Goodies > Cheats' layout, its
 *           list pane drawn by the shim's own list draw (mh_rac1_cfg.c).
 *
 * The menu's pages are pages of the game's own framework, in the shim's state:
 * one MAIN page, two CONFIG pages and the planet picker's (mh_rac1_pick.c),
 * which the game reaches by pointer (the Help row's page word, the reopen
 * fields, the shim's writes of the next page). The game calls the shim's
 * items every frame their page is current:
 *   mh_row_update    the seven one-row panes of the MAIN page; the first runs
 *                    the core's frame and the pane moves;
 *   mh_conf_update   the CONFIG list: the core's frame and the input;
 *   mh_conf_draw     the CONFIG list's draw, into its pane;
 *   mh_pick_update   the planet picker's list, on the Galactic Map's look;
 * and from its main loop:
 *   mh_text_stub     the text ids, on get_ui_string's not-found path (asm);
 *   mh_play_stub     after every play update, which calls mh_play: the pages
 *                    set up, the combo's open, the close seen.
 *
 * "The menu's screen" of the core is whichever of the shim's pages is current:
 * mh_core_start runs as one is arrived on, mh_core_stop as the panes leave it
 * for another page or for the game's, and as the pause closes.
 *
 * Every access to game memory goes through r8..w32 (mh_common.h), so the host
 * tests run this file on qwark's fake console with the real bytes.
 */
#include "mh_common.h"
#include "mh_shim.h"
#include "mh_rac1_shim.h"

#define PANES        14
#define MAIN_PANES   MH_MAIN_ROWS

/* The shim's state starts where the core's ends, and its parts do not overlap. */
typedef char mh_rac1_layout[(MH_STATE + MH_CORE_SIZE <= MH_SHIM &&
                             MH_SHIM + SH_SIZE <= MH_DATA_END &&
                             SH_STRTAB + 4 * R1_ID_COUNT <= SH_PAGE_LIST &&
                             SH_PAGE_CONF + 2 * R1_PAGE_SIZE <= SH_ROWS &&
                             SH_ROWS + MAIN_PANES * R1_LIST_SIZE <= SH_TITLE &&
                             SH_ENTRIES + MAIN_PANES * R1_ENTRY_SIZE <= SH_CUTBUF &&
                             SH_CUTBUF + R1_CUTBUF_LEN <= SH_ENTRY_TEXT &&
                             SH_ENTRY_TEXT + 8 <= SH_TITLE_TEXT &&
                             SH_TITLE_TEXT + MH_TITLE_LEN <= SH_TEXT &&
                             SH_TEXT + MAIN_PANES * MH_TEXT_LEN <= SH_PICK &&
                             SH_PICK_LAST + 4 <= SH_PAGE_MAP &&
                             SH_PAGE_MAP + R1_PAGE_SIZE <= SH_MAP_LIST &&
                             SH_MAP_LIST + R1_LIST_SIZE <= SH_MAP_GALAXY &&
                             SH_MAP_GALAXY + 0x30 <= SH_MAP_ENTRIES &&
                             SH_MAP_ENTRIES + (R1_PLANETS + 1) * R1_GENTRY_SIZE <= SH_VELDIN2 &&
                             SH_VELDIN2 + 8 <= SH_SIZE) ? 1 : -1];

typedef int  (*fn_i_u)(u32);
typedef void (*fn_v_iiu)(int, int, u32);
typedef void (*fn_v_uii)(u32, int, int);

#define list_update   GAME_FN(fn_i_u,   OPD_LIST_UPDATE, mock_list_update)
#define play_sound    GAME_FN(fn_v_iiu, OPD_PLAY_SOUND,  mock_play_sound)
#define anim_set      GAME_FN(fn_v_uii, OPD_ANIM_SET,    mock_anim_set)

/* ------------------------------------------------------------- helpers */

static void count(u32 a)
{
	w32(a, r32(a) + 1);
}

/* The shim's page the game shows now, as AT_*. */
R1_FN u32 r1_at(void)
{
	u32 p = r32(SH(SH_AT_PAGE));

	if (p == 0 || p != r32(R1_MENU_PAGE)) return AT_NONE;
	return p == PAGE_LIST ? AT_LIST : p == PAGE_MAP ? AT_MAP : AT_CONF;
}

R1_FN u32 r1_phase(void)
{
	return r8(SH(SH_PHASE));
}

/*
 * A close asked for (shim_close): the shim's items answer 1, which has the
 * pause close itself, until the game has begun the close.
 */
R1_FN int r1_closing(void)
{
	return r8(SH(SH_CLOSING)) != 0 && r32(R1_MENU_CLOSE) == 0;
}

R1_FN int r1_enabled(u32 i)
{
	return (mh_row_flags(i) & MROWF_ENABLED) != 0;
}

/* Left, Right, L1 and R1, which the game's lists ignore. */
R1_FN u32 r1_steps(u32 e)
{
	if ((e & PADB_LEFT) != 0)  return MH_BTN_LEFT;
	if ((e & PADB_RIGHT) != 0) return MH_BTN_RIGHT;
	if ((e & PADB_L1) != 0)    return MH_BTN_L1;
	if ((e & PADB_R1) != 0)    return MH_BTN_R1;
	return 0;
}

R1_FN void r1_sound(u32 id, u32 moby)
{
	if (moby != 0) play_sound((int)id, 17, moby);
}

/* The render draws pane 6 only with Goodies unlocked; a seven-row page sets its flag while it shows. */
R1_FN void r1_goodies_pane(int want)
{
	if (want && !r8(SH(SH_GOODIES))) {
		w8(SH(SH_GOODIES_WAS), r8(R1_GOODIES_PANE));
		w8(SH(SH_GOODIES), 1);
		w8(R1_GOODIES_PANE, 1);
	} else if (!want && r8(SH(SH_GOODIES))) {
		w8(R1_GOODIES_PANE, r8(SH(SH_GOODIES_WAS)));
		w8(SH(SH_GOODIES), 0);
	}
}

/* --------------------------------------------------------------- setup */

/*
 * What setup() writes besides the poses and the rows, {offset from MH_SHIM,
 * word}: the pages' screen, parent, focus and items (Cheats' panes: the title,
 * the list, Cheats' own hint, and none on the two panes it leaves empty), the
 * title item (Cheats' 0x72F65C with the shim's text id), the list item, the
 * text table's fixed entries.
 */
#define CONF_WORDS(c) \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_PARENT, PAGE_LIST }, \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_SCREEN, R1_SCREEN }, \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_FOCUS, LIST }, \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_ITEM + 0, TITLE }, \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_ITEM + 4 * R1_CONF_LIST_PANE, LIST }, \
	{ SH_PAGE_CONF + (c) * R1_PAGE_SIZE + PG_ITEM + 4 * R1_CONF_HINT_PANE, R1_CHEATS_HINT }

static const u32 setup_words[][2] = {
	{ SH_PAGE_LIST + PG_SCREEN, R1_SCREEN },
	{ SH_PAGE_LIST + PG_FOCUS, ROW(0) },
	CONF_WORDS(0),
	CONF_WORDS(1),
	{ SH_TITLE + IT_UPDATE, OPD_LABEL_UPDATE },
	{ SH_TITLE + IT_DRAW, OPD_LABEL_DRAW },
	{ SH_TITLE + IT_FLAGS, 0x0F },
	{ SH_TITLE + IT_DATA, R1_ID_TITLE },
	{ SH_LIST + IT_UPDATE, MH_FN_CONF },
	{ SH_LIST + IT_DRAW, MH_FN_DRAW },
	{ SH_STRTAB + 4 * (R1_ID_TRAINER - R1_ID_BASE), SH(SH_ENTRY_TEXT) },
	{ SH_STRTAB + 4 * (R1_ID_TITLE - R1_ID_BASE), SH(SH_TITLE_TEXT) }
};

/* The Help row's label, MH_ENTRY_LABEL, as the main page cases its rows. */
static const char entry_label[] = MH_ENTRY_LABEL;

/*
 * The pages and items, as the game's own are laid out: the MAIN page on the
 * main page's poses (0x729A80), its items the seven one-row lists; the two
 * CONFIG pages on Cheats' (0x72F5D4) with In-Level Movies' title pose, their
 * title a copy of Cheats' title item, their list the shim's, their hint
 * Cheats' own, their parent the MAIN page. Once, from the play hook, before
 * the menu can open.
 */
static void setup(void)
{
	u32 k, pose;

	for (k = SH_STRTAB; k < SH_SIZE; k += 4) w32(SH(k), 0);
	for (k = 0; k < PANES; k++) {
		pose = k == 0 ? R1_CONF_TITLE_POSE : k < 5 ? R1_CHEATS_POSE0 + k : k;
		w32(PAGE_LIST + PG_ANIM + 4 * k, k < MAIN_PANES ? R1_MAIN_POSE0 + k : k);
		w32(PAGE_CONF(0) + PG_ANIM + 4 * k, pose);
		w32(PAGE_CONF(1) + PG_ANIM + 4 * k, pose);
	}
	for (k = 0; k < MAIN_PANES; k++) {
		w32(PAGE_LIST + PG_ITEM + 4 * k, ROW(k));
		w32(ROW(k) + IT_UPDATE, MH_FN_ROW);
		w32(ROW(k) + IT_DRAW, OPD_LIST_DRAW);
		w32(ROW(k) + IT_FLAGS, R1_ROW_FLAGS);
		w32(ROW(k) + IT_DATA, ENTRY(k));
	}
	for (k = 0; k < sizeof(setup_words) / sizeof(setup_words[0]); k++)
		w32(SH(setup_words[k][0]), setup_words[k][1]);
	for (k = 0; k < sizeof(entry_label) && k < 8; k++) w8(SH(SH_ENTRY_TEXT) + k, (u8)entry_label[k]);
	r1_pick_setup();
	w32(SH(SH_READY), MH_MAGIC);
}

/*
 * The core's screen starts and stops through these, so the shim knows whether
 * it is started: mh_core_start runs once for each mh_core_stop, as the core
 * takes a start with no switch under way for a fresh open.
 */
static u32 start(void)
{
	w8(SH(SH_STARTED), 1);
	return (u32)mh_core_start(0);
}

static void stop(void)
{
	w8(SH(SH_STARTED), 0);
	mh_core_stop(0);
}

/* ------------------------------------------------------------ the panes */

/* A pose's frames (class +0x48 + 4 * pose, its +0x10), 12 if unknown. */
static u32 frames_of(u32 moby, u32 pose)
{
	u32 cls = r32(moby + R1_MOBY_CLASS), p, f;

	if (cls == 0) return 12;
	p = r32(cls + 0x48 + 4 * pose);
	f = p != 0 ? r8(p + R1_POSE_FRAMES) : 12;
	return f != 0 ? f : 12;
}

/*
 * Panes lo..hi-1 on their pose pose0 + k of a root page's (the main page's,
 * or the Galactic Map's for the planet picker): OUT plays it forwards (the
 * page's layout to hidden), IN backwards (hidden to the layout), HIDE puts it
 * at its last frame, hidden, at once. The phase runs for the slowest, at
 * least a frame.
 */
R1_FN void r1_panes(u32 ph, u32 lo, u32 hi, int how, u32 pose0)
{
	u32 k, f, moby, most = 1;

	for (k = lo; k < hi; k++) {
		moby = r32(R1_PANES + 4 * k);
		if (moby == 0) continue;
		f = frames_of(moby, pose0 + k);
		anim_set(moby, (int)(pose0 + k), how == MOVE_OUT ? 0 : (int)f - 1);
		w32(moby + R1_MOBY_SPEED, how == MOVE_IN ? 0xBF800000 : 0x3F800000);   /* -1.0f, 1.0f */
		if (how != MOVE_HIDE && f > most) most = f;
	}
	if (most > 30) most = 30;
	w8(SH(SH_PHASE), ph);
	w8(SH(SH_TIMER), most);
}

#define panes(ph, lo, hi, how) r1_panes(ph, lo, hi, how, R1_MAIN_POSE0)

/* The MAIN page's panes fly in; the ones it does not use go at once. */
static void show(void)
{
	u32 n = r8(SH(SH_NROWS));

	panes(PH_IN, n, MAIN_PANES, MOVE_HIDE);
	panes(PH_IN, 0, n, MOVE_IN);
}

/* Its panes fly out; `then` follows. */
R1_FN void r1_leave(u32 then)
{
	w8(SH(SH_THEN), then);
	panes(PH_OUT, 0, r8(SH(SH_NROWS)), MOVE_OUT);
}

#define leave r1_leave

/* The game's own transition to page `to` (the next page), and nothing until it arrives. */
static void transition(u32 to)
{
	r1_goodies_pane(0);
	w32(R1_MENU_NEXT, to);
	w8(SH(SH_PHASE), PH_WAIT);
}

/* To a CONFIG page: the other of the two, Cheats' poses forwards from Goodies' layout. */
static void go_conf(void)
{
	u32 c = r8(SH(SH_CONF)) ^ 1u;

	w8(SH(SH_CONF), c);
	transition(PAGE_CONF(c));
}

/* To the MAIN page from a CONFIG page: the CONFIG page's poses backwards. */
static void go_list(void)
{
	w32(PAGE_CONF(r8(SH(SH_CONF))) + PG_PARENT, PAGE_LIST);
	transition(PAGE_LIST);
}

/* ------------------------------------------------------- the MAIN widget */

/*
 * The rows' text as the shim's own copies, so that panes flying out keep the
 * page they belong to while the core already holds the next, and the rows'
 * generic entries, inert (1) or grey (0, the generic list's grey colour).
 * Up and Down between the panes are the game's: at a one-row list's ends its
 * update moves the focus to the item its up and down words name, which link
 * the enabled rows as the main page links its own (0x729BB0..: no wrap) and
 * leave a grey row out.
 */
static void main_rows(void)
{
	u32 n = r8(SH(SH_NROWS)), k, j, e, r, up, down;

	for (k = 0; k < MAIN_PANES; k++) {
		e = ENTRY(k);
		r = ROW(k);
		w16(e, k < n ? R1_ID_ROW0 + k : 0);
		w16(e + 2, k < n && r1_enabled(k) ? 1 : 0);
		w32(STRTAB(R1_ID_ROW0 + k), k < n ? TEXT(k) : 0);
		if (k < n) r1_copy_str(TEXT(k), mh_text(k), MH_TEXT_LEN);
		up = down = 0;
		for (j = k; k < n && r1_enabled(k) && j-- > 0; ) {
			if (r1_enabled(j)) { up = ROW(j); break; }
		}
		for (j = k + 1; k < n && r1_enabled(k) && j < n; j++) {
			if (r1_enabled(j)) { down = ROW(j); break; }
		}
		w32(r + LI_UP, up);
		w32(r + LI_DOWN, down);
	}
}

/* The row the MAIN page's focus is on. */
R1_FN u32 r1_main_cursor(void)
{
	u32 k = (r32(PAGE_LIST + PG_FOCUS) - ROW(0)) / R1_LIST_SIZE;
	return k < MAIN_PANES ? k : 0;
}

#define main_cursor r1_main_cursor

/* The MAIN page the core just started: its rows, links and focus. */
static void main_start(void)
{
	u32 n = mh_nrows(), k, c;

	if (n > MAIN_PANES) n = MAIN_PANES;
	w8(SH(SH_NROWS), n);
	for (k = 0; k < MAIN_PANES; k++) {
		w32(ROW(k) + IT_FLAGS, R1_ROW_FLAGS);
		w32(ROW(k) + LI_CURSOR, 0);
		w32(ROW(k) + LI_SCROLL, 0);
	}
	main_rows();
	c = mh_core_cursor(r8(MH_STATE + ST_LAST_CURSOR));
	w32(PAGE_LIST + PG_FOCUS, ROW(c < n ? c : 0));
	w32(PAGE_LIST + PG_PENDING, 0);
	r1_goodies_pane(n == MAIN_PANES);
}

/*
 * A one-row pane: the main page's size if its text fits the pane's width (with
 * the 6 units the game's own measure keeps), else the small size, cut short
 * only if that is still too wide.
 */
static void fit_row(u32 k)
{
	u32 r = ROW(k), s = TEXT(k);
	int w = (int)r32(r + IT_W);
	u32 flags = R1_ROW_FLAGS;

	if (k == 0) {
		w16(SH(SH_ROW_W), (u32)w);
		w16(SH(SH_ROW_H), r32(r + IT_H));
	}
	if (w <= 6 || r8(R1_JAPANESE) != 0) return;
	if (r1_text_w(R1_FONT_LARGE, s) + 6 > w) {
		flags |= R1_ROW_SMALL;
		r1_cut(R1_FONT_SMALL, s, w - 6, 1.0f);
	}
	w32(r + IT_FLAGS, flags);
}

/* ------------------------------------------------- arriving, the phases */

/*
 * Every update of the shim's items asks first: on which of the shim's pages is
 * the game now, and is that an arrival? An arrival starts the core's screen
 * with the widget the page wants, unless the screen is up already (a page of
 * the other type found on arriving, which the game's transition goes on to
 * at once, starts once): mh_core_start runs once for each mh_core_stop, so
 * the core takes a fresh open only for one.
 */
R1_FN u32 r1_arrive(void)
{
	u32 page = r32(R1_MENU_PAGE), was = r32(SH(SH_AT_PAGE)), type;
	u32 where = page == PAGE_LIST ? AT_LIST : (page == PAGE_CONF(0) || page == PAGE_CONF(1)) ? AT_CONF : AT_NONE;

	/*
	 * The same page as before is no arrival, unless the shim left it for the
	 * game's main page and the Help row has brought it back (the transition
	 * the shim asked for is done once the next page is clear again).
	 */
	if (page == was && !(where == AT_LIST && r8(SH(SH_GONE)) != 0 && r32(R1_MENU_NEXT) == 0)) return where;
	w32(SH(SH_AT_PAGE), page);
	w8(SH(SH_GONE), 0);
	if (where == AT_NONE) return where;

	w32(SH(SH_UP), R1_SCREEN_UP);
	w8(SH(SH_PHASE), PH_IDLE);
	w8(SH(SH_THEN), T_NONE);
	type = r8(SH(SH_STARTED)) != 0 ? r8(MH_STATE + ST_SHOWN) : start();
	if (where == AT_CONF) {
		w8(SH(SH_CONF), page == PAGE_CONF(1));
		if (type == MBOX_PTYPE_CONFIG) r1_conf_start();
		else go_list();
	} else if (type != MBOX_PTYPE_MAIN) {
		go_conf();
	} else {
		main_start();
		if (r1_pick_returned(r8(SH(SH_NROWS)))) {
			/* Back from the planet picker: its transition flew the panes in. */
		} else if (r8(SH(SH_OPENING)) != 0) {
			/* The open brought all seven panes in: the ones this page does not use go. */
			w8(SH(SH_OPENING), 0);
			mh_count(ST_OPENS);
			panes(PH_IN, r8(SH(SH_NROWS)), MAIN_PANES, MOVE_OUT);
		} else {
			/* From the main page (all out) or from a CONFIG page (in Options' layout). */
			show();
		}
	}
	return where;
}

/* Once a frame on the MAIN page: the panes' moves, and what follows a fly-out. */
static void moves(void)
{
	u32 ph = r1_phase(), t = r8(SH(SH_TIMER));

	if (ph != PH_IN && ph != PH_OUT) return;
	if (t > 1) {
		w8(SH(SH_TIMER), t - 1);
		return;
	}
	w8(SH(SH_PHASE), PH_IDLE);
	if (ph == PH_IN) return;
	t = r8(SH(SH_THEN));
	w8(SH(SH_THEN), T_NONE);
	if (t == T_RESTART) {
		/*
		 * The menu's screen stops; it starts again on the next page here for
		 * a MAIN page, or once the game's transition has arrived on a CONFIG
		 * page (r1_arrive).
		 */
		stop();
		if (r8(MH_STATE + ST_PTYPE) == MBOX_PTYPE_MAIN) {
			start();
			main_start();
			show();
		} else {
			go_conf();
		}
	} else if (t == T_GAME) {
		/* The main page as the MAIN page's parent: its poses backwards, all seven in, on the Help row. */
		stop();
		w32(SH(SH_UP), 0);
		w32(PAGE_LIST + PG_PARENT, R1_MAIN_PAGE);
		w8(SH(SH_GONE), 1);
		transition(R1_MAIN_PAGE);
	} else if (t == T_PICK) {
		/* The planet picker: the game's transition to the Galactic Map's page, its panes flying in. */
		r1_pick_open();
	}
}

/* --------------------------------------------------- the shim, for the core */

/*
 * The page shown, refreshed in place: MH_RESTART_* name both of `how`'s bits
 * for both widgets, so a new page or another number of rows never comes here.
 * A MAIN page's rows get their text, colours and links again; a CONFIG page is
 * drawn from the core's rows every frame, and only its window is kept.
 */
MH_API void shim_show(int p, u32 how)
{
	(void)p;
	(void)how;
	if (r1_at() == AT_CONF) {
		r1_keep_visible();
		return;
	}
	main_rows();
	if (!r1_enabled(main_cursor())) w32(PAGE_LIST + PG_FOCUS, ROW(mh_core_cursor(main_cursor())));
}

/*
 * Another page, or the other widget: from the MAIN page its panes fly out
 * first, then the screen starts again (moves); from a CONFIG page the screen
 * stops at once and the game's transition goes to the MAIN page or to the
 * other CONFIG page, where the screen starts again on arrival.
 */
MH_API void shim_switch(int p)
{
	(void)p;
	if (r1_at() != AT_CONF) {
		leave(T_RESTART);
		return;
	}
	stop();
	if (r8(MH_STATE + ST_PTYPE) == MBOX_PTYPE_MAIN) go_list();
	else go_conf();
}

/*
 * Back from the root: to the game's main page when the menu came from its Help
 * row, the panes flying out first; out of the pause when it came from the
 * combo, as the page has no other parent. Start, Select and a close request:
 * out of the pause, which the next update of the shim's items asks for.
 */
MH_API void shim_close(int p, u32 how)
{
	(void)p;
	if (how == MH_CLOSE_BACK && r8(SH(SH_COMBO)) == 0 && r1_at() == AT_LIST) {
		r1_sound(SND_PAGE, r32(ROW(main_cursor()) + IT_MOBY));
		leave(T_GAME);
		return;
	}
	w8(SH(SH_CLOSING), 1);
}

/* A CONFIG page's title is the shim's copy, which the title item asks for every frame. */
MH_API void shim_title(int p)
{
	(void)p;
	if (r1_at() == AT_CONF) r1_copy_str(SH(SH_TITLE_TEXT), mh_title(), MH_TITLE_LEN);
}

MH_API u32 shim_cursor(int p)
{
	(void)p;
	return r1_at() == AT_CONF ? r8(SH(SH_CUR)) : main_cursor();
}

/*
 * The menus' own sounds: the select click for the cross, the cursor's tick for
 * a step and a window turned, the page sound for back on a MAIN page, whose
 * panes the shim moves itself. Back on a CONFIG page is the game's transition,
 * which plays its own.
 */
MH_API void shim_sound(u32 snd)
{
	int conf = r1_at() == AT_CONF;
	u32 moby = conf ? r32(LIST + IT_MOBY) : r32(ROW(main_cursor()) + IT_MOBY);

	if (snd == MH_SND_FORWARD)                         r1_sound(SND_SELECT, moby);
	else if (snd == MH_SND_STEP || snd == MH_SND_MOVE) r1_sound(SND_CURSOR, moby);
	else if (snd == MH_SND_BACK && !conf)              r1_sound(SND_PAGE, moby);
}

/* ------------------------------------------------------------ the entries */

/*
 * The MAIN page's seven one-row panes, every frame it is current. The first
 * runs the panes' moves and the core's frame; while panes move nothing takes
 * input. On the focused row: Triangle is the menu's back, the cross the row's
 * press, Left, Right, L1 and R1 steps and windows; Up, Down (the links) and
 * the Start close are the game's list.
 */
MH_API int mh_row_update(u32 item)
{
	u32 k = (item - ROW(0)) / R1_LIST_SIZE, e, b = 0;

	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC || r1_arrive() != AT_LIST) return 0;
	if (r1_closing()) return 1;
	if (k == 0) {
		moves();
		if (r1_phase() == PH_IDLE && r32(R1_MENU_CLOSE) == 0 && mh_core_frame(0) == MH_GONE) return r1_closing();
	}
	if (r1_phase() != PH_IDLE || k >= r8(SH(SH_NROWS))) return 0;
	fit_row(k);
	if (r32(PAGE_LIST + PG_FOCUS) != item || r32(R1_MENU_CLOSE) != 0) return list_update(item);

	e = r32(R1_PAD_EDGES);
	if ((e & PADB_TRIANGLE) != 0)               b = MH_BTN_BACK;
	else if ((e & r32(R1_CONFIRM_MASK)) != 0)   b = MH_BTN_CROSS;
	else if ((e & (PADB_UP | PADB_DOWN | PADB_CLOSE)) == 0) b = r1_steps(e);
	if (b == 0) return list_update(item);
	mh_core_press(0, b, k);
	return r1_closing();
}

/* Start's gates (0x16734C..0x1673BC), nothing else opening the pause, and no load of qwark's helper waiting. */
static int can_open(void)
{
	u32 ps = r32(R1_PLAYER_STATE);

	return r32(R1_STATE) == 0 && (int)r32(R1_FRAMES) >= 8 &&
	       ps != 29 && ps != 50 && ps != 114 &&
	       r8(R1_HAND_LOCK) != 2 && r8(R1_96BD91) == 0 && r32(R1_HP) != 0 &&
	       r32(R1_DEBUG_CAM) == 0 && r32(R1_RESUME_PAGE) == 0 && r32(R1_SHOULD_LOAD) == 0 &&
	       r8(R1_SF_LOAD) == 0 && r32(R1_SF_WAIT) == 0;
}

/*
 * After every play update. The pages and items set up the first time. A
 * written open the game has not taken in half a second is taken back, so the
 * pause never opens later on its own. In play with nothing waiting the menu is
 * down: the core's screen stops if it was up (the pause closed under it), and
 * everything is as the Help row expects it. An open request from qwark, when
 * Start itself could open the pause, writes the game's reopen fields, as probe
 * N1c's combo did: the next play frame opens the pause on the MAIN page, whose
 * open brings all seven panes in, and the spare panes then go.
 */
MH_API void mh_play(void)
{
	u32 st, req, pend;

	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC) return;
	if (r32(SH(SH_READY)) != MH_MAGIC) setup();

	st = r32(R1_STATE);
	pend = r8(SH(SH_PENDING));
	if (pend != 0) {
		if (r32(R1_RESUME_PAGE) != PAGE_LIST || st == R1_STATE_PAUSE) {
			pend = 0;
		} else if (st != 0 || --pend == 0) {
			w32(R1_RESUME_PAGE, 0);
			w32(R1_RESUME_SCREEN, 0);
			count(SH(SH_TIMEOUTS));
			pend = 0;
		}
		w8(SH(SH_PENDING), pend);
	}
	if (st == 0 && pend == 0) {
		r1_pick_reset();
		if (r8(SH(SH_STARTED)) != 0) {
			/* The pause closed with the menu up: its screen stops, the menu is down. */
			w8(MH_STATE + ST_SWITCHING, 0);
			stop();
		}
		w32(SH(SH_UP), 0);
		w32(SH(SH_AT_PAGE), 0);
		w32(PAGE_LIST + PG_PARENT, 0);
		w32(SH(SH_PHASE), 0);       /* phase, then, timer, pending */
		w32(SH(SH_COMBO), 0);       /* combo, opening, gone, closing */
		r1_goodies_pane(0);
	}

	req = r8(MH_BOX + MBOX_OFF_REQUEST);
	if (req == MBOX_REQ_CLOSE && st == 0) {
		/* Nothing to close in play. */
		w8(MH_BOX + MBOX_OFF_REQUEST, MBOX_REQ_NONE);
	} else if (req == MBOX_REQ_OPEN && pend == 0 && can_open() && mh_core_open_request()) {
		w8(SH(SH_COMBO), 1);
		w8(SH(SH_OPENING), 1);
		w32(R1_RESUME_CD4, r32(R1_969CD4));
		w32(R1_RESUME_SCREEN, R1_SCREEN);
		w32(R1_RESUME_PAGE, PAGE_LIST);
		w8(SH(SH_PENDING), 30);
	}
}
