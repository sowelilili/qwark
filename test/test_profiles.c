/*
 * Revision 1.24, build 56: settings profiles on qwark's fake console.
 *
 *   the split      which side every kind of key is on (config_key_scope);
 *   the switch     off out of the box, nothing read from a profile; on with a
 *                  copy of the console's per-user keys and position files, or
 *                  empty; a profile that exists never overwritten;
 *   the routing    per-user writes into the profile's file only, console keys
 *                  into config.txt only, whichever profile is active;
 *   the users      a new user seen at the XMB within a second, never in a game;
 *                  a user with no profile reads the defaults and nothing is
 *                  made for them until they write; nobody signed in changes
 *                  nothing;
 *   the combos     an override per combo that wins for that combo alone, goes
 *                  with Clear, survives the switch going off, and leaves
 *                  COMBO_SET writing the console's mask;
 *   the auto flags a mod's auto flag that follows the profile;
 *   the files      a write cut off before its rename read back from ".new", and
 *                  a profile whose folder cannot be made;
 *   RPCS3          no users, so no profiles and no flags;
 *   the big config a config.txt of everything qwark writes (build 56), which
 *                  PROFILE_ENABLE answered FULL: copied, written through,
 *                  switched between users and turned off whole, and a setup
 *                  that fails, or a file with no room, leaving nothing changed.
 *
 * It runs last and puts back the console's config.txt and position files it
 * found, so the suites after it (none today) see the HDD as it was.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/mods.h"
#include "../src/core/util.h"
#include "../src/games/game.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define PR_FILE_MAX 24576

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

/* A whole file into buf, NUL-terminated; -1 when it cannot be opened. */
static int slurp(const char *path, char *buf, u32 cap)
{
	plat_file_t f;
	u32 got = 0;

	buf[0] = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return -1;
	if (plat_file_read(f, buf, cap - 1, &got) != 0) got = 0;
	plat_file_close(f);
	buf[got] = 0;
	return (int)got;
}

static int spill(const char *path, const char *text)
{
	plat_file_t f;
	int rc;

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return -1;
	rc = plat_file_write(f, text, qstrlen(text));
	plat_file_close(f);
	return rc;
}

static int exists(const char *path)
{
	return plat_path_exists(path, NULL, NULL);
}

static int has_line(const char *path, const char *line)
{
	static char buf[PR_FILE_MAX];
	if (slurp(path, buf, sizeof(buf)) < 0) return 0;
	return strstr(buf, line) != NULL;
}

/* Every profile folder this suite makes, emptied and taken away. */
static void forget_profile(const char *folder)
{
	static const char * const games[4] = { "rac1", "rac2", "rac3", "rac4" };
	char path[160];
	int i;

	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/%s/positions/%s.txt", QWARK_PROFILEDIR, folder, games[i]);
		plat_file_unlink(path);
	}
	snprintf(path, sizeof(path), "%s/%s/positions", QWARK_PROFILEDIR, folder);
	plat_dir_remove(path);
	snprintf(path, sizeof(path), "%s/%s/config.txt", QWARK_PROFILEDIR, folder);
	plat_file_unlink(path);
	snprintf(path, sizeof(path), "%s/%s/config.txt.new", QWARK_PROFILEDIR, folder);
	plat_file_unlink(path);
	snprintf(path, sizeof(path), "%s/%s", QWARK_PROFILEDIR, folder);
	plat_dir_remove(path);
	plat_file_unlink(path);
}

/* Steps the XMB a little over a second, the poll's period. */
static void xmb_second(void)
{
	tick(130);
}

static void test_scopes(void)
{
	group("profiles: which side each key is on");

	check(config_key_scope("selected.slot") == CONFIG_SCOPE_USER, "the selected slot is per user");
	check(config_key_scope("selected.rac2.planet") == CONFIG_SCOPE_USER &&
	      config_key_scope("selected.rac4.planet_flags") == CONFIG_SCOPE_USER,
	      "so is each game's planet selection");
	check(config_key_scope("selected.planet") == CONFIG_SCOPE_USER,
	      "and the console's old pair, which a copy carries");
	check(config_key_scope("manip.rac1.category") == CONFIG_SCOPE_USER, "the manip options are per user");
	check(config_key_scope("menu.help") == CONFIG_SCOPE_USER, "In-game menus is per user");
	check(config_key_scope("NPEA00385.auto.infinite_ammo") == CONFIG_SCOPE_USER,
	      "a toggle's auto flag is per user");
	check(config_key_scope("NPEA00423.mod.racman-tas.auto") == CONFIG_SCOPE_USER,
	      "so is a mod's");
	check(config_key_scope("BCES01503.auto.x") == CONFIG_SCOPE_USER, "the disc trilogy's too");
	check(config_key_scope("profile.anything") == CONFIG_SCOPE_USER, "and the profile's own keys");
	check(config_key_scope("combo.save") == CONFIG_SCOPE_COMBO &&
	      config_key_scope("combo.menu") == CONFIG_SCOPE_COMBO &&
	      config_key_scope("combo.fastloads") == CONFIG_SCOPE_COMBO,
	      "the combo masks are combos, overridable one at a time");
	check(config_key_scope("combo.enabled") == CONFIG_SCOPE_CONSOLE, "the combo switch is the console's");
	check(config_key_scope("profiles.enabled") == CONFIG_SCOPE_CONSOLE, "so is the profiles switch");
	check(config_key_scope("menu.enabled") == CONFIG_SCOPE_CONSOLE &&
	      config_key_scope("log") == CONFIG_SCOPE_CONSOLE &&
	      config_key_scope("savefile_helper") == CONFIG_SCOPE_CONSOLE,
	      "and the menu's own switch, the log and the savefile helper");
	check(config_key_scope("npea00385.auto.x") == CONFIG_SCOPE_CONSOLE &&
	      config_key_scope("NPEA0038.auto.x") == CONFIG_SCOPE_CONSOLE &&
	      config_key_scope("NPEA00385.other") == CONFIG_SCOPE_CONSOLE,
	      "a key that only looks like a title's is the console's");
	check(config_key_scope("") == CONFIG_SCOPE_CONSOLE && config_key_scope(NULL) == CONFIG_SCOPE_CONSOLE,
	      "and an empty one");
}

static void test_flow(void)
{
	static char before_cfg[PR_FILE_MAX];
	static char before_pos[PR_FILE_MAX];
	static char buf[PR_FILE_MAX];
	int had_cfg, had_pos;
	u8 seq;
	char folder[PROFILE_FOLDER_MAX];
	u8 info[SESSION_INFO_SIZE];
	u32 mask_shared, mask_own;
	const char *mod_dir = NULL;
	int r;

	had_cfg = slurp(QWARK_CONFIG, before_cfg, sizeof(before_cfg)) >= 0;
	had_pos = slurp(QWARK_POSDIR "/rac1.txt", before_pos, sizeof(before_pos)) >= 0;
	forget_profile("user-00000001");
	forget_profile("user-00000002");
	forget_profile("user-00000003");
	host_set_user(1);
	host_set_has_users(1);

	group("profiles: off out of the box");
	if (session_state() != SESSION_XMB) quit();
	check(session_state() == SESSION_XMB, "at the XMB");
	check(!config_profiles_enabled(), "the switch is off");
	check(config_profile_flags() == SESSION_PROFILE_SUPPORTED,
	      "the simulator has users, so flags2 bit1 is set and nothing else");
	xmb_second();
	check(!config_profile_active(), "a second at the XMB makes nothing active");
	check(!exists(QWARK_PROFILEDIR "/user-00000001"), "and makes no folder");
	session_info_copy(info, sizeof(info));
	check_eq_u64(info[29] & (u8)~(SESSION_FLAG2_USB_MODS | SESSION_FLAG2_DEV), SESSION_PROFILE_SUPPORTED, "SessionInfo byte 29, flags2, says the same");
	check_eq_u64(info[31], SESSION_FLAG3_RUN_WARN, "byte 31, flags3 since revision 1.30, has nothing of the profile's: the run-start warnings, on");

	config_profile_folder_for(1, folder, sizeof(folder));
	check(strcmp(folder, "user-00000001") == 0, "user 1's folder is user-00000001");
	config_profile_folder_for(12345678, folder, sizeof(folder));
	check(strcmp(folder, "user-12345678") == 0, "and user 12345678's user-12345678");
	config_profile_folder_for(0, folder, sizeof(folder));
	check(folder[0] == 0, "nobody has none");
	config_profile_folder_for(0xFFFFFFFFu, folder, sizeof(folder));
	check(folder[0] == 0, "nor does the VSH's negative answer");

	/* The console's settings a copy is to carry. */
	config_set_u32("menu.help", 0);
	config_set_u32("selected.slot", 3);
	config_set_u32("manip.rac1.remember", 1);
	config_set_u32("NPEA00385.auto.pr_test", 1);
	config_set_u32("log", 1);
	spill(QWARK_POSDIR "/rac1.txt", "5.2 = 0102030405\n");

	group("profiles: on, with a copy");
	seq = config_profile_seq();
	check(config_set_profiles_enabled(1) == ST_OK, "the switch goes on");
	check(config_set_profiles_enabled(2) == ST_BAD_ARG, "and takes only 0 or 1");
	check(config_profile_resolve(1, PROFILE_SETUP_COPY) == 1, "the resolver makes user 1's profile active");
	check(config_profile_active() && config_profile_user() == 1, "user 1's is the active one");
	check(exists(QWARK_PROFILEDIR "/user-00000001/config.txt"), "in user-00000001");
	check(config_profile_seq() == (u8)(seq + 1), "and the sequence moves by one");
	check(config_profile_flags() == (SESSION_PROFILE_SUPPORTED | SESSION_PROFILE_ON | SESSION_PROFILE_ACTIVE),
	      "flags2 bits 1, 2 and 3");
	check(config_profile_exists(1), "the profile has been made");
	check(exists(QWARK_PROFILEDIR "/user-00000001/positions"), "with its positions folder");
	check(!config_menu_help() && config_selected_slot() == 3 &&
	      config_manip("rac1", "remember", 0) == 1 && config_feature_auto("NPEA00385", "pr test"),
	      "every per-user key reads as the console had it");
	check(has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "menu.help = 0") &&
	      has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "NPEA00385.auto.pr_test = 1"),
	      "because they were copied into its config.txt");
	check(!has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "profiles.enabled") &&
	      !has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "log ="),
	      "and nothing of the console's went with them");
	check(has_line(QWARK_PROFILEDIR "/user-00000001/positions/rac1.txt", "5.2 = 0102030405"),
	      "the position files were copied too");
	check(config_profile_resolve(1, PROFILE_SETUP_COPY) == 0, "asking again changes nothing");

	group("profiles: writes go to their side");
	check(config_set_menu_help(1) == ST_OK && config_menu_help(), "In-game menus on for user 1");
	check(has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "menu.help = 1"),
	      "in the profile's file");
	check(has_line(QWARK_CONFIG, "menu.help = 0"), "and the console's still says 0");
	config_set_selected_slot(6);
	check(has_line(QWARK_CONFIG, "selected.slot = 3"), "the slot selection is the profile's too");
	/* Revision 1.24: a colour kept for the boot is the user's, as a toggle's flag is. */
	config_set_feature_auto("NPEA00423", "HUD colour", 0x80102030u);
	check(config_feature_auto("NPEA00423", "HUD colour") == 0x80102030u &&
	      has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "NPEA00423.auto.hud_colour = 2148540464") &&
	      !has_line(QWARK_CONFIG, "NPEA00423.auto.hud_colour"),
	      "a colour to re-apply at boot goes into the profile's file, value and all");
	check(config_set_u32("log", 1) == ST_OK && has_line(QWARK_CONFIG, "log = 1") &&
	      !has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "log"),
	      "a console key goes into config.txt and nowhere else");
	check(config_set_combo_enabled(0) == ST_OK && has_line(QWARK_CONFIG, "combo.enabled = 0"),
	      "so does the combo switch");
	config_set_combo_enabled(1);
	tick(4);   /* the block a client reads is the one the tick publishes */
	session_info_copy(info, sizeof(info));
	check_eq_u64(info[29] & (u8)~(SESSION_FLAG2_USB_MODS | SESSION_FLAG2_DEV), SESSION_PROFILE_SUPPORTED | SESSION_PROFILE_ON | SESSION_PROFILE_ACTIVE,
	             "SessionInfo byte 29 has the profile on and active");
	check_eq_u64(info[30], config_profile_seq(), "and byte 30 its sequence");
	check_eq_u64(info[25], 6, "the selected slot SessionInfo carries is the profile's");

	group("profiles: a second user at the XMB");
	seq = config_profile_seq();
	host_set_user(2);
	tick(5);
	check(config_profile_user() == 1, "not looked for every tick");
	xmb_second();
	check(config_profile_active() && config_profile_user() == 2, "within a second, user 2's is active");
	check(config_profile_seq() == (u8)(seq + 1), "and the sequence moved");
	check(!config_profile_exists(2) && !exists(QWARK_PROFILEDIR "/user-00000002"),
	      "nothing was made for user 2: no seeding");
	check(config_menu_help() && config_selected_slot() == 0 &&
	      config_manip("rac1", "remember", 0) == 0 && !config_feature_auto("NPEA00385", "pr test"),
	      "and every per-user key reads its default, not the console's or user 1's");
	check(config_profile_resolve(2, PROFILE_SETUP_NONE) == 0, "the resolver agrees nothing is to change");
	config_set_selected_slot(5);
	check(config_profile_exists(2) && has_line(QWARK_PROFILEDIR "/user-00000002/config.txt", "selected.slot = 5"),
	      "user 2's first write makes their profile, with that write alone in it");
	check(!has_line(QWARK_PROFILEDIR "/user-00000002/config.txt", "menu.help") &&
	      !exists(QWARK_PROFILEDIR "/user-00000002/positions/rac1.txt"),
	      "and nothing copied");

	group("profiles: never in a game");
	check(boot("NPEA00385"), "RaC1 comes up as user 2");
	host_set_user(1);
	tick(400);
	check(config_profile_user() == 2, "user 1 signing in mid-game changes nothing");
	check(quit(), "back at the XMB");
	xmb_second();
	check(config_profile_user() == 1, "and the XMB's next second makes user 1's active");
	check(config_selected_slot() == 6 && config_menu_help(), "with user 1's settings");

	group("profiles: nobody signed in");
	host_set_user(0);
	xmb_second();
	check(config_profile_active() && config_profile_user() == 1, "user 1's stays at the user-select screen");
	host_set_user(0xFFFFFFFFu);
	xmb_second();
	check(config_profile_user() == 1, "and when the VSH answers a negative number");
	host_set_user(1);

	group("profiles: a combo override");
	config_set_combo(COMBO_SAVE_POSITION, PAD_SELECT | PAD_L1);
	mask_shared = config_combo(COMBO_SAVE_POSITION);
	check(mask_shared == (PAD_SELECT | PAD_L1) && !config_combo_overridden(COMBO_SAVE_POSITION),
	      "the console's mask, no override");
	mask_own = PAD_SELECT | PAD_R1;
	check(config_set_combo_override(COMBO_SAVE_POSITION, 1, mask_own) == ST_OK, "user 1 overrides it");
	check(config_combo(COMBO_SAVE_POSITION) == mask_own && config_combo_overridden(COMBO_SAVE_POSITION),
	      "the override wins");
	check(config_combo(COMBO_LOAD_POSITION) == config_get_u32("combo.load", 0) &&
	      !config_combo_overridden(COMBO_LOAD_POSITION),
	      "for that combo alone");
	check(has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "combo.save = 0x") &&
	      has_line(QWARK_CONFIG, "combo.save = 0x"),
	      "it is in the profile's file, beside the console's mask in config.txt");
	check(config_set_combo(COMBO_SAVE_POSITION, PAD_SELECT | PAD_L2) == ST_OK &&
	      config_combo(COMBO_SAVE_POSITION) == mask_own,
	      "COMBO_SET writes the console's mask, which the override hides");
	host_set_user(2);
	xmb_second();
	check(config_combo(COMBO_SAVE_POSITION) == (PAD_SELECT | PAD_L2) &&
	      !config_combo_overridden(COMBO_SAVE_POSITION),
	      "user 2 has no override and sees the console's new mask");
	host_set_user(1);
	xmb_second();
	check(config_combo(COMBO_SAVE_POSITION) == mask_own, "user 1 gets their override back");
	check(config_set_combo_override(COMBO_DIE, 1, 0) == ST_OK && config_combo(COMBO_DIE) == 0 &&
	      config_combo_overridden(COMBO_DIE),
	      "an override of 0 turns a combo off for this user");
	check(config_set_combo_override(COMBO_DIE, 0, 0) == ST_OK && !config_combo_overridden(COMBO_DIE),
	      "and goes again");
	check(config_set_combo_override(COMBO_DIE, 0, 0) == ST_OK, "removing one that is not there is OK");
	check(config_set_combo_override(COMBO_COUNT, 1, 1) == ST_BAD_ARG, "an action past the last is BAD_ARG");

	group("profiles: the switch off and on again");
	seq = config_profile_seq();
	config_set_profiles_enabled(0);
	tick(5);
	check(config_profile_active(), "a written switch waits for the resolver");
	r = config_profile_resolve(1, PROFILE_SETUP_NONE);
	check(r == 1 && !config_profile_active(), "off: no profile is active");
	check(config_profile_seq() == (u8)(seq + 1), "the sequence moved");
	check(config_selected_slot() == 3 && !config_menu_help(), "the console's settings are back");
	check(config_combo(COMBO_SAVE_POSITION) == (PAD_SELECT | PAD_L2) &&
	      !config_combo_overridden(COMBO_SAVE_POSITION),
	      "and its combos, with no override");
	check(config_set_combo_override(COMBO_SAVE_POSITION, 1, 1) == ST_NOT_FOUND,
	      "with no profile active an override is NOT_FOUND");
	check(config_profile_exists(1) && config_profile_exists(2), "both profiles are still there");
	config_set_u32("menu.help", 1);
	config_set_profiles_enabled(1);
	check(config_profile_resolve(1, PROFILE_SETUP_FRESH) == 1, "on again, asking for an empty profile");
	check(config_selected_slot() == 6 && config_combo(COMBO_SAVE_POSITION) == mask_own,
	      "an existing profile is never overwritten: user 1's slot and override are back");
	config_set_u32("menu.help", 0);
	check(!config_menu_help() && has_line(QWARK_CONFIG, "menu.help = 1"),
	      "a per-user write while it is on is the profile's, and config.txt keeps the console's");
	config_set_u32("menu.help", 1);

	group("profiles: from the XMB, the switch going off by hand");
	config_set_profiles_enabled(0);
	xmb_second();
	check(!config_profile_active(), "the XMB's next second finds the switch off");
	config_set_profiles_enabled(1);
	xmb_second();
	check(config_profile_active() && config_profile_user() == 1, "and on again finds user 1");

	group("profiles: a mod's auto flag follows the user");
	check(boot("NPEA00385"), "RaC1 comes up as user 1");
	if (mods_count() > 0 && mods_at(0) != NULL) mod_dir = mods_at(0)->dirname;
	check(mod_dir != NULL, "RaC1 has a mod in the fixtures");
	if (mod_dir != NULL) {
		check(mods_set_auto(mod_dir, 1) == ST_OK && (mods_auto_mask() & 1u),
		      "user 1 auto-loads it");
		check(quit(), "back at the XMB");
		host_set_user(2);
		xmb_second();
		check((mods_auto_mask() & 1u) == 0, "user 2 does not");
		host_set_user(1);
		xmb_second();
		check((mods_auto_mask() & 1u) != 0, "user 1 still does");
		check(boot("NPEA00385"), "RaC1 again");
		check((mods_loaded_mask() & 1u) != 0, "and the mod went in with the game, for user 1");
		mods_set_auto(mod_dir, 0);
		quit();
	}

	group("profiles: a write cut off before its rename");
	spill(QWARK_PROFILEDIR "/user-00000001/config.txt.new", "selected.slot = 4\n");
	plat_file_unlink(QWARK_PROFILEDIR "/user-00000001/config.txt");
	check(config_load() == ST_OK && config_selected_slot() == 4,
	      "a profile with only its .new reads the .new");
	config_set_selected_slot(4);
	check(exists(QWARK_PROFILEDIR "/user-00000001/config.txt") &&
	      !exists(QWARK_PROFILEDIR "/user-00000001/config.txt.new"),
	      "and the next write puts a whole file in its place");
	slurp(QWARK_CONFIG, buf, sizeof(buf));
	spill(QWARK_CONFIG ".new", buf);
	plat_file_unlink(QWARK_CONFIG);
	check(config_load() == ST_OK && config_profiles_enabled(), "so does the console's config.txt");
	config_set_u32("log", 1);
	check(exists(QWARK_CONFIG) && !exists(QWARK_CONFIG ".new"), "and no .new is left after a write");

	group("profiles: a folder that cannot be made");
	spill(QWARK_PROFILEDIR "/user-00000003", "not a folder");
	host_set_user(3);
	xmb_second();
	check(config_profile_user() == 3 && !config_profile_exists(3), "user 3 is active with nothing made");
	check(config_set_u32("selected.slot", 2) == ST_IO_ERROR, "a write that cannot make the folder is IO_ERROR");
	check(config_selected_slot() == 0, "and nothing is pretended");
	check(config_set_combo_override(COMBO_DIE, 1, 1) == ST_IO_ERROR, "an override too");
	plat_file_unlink(QWARK_PROFILEDIR "/user-00000003");
	host_set_user(1);
	xmb_second();

	group("profiles: RPCS3 has no users");
	host_set_has_users(0);
	check(config_profile_flags() == 0, "no flags at all");
	host_set_user(2);
	xmb_second();
	check(config_profile_user() == 1, "the XMB looks for no user");
	config_set_profiles_enabled(0);
	check(config_profile_resolve(2, PROFILE_SETUP_COPY) == 1 && !config_profile_active(),
	      "and the resolver turns the profile off rather than make one");
	check(config_profile_resolve(2, PROFILE_SETUP_COPY) == 0 && !config_profile_exists(3),
	      "even asked to copy");
	host_set_has_users(1);
	host_set_user(1);

	/* Back as it was. */
	config_profile_resolve(0, PROFILE_SETUP_NONE);
	if (had_cfg) spill(QWARK_CONFIG, before_cfg); else plat_file_unlink(QWARK_CONFIG);
	if (had_pos) spill(QWARK_POSDIR "/rac1.txt", before_pos); else plat_file_unlink(QWARK_POSDIR "/rac1.txt");
	config_load();
	forget_profile("user-00000001");
	forget_profile("user-00000002");
	forget_profile("user-00000003");
	group("profiles: the HDD put back");
	check(!config_profile_active() && !config_profiles_enabled(), "no profile, the switch off");
}

/* ------------------------------------------------------------------------
 * Build 56: a config.txt the size of everything qwark itself writes.
 *
 * Build 56's PROFILE_ENABLE answered a real console FULL: its config.txt held
 * the 64 keys the old table had room for, so `profiles.enabled` had none, and
 * a COPY would have copied at most 64 keys into a profile. This builds the
 * largest file qwark can write, from the games' own tables: the fixed keys,
 * the selections and manips of all four games, the auto flag of every toggle
 * and colour under every title id the games answer to (colours at their
 * widest value), and a full mod library, QWARK_MAX_MODS mods with the longest
 * dirnames, auto-flagged on each game's own title.
 */
#define BIG_MAX 24576

extern const struct game_api rac1_game;
extern const struct game_api rac2_game;
extern const struct game_api rac3_game;
extern const struct game_api rac4_game;

static char g_big[BIG_MAX];
static u32  g_big_len;
static u32  g_big_keys;
static u32  g_big_user;
static u32  g_big_pool;

static void big_add(const char *key, const char *value)
{
	char probe[CONFIG_KEY_MAX + 8];
	int n;

	snprintf(probe, sizeof(probe), "\n%s = ", key);
	if (strstr(g_big, probe) != NULL) return;   /* BCES01503 is three games' title */

	n = snprintf(g_big + g_big_len, BIG_MAX - g_big_len, "%s = %s\n", key, value);
	if (n > 0) g_big_len += (u32)n;
	g_big_keys++;
	g_big_pool += 1 + (u32)strlen(key) + 1 + (u32)strlen(value) + 1;
	if (config_key_scope(key) == CONFIG_SCOPE_USER) g_big_user++;
}

static void big_build(void)
{
	static const struct game_api * const games[4] = { &rac1_game, &rac2_game, &rac3_game, &rac4_game };
	static const char * const keys[4] = { "rac1", "rac2", "rac3", "rac4" };
	static const char * const fixed[] = {
		"combo.enabled", "menu.help", "menu.enabled", "log", "savefile_helper",
		"selected.slot", "selected.planet", "selected.planet_flags"
	};
	static const char * const combos[] = {
		"combo.save", "combo.load", "combo.die", "combo.planet", "combo.setaside",
		"combo.menu", "combo.fastloads"
	};
	char key[CONFIG_KEY_MAX + 16];
	char slug[48];
	int g, t, i;

	g_big_len = 0;
	g_big_keys = g_big_user = g_big_pool = 0;
	g_big_len = (u32)snprintf(g_big, BIG_MAX, "# everything qwark writes\n");

	for (i = 0; i < (int)(sizeof(combos) / sizeof(combos[0])); i++) big_add(combos[i], "0xFFFFFFFF");
	for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++) big_add(fixed[i], "1");

	for (g = 0; g < 4; g++) {
		const struct game_describe *d = games[g]->describe();

		snprintf(key, sizeof(key), "selected.%s.planet", keys[g]);       big_add(key, "255");
		snprintf(key, sizeof(key), "selected.%s.planet_flags", keys[g]); big_add(key, "255");
		snprintf(key, sizeof(key), "manip.%s.auto_reset", keys[g]);      big_add(key, "1");
		snprintf(key, sizeof(key), "manip.%s.remember", keys[g]);        big_add(key, "1");
		snprintf(key, sizeof(key), "manip.%s.category", keys[g]);        big_add(key, "4294967295");

		for (t = 0; t < 4 && game_title_of(games[g])->title_ids[t] != NULL; t++) {
			for (i = 0; i < d->nfeatures; i++) {
				const struct feature_desc *f = &d->features[i];

				if (f->kind != FEATURE_TOGGLE && f->kind != FEATURE_COLOR) continue;
				if ((f->flags & FEATURE_FLAG_LIVE) != 0) continue;
				qslug(f->label, slug, sizeof(slug));
				snprintf(key, sizeof(key), "%s.auto.%s", game_title_of(games[g])->title_ids[t], slug);
				big_add(key, f->kind == FEATURE_COLOR ? "2181038079" : "1");
			}
			for (i = 0; i < d->nformer; i++) {
				qslug(d->former[i].label, slug, sizeof(slug));
				snprintf(key, sizeof(key), "%s.auto.%s", game_title_of(games[g])->title_ids[t], slug);
				big_add(key, "1");
			}
		}

		for (i = 0; i < QWARK_MAX_MODS; i++) {
			char dir[32];
			snprintf(dir, sizeof(dir), "mod-%02d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
			dir[31] = 0;
			snprintf(key, sizeof(key), "%s.mod.%s.auto", game_title_of(games[g])->title_ids[0], dir);
			big_add(key, "1");
		}
	}
}

#define BIG_ALL      0
#define BIG_USER     1
#define BIG_CONSOLE  2

/*
 * How many of the big file's lines are in `path`, exactly, of the side asked
 * for; `*want` is how many there should be.
 */
static u32 big_lines_in(const char *path, int side, u32 *want)
{
	static char file[BIG_MAX + 2];
	char line[160];
	const char *p = g_big;
	u32 found = 0;

	*want = 0;
	file[0] = '\n';
	if (slurp(path, file + 1, sizeof(file) - 1) < 0) file[1] = 0;

	while (*p != 0) {
		const char *nl = strchr(p, '\n');
		u32 n = nl ? (u32)(nl - p) : (u32)strlen(p);
		char probe[170];
		char key[CONFIG_KEY_MAX + 8];
		const char *eq;
		int user;

		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, p, n);
		line[n] = 0;
		p = nl ? nl + 1 : p + n;
		if (line[0] == '#' || line[0] == 0) continue;

		eq = strstr(line, " = ");
		if (eq == NULL) continue;
		memcpy(key, line, (size_t)(eq - line));
		key[eq - line] = 0;
		user = config_key_scope(key) == CONFIG_SCOPE_USER;
		if (side == BIG_USER && !user) continue;
		if (side == BIG_CONSOLE && user) continue;

		(*want)++;
		snprintf(probe, sizeof(probe), "\n%s\n", line);
		if (strstr(file, probe) != NULL) found++;
	}
	return found;
}

/* Every key of the big file reads back through config_get with its value. */
static u32 big_reads_back(int side, u32 *want)
{
	const char *p = g_big;
	u32 ok = 0;

	*want = 0;
	while (*p != 0) {
		const char *nl = strchr(p, '\n');
		u32 n = nl ? (u32)(nl - p) : (u32)strlen(p);
		char line[160];
		const char *eq;
		const char *v;
		int user;

		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, p, n);
		line[n] = 0;
		p = nl ? nl + 1 : p + n;
		if (line[0] == '#' || line[0] == 0) continue;

		eq = strstr(line, " = ");
		if (eq == NULL) continue;
		line[eq - line] = 0;
		user = config_key_scope(line) == CONFIG_SCOPE_USER;
		if (side == BIG_USER && !user) continue;
		if (side == BIG_CONSOLE && user) continue;

		(*want)++;
		v = config_get(line);
		if (v != NULL && strcmp(v, eq + 3) == 0) ok++;
	}
	return ok;
}

/* A config.txt of filler keys whose pool comes to `bytes`, give or take a key. */
static void fill_config(u32 bytes)
{
	static char text[CONFIG_POOL_BYTES * 2];
	u32 len = 0;
	u32 pool = 0;
	u32 i = 0;

	text[0] = 0;
	while (pool < bytes) {
		char line[64];
		u32 klen, need;

		snprintf(line, sizeof(line), "fill.%05u", (unsigned)i++);
		klen = (u32)strlen(line);
		need = 1 + klen + 1 + 1 + 1;
		if (pool + need > bytes) {
			/* The last key pads to exactly `bytes`, with a longer value. */
			u32 room = bytes - pool;
			char value[64];
			if (room < 1 + klen + 1 + 1) break;
			memset(value, '7', sizeof(value));
			value[room - (1 + klen + 1 + 1) < 31 ? room - (1 + klen + 1 + 1) : 31] = 0;
			if (value[0] == 0) break;
			len += (u32)snprintf(text + len, sizeof(text) - len, "%s = %s\n", line, value);
			pool += 1 + klen + 1 + (u32)strlen(value) + 1;
			continue;
		}
		len += (u32)snprintf(text + len, sizeof(text) - len, "%s = 1\n", line);
		pool += need;
	}
	spill(QWARK_CONFIG, text);
}

static int same_file(const char *path, const char *text)
{
	static char now[BIG_MAX];
	if (slurp(path, now, sizeof(now)) < 0) return text == NULL;
	return text != NULL && strcmp(now, text) == 0;
}

static void test_big_config(void)
{
	static char before_cfg[BIG_MAX];
	static char before_pos[PR_FILE_MAX];
	static char snap[BIG_MAX];
	int had_cfg, had_pos;
	int changed = 0;
	u32 want = 0;
	u32 got;
	char msg[160];

	had_cfg = slurp(QWARK_CONFIG, before_cfg, sizeof(before_cfg)) >= 0;
	had_pos = slurp(QWARK_POSDIR "/rac1.txt", before_pos, sizeof(before_pos)) >= 0;
	forget_profile("user-00000001");
	forget_profile("user-00000002");
	forget_profile("user-00000003");
	host_set_has_users(1);
	host_set_user(1);
	if (session_state() != SESSION_XMB) quit();
	config_profile_resolve(0, PROFILE_SETUP_NONE);

	big_build();
	spill(QWARK_CONFIG, g_big);
	spill(QWARK_POSDIR "/rac1.txt", "5.2 = 0102030405\n");

	group("profiles: a config.txt of everything qwark writes");
	snprintf(msg, sizeof(msg), "the file is %u keys, %u of them per user: far past the old table's 64",
	         (unsigned)g_big_keys, (unsigned)g_big_user);
	check(g_big_keys > 200 && g_big_user > 180, msg);
	snprintf(msg, sizeof(msg), "and %u bytes of pool, %u bytes of file, inside the pool's %u",
	         (unsigned)g_big_pool, (unsigned)g_big_len, (unsigned)CONFIG_POOL_BYTES);
	check(g_big_pool + 512 < CONFIG_POOL_BYTES, msg);
	check(config_load() == ST_OK, "it loads whole");
	got = big_reads_back(BIG_ALL, &want);
	check_eq_u64(got, want, "every key reads back with its value");
	check(config_set_u32("pr.extra", 1) == ST_OK, "and there is room for more");

	group("profiles: on, with a copy of all of it");
	check(config_profiles_enable(1, PROFILE_SETUP_COPY, 1, &changed) == ST_OK && changed,
	      "PROFILE_ENABLE(1, COPY) is OK, where it was FULL");
	check(config_profile_active() && config_profile_exists(1) && config_profiles_enabled(),
	      "user 1's profile is made and active, the switch on");
	got = big_lines_in(QWARK_PROFILEDIR "/user-00000001/config.txt", BIG_USER, &want);
	check_eq_u64(got, want, "every per-user line is in the profile's config.txt");
	got = big_lines_in(QWARK_PROFILEDIR "/user-00000001/config.txt", BIG_CONSOLE, &want);
	check_eq_u64(got, 0, "and none of the console's");
	got = big_lines_in(QWARK_CONFIG, BIG_ALL, &want);
	check_eq_u64(got, want, "config.txt still has every line it had");
	check(has_line(QWARK_CONFIG, "profiles.enabled = 1"), "and the switch");
	got = big_reads_back(BIG_ALL, &want);
	check_eq_u64(got, want, "every key reads the same through the profile");
	check(has_line(QWARK_PROFILEDIR "/user-00000001/positions/rac1.txt", "5.2 = 0102030405"),
	      "and the positions came too");

	group("profiles: writes with all of it in");
	check(config_set_feature_auto("NPEA00423", "HUD colour", 0x80000001u) == ST_OK &&
	      has_line(QWARK_PROFILEDIR "/user-00000001/config.txt", "NPEA00423.auto.hud_colour = 2147483649"),
	      "a colour kept for the boot goes into the profile");
	check(config_set_u32("log", 0) == ST_OK && has_line(QWARK_CONFIG, "log = 0"),
	      "a console key into config.txt");
	got = big_lines_in(QWARK_CONFIG, BIG_USER, &want);
	check_eq_u64(got, want, "which carries the console's own per-user lines through the write");
	check(has_line(QWARK_CONFIG, "NPEA00423.auto.hud_colour = 2181038079"),
	      "the console's colour among them, not user 1's");
	config_set_u32("log", 1);
	{
		u8 a;
		int ok = 1;
		for (a = 0; a < COMBO_COUNT; a++)
			if (config_set_combo_override(a, 1, 0x1000u + a) != ST_OK || !config_combo_overridden(a)) ok = 0;
		check(ok, "COMBO_OVERRIDE on all seven combos");
		for (a = 0; a < COMBO_COUNT; a++)
			if (config_set_combo_override(a, 0, 0) != ST_OK || config_combo_overridden(a)) ok = 0;
		check(ok, "and off all seven again");
	}

	group("profiles: switching between two users who each have all of it");
	host_set_user(2);
	xmb_second();
	check(config_profile_user() == 2 && !config_profile_exists(2), "user 2 at the XMB, not made");
	check(config_profiles_enable(1, PROFILE_SETUP_COPY, 2, &changed) == ST_OK && config_profile_exists(2),
	      "user 2 takes a copy too");
	config_set_feature_auto("NPEA00423", "HUD colour", 0x80000002u);
	host_set_user(1);
	xmb_second();
	host_set_user(2);
	xmb_second();
	host_set_user(1);
	xmb_second();
	check(config_profile_user() == 1 && config_feature_auto("NPEA00423", "HUD colour") == 0x80000001u,
	      "user 1 back, with their own colour");
	got = big_reads_back(BIG_CONSOLE, &want);
	check_eq_u64(got, want, "and the console's keys");
	check(config_set_u32("selected.slot", 4) == ST_OK, "with room to write");
	got = big_lines_in(QWARK_PROFILEDIR "/user-00000002/config.txt", BIG_USER, &want);
	check(got + 1 == want && has_line(QWARK_PROFILEDIR "/user-00000002/config.txt",
	      "NPEA00423.auto.hud_colour = 2147483650"), "user 2's profile is whole, their colour in it");

	group("profiles: off again, all of it back");
	check(config_profiles_enable(0, 0, 1, &changed) == ST_OK && !config_profile_active(),
	      "PROFILE_ENABLE(0) turns it off");
	got = big_reads_back(BIG_USER, &want);
	check_eq_u64(got, want, "every per-user key is the console's again");
	got = big_lines_in(QWARK_CONFIG, BIG_ALL, &want);
	check_eq_u64(got, want, "and config.txt has every line it started with");

	group("profiles: a setup that fails leaves nothing");
	slurp(QWARK_CONFIG, snap, sizeof(snap));
	spill(QWARK_PROFILEDIR "/user-00000003", "not a folder");
	host_set_user(3);
	check(config_profiles_enable(1, PROFILE_SETUP_COPY, 3, &changed) == ST_IO_ERROR,
	      "a profile whose folder cannot be made is IO_ERROR");
	check(!config_profile_active() && !config_profiles_enabled() && !config_profile_exists(3),
	      "no profile, the switch off");
	check(same_file(QWARK_CONFIG, snap), "config.txt exactly as it was");
	got = big_reads_back(BIG_USER, &want);
	check_eq_u64(got, want, "and the console's settings all there");
	plat_file_unlink(QWARK_PROFILEDIR "/user-00000003");

	plat_dir_create(QWARK_PROFILEDIR "/user-00000003");
	spill(QWARK_PROFILEDIR "/user-00000003/positions", "not a folder");
	check(config_profiles_enable(1, PROFILE_SETUP_COPY, 3, &changed) == ST_IO_ERROR,
	      "a copy whose positions cannot be written is IO_ERROR");
	check(!config_profile_active() && !config_profiles_enabled() && !config_profile_exists(3) &&
	      !exists(QWARK_PROFILEDIR "/user-00000003/config.txt"),
	      "and takes away the config.txt it had written");
	check(same_file(QWARK_CONFIG, snap), "config.txt exactly as it was");
	plat_file_unlink(QWARK_PROFILEDIR "/user-00000003/positions");
	plat_dir_remove(QWARK_PROFILEDIR "/user-00000003");
	host_set_user(1);

	group("profiles: a config.txt with no room for the switch");
	fill_config(CONFIG_POOL_BYTES - 1);
	slurp(QWARK_CONFIG, snap, sizeof(snap));
	check(config_load() == ST_OK, "a config.txt that fills the pool to the byte loads");
	check(config_profiles_enable(1, PROFILE_SETUP_FRESH, 1, &changed) == ST_FULL,
	      "PROFILE_ENABLE is FULL");
	check(!config_profile_active() && !config_profiles_enabled(), "with nothing active and the switch off");
	check(same_file(QWARK_CONFIG, snap), "and config.txt as it was");

	group("profiles: a config.txt bigger than the pool");
	fill_config(CONFIG_POOL_BYTES + 600);
	slurp(QWARK_CONFIG, snap, sizeof(snap));
	check(config_load() == ST_FULL, "loads what fits and says FULL");
	check(config_get("fill.00000") != NULL, "what fits reads");
	check(config_set_u32("fill.00000", 2) == ST_FULL && config_save() == ST_FULL,
	      "every write is FULL, even to a key it holds");
	check(config_profiles_enable(1, PROFILE_SETUP_COPY, 1, &changed) == ST_FULL,
	      "PROFILE_ENABLE too");
	check(same_file(QWARK_CONFIG, snap), "so the part that did not fit is never cut out of the file");

	/* Back as it was. */
	config_profile_resolve(0, PROFILE_SETUP_NONE);
	if (had_cfg) spill(QWARK_CONFIG, before_cfg); else plat_file_unlink(QWARK_CONFIG);
	if (had_pos) spill(QWARK_POSDIR "/rac1.txt", before_pos); else plat_file_unlink(QWARK_POSDIR "/rac1.txt");
	config_load();
	forget_profile("user-00000001");
	forget_profile("user-00000002");
	forget_profile("user-00000003");
	group("profiles: the HDD put back after the big config");
	check(!config_profile_active() && !config_profiles_enabled(), "no profile, the switch off");
}

void test_profiles(void)
{
	test_scopes();
	test_flow();
	test_big_config();
}
