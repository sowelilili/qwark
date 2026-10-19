/*
 * Ratchet & Clank's shim, the planet picker: the game's own Galactic Map for
 * the Trainer menu's Planets row (mh_rac1.h; rewrite's picker-shared.md).
 *
 * The page is the Galactic Map's as the ship opens it (0x72D87C), copied into
 * the shim's state: its poses, its title, its galaxy (the map's own draw),
 * its hint and its planet picture are the game's, and its list is the map
 * list's look (the scroll list's draw and flags) with every planet of qwark's
 * PLANET_LIST, Veldin to Veldin2, and the shim's own update. The game's map is
 * reached only from the ship and every close of it travels; this page is the
 * shim's, under the shim's screen 50, so it closes plainly, and the game's own
 * map and its words are never touched.
 *
 * It shows in the pause the Trainer menu is in, the way the MAIN pages move:
 *   open    the MAIN page's panes fly out, then the game's transition goes to
 *           the map page as if it were the MAIN page's parent, which plays the
 *           current page's poses backwards: those poses are the map's for that
 *           moment, so the panes fly in from hidden to the map's layout, as
 *           the map's opening flies them in;
 *   back    Triangle: the map's panes fly out, then the game's transition back
 *           to the MAIN page as the map page's parent, on the main page's
 *           poses backwards, so the MAIN panes fly in; the page and the row
 *           the picker came from are back;
 *   pick    the cross: mh_core_pick hands the planet to qwark (the mailbox's
 *           pick fields) and the pause closes; qwark loads the planet once
 *           the game is back in play;
 *   close   Start, Select, R3 or the combo: mh_core_picker_gone, and the
 *           pause closes, as anywhere.
 *
 * The core marks the picker up (mh_picker_up) before it calls shim_picker,
 * and takes no press while it is; the menu's screen stays started and the
 * core is not run (no page taken, no event posted), so the push is no close
 * and hstate stays open. Back on the MAIN page, mh_core_picker_back: the menu
 * comes back on the page it left. A close from the picker is a close.
 *
 * The map's planet page (0x72DC78) and its loader of planet maps (0x72DBC8)
 * are left out: both use the pause's map-only memory, which the game sets up
 * only for a pause that opens on the map (0x13B108).
 */
#include "mh_common.h"
#include "mh_shim.h"
#include "mh_rac1_shim.h"

#define PANES 14

/* Planet 18's name in the list: the game names both Veldins alike. */
static const char veldin2[] = "Veldin2";

static void map_count(u32 a)
{
	w32(a, r32(a) + 1);
}

/* Each pane on its pose pose0 + k for k < n, and on the hidden pose k past them. */
static void map_poses(u32 page, u32 pose0, u32 n)
{
	u32 k;

	for (k = 0; k < PANES; k++) w32(page + PG_ANIM + 4 * k, k < n ? pose0 + k : k);
}

/*
 * The picker's page, list and galaxy, once, from the play hook's setup: the
 * map page's poses, the game's title, hint and picture, the shim's list and
 * galaxy items ({offset from MH_SHIM, word}), and the planets' entries as the
 * map list's enter builds them (0x14B480: name, kind 1, subtitle, from the
 * planet table 0x7352B0), every planet in qwark's order.
 */
static const u32 map_words[][2] = {
	{ SH_PAGE_MAP + PG_SCREEN, R1_SCREEN },
	{ SH_PAGE_MAP + PG_FOCUS, MAP_LIST },
	{ SH_PAGE_MAP + PG_ITEM + 0, R1_MAP_TITLE },
	{ SH_PAGE_MAP + PG_ITEM + 4, MAP_LIST },
	{ SH_PAGE_MAP + PG_ITEM + 8, MAP_GALAXY },
	{ SH_PAGE_MAP + PG_ITEM + 12, R1_MAP_HINT },
	{ SH_PAGE_MAP + PG_ITEM + 16, R1_MAP_SHOT },
	{ SH_MAP_LIST + IT_UPDATE, MH_FN_PICK },
	{ SH_MAP_LIST + IT_DRAW, OPD_SCROLL_DRAW },
	{ SH_MAP_LIST + IT_FLAGS, R1_MAP_LIST_FLAGS },
	{ SH_MAP_LIST + IT_DATA, MAP_ENTRY(0) },
	{ SH_MAP_GALAXY + IT_DRAW, OPD_GALAXY_DRAW },
	{ SH_STRTAB + 4 * (R1_ID_VELDIN2 - R1_ID_BASE), SH(SH_VELDIN2) }
};

R1_FN void r1_pick_setup(void)
{
	u32 k, t;

	map_poses(PAGE_MAP, R1_MAP_POSE0, R1_MAP_PANES);
	for (k = 0; k < sizeof(map_words) / sizeof(map_words[0]); k++) w32(SH(map_words[k][0]), map_words[k][1]);
	for (k = 0; k < R1_PLANETS; k++) {
		t = R1_PLANET_TABLE + k * 12;
		w16(MAP_ENTRY(k) + 0, k == R1_VELDIN2 ? R1_ID_VELDIN2 : r32(t));
		w16(MAP_ENTRY(k) + 2, 1);
		w16(MAP_ENTRY(k) + 8, r32(t + 4));
	}
	for (k = 0; k < sizeof(veldin2); k++) w8(SH(SH_VELDIN2) + k, (u8)veldin2[k]);
}

/*
 * The core's call for the Planets row (MROWF_PICKER): the picker opens when
 * the MAIN page is up and still, its panes flying out first (mh_rac1.c's
 * moves, then r1_pick_open), and the core plays the forward sound. Answers 0
 * anywhere else, and qwark shows its Planets page.
 */
MH_API u32 shim_picker(int p)
{
	(void)p;
	if (r1_at() != AT_LIST || r1_phase() != PH_IDLE || r1_closing() || r32(R1_MENU_CLOSE) != 0) return 0;
	w8(SH(SH_PICK), PK_OPENING);
	w8(SH(SH_PICK_ROW), r1_main_cursor());
	w32(SH(SH_PICK_SEL), r32(R1_SELECTED));
	map_count(SH(SH_PICK_OPENS));
	r1_leave(T_PICK);
	return 1;
}

/*
 * The MAIN page's panes are out: the game's transition to the map page as the
 * MAIN page's parent, which plays the MAIN page's poses backwards, the map's
 * for now: the panes fly in to the map's layout.
 */
R1_FN void r1_pick_open(void)
{
	u32 k;

	for (k = 0; k < PANES; k++) w32(PAGE_LIST + PG_ANIM + 4 * k, r32(PAGE_MAP + PG_ANIM + 4 * k));
	w32(SH(SH_PICK_PARENT), r32(PAGE_LIST + PG_PARENT));
	w32(PAGE_LIST + PG_PARENT, PAGE_MAP);
	w32(PAGE_MAP + PG_PARENT, 0);
	r1_goodies_pane(0);
	w32(R1_MENU_NEXT, PAGE_MAP);
	w8(SH(SH_PHASE), PH_WAIT);
}

/*
 * The map page arrived: the MAIN page's own poses and parent back, the list
 * on the planet the player is on, which the galaxy labels and the picture
 * shows, the scroll draw told it is the list's first frame (the map list's
 * enter does the same, 0x14B54C).
 */
static void map_arrive(u32 item)
{
	u32 cur = r32(R1_CURRENT_PLANET);

	map_poses(PAGE_LIST, R1_MAIN_POSE0, MH_MAIN_ROWS);
	w32(PAGE_LIST + PG_PARENT, r32(SH(SH_PICK_PARENT)));
	w32(SH(SH_AT_PAGE), PAGE_MAP);
	w8(SH(SH_PICK), PK_UP);
	w8(SH(SH_PHASE), PH_IDLE);
	if (cur >= R1_PLANETS) cur = 0;
	w32(item + LI_CURSOR, cur);
	w32(item + LI_SCROLL, 0);
	w32(item + IT_FLAGS, R1_MAP_LIST_FLAGS | R1_LIST_FIRST);
	w32(R1_SELECTED, cur);
}

/*
 * The map's panes flying out after Triangle, and then the game's transition
 * back to the MAIN page as the map page's parent, on the main page's poses
 * backwards: the MAIN panes fly in, the seventh drawn for a page of seven.
 */
static void map_going(void)
{
	u32 t = r8(SH(SH_TIMER));

	if (t > 1) {
		w8(SH(SH_TIMER), t - 1);
		return;
	}
	map_poses(PAGE_MAP, R1_MAIN_POSE0, MH_MAIN_ROWS);
	w32(PAGE_MAP + PG_PARENT, PAGE_LIST);
	w32(R1_SELECTED, r32(SH(SH_PICK_SEL)));
	r1_goodies_pane(r8(SH(SH_NROWS)) >= MH_MAIN_ROWS);
	w32(R1_MENU_NEXT, PAGE_LIST);
	w8(SH(SH_PHASE), PH_WAIT);
}

/*
 * The map list, every frame the picker's page is current, as the map list's
 * generic update reads the pad (0x141408 with flags 0x138: the pad's edges,
 * Up and Down and L1 and R1 moving the cursor without wrapping, the cursor's
 * tick, the selected planet following the cursor), but for the keys the map
 * gives to travel: the cross picks (mh_core_pick), Triangle goes back to the
 * Trainer menu, Start, Select and R3 close, and so does the combo's close
 * request (mh_core_picker_gone).
 */
MH_API int mh_pick_update(u32 item)
{
	u32 e, cur, moby;

	if (r32(MH_STATE + ST_MAGIC) != MH_MAGIC || r32(R1_MENU_PAGE) != PAGE_MAP) return 0;
	if (r32(SH(SH_AT_PAGE)) != PAGE_MAP) map_arrive(item);
	if (r1_closing()) return 1;
	if (r1_phase() == PH_OUT) {
		map_going();
		return 0;
	}
	if (r1_phase() != PH_IDLE || r32(R1_MENU_CLOSE) != 0) return 0;

	if (r32(PAGE_MAP + PG_FOCUS) != item) return 0;

	e = r32(R1_PAD_EDGES);
	moby = r32(item + IT_MOBY);
	cur = r32(item + LI_CURSOR);
	if (mh_core_close_request() || ((e & PADB_CLOSE) != 0 && r32(R1_UNCLOSABLE) == 0)) {
		mh_core_picker_gone(0);
		return 1;
	}
	if ((e & PADB_TRIANGLE) != 0) {
		r1_sound(SND_PAGE, moby);
		w8(SH(SH_PICK), PK_RETURN);
		r1_panes(PH_OUT, 0, R1_MAP_PANES, MOVE_OUT, R1_MAP_POSE0);
		return 0;
	}
	if ((e & r32(R1_CONFIRM_MASK)) != 0 && cur < R1_PLANETS) {
		r1_sound(SND_SELECT, moby);
		w32(SH(SH_PICK_LAST), cur);
		map_count(SH(SH_PICK_POSTS));
		mh_core_pick(0, (s32)cur);
		return 1;
	}
	if ((e & (PADB_UP | PADB_L1)) != 0 && cur > 0) {
		cur--;
	} else if ((e & (PADB_DOWN | PADB_R1)) != 0 && cur + 1 < R1_PLANETS) {
		cur++;
	} else {
		return 0;
	}
	r1_sound(SND_CURSOR, moby);
	w32(item + LI_CURSOR, cur);
	w32(R1_SELECTED, cur);
	return 0;
}

/*
 * The MAIN page arrived (r1_arrive, after main_start): if it is the picker's
 * way back, the picker down for the core (mh_core_picker_back: the screen
 * never stopped), the map page's own poses back, the focus on the row the
 * picker was opened from, and the panes the page does not use sent away (the
 * transition flew all seven in). Answers 1 then.
 */
R1_FN int r1_pick_returned(u32 n)
{
	u32 row = r8(SH(SH_PICK_ROW));

	if (r8(SH(SH_PICK)) != PK_RETURN) return 0;
	w8(SH(SH_PICK), PK_NONE);
	mh_core_picker_back(0);
	map_poses(PAGE_MAP, R1_MAP_POSE0, R1_MAP_PANES);
	w32(PAGE_MAP + PG_PARENT, 0);
	if (row < n && r1_enabled(row)) w32(PAGE_LIST + PG_FOCUS, ROW(row));
	r1_panes(PH_IN, n, MH_MAIN_ROWS, MOVE_OUT, R1_MAIN_POSE0);
	return 1;
}

/*
 * In play with the menu down (mh_play, before the core's screen stops):
 * whatever way the pause closed, the picker down for the core (a close, if
 * nothing has said so yet), the pages' poses and parents as setup left them
 * and the map's selected planet as the game had it.
 */
R1_FN void r1_pick_reset(void)
{
	if (mh_picker_up()) mh_core_picker_gone(0);
	if (r8(SH(SH_PICK)) == PK_NONE) return;
	map_poses(PAGE_LIST, R1_MAIN_POSE0, MH_MAIN_ROWS);
	map_poses(PAGE_MAP, R1_MAP_POSE0, R1_MAP_PANES);
	w32(PAGE_MAP + PG_PARENT, 0);
	w32(R1_SELECTED, r32(SH(SH_PICK_SEL)));
	w8(SH(SH_PICK), PK_NONE);
}
