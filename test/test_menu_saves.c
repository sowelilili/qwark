/*
 * Revision 1.34, build 59: the Trainer's Save files page lists a category by
 * planet, the order RaCMAN Reloaded's Save files panel calls "By planet"
 * (SaveFilePlanet.cs, SaveFilePlanetOrder, SaveFileOrder in the client).
 *
 *   natural order  qstrnatcmp against the client's CompareNatural cases;
 *   the order      for every game, the same saves, names and levels as the
 *                  client's SaveFileOrderTests and the order they expect,
 *                  written into the console's library and sorted by
 *                  savefile_menu_sort with the game's own menu_save_order and
 *                  menu.c's ranks, so the two orders cannot drift apart
 *                  silently: a change to either side's ranks, moves or level
 *                  check fails here or there;
 *   the page       the order on the Trainer's own Save files page, at once for
 *                  a short category and a few saves a tick for a long one, the
 *                  page built again as a new visit when it is done.
 *
 * The saves are made here, not copied from the client's premade ones: a head
 * of the game's own block sizes with field 0 where the game writes it, which
 * is all either side reads (the client edits the level word of a real save
 * the same way).
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/savefile.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/game.h"
#include "../src/games/tod.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot_as(const char *title, const char *which)
{
	int i;

	host_boot_as(title, which);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME && session_game() != NULL) return 1;
	}
	return 0;
}

static int quit(void)
{
	int i;

	host_quit();
	for (i = 0; i < 2000; i++) {
		tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

/* ------------------------------------------------------- the library */

static void lib_path(char *out, u32 cap, const char *title, const char *cat, const char *name)
{
	qstrcpy(out, cap, QWARK_SAVEDIR "/");
	qstrcat(out, cap, title);
	if (cat == NULL) return;
	qstrcat(out, cap, "/");
	qstrcat(out, cap, cat);
	if (name == NULL) return;
	qstrcat(out, cap, "/");
	qstrcat(out, cap, name);
}

void lib_save(const char *title, const char *cat, const char *name, const u8 *bytes, u32 len)
{
	static u8 fill[4096];
	char path[256];
	plat_file_t f;
	u32 done = 0, n;

	plat_dir_create(QWARK_SAVEDIR);
	lib_path(path, sizeof(path), title, NULL, NULL);
	plat_dir_create(path);
	lib_path(path, sizeof(path), title, cat, NULL);
	plat_dir_create(path);
	lib_path(path, sizeof(path), title, cat, name);
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	if (bytes != NULL) {
		plat_file_write(f, bytes, len);
	} else {
		memset(fill, 0x5A, sizeof(fill));
		while (done < len) {
			n = len - done < sizeof(fill) ? len - done : (u32)sizeof(fill);
			plat_file_write(f, fill, n);
			done += n;
		}
	}
	plat_file_close(f);
}

void lib_drop(const char *title, const char *cat, const char *name)
{
	char path[256];

	lib_path(path, sizeof(path), title, cat, name);
	if (name == NULL) {
		plat_dir_remove(path);
		return;
	}
	plat_file_unlink(path);
	qstrcat(path, sizeof(path), SAVEFILE_SUM_EXT);
	plat_file_unlink(path);
}

/* ------------------------------------------------------- the saves */

/* How a test save is made. */
#define SV_HEAD   0      /* the game's own head, on `level` */
#define SV_EMPTY  1      /* an empty file */
#define SV_ZEROS  2      /* sixteen zero bytes: too short for any head */
#define SV_OTHER  3      /* a head of another game's block sizes (UYA's), on `level` */
#define SV_TOD    4      /* a whole ToD GAME.SAV on `level` */

struct sv {
	const char *name;    /* without the .sav */
	s32 level;
	u8 kind;
};

/* The block sizes of each original game's own save, as its restore takes them (the client's Heads). */
static void head_sizes(u8 game_id, u32 *global, u32 *planet)
{
	switch (game_id) {
	case GAME_RAC1: *global = 0x153C; *planet = 0x82A4; return;
	case GAME_RAC2: *global = 0x1D08; *planet = 0x84D0; return;
	case GAME_RAC3: *global = 0x37B0; *planet = 0x84C0; return;
	default:        *global = 0x41F0; *planet = 0x031C; return;
	}
}

static u8 g_tod[TOD_SAVE_SIZE];

static void put_save(const char *title, u8 game_id, const char *cat, const struct sv *s)
{
	char name[40];
	u8 head[0x1C];
	u32 global, planet;

	qstrcpy(name, sizeof(name), s->name);
	qstrcat(name, sizeof(name), SAVEFILE_EXT);
	switch (s->kind) {
	case SV_EMPTY:
		lib_save(title, cat, name, head, 0);
		return;
	case SV_ZEROS:
		memset(head, 0, sizeof(head));
		lib_save(title, cat, name, head, 16);
		return;
	case SV_TOD:
		memset(g_tod, 0, sizeof(g_tod));
		be32_put(g_tod + TOD_SAVE_LEVEL_OFF, (u32)s->level);
		lib_save(title, cat, name, g_tod, sizeof(g_tod));
		return;
	default:
		head_sizes(s->kind == SV_OTHER ? GAME_RAC3 : game_id, &global, &planet);
		memset(head, 0, sizeof(head));
		be32_put(head, global);
		be32_put(head + 4, planet);
		be32_put(head + 8, global - 8);
		be32_put(head + 0x14, 4);
		be32_put(head + 0x18, (u32)s->level);
		lib_save(title, cat, name, head, sizeof(head));
		return;
	}
}

static void drop_saves(const char *title, const char *cat, const struct sv *s, int n)
{
	char name[40];
	int i;

	for (i = 0; i < n; i++) {
		qstrcpy(name, sizeof(name), s[i].name);
		qstrcat(name, sizeof(name), SAVEFILE_EXT);
		lib_drop(title, cat, name);
	}
	lib_drop(title, cat, NULL);
}

/*
 * The category sorted as the Trainer sorts it (savefile_menu_sort, with the
 * running game's menu_save_order and menu.c's ranks), a step at a time until
 * it is done, then listed; 1 when the names come out as `want`.
 */
static int sorted_as(const char *cat, const char * const *want, int nwant, int *steps)
{
	const struct game_api *g = session_game();
	const struct menu_save_order *so = g != NULL && g->menu != NULL ? g->menu->save_order : NULL;
	static char names[255 * (SAVEFILE_NAME_LEN + 1)];
	char *dot;
	u32 sig;
	u8 n, i;

	*steps = 0;
	if (so == NULL) return 0;
	savefile_menu_sort_forget();
	while (*steps < 64 && savefile_menu_sort(cat, so->at, so->len, so->level, menu_test_save_rank, 16) == 0) (*steps)++;
	n = savefile_menu_list(cat, 0, 255, names, &sig);
	if (!savefile_menu_sorted(sig) || n != nwant) return 0;
	for (i = 0; i < n; i++) {
		char *nm = names + (u32)i * (SAVEFILE_NAME_LEN + 1);
		dot = strstr(nm, SAVEFILE_EXT);
		if (dot != NULL) *dot = 0;
		if (strcmp(nm, want[i]) != 0) {
			printf("    row %u: %s, want %s\n", (unsigned)i, nm, want[i]);
			return 0;
		}
	}
	return 1;
}

static void order_case(const char *title, const char *which, u8 game_id, const char *what,
                       const struct sv *s, int n, const char * const *want, int nwant)
{
	char msg[200];
	int i, steps = 0;

	snprintf(msg, sizeof(msg), "%s boots", what);
	check(boot_as(title, which), msg);
	for (i = 0; i < n; i++) put_save(title, game_id, "story", &s[i]);
	snprintf(msg, sizeof(msg), "%s: the client's SaveFileOrderTests order, by planet", what);
	check(sorted_as("story", want, nwant, &steps), msg);
	if (n > 16) {
		snprintf(msg, sizeof(msg), "%s: %d saves, read sixteen a step at most (%d steps before the sort)", what, n, steps);
		check(steps == (n - 1) / 16, msg);
	}
	drop_saves(title, "story", s, n);
	quit();
}

/* ALibraryIsInLevelOrderByWhatEachSaveSaysWithItsRevisitLastAndUnknownsBelow. */
static const struct sv g_rac1[] = {
	{ "Kerwan 10", 3, SV_HEAD }, { "kerwan 9", 3, SV_HEAD }, { "Veldin start", 18, SV_HEAD },
	{ "novalis", 1, SV_HEAD }, { "a fresh file", 0, SV_HEAD }, { "beyond", 19, SV_HEAD },
	{ "uya save", 1, SV_OTHER }, { "notes 2", 0, SV_ZEROS }, { "Aridia 1", 0, SV_EMPTY }
};
static const char * const g_rac1_want[] = {
	"a fresh file", "novalis", "kerwan 9", "Kerwan 10", "Veldin start", "Aridia 1", "beyond", "notes 2", "uya save"
};

/* ToDsFastoonIIGoesAfterEveryOtherPlanet. */
static const struct sv g_tods[] = {
	{ "Fastoon II", 18, SV_TOD }, { "Rykan", 9, SV_TOD }, { "Fastoon", 3, SV_TOD }, { "Kerwan", 0, SV_TOD },
	{ "level 19", 19, SV_TOD }
};
static const char * const g_tod_want[] = { "Kerwan", "Fastoon", "Rykan", "Fastoon II", "level 19" };

/* UyaSavesAreInThePickersOrderWithEachRangerLevelAfterItsPlanet. */
static const struct sv g_uya[] = {
	{ "Vid Comic 1 SE", 36, SV_HEAD }, { "Vid Comic 5", 35, SV_HEAD }, { "Vid Comic 3", 34, SV_HEAD },
	{ "Vid Comic 2", 33, SV_HEAD }, { "Vid Comic 4", 32, SV_HEAD }, { "Vid Comic 1", 31, SV_HEAD },
	{ "Vid Comic 6", 30, SV_HEAD }, { "Tyhrranosis Rangers", 29, SV_HEAD }, { "Aquatos Sewers", 28, SV_HEAD },
	{ "Aquatos Clank", 27, SV_HEAD }, { "Metropolis Rangers", 26, SV_HEAD }, { "Insomniac Museum", 24, SV_HEAD },
	{ "Holostar Clank", 23, SV_HEAD }, { "Command Center", 22, SV_HEAD }, { "Obani Draco", 21, SV_HEAD },
	{ "Launch Site", 20, SV_HEAD }, { "Qwark's Hideout", 19, SV_HEAD }, { "Aridia", 18, SV_HEAD },
	{ "Crash Site", 17, SV_HEAD }, { "Metropolis", 16, SV_HEAD }, { "Koros", 14, SV_HEAD },
	{ "Holostar", 13, SV_HEAD }, { "Blackwater City", 12, SV_HEAD }, { "Obani Gemini", 11, SV_HEAD },
	{ "Zeldrin Starport", 10, SV_HEAD }, { "Tyhrranosis", 9, SV_HEAD }, { "Aquatos", 8, SV_HEAD },
	{ "aquatos 2", 8, SV_HEAD }, { "Annihilation Nation", 7, SV_HEAD }, { "Phoenix Rescue", 6, SV_HEAD },
	{ "Daxx", 5, SV_HEAD }, { "Marcadia", 4, SV_HEAD }, { "Starship Phoenix", 3, SV_HEAD },
	{ "Florana", 2, SV_HEAD }, { "Veldin", 1, SV_HEAD }, { "at the menu", 0, SV_HEAD }, { "gap 15", 15, SV_HEAD }
};
static const char * const g_uya_want[] = {
	"Veldin", "Florana", "Starship Phoenix", "Marcadia", "Daxx", "Obani Gemini", "Blackwater City",
	"Annihilation Nation", "Aquatos", "aquatos 2", "Aquatos Clank", "Aquatos Sewers",
	"Tyhrranosis", "Tyhrranosis Rangers", "Zeldrin Starport", "Holostar", "Holostar Clank", "Obani Draco",
	"Metropolis", "Metropolis Rangers", "Crash Site", "Aridia", "Qwark's Hideout",
	"Phoenix Rescue", "Koros", "Command Center", "Launch Site", "Insomniac Museum",
	"Vid Comic 1", "Vid Comic 2", "Vid Comic 3", "Vid Comic 4", "Vid Comic 5", "Vid Comic 6", "Vid Comic 1 SE",
	"at the menu", "gap 15"
};

/* RaC2SavesFollowThePickerWithEachOrbitAfterItsPlanet. */
static const struct sv g_rac2[] = {
	{ "Jamming Array", 26, SV_HEAD }, { "Wupash", 25, SV_HEAD }, { "Slim Cognito", 24, SV_HEAD },
	{ "Damosel Orbit", 23, SV_HEAD }, { "Dobbo Orbit", 22, SV_HEAD }, { "Insomniac Museum", 21, SV_HEAD },
	{ "Yeedil", 20, SV_HEAD }, { "Grelbin", 19, SV_HEAD }, { "Damosel", 18, SV_HEAD }, { "Smolg", 17, SV_HEAD },
	{ "Snivelak", 16, SV_HEAD }, { "Gorn", 15, SV_HEAD }, { "Aranos 2", 14, SV_HEAD }, { "Boldan", 13, SV_HEAD },
	{ "Todano", 12, SV_HEAD }, { "Joba", 11, SV_HEAD }, { "Hrugis", 10, SV_HEAD }, { "Dobbo", 9, SV_HEAD },
	{ "Tabora", 8, SV_HEAD }, { "Siberius", 7, SV_HEAD }, { "Notak", 6, SV_HEAD }, { "Feltzin", 5, SV_HEAD },
	{ "Barlow", 4, SV_HEAD }, { "Endako", 3, SV_HEAD }, { "Maktar", 2, SV_HEAD }, { "Oozla", 1, SV_HEAD },
	{ "Aranos", 0, SV_HEAD }, { "level 27", 27, SV_HEAD }
};
static const char * const g_rac2_want[] = {
	"Aranos", "Oozla", "Wupash", "Maktar", "Jamming Array", "Endako", "Barlow", "Feltzin", "Notak",
	"Siberius", "Tabora", "Dobbo", "Dobbo Orbit", "Hrugis", "Joba", "Todano", "Boldan", "Aranos 2",
	"Gorn", "Snivelak", "Smolg", "Damosel", "Damosel Orbit", "Grelbin", "Yeedil", "Insomniac Museum",
	"Slim Cognito", "level 27"
};

/* DeadlockedSavesAreInThePickersOrderWithOrxonBeforeValix. */
static const struct sv g_dl[] = {
	{ "Interior", 15, SV_HEAD }, { "Ghost Station", 14, SV_HEAD }, { "Maraxus", 13, SV_HEAD },
	{ "Stygia", 11, SV_HEAD }, { "Torval", 10, SV_HEAD }, { "Orxon", 8, SV_HEAD }, { "Valix", 7, SV_HEAD },
	{ "Shaar", 6, SV_HEAD }, { "Kronos", 5, SV_HEAD }, { "Sarathos", 4, SV_HEAD }, { "Catacrom", 2, SV_HEAD },
	{ "DreadZone", 1, SV_HEAD }, { "a loop", 3, SV_HEAD }, { "id 0", 0, SV_HEAD }
};
static const char * const g_dl_want[] = {
	"DreadZone", "Catacrom", "Sarathos", "Kronos", "Shaar", "Orxon", "Valix", "Torval", "Stygia",
	"Maraxus", "Ghost Station", "Interior", "a loop", "id 0"
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------- the levels */

/* What each game's level check takes and refuses, beside the orders. */
static void test_levels(void)
{
	const struct game_api *g;
	const struct menu_save_order *so;
	u8 head[0x1C];

	group("save files by planet: what a save's head says");
	check(boot_as("NPEA00386", "rac2"), "RaC2 boots");
	g = session_game();
	so = g != NULL && g->menu != NULL ? g->menu->save_order : NULL;
	check(so != NULL && so->at == 0 && so->len == 0x1C, "RaC2 reads the head of a save, 0x1C bytes from its start");
	if (so != NULL) {
		memset(head, 0, sizeof(head));
		be32_put(head, 0x1CF8);
		be32_put(head + 4, 0x0CD0);
		be32_put(head + 8, 0x1CF8 - 8);
		be32_put(head + 0x14, 4);
		be32_put(head + 0x18, 14);
		check(so->level(head, 0x20000) == 14, "the older block sizes the game's restore still takes: a level");
		be32_put(head + 0x10, 1);
		check(so->level(head, 0x20000) == -1, "field 0's tag not where it belongs: no level");
		be32_put(head + 0x10, 0);
		be32_put(head + 8, 0);
		check(so->level(head, 0x20000) == -1, "the global block's length not its size less 8: no level");
		be32_put(head + 8, 0x1CF8 - 8);
		check(so->level(head, 0x18) == -1, "a file cut off before the level: none");
		be32_put(head + 0x18, 0xFFFFFFFFu);
		check(so->level(head, 0x20000) == -1, "a negative level: none");
	}
	quit();

	check(boot_as(TOD_TITLE, "tod"), "ToD boots");
	g = session_game();
	so = g != NULL && g->menu != NULL ? g->menu->save_order : NULL;
	check(so != NULL && so->at == TOD_SAVE_LEVEL_OFF && so->len == 4,
	      "ToD reads the four bytes at 0x906E8, the end of its GAME.SAV, never the whole file");
	if (so != NULL) {
		be32_put(head, 18);
		check(so->level(head, TOD_SAVE_SIZE) == 18 && so->level(head, TOD_SAVE_SIZE + 4) == -1 &&
		      so->level(head, 0x1000) == -1, "a level in a whole save only, of any other size none");
	}
	quit();
}

/* ------------------------------------------------------- the page */

/*
 * The order on the Trainer's own page, in Deadlocked: a short category in its
 * order as it opens; a long one in the folder's order until the steps are
 * done, then a new visit of the page in the order by planet.
 */
static void test_page(void)
{
	static const struct sv shrt[] = {
		{ "zz Shaar", 6, SV_HEAD }, { "aa Ghost", 14, SV_HEAD }, { "mm Catacrom", 2, SV_HEAD }, { "junk", 0, SV_ZEROS }
	};
	static struct sv lng[24];
	static char names[24][16];
	struct menu_view v;
	u32 key;
	int i, ok;

	group("save files by planet: the Trainer's Save files page");
	check(boot_as("NPEA00423", "dl"), "Deadlocked boots");
	for (i = 0; i < N(shrt); i++) put_save("NPEA00423", GAME_RAC4, "short", &shrt[i]);

	menu_model_reset(8, 16, MBOX_CAP_STEPS | MBOX_CAP_BACK | MBOX_CAP_REQUEST);
	menu_model_view(&v);
	for (i = 0; i < v.nrows && strcmp(v.rows[i].label, "Save files") != 0; i++) { }
	menu_model_event(MBOX_EV_ACTIVATE, (u8)i, v.rows[i].id, v.key);
	menu_model_view(&v);
	for (i = 0; i < v.nrows && strcmp(v.rows[i].label, "short") != 0; i++) { }
	check(i < v.nrows && menu_model_event(MBOX_EV_ACTIVATE, (u8)i, v.rows[i].id, v.key) == ST_OK, "the category opens");
	menu_model_view(&v);
	check(v.nrows == 4 && strcmp(v.rows[0].label, "mm Catacrom") == 0 && strcmp(v.rows[1].label, "zz Shaar") == 0 &&
	      strcmp(v.rows[2].label, "aa Ghost") == 0 && strcmp(v.rows[3].label, "junk") == 0,
	      "a short category is in its order as it opens: Catacrom, Shaar, Ghost Station, then the save with no planet");
	drop_saves("NPEA00423", "short", shrt, N(shrt));

	/* 24 saves, named against the order: z23 on DreadZone .. z00 on Interior, so the folder's order is the reverse. */
	for (i = 0; i < 24; i++) {
		static const s32 lv[12] = { 15, 14, 13, 11, 10, 7, 8, 6, 5, 4, 2, 1 };
		snprintf(names[i], sizeof(names[i]), "z%02d", i);
		lng[i].name = names[i];
		lng[i].level = lv[i / 2];
		lng[i].kind = SV_HEAD;
		put_save("NPEA00423", GAME_RAC4, "long", &lng[i]);
	}
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	for (i = 0; i < v.nrows && strcmp(v.rows[i].label, "long") != 0; i++) { }
	menu_model_event(MBOX_EV_ACTIVATE, (u8)i, v.rows[i].id, v.key);
	menu_model_view(&v);
	key = v.key;
	check(strcmp(v.rows[0].label, "z00") == 0, "a long category opens in the folder's order, sixteen of its 24 read");
	/* What the driver does a tick later (menu.c serve): the rest read, the sort, a new visit. */
	{
		const struct menu_save_order *so = session_game()->menu->save_order;
		check(savefile_menu_sort("long", so->at, so->len, so->level, menu_test_save_rank, 16) == 1,
		      "the next step reads the other eight and sorts it");
	}
	menu_model_view(&v);
	ok = strcmp(v.rows[0].label, "z22") == 0 && strcmp(v.rows[1].label, "z23") == 0 &&
	     strcmp(v.rows[2].label, "z20") == 0 && strcmp(v.rows[10].label, "z12") == 0 && strcmp(v.rows[11].label, "z13") == 0;
	check(ok, "then DreadZone's two first, natural order within a planet, Orxon (z12, z13) before Valix (z10, z11)");
	(void)key;
	for (i = 0; i < 24; i++) {
		char nm[16];
		snprintf(nm, sizeof(nm), "%s.sav", names[i]);
		lib_drop("NPEA00423", "long", nm);
	}
	lib_drop("NPEA00423", "long", NULL);
	quit();
}

void test_menu_saves(void)
{
	group("save files by planet: natural order, as the client's CompareNatural");
	check(qstrnatcmp("Kerwan 9", "kerwan 10") < 0, "Kerwan 9 before kerwan 10: numbers as numbers, case aside");
	check(qstrnatcmp("Kerwan 010", "Kerwan 9") > 0, "Kerwan 010 after Kerwan 9: leading zeros dropped");
	check(qstrnatcmp("Kerwan", "Kerwan 2") < 0, "Kerwan before Kerwan 2: the shorter first");
	check(qstrnatcmp("kerwan b", "Kerwan A") > 0, "kerwan b after Kerwan A: letters folded to lower case");
	check(qstrnatcmp("Kerwan", "kerwan") != 0 && qstrnatcmp("Kerwan", "kerwan") == -qstrnatcmp("kerwan", "Kerwan"),
	      "Kerwan and kerwan still in one fixed order");
	check(qstrnatcmp("Save 9", "save 10") < 0 && qstrnatcmp("Aridia", "kerwan") < 0 && qstrnatcmp("kerwan", "Save 9") < 0,
	      "AlphabeticalIsNaturalOrderWhateverTheSavesSay: Aridia, kerwan, Save 9, save 10");
	check(qstrnatcmp("a 01", "a 1") != 0 && qstrnatcmp("a 01", "a 1") < 0, "equal numbers, different zeros: byte order");

	group("save files by planet: every game in the client's order (SaveFileOrderTests)");
	order_case("NPEA00385", "rac1", GAME_RAC1, "RaC1", g_rac1, N(g_rac1), g_rac1_want, N(g_rac1_want));
	order_case(TOD_TITLE, "tod", GAME_TOD, "ToD", g_tods, N(g_tods), g_tod_want, N(g_tod_want));
	order_case("NPEA00387", "rac3", GAME_RAC3, "UYA", g_uya, N(g_uya), g_uya_want, N(g_uya_want));
	order_case("NPEA00386", "rac2", GAME_RAC2, "RaC2", g_rac2, N(g_rac2), g_rac2_want, N(g_rac2_want));
	order_case("NPEA00423", "dl", GAME_RAC4, "Deadlocked", g_dl, N(g_dl), g_dl_want, N(g_dl_want));

	test_levels();
	test_page();
}
