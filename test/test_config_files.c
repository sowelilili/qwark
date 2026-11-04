/*
 * Build 57: config.txt and a profile's config.txt on a console's HDD, which is
 * less kind than the PC's, and in the hands of a user who edits them on
 * Windows. Built into both suites: test/qwark-test.exe, a test build, and
 * test/qwark-test-release.exe, a release, both with the settings profiles
 * (QWARK_PREVIEW 1) since build 57; built with QWARK_PREVIEW 0, as client
 * 1.4.2's build 56 release module was, it leaves the profiles' part out.
 *
 *   a short read    every read of the HDD hands back seven bytes at a time, as
 *                   cellFsRead may, through a whole day of the console's: the
 *                   switches of the profile, writes from a client and the menu,
 *                   a game booted, and every key of config.txt is still there;
 *   an open that    fails with the file there (a busy drive, an FTP server
 *     fails         holding it): the load is not taken for "no file", so the
 *                   next write does not leave config.txt the one key it set;
 *   a write cut     part of the way, or a close that fails: the old file stays;
 *   a hand edit     CR LF, no newline at the end, comments, a key qwark does
 *                   not know, a value or a key longer than qwark holds and a key
 *                   twice: read as meant, and every line qwark does not own
 *                   written back as it was;
 *   the carry       (profiles) config.txt's per-user lines, with no newline at
 *                   the end, carried by a console write with the active
 *                   profile's longer file read just before: nothing of the
 *                   profile's comes into config.txt;
 *   the override    (profiles) a combo override removed goes from the file.
 *
 * It puts config.txt back as it found it, and takes away the profiles it made.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/mods.h"
#include "../src/core/util.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define CF_FILE_MAX 24576
#define CF_NEW      QWARK_CONFIG ".new"

/*
 * A console's config.txt as it was before it lost most of itself, 66 keys:
 * the per-user ones first, as a console that has had its settings a while
 * holds them, then the console's own, `profiles.enabled` last.
 */
static const char g_console_cfg[] =
	"selected.slot = 0\n"
	"NPEA00423.auto.fast_loads = 0\n"
	"NPEA00385.auto.update_ratchet = 0\n"
	"NPEA00386.auto.auto_reset_any = 0\n"
	"NPEA00386.auto.fast_loads = 0\n"
	"NPEA00423.mod.rc4-save.auto = 0\n"
	"NPEA00423.mod.crash-patch.auto = 0\n"
	"NPEA00423.mod.il-igt.auto = 0\n"
	"selected.planet = 5\n"
	"selected.planet_flags = 1\n"
	"NPEA00423.mod.bot-display.auto = 0\n"
	"NPEA00386.mod.spiderbot-fix.auto = 1\n"
	"NPEA00386.mod.bl-mod.auto = 1\n"
	"NPEA00423.mod.force-misson-cutscenes.auto = 0\n"
	"NPEA00423.mod.stereo-music.auto = 1\n"
	"NPEA00385.mod.charge-boots.auto = 1\n"
	"NPEA00385.auto.infinite_ammo = 0\n"
	"NPEA00385.auto.fast_loads = 0\n"
	"manip.rac2.auto_reset = 1\n"
	"manip.rac2.category = 5\n"
	"manip.rac4.category = 1\n"
	"manip.rac1.category = 2\n"
	"manip.rac1.auto_reset = 1\n"
	"manip.rac3.category = 0\n"
	"manip.rac3.remember = 0\n"
	"manip.rac4.remember = 1\n"
	"selected.rac3.planet = 5\n"
	"selected.rac3.planet_flags = 1\n"
	"selected.rac1.planet = 8\n"
	"selected.rac1.planet_flags = 0\n"
	"NPEA00387.auto.fast_loads = 0\n"
	"selected.rac4.planet = 6\n"
	"selected.rac4.planet_flags = 1\n"
	"manip.rac4.auto_reset = 1\n"
	"NPEA00423.auto.crash_patches = 1\n"
	"NPEA00387.auto.reset_trophies_on_veldin_load = 1\n"
	"NPEA00387.auto.freeze_health = 0\n"
	"NPEA00385.auto.ghost_ratchet = 0\n"
	"NPEA00386.auto.infinite_ammo = 0\n"
	"NPEA00423.auto.ghost_ratchet = 0\n"
	"NPEA00387.auto.infinite_ammo = 1\n"
	"NPEA00423.mod.dl-cs.auto = 0\n"
	"NPEA00423.auto.freeze_health = 0\n"
	"menu.help = 1\n"
	"NPEA00423.auto.refill_ammo = 0\n"
	"selected.rac2.planet = 3\n"
	"selected.rac2.planet_flags = 1\n"
	"NPEA00386.auto.ghost_ratchet = 0\n"
	"NPEA00423.auto.hud_colour = 2164222976\n"
	"NPEA00423.auto.hud_vivid_shade = 2150437888\n"
	"NPEA00423.auto.hud_dark_shade = 2149453056\n"
	"NPEA00423.auto.hud_light_shade = 2164222976\n"
	"NPEA00423.auto.weapon_colour_v11 = 2164240384\n"
	"NPEA00423.auto.weapon_colour_v99 = 2164229888\n"
	"NPEA00423.auto.gadget_colour = 2178927762\n"
	"NPEA00423.auto.correct_hud_brightness = 1\n"
	"NPEA00423.auto.correct_gradient_brightness = 0\n"
	"combo.save = 0x00002003\n"
	"combo.die = 0x00004003\n"
	"combo.load = 0x00008003\n"
	"combo.setaside = 0x00001003\n"
	"combo.planet = 0x00000203\n"
	"combo.enabled = 1\n"
	"combo.menu = 0x00000103\n"
	"combo.fastloads = 0x00000083\n"
	"profiles.enabled = 1\n";

/* The console's combos among them, which a profile never holds. */
static const char * const g_shared[] = {
	"combo.save = 0x00002003", "combo.die = 0x00004003", "combo.load = 0x00008003",
	"combo.setaside = 0x00001003", "combo.planet = 0x00000203", "combo.enabled = 1",
	"combo.menu = 0x00000103", "combo.fastloads = 0x00000083",
	NULL
};

static void cf_tick(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int cf_boot(const char *title)
{
	int i;
	host_boot(title);
	for (i = 0; i < 4000; i++) {
		cf_tick(1);
		if (session_state() == SESSION_INGAME) return 1;
	}
	return 0;
}

static int cf_quit(void)
{
	int i;
	host_quit();
	for (i = 0; i < 2000; i++) {
		cf_tick(1);
		if (session_state() == SESSION_XMB) return 1;
	}
	return 0;
}

/* A whole file, read on however the HDD hands it back; -1 for none. */
static int cf_slurp(const char *path, char *buf, u32 cap)
{
	plat_file_t f;
	u32 total = 0;

	buf[0] = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return -1;
	for (;;) {
		u32 got = 0;
		if (total + 1 >= cap || plat_file_read(f, buf + total, cap - 1 - total, &got) != 0 || got == 0) break;
		total += got;
	}
	plat_file_close(f);
	buf[total] = 0;
	return (int)total;
}

static void cf_spill(const char *path, const char *text)
{
	plat_file_t f;
	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return;
	plat_file_write(f, text, qstrlen(text));
	plat_file_close(f);
}

static int cf_exists(const char *path)
{
	return plat_path_exists(path, NULL, NULL);
}

/* `line` is one whole line of the file, CR or not. */
static int cf_has_line(const char *path, const char *line)
{
	static char buf[CF_FILE_MAX];
	const char *p;
	u32 n = qstrlen(line);

	if (cf_slurp(path, buf, sizeof(buf)) < 0) return 0;
	for (p = buf; (p = strstr(p, line)) != NULL; p++) {
		if ((p == buf || p[-1] == '\n') && (p[n] == 0 || p[n] == '\n' || p[n] == '\r')) return 1;
	}
	return 0;
}

static u32 cf_count_lines(const char *path)
{
	static char buf[CF_FILE_MAX];
	u32 n = 0;
	char *p;

	if (cf_slurp(path, buf, sizeof(buf)) <= 0) return 0;
	for (p = buf; *p != 0; p++) if (*p == '\n') n++;
	if (p[-1] != '\n') n++;
	return n;
}

static int cf_same(const char *path, const char *text)
{
	static char buf[CF_FILE_MAX];
	return cf_slurp(path, buf, sizeof(buf)) >= 0 && strcmp(buf, text) == 0;
}

/*
 * Every key of `text` is in the file at `path` (with the value `text` gives it
 * unless `any_value`), less the ones with a prefix in `except`.
 */
static int cf_keys_in(const char *path, const char *text, int any_value, const char *except)
{
	static char file[CF_FILE_MAX];
	static char line[256];
	const char *s = text;

	if (cf_slurp(path, file, sizeof(file)) < 0) return 0;
	while (*s != 0) {
		const char *e = strchr(s, '\n');
		u32 n = e != NULL ? (u32)(e - s) : qstrlen(s);
		const char *eq;
		const char *p;
		int found = 0;

		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, s, n);
		line[n] = 0;
		s += n + (e != NULL ? 1 : 0);
		eq = strstr(line, " = ");
		if (eq == NULL) continue;
		if (except != NULL && strncmp(line, except, qstrlen(except)) == 0) continue;
		if (!any_value) {
			if (!cf_has_line(path, line)) return 0;
			continue;
		}
		line[eq - line + 3] = 0;   /* "key = " */
		for (p = file; (p = strstr(p, line)) != NULL; p++) {
			if (p == file || p[-1] == '\n') { found = 1; break; }
		}
		if (!found) return 0;
	}
	return 1;
}

static int cf_shared_in_config(void)
{
	int i;
	for (i = 0; g_shared[i] != NULL; i++) {
		if (!cf_has_line(QWARK_CONFIG, g_shared[i])) return 0;
	}
	return 1;
}

static void cf_faults(const char *prefix, u32 chunk, u32 open_fails, u32 cut, u32 close_fails)
{
	struct host_file_faults f;

	f.read_chunk = chunk;
	f.open_fails = open_fails;
	f.write_cut = cut;
	f.close_fails = close_fails;
	host_set_file_faults(prefix, &f);
}

static void cf_no_faults(void)
{
	host_set_file_faults(NULL, NULL);
}

/* ------------------------------------------------------------ short reads */

static void test_short_reads(void)
{
	group("config files: a console's short reads, through a day of it");
	if (session_state() != SESSION_XMB) cf_quit();
	cf_spill(QWARK_CONFIG, g_console_cfg);
	plat_file_unlink(CF_NEW);
	cf_faults(QWARK_CONFIG, 7, 0, 0, 0);
	check(config_load() == ST_OK, "config.txt loads, seven bytes a read");
	check(config_combo(COMBO_SAVE_POSITION) == 0x2003 && config_menu_help() &&
	      config_feature_auto("NPEA00423", "HUD colour") == 2164222976u,
	      "the first key, the per-user keys and the last read back whole");
	check(cf_boot("NPEA00423"), "Deadlocked comes up");
	check(config_set_combo(COMBO_DIE, 0x4003) == ST_OK && config_set_u32("selected.slot", 4) == ST_OK,
	      "a combo and a slot are written");
	check(cf_quit(), "back at the XMB");
	cf_tick(300);
	cf_no_faults();
	check(cf_keys_in(QWARK_CONFIG, g_console_cfg, 1, NULL) && cf_shared_in_config(),
	      "config.txt still has every one of its 66 keys");
	check(cf_has_line(QWARK_CONFIG, "selected.slot = 4"), "with the slot written");
}

/* -------------------------------------------------------- failed opens */

static void test_open_fails(void)
{
	group("config files: an open that fails with the file there");
	cf_spill(QWARK_CONFIG, g_console_cfg);
	plat_file_unlink(CF_NEW);
	cf_faults(QWARK_CONFIG, 0, 1, 0, 0);
	check(config_load() == ST_OK && config_combo(COMBO_SAVE_POSITION) == 0x2003,
	      "one open that fails is tried again, and the file loads");

	cf_faults(QWARK_CONFIG, 0, 2, 0, 0);
	check(config_load() == ST_IO_ERROR, "two in a row: CONFIG_RELOAD answers IO_ERROR, not \"no file\"");
	cf_no_faults();
	check(config_set_combo(COMBO_DIE, 0x1111) != ST_OK,
	      "and a write is refused rather than made into a store that did not load");
	check(cf_same(QWARK_CONFIG, g_console_cfg), "config.txt is as it was, every key");
	check(config_load() == ST_OK && config_combo(COMBO_DIE) == 0x4003, "the next reload reads it all again");

	cf_faults(QWARK_CONFIG, 0, 2, 0, 0);
	check(config_set_combo(COMBO_DIE, 0x2222) == ST_IO_ERROR,
	      "a write whose old file will not open is IO_ERROR");
	cf_no_faults();
	check(cf_same(QWARK_CONFIG, g_console_cfg) && !cf_exists(CF_NEW), "and leaves the file as it was");
	config_load();
}

/* ------------------------------------------------- cut writes and closes */

static void test_cut_writes(void)
{
	group("config files: a write cut short, and a close that fails");
	cf_spill(QWARK_CONFIG, g_console_cfg);
	plat_file_unlink(CF_NEW);
	config_load();

	cf_faults(QWARK_CONFIG, 0, 0, 1000, 0);
	check(config_set_combo(COMBO_DIE, 0x3333) == ST_IO_ERROR, "the disk takes 1000 bytes of 2 KB: IO_ERROR");
	cf_no_faults();
	check(cf_same(QWARK_CONFIG, g_console_cfg) && !cf_exists(CF_NEW),
	      "config.txt is the whole old file, and no .new is left");

	cf_faults(QWARK_CONFIG, 0, 0, 0, 1);
	check(config_set_combo(COMBO_DIE, 0x4444) == ST_IO_ERROR, "a close that fails: IO_ERROR");
	cf_no_faults();
	check(cf_same(QWARK_CONFIG, g_console_cfg) && !cf_exists(CF_NEW),
	      "and the file the close may not have finished is not put in place");

	check(config_set_combo(COMBO_DIE, 0x4003) == ST_OK && cf_same(QWARK_CONFIG, g_console_cfg),
	      "the next write goes through, and the file is byte for byte the one it replaced");
}

/* ------------------------------------------------------------ hand edits */

/*
 * What a user leaves after editing config.txt on Windows: CR LF, a comment, a
 * key qwark does not know, a value too long for the store (an old client's
 * note), a key too long, a key twice (the second wins, as it always has), and
 * a last line with no newline.
 */
static const char g_hand[] =
	"# my console\r\n"
	"combo.save = 0x00002003\r\n"
	"my.own.key = keep me\r\n"
	"note = this value is far longer than the thirty-one characters qwark keeps\r\n"
	"NPEA00423.auto.a_label_long_enough_that_its_key_is_more_than_sixty_four = 1\r\n"
	"\r\n"
	"combo.die = 0x00000001\r\n"
	"combo.die = 0x00004003\r\n"
	"combo.load = 0x00008003";

static void test_hand_edit(void)
{
	group("config files: a hand edit from Windows");
	cf_spill(QWARK_CONFIG, g_hand);
	plat_file_unlink(CF_NEW);
	check(config_load() == ST_OK, "CR LF and no newline at the end load");
	check(config_combo(COMBO_SAVE_POSITION) == 0x2003 && config_combo(COMBO_DIE) == 0x4003 &&
	      config_combo(COMBO_LOAD_POSITION) == 0x8003,
	      "every value reads without its CR, the last line's too, and the later of two lines wins");
	check(config_get("note") == NULL, "a value longer than the store keeps is not held cut short");

	check(config_set_combo(COMBO_LOAD_PLANET, 0x0203) == ST_OK, "a write");
	check(cf_has_line(QWARK_CONFIG, "# my console") && cf_has_line(QWARK_CONFIG, "my.own.key = keep me"),
	      "keeps the comment and the key qwark does not know");
	check(cf_has_line(QWARK_CONFIG, "note = this value is far longer than the thirty-one characters qwark keeps") &&
	      cf_has_line(QWARK_CONFIG, "NPEA00423.auto.a_label_long_enough_that_its_key_is_more_than_sixty_four = 1"),
	      "and the value and the key too long to hold, whole");
	check(cf_has_line(QWARK_CONFIG, "combo.die = 0x00004003") && !cf_has_line(QWARK_CONFIG, "combo.die = 0x00000001"),
	      "the key that was there twice is there once, with its value");
	check(cf_has_line(QWARK_CONFIG, "combo.load = 0x00008003") && cf_has_line(QWARK_CONFIG, "combo.planet = 0x00000203"),
	      "the last line is whole, and the new key follows it");
	check(cf_count_lines(QWARK_CONFIG) == 9, "nothing else came in or went");
	{
		static char buf[CF_FILE_MAX];
		cf_slurp(QWARK_CONFIG, buf, sizeof(buf));
		check(strchr(buf, '\r') == NULL && buf[strlen(buf) - 1] == '\n', "written back with LF, a newline at the end");
	}
}

#if QWARK_PREVIEW
/* ------------------------------------------------------ the profiles' side */

#define CF_P1 QWARK_PROFILEDIR "/user-00000001"
#define CF_P2 QWARK_PROFILEDIR "/user-00000002"

static void cf_forget(const char *folder)
{
	static const char * const leaf[] = {
		"positions/rac1.txt", "positions/rac2.txt", "positions/rac3.txt", "positions/rac4.txt",
		"positions", "config.txt", "config.txt.new", "", NULL
	};
	char path[160];
	int i;

	for (i = 0; leaf[i] != NULL; i++) {
		snprintf(path, sizeof(path), "%s%s%s", folder, leaf[i][0] ? "/" : "", leaf[i]);
		if (plat_file_unlink(path) != 0) plat_dir_remove(path);
	}
}

static u32 cf_count_lines_of(const char *text)
{
	u32 n = 0;
	const char *p;

	if (*text == 0) return 0;
	for (p = text; *p != 0; p++) if (*p == '\n') n++;
	if (p[-1] != '\n') n++;
	return n;
}

/* The per-user lines of the console's file: everything before its first combo. */
static void cf_user_part(char *out, u32 cap, int final_newline)
{
	const char *end = strstr(g_console_cfg, "combo.save");
	u32 n = (u32)(end - g_console_cfg);

	if (n >= cap) n = cap - 1;
	memcpy(out, g_console_cfg, n);
	out[n] = 0;
	if (!final_newline && n > 0) out[n - 1] = 0;
}

static void test_profile_day(void)
{
	static char user1[CF_FILE_MAX];
	int changed = 0;
	const char *mod_dir = NULL;

	group("config files: the console's day with profiles, on short reads");
	cf_forget(CF_P1);
	cf_forget(CF_P2);
	if (session_state() != SESSION_XMB) cf_quit();
	host_set_has_users(1);
	host_set_user(1);

	/* User 1's profile, a copy of the console's per-user keys and a menu combo of their own. */
	cf_user_part(user1, sizeof(user1), 1);
	qstrcat(user1, sizeof(user1), "combo.menu = 0x00004000\n");
	plat_dir_create(CF_P1 "/positions");
	cf_spill(CF_P1 "/config.txt", user1);
	cf_spill(QWARK_CONFIG, g_console_cfg);
	plat_file_unlink(CF_NEW);
	cf_faults(QWARK_CONFIG, 7, 0, 0, 0);
	config_load();
	cf_tick(300);
	check(config_profile_active() && config_profile_user() == 1, "at the XMB, user 1's profile is active");
	check(config_combo(COMBO_OPEN_MENU) == 0x4000 && config_combo(COMBO_SAVE_POSITION) == 0x2003,
	      "with their menu combo and the console's others");
	check(cf_boot("NPEA00385"), "RaC1 comes up");
	if (mods_count() > 0 && mods_at(0) != NULL) mod_dir = mods_at(0)->dirname;
	check(config_profiles_enable(0, PROFILE_SETUP_NONE, 1, &changed) == ST_OK && changed &&
	      !config_profile_active(),
	      "in the game, the client turns the profiles off");
	check(config_set_menu_help(0) == ST_OK && config_set_combo(COMBO_DIE, 0x4003) == ST_OK,
	      "and writes a per-user key and a combo into config.txt");
	if (mod_dir != NULL) check(mods_set_auto(mod_dir, 1) == ST_OK, "and a mod's auto flag");
	check(config_profiles_enable(1, PROFILE_SETUP_NONE, 1, &changed) == ST_OK && config_profile_active(),
	      "and turns them on again");
	check(config_set_combo_override(COMBO_OPEN_MENU, 1, 0x4000) == ST_OK &&
	      config_set_u32("selected.slot", 7) == ST_OK && config_set_combo(COMBO_LOAD_POSITION, 0x8003) == ST_OK,
	      "an override, a per-user write and a console write with the profile on");
	check(cf_quit(), "back at the XMB");
	host_set_user(2);
	cf_tick(300);
	check(config_profile_user() == 2, "user 2 signs in");
	check(config_set_combo_override(COMBO_OPEN_MENU, 1, 0x4000) == ST_OK, "and sets an override, making their profile");
	host_set_user(1);
	cf_tick(300);
	check(config_profile_user() == 1, "user 1 again");
	check(config_profiles_enable(0, PROFILE_SETUP_NONE, 1, &changed) == ST_OK, "and the profiles off at the XMB");
	cf_no_faults();

	check(cf_shared_in_config() && cf_has_line(QWARK_CONFIG, "profiles.enabled = 0"),
	      "config.txt has every combo as it was, and the switch as the client left it");
	check(cf_keys_in(QWARK_CONFIG, g_console_cfg, 1, NULL), "and every one of its 66 keys");
	check(cf_has_line(QWARK_CONFIG, "menu.help = 0") && cf_has_line(QWARK_CONFIG, "selected.slot = 0"),
	      "the console's per-user keys are its own: menu.help from the write with profiles off, the slot not user 1's");
	check(cf_keys_in(CF_P1 "/config.txt", user1, 1, NULL) && cf_has_line(CF_P1 "/config.txt", "selected.slot = 7"),
	      "user 1's file has every key it had, and their slot");
	check(!cf_has_line(CF_P1 "/config.txt", "profiles.enabled = 1") && !cf_has_line(CF_P1 "/config.txt", "combo.save = 0x00002003"),
	      "and none of the console's");
	check(cf_count_lines(CF_P2 "/config.txt") == 1 && cf_has_line(CF_P2 "/config.txt", "combo.menu = 0x00004000"),
	      "user 2's has their override alone");

	group("config files: a removed override goes from the file");
	check(config_profiles_enable(1, PROFILE_SETUP_NONE, 1, &changed) == ST_OK && config_profile_active(), "profiles on");
	check(config_set_combo_override(COMBO_OPEN_MENU, 0, 0) == ST_OK &&
	      !cf_has_line(CF_P1 "/config.txt", "combo.menu = 0x00004000"),
	      "Clear takes user 1's override out of their config.txt");
	check(cf_keys_in(CF_P1 "/config.txt", user1, 1, "combo.menu"), "and nothing else");
	config_profiles_enable(0, PROFILE_SETUP_NONE, 1, &changed);
}

/*
 * Build 56's carry kept each per-user line where it was in the core's text
 * buffer. A config.txt whose lines were all per-user and whose last had no
 * newline had that newline written over the buffer's terminating NUL, and the
 * carry read on into what the buffer last held: the active profile's file,
 * read just before, whose lines came into config.txt.
 */
static void test_carry_no_newline(void)
{
	static char user_part[CF_FILE_MAX];
	static char profile[CF_FILE_MAX];
	int changed = 0;

	group("config files: config.txt's per-user lines with no newline at the end, carried");
	cf_forget(CF_P1);
	if (session_state() != SESSION_XMB) cf_quit();
	host_set_has_users(1);
	host_set_user(1);

	/* config.txt: the per-user lines and nothing else, LF, no newline at the end. */
	cf_user_part(user_part, sizeof(user_part), 0);
	/* User 1's file: longer than config.txt, with keys config.txt does not have. */
	profile[0] = 0;
	qstrcat(profile, sizeof(profile), g_console_cfg);
	qstrcat(profile, sizeof(profile), "NPEA00385.auto.only_in_the_profile = 1\n");
	plat_dir_create(CF_P1 "/positions");
	cf_spill(CF_P1 "/config.txt", profile);
	cf_spill(QWARK_CONFIG, "profiles.enabled = 1\n");
	config_load();
	cf_tick(300);
	check(config_profile_active(), "user 1's profile is active");
	cf_spill(QWARK_CONFIG, user_part);
	check(config_load() == ST_OK, "the hand-edited config.txt reloads, and the profile after it");
	check(config_set_u32("selected.slot", 3) == ST_OK, "a per-user write, which writes the profile's file");
	check(config_set_combo(COMBO_SAVE_POSITION, 0x2003) == ST_OK, "then a console write");
	check(!cf_has_line(QWARK_CONFIG, "NPEA00385.auto.only_in_the_profile = 1") &&
	      !cf_has_line(QWARK_CONFIG, "combo.die = 0x00004003"),
	      "brings nothing of the profile's file into config.txt");
	check(cf_keys_in(QWARK_CONFIG, user_part, 0, NULL) &&
	      cf_count_lines(QWARK_CONFIG) == cf_count_lines_of(user_part) + 1,
	      "config.txt is its own lines and the one key written");
	config_profiles_enable(0, PROFILE_SETUP_NONE, 1, &changed);
}
#endif /* QWARK_PREVIEW */

void test_config_files(void)
{
	static char before[CF_FILE_MAX];
	int had = cf_slurp(QWARK_CONFIG, before, sizeof(before)) >= 0;

	/* No users until the profiles' own part: the first tests are config.txt's alone. */
	host_set_has_users(0);

	test_short_reads();
	test_open_fails();
	test_cut_writes();
	test_hand_edit();
#if QWARK_PREVIEW
	test_profile_day();
	test_carry_no_newline();
	cf_forget(CF_P1);
	cf_forget(CF_P2);
	host_set_user(1);
#endif
	host_set_has_users(1);
	cf_no_faults();
	/* The games booted here leave no slot table behind to be written over test_profiles' files. */
	pos_use_title(NULL);
	if (had) cf_spill(QWARK_CONFIG, before); else plat_file_unlink(QWARK_CONFIG);
	plat_file_unlink(CF_NEW);
	config_load();
}
