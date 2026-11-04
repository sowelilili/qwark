/*
 * Revision 1.30 (pending): the run-start warnings (src/core/runwarn.h), on
 * qwark's fake console. The notification's text; a mod's `#- legal:` line
 * (true, false, missing, garbage) as MOD_LIST carries it; at each of the four
 * original games' run starts, the autosplitter's START from its own watcher,
 * the cheats its table counts and nothing else; a death or another edge
 * saying nothing; the illegal mods at every run start and the unknown ones
 * once; and `run.warnings` (RUN_WARN) switching it all off. Tools of
 * Destruction's run start is test_tod_game.c's, which checks it there.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/autosplit.h"
#include "../src/core/runwarn.h"
#include "../src/core/mods.h"
#include "../src/core/util.h"
#include "../src/games/game.h"
#include "../src/games/rac1.h"
#include "../src/games/rac2.h"
#include "../src/games/rac3.h"
#include "../src/games/rac4.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define T1   "NPEA00385"
#define HDD  "/dev_hdd0/qwark/mods/" T1

/* What the four watchers read (test_qwark.c's names). */
#define A1_GAME_STATE   0xA10708u
#define A1_PLANET       0x969C70u
#define A2_PLANET       0x1329A3Cu
#define A2_PLAYER_STATE 0x1481474u
#define A3_PLANET       0x00C1E438u
#define A3_GAME_STATE   0x00EE9334u
#define A4_REQUEST_LOAD 0x00B36DCCu
#define A4_TARGET       0x00B36DD0u
#define A4_TUTORIAL     0x00B1F46Cu

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

static void mod(const char *dir, const char *patch)
{
	char folder[256];
	snprintf(folder, sizeof(folder), "%s/%s", HDD, dir);
	put(folder, "patch.txt", patch);
}

static void rm_mod(const char *dir)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s/patch.txt", HDD, dir);
	plat_file_unlink(p);
	snprintf(p, sizeof(p), "%s/%s", HDD, dir);
	plat_dir_remove(p);
}

static const char *const g_dirs[] = { "rw-legal", "rw-illegal", "rw-none", "rw-junk", "rw-empty", "rw-new", "rw-off" };
#define NDIRS (sizeof(g_dirs) / sizeof(g_dirs[0]))

static u8 flags_of(const char *dir)
{
	int i = mods_find(dir);
	return i < 0 ? 0xFF : mods_at((u32)i)->flags;
}

/* The flags MOD_LIST sends for `dir`, 0xFF when it sends no such row. */
static u8 wire_flags_of(const char *dir)
{
	static u8 out[1 + 64 * MOD_WIRE_SIZE];
	u32 len = 0, k;

	if (mods_list_encode(out, sizeof(out), &len) != ST_OK) return 0xFF;
	for (k = 0; k < out[0]; k++) {
		const u8 *row = out + 1 + k * MOD_WIRE_SIZE;
		if (strncmp((const char *)row + 8, dir, 32) == 0) return row[1];
	}
	return 0xFF;
}

static u8 info_flags3(void)
{
	u8 info[SESSION_INFO_SIZE];
	tick(16);   /* the block is published every few ticks */
	session_info_copy(info, sizeof(info));
	return info[31];
}

/*
 * Each game's run start, as its autosplit watcher sees one (test_qwark.c's
 * test_autosplit drives the same edges). Returns 1 when an event went out.
 */
static int run_start(u8 game)
{
	u32 seq = autosplit_latest_seq();

	switch (game) {
	case GAME_RAC1:
		poke32(A1_PLANET, 0);
		poke32(A1_GAME_STATE, 6);
		tick(2);
		poke32(A1_GAME_STATE, 0);
		tick(2);
		break;
	case GAME_RAC2:
		poke32(A2_PLANET, 0);
		poke32(A2_PLAYER_STATE, 0);
		tick(2);
		poke32(A2_PLAYER_STATE, 98);
		tick(2);
		poke32(A2_PLAYER_STATE, 0);
		tick(2);
		break;
	case GAME_RAC3:
		poke32(A3_PLANET, 1);
		poke32(A3_GAME_STATE, 6);
		tick(2);
		poke32(A3_GAME_STATE, 0);
		tick(2);
		break;
	case GAME_RAC4:
		poke32(A4_TARGET, 1);
		poke32(A4_TUTORIAL, 0);
		poke32(A4_REQUEST_LOAD, 0);
		tick(2);
		poke32(A4_REQUEST_LOAD, 1);
		tick(2);
		poke32(A4_REQUEST_LOAD, 0);
		tick(2);
		break;
	}
	return autosplit_latest_seq() != seq;
}

static int notified_since(u32 count, const char *text)
{
	return host_notify_count() == count + 1 && strcmp(host_notify_last(), text) == 0;
}

static void test_compose(void)
{
	char out[160];

	group("run-start warnings: the notification's text");

	check(runwarn_compose(out, sizeof(out), 0, 0, 0) == 0 && out[0] == 0, "nothing to say: no line, no text");
	check(runwarn_compose(out, sizeof(out), 1, 0, 0) == 1 && strcmp(out, "Warning: 1 cheat enabled") == 0,
	      "one cheat: \"Warning: 1 cheat enabled\"");
	check(runwarn_compose(out, sizeof(out), 3, 0, 0) == 1 && strcmp(out, "Warning: 3 cheats enabled") == 0,
	      "three: \"Warning: 3 cheats enabled\"");
	check(runwarn_compose(out, sizeof(out), 0, 1, 0) == 1 && strcmp(out, "Warning: 1 mod enabled") == 0,
	      "an illegal mod alone: \"Warning: 1 mod enabled\"");
	check(runwarn_compose(out, sizeof(out), 0, 0, 2) == 1 &&
	      strcmp(out, "Warning: 2 mods of unknown legality enabled") == 0,
	      "unknown ones alone: \"Warning: 2 mods of unknown legality enabled\"");
	check(runwarn_compose(out, sizeof(out), 2, 1, 1) == 3 &&
	      strcmp(out, "Warning: 2 cheats enabled\nWarning: 1 mod enabled\nWarning: 1 mod of unknown legality enabled") == 0,
	      "all three: one notification, a line each, cheats first");
	check(runwarn_compose(out, 20, 2, 1, 1) == 3 && strlen(out) == 19, "a short buffer is cut, never overrun");
}

static void test_legality(void)
{
	u32 i;

	group("run-start warnings: a mod's #- legal: line");

	for (i = 0; i < NDIRS; i++) rm_mod(g_dirs[i]);
	mod("rw-legal",   "#- name: RW Legal\n#- Legal: true\n0x00880000: 0x60000000\n");
	mod("rw-illegal", "#- name: RW Illegal\n#- legal:  FALSE \n0x00880010: 0x60000000\n");
	mod("rw-none",    "#- name: RW None\n0x00880020: 0x60000000\n");
	mod("rw-junk",    "#- name: RW Junk\n#- legal: maybe\n0x00880030: 0x60000000\n");
	mod("rw-empty",   "#- name: RW Empty\n#- legal:\n#- legal: True\n#- legal: yes\n0x00880040: 0x60000000\n");
	check(mods_rescan() == ST_OK, "rescan with five new mods");

	check((flags_of("rw-legal") & (MOD_FLAG_LEGAL | MOD_FLAG_ILLEGAL)) == MOD_FLAG_LEGAL, "Legal: true is LEGAL, the key in any case");
	check((flags_of("rw-illegal") & (MOD_FLAG_LEGAL | MOD_FLAG_ILLEGAL)) == MOD_FLAG_ILLEGAL,
	      "legal: FALSE, spaced and in capitals, is ILLEGAL");
	check((flags_of("rw-none") & (MOD_FLAG_LEGAL | MOD_FLAG_ILLEGAL)) == 0, "no line: unknown, neither bit");
	check((flags_of("rw-junk") & (MOD_FLAG_LEGAL | MOD_FLAG_ILLEGAL)) == 0, "legal: maybe: unknown");
	check((flags_of("rw-empty") & (MOD_FLAG_LEGAL | MOD_FLAG_ILLEGAL)) == 0,
	      "the last line wins: empty, True, then yes is unknown");
	check((flags_of("rw-legal") & MOD_FLAG_PARSE_ERROR) == 0 && (flags_of("rw-junk") & MOD_FLAG_PARSE_ERROR) == 0,
	      "a legal line is never a parse error, whatever it says");
	check((wire_flags_of("rw-legal") & 0xC0) == MOD_FLAG_LEGAL && (wire_flags_of("rw-illegal") & 0xC0) == MOD_FLAG_ILLEGAL &&
	      (wire_flags_of("rw-none") & 0xC0) == 0 && (wire_flags_of("rw-junk") & 0xC0) == 0,
	      "MOD_LIST carries them in flags bit6 LEGAL and bit7 ILLEGAL");
	check((wire_flags_of("flight") & 0xC0) == 0, "a fixture mod with no line: unknown on the wire too");
}

static void set_on(u8 id, const char *what)
{
	check(features_set(id, 1) == ST_OK && (features_toggle_state() & ((u64)1 << id)) != 0, what);
}

static void test_rac1(void)
{
	u32 n, starts, i;

	group("run-start warnings: RaC1, cheats, mods and the switch");

	check(quit() && boot(T1), "RaC1 boots");
	runwarn_forget();
	config_set_run_warnings(1);
	check((info_flags3() & (SESSION_FLAG3_RUN_WARN | SESSION_FLAG3_RUN_WARN_OFF)) == SESSION_FLAG3_RUN_WARN,
	      "SessionInfo flags3: bit0 the warnings are here, bit1 clear, on by default");

	n = host_notify_count();
	starts = runwarn_starts();
	check(run_start(GAME_RAC1) && runwarn_starts() == starts + 1, "Veldin's game-state edge: START, and one run start");
	check(host_notify_count() == n, "nothing on: nothing said");

	test_legality();
	group("run-start warnings: RaC1's run starts with cheats and mods");

	set_on(F_GHOST, "Ghost Ratchet on");
	set_on(F_FREEZE_HEALTH, "Freeze health on");
	set_on(F_DBG_RATCHET, "Update Ratchet on (Debug, not a cheat)");
	n = host_notify_count();
	check(run_start(GAME_RAC1), "a run start");
	check(runwarn_last_cheats() == 2 && notified_since(n, "Warning: 2 cheats enabled"),
	      "\"Warning: 2 cheats enabled\": Ghost and Freeze health, not the Debug toggle");

	/* A death, and a game-state change that is not the run's, are no run start. */
	n = host_notify_count();
	starts = runwarn_starts();
	check(features_trigger(F_DIE) == ST_OK, "Die");
	tick(60);
	poke32(A1_GAME_STATE, 2);
	tick(4);
	poke32(A1_GAME_STATE, 0);
	tick(4);
	check(runwarn_starts() == starts && host_notify_count() == n, "a death and a state change off 6: no run start, nothing said");

	/* Mods: the illegal one every time, the unknown ones once. */
	check(mods_load("rw-legal") == ST_OK && mods_load("rw-illegal") == ST_OK &&
	      mods_load("rw-none") == ST_OK && mods_load("rw-junk") == ST_OK, "four mods loaded");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(runwarn_last_illegal() == 1 && runwarn_last_unknown() == 2 &&
	      notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled\nWarning: 2 mods of unknown legality enabled"),
	      "one notification: 2 cheats, 1 illegal mod, 2 of unknown legality; the legal one is not counted");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled"),
	      "the next run start: the cheats and the illegal mod again, the unknown ones not");
	check(mods_unload("rw-junk") == ST_OK && mods_load("rw-junk") == ST_OK, "an unknown one unloaded and loaded again");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled"), "the same set: still not again");
	check(mods_rescan() == ST_OK, "a rescan");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled"), "nor after a rescan");

	mod("rw-new", "#- name: RW New\n0x00880050: 0x60000000\n");
	check(mods_rescan() == ST_OK && mods_load("rw-new") == ST_OK, "a new unknown mod loaded");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(runwarn_last_unknown() == 3 &&
	      notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled\nWarning: 3 mods of unknown legality enabled"),
	      "the set grew: the unknown line again, counting all three");

	/* The switch. */
	check(config_set_run_warnings(2) == ST_BAD_ARG, "run.warnings takes 0 or 1 alone");
	check(config_set_run_warnings(0) == ST_OK && !config_run_warnings(), "run.warnings = 0");
	check((info_flags3() & (SESSION_FLAG3_RUN_WARN | SESSION_FLAG3_RUN_WARN_OFF)) ==
	      (SESSION_FLAG3_RUN_WARN | SESSION_FLAG3_RUN_WARN_OFF), "flags3 bit1 says so");
	mod("rw-off", "#- name: RW Off\n0x00880060: 0x60000000\n");
	check(mods_rescan() == ST_OK && mods_load("rw-off") == ST_OK, "another unknown mod loaded while off");
	n = host_notify_count();
	starts = runwarn_starts();
	check(run_start(GAME_RAC1) && runwarn_starts() == starts + 1 && host_notify_count() == n,
	      "a run start with the warnings off: counted, nothing said");
	check(config_set_run_warnings(1) == ST_OK && config_run_warnings(), "run.warnings = 1");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(runwarn_last_unknown() == 4 &&
	      notified_since(n, "Warning: 2 cheats enabled\nWarning: 1 mod enabled\nWarning: 4 mods of unknown legality enabled"),
	      "on again: the mod loaded while off was never marked, so the unknown line comes back");

	/* All off. */
	for (i = 0; i < NDIRS; i++) mods_unload(g_dirs[i]);
	features_set(F_GHOST, 0);
	features_set(F_FREEZE_HEALTH, 0);
	features_set(F_DBG_RATCHET, 0);
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(runwarn_last_cheats() == 0 && runwarn_last_illegal() == 0 && runwarn_last_unknown() == 0 &&
	      host_notify_count() == n, "everything off again: nothing said");

	/* A legal mod alone is nothing to warn about. */
	check(mods_load("rw-legal") == ST_OK, "the legal mod alone");
	n = host_notify_count();
	run_start(GAME_RAC1);
	check(host_notify_count() == n, "nothing said");
	mods_unload("rw-legal");

	for (i = 0; i < NDIRS; i++) rm_mod(g_dirs[i]);
	mods_rescan();
}

static void test_game(const char *title, u8 game, const char *name, const u8 *ids, u32 nids,
                      const u8 *not_ids, u32 nnot, const char *want)
{
	char what[160];
	u32 n, i;

	snprintf(what, sizeof(what), "run-start warnings: %s", name);
	group(what);

	check(quit() && boot(title), "boots");
	for (i = 0; i < nids; i++) {
		snprintf(what, sizeof(what), "cheat id %u on", (unsigned)ids[i]);
		set_on(ids[i], what);
	}
	for (i = 0; i < nnot; i++) {
		snprintf(what, sizeof(what), "toggle id %u on, which is no cheat", (unsigned)not_ids[i]);
		set_on(not_ids[i], what);
	}
	n = host_notify_count();
	check(run_start(game), "the watcher's START");
	snprintf(what, sizeof(what), "\"%s\"", want);
	check(notified_since(n, want), what);
	for (i = 0; i < nids; i++) features_set(ids[i], 0);
	for (i = 0; i < nnot; i++) features_set(not_ids[i], 0);
	n = host_notify_count();
	run_start(game);
	check(host_notify_count() == n, "the same with them off: nothing");
}

void test_runwarn(void)
{
	static const u8 r2[] = { R2_GHOST, R2_DEATH_BOSSES, R2_FREEZE_HEALTH };
	static const u8 r3[] = { R3_GHOST, R3_QS_PAUSE };
	static const u8 r4[] = { R4_GHOST, R4_FAST_LOADS };
	static const u8 r4_not[] = { R4_HUD_CORRECT };

	test_compose();
	test_rac1();
	/* Deadlocked's crash patches and softlock fix are on out of the box and count for nothing. */
	test_game("NPEA00386", GAME_RAC2, "RaC2", r2, 3, NULL, 0, "Warning: 3 cheats enabled");
	test_game("NPEA00387", GAME_RAC3, "UYA", r3, 2, NULL, 0, "Warning: 2 cheats enabled");
	test_game("NPEA00423", GAME_RAC4, "Deadlocked", r4, 2, r4_not, 1, "Warning: 2 cheats enabled");
	check(quit(), "quit");
}
