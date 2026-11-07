/*
 * Build 56, a preview feature: the console menu's Mods page (src/core/menu.c,
 * PROTOCOL.md 9.1), on RaC1 on the fake console, at the model: the root's
 * Mods row, the page's rows (the installed mods and those only a USB drive
 * has), Off / On / On boot through MOD_LOAD's, MOD_UNLOAD's and
 * MOD_SET_AUTO's code, and the rules a load from the page keeps
 * (mods_load_guarded, mem_owner): a missing dependency, a parse error, a mod
 * that would write over a loaded mod's cave or word or over qwark's own code
 * (a feature's patch, the savefile helper), a dependency taken back when the
 * load fails, the refusal's notification; a USB-only mod copied in; a page
 * longer than a window.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/mem.h"
#include "../src/core/mods.h"
#include "../src/core/modusb.h"
#include "../src/core/savefile.h"
#include "../src/core/menu.h"
#include "../src/games/sfhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define T    "NPEA00385"
#define HDD  "/dev_hdd0/qwark/mods/" T
#define USB0 "/dev_usb000/qwark/mods/" T

#define CAPS_ALL (MBOX_CAP_STEPS | MBOX_CAP_PAGE_KEYS | MBOX_CAP_BACK | MBOX_CAP_REQUEST)

static void tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int boot(const char *title)
{
	int i;

	host_boot(title);
	for (i = 0; i < 4000; i++) {
		tick(1);
		if (session_state() == SESSION_INGAME) return 1;
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

static u32 peek32(u32 a)
{
	u8 b[4] = { 0, 0, 0, 0 };
	host_peek(a, b, 4);
	return be32_get(b);
}

static void poke32(u32 a, u32 v)
{
	u8 b[4];
	be32_put(b, v);
	host_poke(a, b, 4);
}

static void put(const char *folder, const char *leaf, const char *text)
{
	char path[256];
	plat_file_t f;

	snprintf(path, sizeof(path), "%s/%s", folder, leaf);
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, (u32)strlen(text));
	plat_file_close(f);
}

static void mod(const char *root, const char *dir, const char *patch)
{
	char folder[256];
	snprintf(folder, sizeof(folder), "%s/%s", root, dir);
	put(folder, "patch.txt", patch);
}

/* Test code, so plain recursion: the trees here are two levels deep. */
static void rm_tree(const char *path)
{
	plat_dir_t d;
	struct plat_dirent e;
	int is_dir = 0;

	if (!plat_path_exists(path, &is_dir, NULL)) return;
	if (!is_dir) { plat_file_unlink(path); return; }
	if (plat_dir_open(path, &d) == 0) {
		while (plat_dir_next(&d, &e) == 1) {
			char child[512];
			if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
			snprintf(child, sizeof(child), "%s/%s", path, e.name);
			rm_tree(child);
		}
		plat_dir_close(&d);
	}
	plat_dir_remove(path);
}

static const char *const g_dirs[] = {
	"mm-a", "mm-b", "mm-c", "mm-ch", "mm-n", "mm-m", "mm-bad", "mm-f", "mm-h", "mm-usb", "mm-usb2",
	"mm-lo", "mm-up", "mm-rta",
};

static void clean(void)
{
	char p[256];
	u32 i;

	for (i = 0; i < sizeof(g_dirs) / sizeof(g_dirs[0]); i++) {
		snprintf(p, sizeof(p), "%s/%s", HDD, g_dirs[i]);
		rm_tree(p);
		config_set_mod_auto(T, g_dirs[i], 0);
	}
	for (i = 0; i < 20; i++) {
		snprintf(p, sizeof(p), "%s/mm-p%02u", HDD, (unsigned)i);
		rm_tree(p);
	}
	rm_tree("/dev_usb000");
	rm_tree("/dev_hdd0/qwark/usbtmp");
}

static const struct mod_entry *hdd(const char *dir)
{
	int i = mods_find(dir);
	return i >= 0 ? mods_at((u32)i) : NULL;
}

static int loaded(const char *dir)
{
	const struct mod_entry *m = hdd(dir);
	return m != NULL && (m->flags & MOD_FLAG_LOADED) != 0;
}

/* The installed mods, holes in the table aside. */
static u32 used_mods(void)
{
	u32 i, n = 0;
	for (i = 0; i < mods_count(); i++) n += mods_at(i) != NULL;
	return n;
}

static int view_index(const struct menu_view *v, const char *label)
{
	u8 i;
	for (i = 0; i < v->nrows; i++) {
		if (strcmp(v->rows[i].label, label) == 0) return i;
	}
	return -1;
}

static const char *value_of(const struct menu_view *v, const char *label)
{
	int i = view_index(v, label);
	return i >= 0 ? v->rows[i].value : "(no row)";
}

/* An event on the row labelled `label` of the view `v` was built as; the view built again. */
static int act(struct menu_view *v, u8 kind, const char *label)
{
	int i = view_index(v, label);
	int rc;

	if (i < 0) return -1;
	rc = menu_model_event(kind, (u8)i, v->rows[i].id, v->key);
	menu_model_view(v);
	return rc;
}

static int notified(const char *text)
{
	return strcmp(host_notify_last(), text) == 0;
}

static void seed(void)
{
	char big[17100];
	char folder[256];
	u32 i;

	mod(HDD, "mm-a", "#- name: Alpha\n0x00870000: 0x60000000\n");
	mod(HDD, "mm-b", "#- name: Beta\n0x00871000: beta.bin\n0x00871800: 0x48000000\n");
	put(HDD "/mm-b", "beta.bin", "BBBBBBBBBBBBBBBB");
	/* A cave half over Beta's, and a word of its own clear of everything. */
	mod(HDD, "mm-c", "#- name: Clash\n0x00871008: clash.bin\n0x00872000: 0x60000000\n");
	put(HDD "/mm-c", "clash.bin", "CCCCCCCCCCCCCCCC");
	/* Alpha first, then a word on Beta's word. */
	mod(HDD, "mm-ch", "#- name: Chain\n#- depends: Alpha\n0x00871800: 0x60000000\n");
	mod(HDD, "mm-n", "#- name: Needy\n#- depends: Alpha\n0x00873000: 0x60000000\n");
	mod(HDD, "mm-m", "#- name: Missing\n#- depends: Nobody\n0x00874000: 0x60000000\n");
	/* Over 16 KB: qwark cannot read it, a parse error, named by its folder. */
	memset(big, '#', sizeof(big) - 1);
	for (i = 64; i < sizeof(big) - 1; i += 64) big[i] = '\n';
	big[sizeof(big) - 1] = 0;
	snprintf(folder, sizeof(folder), "%s/mm-bad", HDD);
	put(folder, "patch.txt", big);
	/* A word on RaC1's own fast-load patch (rac1.c, rac1_fastload_words). */
	mod(HDD, "mm-f", "#- name: Faster\n0x00165490: 0x60000000\n");
	mod(USB0, "mm-usb", "#- name: Usbie\n#- version: 1.0\n0x00875000: 0x60000000\n");
	mod(USB0, "mm-usb2", "#- name: Usbie Two\n#- version: 1.0\n0x00876000: 0x60000000\n");
}

static void test_page(void)
{
	struct menu_view v;
	const struct menu_row *r;
	int i, rc, usb_at;
	u32 mask;

	group("console menu: the Mods page (build 56, a preview feature)");

	menu_model_reset(7, 24, CAPS_ALL);
	menu_model_view(&v);
	i = view_index(&v, "Mods");
	check(i == 5 && v.nrows == 7 && strcmp(v.rows[6].label, "Save files") == 0 && v.rows[i].kind == MROW_LINK &&
	      v.rows[i].flags == MROWF_ENABLED,
	      "the root: Mods, a link, between Unlocks and Save files");

	/* A drive put in and a mod copied in by FTP since the boot: the page reads both as it opens. */
	seed();
	tick(130);                                /* the boot's look at the drives over a second old */
	check(hdd("mm-a") == NULL, "qwark has not read the new folders yet");
	act(&v, MBOX_EV_ACTIVATE, "Mods");
	check(menu_model_page() == MENU_PAGE_ID(MENU_PAGE_MODS, 0) && strcmp(v.title, "Mods") == 0 &&
	      v.ptype == MBOX_PTYPE_CONFIG, "the page: Mods, a CONFIG page");
	check(hdd("mm-a") != NULL && modusb_find("mm-usb") >= 0,
	      "opening it read the console's library and its USB drive again");
	check(v.nrows == (u8)(used_mods() + 2) && view_index(&v, "Alpha") >= 0 && view_index(&v, "IL Ghost") < 0 &&
	      view_index(&v, "Usbie") > view_index(&v, "Alpha") && view_index(&v, "Usbie Two") >= 0,
	      "a row for every installed mod (by its #- name), then each only the USB drive has");
	check(view_index(&v, "mm-bad") >= 0, "a mod qwark cannot parse is listed by its folder name");
	r = &v.rows[view_index(&v, "Alpha")];
	check(r->kind == MROW_TOGGLE && (r->flags & MROWF_ENABLED) && (r->flags & MROWF_ON) == 0 &&
	      strcmp(r->value, "Off") == 0, "each a checkbox, Off");

	/* Off, On, On boot, and round again. */
	poke32(0x00870000u, 0x11111111u);
	rc = act(&v, MBOX_EV_ACTIVATE, "Alpha");
	check(rc == ST_OK && loaded("mm-a") && peek32(0x00870000u) == 0x60000000u &&
	      strcmp(value_of(&v, "Alpha"), "On") == 0 && (v.rows[view_index(&v, "Alpha")].flags & MROWF_ON),
	      "the cross: On, MOD_LOAD's load, its word in the game");
	mask = mods_loaded_mask();
	check((mask & (1u << (u32)mods_find("mm-a"))) != 0 && !config_mod_auto(T, "mm-a"),
	      "telemetry's mod_loaded has its bit; no auto flag");
	rc = act(&v, MBOX_EV_RIGHT, "Alpha");
	check(rc == ST_OK && loaded("mm-a") && config_mod_auto(T, "mm-a") &&
	      (mods_auto_mask() & (1u << (u32)mods_find("mm-a"))) != 0 && strcmp(value_of(&v, "Alpha"), "On boot") == 0,
	      "Right: On boot, the auto flag in config.txt as MOD_SET_AUTO keeps it, mod_auto's bit set");
	rc = act(&v, MBOX_EV_RIGHT, "Alpha");
	check(rc == ST_OK && !loaded("mm-a") && !config_mod_auto(T, "mm-a") && peek32(0x00870000u) == 0x11111111u &&
	      strcmp(value_of(&v, "Alpha"), "Off") == 0,
	      "Right again: Off, the game's word back and the auto flag cleared");
	rc = act(&v, MBOX_EV_LEFT, "Alpha");
	check(rc == ST_OK && loaded("mm-a") && config_mod_auto(T, "mm-a") && strcmp(value_of(&v, "Alpha"), "On boot") == 0,
	      "Left from Off: On boot, loaded and auto");
	rc = act(&v, MBOX_EV_LEFT, "Alpha");
	check(rc == ST_OK && loaded("mm-a") && !config_mod_auto(T, "mm-a") && strcmp(value_of(&v, "Alpha"), "On") == 0,
	      "Left: On, still loaded, the auto flag cleared");
	{
		u8 buf[1 + QWARK_MAX_MODS * MOD_WIRE_SIZE];
		u32 len = 0, k, idx = (u32)mods_find("mm-a");
		int ok = 0;
		mods_list_encode(buf, sizeof(buf), &len);
		for (k = 0; k < buf[0]; k++) {
			if (buf[1 + k * MOD_WIRE_SIZE] == idx) ok = (buf[2 + k * MOD_WIRE_SIZE] & (MOD_FLAG_LOADED | MOD_FLAG_AUTO)) == MOD_FLAG_LOADED;
		}
		check(ok, "MOD_LIST says LOADED, not AUTO, as the client's Mods panel reads it");
	}
	act(&v, MBOX_EV_LEFT, "Alpha");
	check(!loaded("mm-a"), "Left: Off");

	/*
	 * A config.txt with no room (a store bigger than the pool answers every
	 * write FULL and changes nothing): the auto flag is not pretended.
	 */
	{
		static char saved[16384];
		u32 saved_len = 0, k;
		plat_file_t f;
		check(qread_file(QWARK_CONFIG, saved, sizeof(saved) - 1, &saved_len) == ST_OK, "config.txt read aside");
		if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) == 0) {
			for (k = 0; k < 650u; k++) {
				char line[40];
				snprintf(line, sizeof(line), "fill%u = 1000000000\n", (unsigned)k);
				plat_file_write(f, line, (u32)strlen(line));
			}
			plat_file_close(f);
		}
		check(config_load() == ST_FULL, "a config.txt with no room left");
		rc = act(&v, MBOX_EV_ACTIVATE, "Alpha");
		check(rc == ST_OK && loaded("mm-a"), "Alpha On: a load writes no config");
		rc = act(&v, MBOX_EV_RIGHT, "Alpha");
		check(rc == ST_FULL && loaded("mm-a") && !config_mod_auto(T, "mm-a") && strcmp(value_of(&v, "Alpha"), "full") == 0 &&
		      notified("Trainer: Alpha: full"),
		      "On boot with no room: FULL, the row says so, a notification, and the mod stays On");
		menu_model_view(&v);
		check(strcmp(value_of(&v, "Alpha"), "full") == 0 && (hdd("mm-a")->flags & MOD_FLAG_AUTO) == 0,
		      "its auto flag not set, MOD_LIST's AUTO not either");
		check(mods_set_auto("mm-a", 1) == ST_FULL, "MOD_SET_AUTO answers FULL too, as the config write did");
		if (plat_file_open(QWARK_CONFIG, PLAT_OPEN_WRITE, &f) == 0) {
			plat_file_write(f, saved, saved_len);
			plat_file_close(f);
		}
		check(config_load() == ST_OK, "config.txt put back");
		act(&v, MBOX_EV_LEFT, "Alpha");
		check(!loaded("mm-a"), "Alpha Off");
	}

	/* A dependency comes with it, as MOD_LOAD brings it. */
	rc = act(&v, MBOX_EV_ACTIVATE, "Needy");
	check(rc == ST_OK && loaded("mm-n") && loaded("mm-a") && strcmp(value_of(&v, "Alpha"), "On") == 0,
	      "Needy On: Alpha, which it depends on, loaded first");
	act(&v, MBOX_EV_LEFT, "Needy");
	act(&v, MBOX_EV_LEFT, "Alpha");
	check(!loaded("mm-n") && !loaded("mm-a"), "both Off again");

	/* A dependency the console does not have. */
	poke32(0x00874000u, 0x22222222u);
	rc = act(&v, MBOX_EV_ACTIVATE, "Missing");
	check(rc == ST_NOT_FOUND && !loaded("mm-m") && peek32(0x00874000u) == 0x22222222u &&
	      strcmp(value_of(&v, "Missing"), "not found") == 0,
	      "a mod whose dependency is not on the console: refused, nothing written, \"not found\" in its row");
	check(notified("Trainer: Missing: a mod it needs is missing"), "and a notification says why");

	/* A mod qwark could not parse. */
	rc = act(&v, MBOX_EV_ACTIVATE, "mm-bad");
	check(rc == ST_IO_ERROR && !loaded("mm-bad") && strcmp(value_of(&v, "mm-bad"), "failed") == 0 &&
	      notified("Trainer: mm-bad: failed"), "a parse error: refused, \"failed\", and notified");

	/* Beta's cave and word in the game; Clash's cave would land on Beta's. */
	poke32(0x00872000u, 0x33333333u);
	rc = act(&v, MBOX_EV_ACTIVATE, "Beta");
	check(rc == ST_OK && loaded("mm-b") && peek32(0x00871008u) == 0x42424242u, "Beta On: its cave in the game");
	check(mem_owner(0x0087100Fu, 1) != NULL && strcmp(mem_owner(0x0087100Fu, 1), "Beta") == 0 &&
	      mem_owner(0x00871010u, 1) == NULL && mem_owner(0x00871800u, 4) != NULL,
	      "its cave and its word are Beta's to the guard, the byte past the cave nobody's");
	rc = act(&v, MBOX_EV_ACTIVATE, "Clash");
	check(rc == ST_BAD_ARG && !loaded("mm-c") && peek32(0x00871008u) == 0x42424242u &&
	      peek32(0x00872000u) == 0x33333333u && strcmp(value_of(&v, "Clash"), "refused") == 0,
	      "Clash, whose cave is half over Beta's: refused, not a byte of it written, its own word not either");
	check(notified("Trainer: Clash would write over Beta"), "the notification names what is in the way");
	check(mods_load("mm-c") == ST_OK && peek32(0x00871008u) == 0x43434343u,
	      "MOD_LOAD from a client is as it was: no rule of the page's (the client's own rules stand)");
	mods_unload("mm-c");
	menu_model_view(&v);

	/* A chain: Alpha goes in for it, then its own word lands on Beta's: Alpha comes out again. */
	poke32(0x00871800u, 0x48000000u);
	check(!loaded("mm-a"), "Alpha is Off");
	rc = act(&v, MBOX_EV_ACTIVATE, "Chain");
	check(rc == ST_BAD_ARG && !loaded("mm-ch") && !loaded("mm-a") && peek32(0x00870000u) == 0x11111111u,
	      "Chain, which depends on Alpha and writes Beta's word: refused, and Alpha, loaded for it, unloaded again");
	check(notified("Trainer: Chain would write over Beta"), "notified");

	/* Beta out: its cave is nobody's, and Clash goes in. */
	act(&v, MBOX_EV_LEFT, "Beta");
	check(!loaded("mm-b") && mem_owner(0x0087100Fu, 1) == NULL, "Beta Off: its cave stays in the game, nobody's");
	rc = act(&v, MBOX_EV_ACTIVATE, "Clash");
	check(rc == ST_OK && loaded("mm-c") && peek32(0x00871008u) == 0x43434343u, "Clash On now");
	act(&v, MBOX_EV_LEFT, "Clash");

	/* qwark's own code: a feature's patch, the savefile helper. */
	{
		u8 fl = (u8)(session_game()->fast_loads - 1);
		const struct sf_desc *sf = sf_desc_for_game(GAME_RAC1);

		check(features_set(fl, 1) == ST_OK, "Fast loads on: its patch in the game");
		rc = act(&v, MBOX_EV_ACTIVATE, "Faster");
		check(rc == ST_BAD_ARG && !loaded("mm-f") && peek32(0x00165490u) == 0x60000000u &&
		      notified("Trainer: Faster would write over Fast loads"),
		      "a mod with a word on the fast-load patch: refused, named after the feature");
		features_set(fl, 0);
		rc = act(&v, MBOX_EV_ACTIVATE, "Faster");
		check(rc == ST_OK && loaded("mm-f"), "Fast loads off: it loads");
		act(&v, MBOX_EV_LEFT, "Faster");

		check(savefile_install() == ST_OK && sf != NULL && sf->ncaves > 0, "the savefile helper in the game");
		check(mem_owner(sf->caves[0].addr + 4, 1) != NULL &&
		      strcmp(mem_owner(sf->caves[0].addr + 4, 1), "the savefile helper") == 0 &&
		      mem_owner(sf->hooks[0].addr, 4) != NULL,
		      "its caves and hook words are qwark's own to the guard");
		{
			char text[96];
			snprintf(text, sizeof(text), "#- name: Helper\n0x%08X: 0x60000000\n", (unsigned)sf->hooks[0].addr);
			mod(HDD, "mm-h", text);
			mods_rescan();
			menu_model_view(&v);
			rc = act(&v, MBOX_EV_ACTIVATE, "Helper");
			check(rc == ST_BAD_ARG && !loaded("mm-h") && notified("Trainer: Helper would write over the savefile helper"),
			      "a mod with a word on the helper's hook: refused");
		}
	}

	/* A mod only the USB drive has: copied in, then loaded. */
	usb_at = view_index(&v, "Usbie");
	check(usb_at >= 0 && strcmp(v.rows[usb_at].value, "Off") == 0 && hdd("mm-usb") == NULL, "Usbie, Off, only on USB");
	rc = act(&v, MBOX_EV_ACTIVATE, "Usbie");
	check(rc == ST_OK && loaded("mm-usb") && peek32(0x00875000u) == 0x60000000u &&
	      plat_path_exists(HDD "/mm-usb/qwark.sum", NULL, NULL) && strcmp(value_of(&v, "Usbie"), "On") == 0 &&
	      !config_mod_auto(T, "mm-usb"),
	      "the cross: copied into the console's library, loaded, On, no auto flag");
	check(view_index(&v, "Usbie") >= 0 && v.nrows == (u8)(used_mods() + 1),
	      "one row for it still, an installed mod's now");
	rc = act(&v, MBOX_EV_LEFT, "Usbie Two");
	check(rc == ST_OK && loaded("mm-usb2") && config_mod_auto(T, "mm-usb2") &&
	      strcmp(value_of(&v, "Usbie Two"), "On boot") == 0,
	      "Left on the other: copied, loaded and auto-loaded at every boot");
	act(&v, MBOX_EV_RIGHT, "Usbie");
	act(&v, MBOX_EV_RIGHT, "Usbie");
	act(&v, MBOX_EV_LEFT, "Usbie Two");
	act(&v, MBOX_EV_LEFT, "Usbie Two");
	check(!loaded("mm-usb") && !loaded("mm-usb2") && !config_mod_auto(T, "mm-usb2"), "both Off again");

	/* A stale press: an event on a row whose id the last build does not have is refused. */
	i = view_index(&v, "Alpha");
	rc = menu_model_event(MBOX_EV_ACTIVATE, (u8)i, MENU_ROW_ID(MENU_ROW_MOD, MENU_MOD_USB | 9), v.key);
	check(rc == ST_BAD_ARG && !loaded("mm-a"), "an event whose row id is not the row's is refused");

	/* Back to the root, on Mods. */
	rc = menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && menu_model_depth() == 1 && v.cursor == 5, "back: the root, the cursor on Mods");
}

/* More mods than a CONFIG window holds: the page goes on below, and slides. */
static void test_long(void)
{
	struct menu_view v;
	char dir[16], text[96];
	u32 i, n;
	int rc;

	group("console menu: the Mods page, more mods than a window");

	for (i = 0; i < 20; i++) {
		snprintf(dir, sizeof(dir), "mm-p%02u", (unsigned)i);
		snprintf(text, sizeof(text), "#- name: Page %02u\n0x%08X: 0x60000000\n", (unsigned)i, 0x00880000u + 4u * i);
		mod(HDD, dir, text);
	}
	menu_model_reset(7, 24, CAPS_ALL);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Mods");
	n = used_mods();
	check(n > 24 && v.nrows == 24 && (v.pflags & MBOX_PAGE_MORE_DOWN) != 0 && (v.pflags & MBOX_PAGE_MORE_UP) == 0,
	      "over 24 mods: a window of 24, the page going on below");
	rc = menu_model_event(MBOX_EV_SCROLL_DOWN, 23, v.rows[23].id, v.key);
	menu_model_view(&v);
	check(rc == ST_OK && (v.pflags & MBOX_PAGE_MORE_UP) != 0, "SCROLL_DOWN at its last row: the window slides on");
	rc = menu_model_event(MBOX_EV_PAGE_NEXT, 0, v.rows[0].id, v.key);
	menu_model_view(&v);
	check(view_index(&v, "Usbie") >= 0 || (v.pflags & MBOX_PAGE_MORE_DOWN) == 0,
	      "R1: to the page's end, the USB drive's mods last");
	for (i = 0; i < 20; i++) {
		snprintf(text, sizeof(text), "%s/mm-p%02u", HDD, (unsigned)i);
		rm_tree(text);
	}
}

/* Back out of the Mods page and into it again, which rescans the library as the page opens. */
static void reopen(struct menu_view *v)
{
	menu_model_event(MBOX_EV_BACK, 0, 0, v->key);
	menu_model_view(v);
	act(v, MBOX_EV_ACTIVATE, "Mods");
}

/* Whether the applied patch named `name` starts at `addr`, as PATCH_LIST would say. */
static int patch_first(const char *name, u32 addr)
{
	u32 i;
	for (i = 0; i < patch_count(); i++) {
		const struct patch_def *d = patch_at(i);
		if (d != NULL && strcmp(d->name, name) == 0) return d->count > 0 && d->words[0].addr == addr;
	}
	return 0;
}

/*
 * Build 57's hardware report (2026-11-07, Deadlocked): a mod switched Off from
 * the Mods page stayed in the game, and a mod would not load because it "would
 * write over" a mod that has no byte where it writes. Both were the rescan's
 * packing of the word pool (mods_pack_pools), which walked the loaded mods in
 * table order and assumed their slices of the pool lay in that order too. They
 * do not once a mod with a higher slot is loaded first: the rescan that follows
 * packs it to the front, every other mod is parsed again after it, and when the
 * lower-slot mod is loaded and the page opened again, packing it to the front
 * wrote over the first mod's words and originals before they were moved, and
 * the first mod's def was then pointed at the second mod's words. Its Off
 * reverted the other mod's words and left its own in, and the guard read the
 * other mod's addresses as its own. Deadlocked's Stereo Music (auto-loaded,
 * index 10) and IL Decimal Timer (index 4, loaded for IL RTA) were exactly
 * that: PATCH_LIST on the console named Stereo Music for 16 of IL Decimal
 * Timer's words at 0x003D8858.
 */
static void test_rescan_order(void)
{
	struct menu_view v;
	int rc, lo, hi;

	group("console menu: the Mods page, a mod loaded after one in a later slot (build 57's report)");

	mod(HDD, "mm-lo", "#- name: Low\n0x00890000: 0x60000001\n0x00890100: 0x60000002\n0x00890200: 0x60000003\n");
	mod(HDD, "mm-up", "#- name: Upper\n0x00891000: 0x60000011\n0x00891100: 0x60000012\n");
	mod(HDD, "mm-rta", "#- name: Rta\n#- depends: Low\n0x00892000: 0x60000021\n");
	poke32(0x00890000u, 0x11110000u);
	poke32(0x00890100u, 0x11110100u);
	poke32(0x00890200u, 0x11110200u);
	poke32(0x00891000u, 0x22221000u);
	poke32(0x00891100u, 0x22221100u);
	poke32(0x00892000u, 0x33332000u);

	menu_model_reset(7, 24, CAPS_ALL);
	menu_model_view(&v);
	act(&v, MBOX_EV_ACTIVATE, "Mods");
	/* test_long left the table full, so the new folders get their slots at the second look. */
	reopen(&v);
	lo = mods_find("mm-lo");
	hi = mods_find("mm-up");
	check(lo >= 0 && hi > lo, "Low has the lower slot, Upper the higher");

	/* Upper first (Stereo Music, auto-loaded at boot), and the page opened again. */
	rc = act(&v, MBOX_EV_ACTIVATE, "Upper");
	check(rc == ST_OK && loaded("mm-up") && peek32(0x00891000u) == 0x60000011u, "Upper On");
	reopen(&v);
	/* Then Low (IL Decimal Timer), and the page opened again. */
	rc = act(&v, MBOX_EV_ACTIVATE, "Low");
	check(rc == ST_OK && loaded("mm-lo") && peek32(0x00890000u) == 0x60000001u, "Low On");
	reopen(&v);
	check(loaded("mm-lo") && loaded("mm-up"), "both still loaded after the rescan");
	check(patch_first("Upper", 0x00891000u) && patch_first("Low", 0x00890000u),
	      "each applied patch still names its own words (PATCH_LIST)");
	check(hdd("mm-up")->def.count == 2 && hdd("mm-up")->def.words[1].addr == 0x00891100u &&
	      hdd("mm-up")->def.originals[0] == 0x22221000u && hdd("mm-up")->def.originals[1] == 0x22221100u,
	      "Upper's words and originals are its own");
	check(mem_owner(0x00890000u, 4) != NULL && strcmp(mem_owner(0x00890000u, 4), "Low") == 0,
	      "Low's word is Low's to the guard, not Upper's");

	/* Off from the page: every word of it back, and nothing of the other mod's touched. */
	rc = act(&v, MBOX_EV_LEFT, "Upper");
	check(rc == ST_OK && !loaded("mm-up") && strcmp(value_of(&v, "Upper"), "Off") == 0, "Upper Off");
	check(peek32(0x00891000u) == 0x22221000u && peek32(0x00891100u) == 0x22221100u,
	      "Upper's words are the game's again");
	check(peek32(0x00890000u) == 0x60000001u && peek32(0x00890100u) == 0x60000002u &&
	      peek32(0x00890200u) == 0x60000003u, "Low's words stay in");
	check(mem_owner(0x00891000u, 4) == NULL && mem_owner(0x00891100u, 4) == NULL,
	      "nothing is left owning Upper's addresses");
	rc = act(&v, MBOX_EV_LEFT, "Low");
	check(rc == ST_OK && !loaded("mm-lo") && peek32(0x00890000u) == 0x11110000u &&
	      peek32(0x00890100u) == 0x11110100u && peek32(0x00890200u) == 0x11110200u,
	      "Low Off: its words are the game's again");
	check(mem_owner(0x00890000u, 4) == NULL && mem_owner(0x00890200u, 4) == NULL,
	      "nothing is left owning Low's addresses");

	/* The false overlap: Upper back on, the page opened again, and a mod that depends on Low. */
	act(&v, MBOX_EV_ACTIVATE, "Upper");
	act(&v, MBOX_EV_ACTIVATE, "Low");
	reopen(&v);
	act(&v, MBOX_EV_LEFT, "Low");
	reopen(&v);
	check(loaded("mm-up") && !loaded("mm-lo") && patch_first("Upper", 0x00891000u),
	      "Upper on, Low off, Upper's patch still its own words");
	rc = act(&v, MBOX_EV_ACTIVATE, "Rta");
	check(rc == ST_OK && loaded("mm-rta") && loaded("mm-lo") && peek32(0x00892000u) == 0x60000021u &&
	      peek32(0x00890000u) == 0x60000001u,
	      "Rta, which depends on Low and shares no byte with Upper, loads (IL RTA and Stereo Music)");
	check(strcmp(host_notify_last(), "Trainer: Rta would write over Upper") != 0, "and no \"would write over Upper\"");

	act(&v, MBOX_EV_LEFT, "Rta");
	act(&v, MBOX_EV_LEFT, "Low");
	act(&v, MBOX_EV_LEFT, "Upper");
	check(!loaded("mm-rta") && !loaded("mm-lo") && !loaded("mm-up") && peek32(0x00890000u) == 0x11110000u &&
	      peek32(0x00891000u) == 0x22221000u && peek32(0x00892000u) == 0x33332000u,
	      "all three Off, every word the game's");
	menu_model_event(MBOX_EV_BACK, 0, 0, v.key);
}

void test_menu_mods(void)
{
	host_freeze_time(1);
	clean();
	check(boot(T), "RaC1 boots");
	test_page();
	test_long();
	test_rescan_order();
	check(quit(), "quit RaC1");
	clean();
	mods_rescan();
	modusb_scan(1);
	host_freeze_time(0);
}
