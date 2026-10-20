/*
 * Revision 1.23, build 55: combo action 6, TOGGLE_FAST_LOADS, on qwark's fake
 * console, in all four games: no mask out of the box, the game's Fast loads
 * toggle flipped through FEATURE_SET's own code (the same writes a client's
 * checkbox makes), a console notification each way, the COMBO_ENABLE switch
 * holding it off, and under RPCS3 without the code switches the checkbox's own
 * refusal, said on the console.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/config.h"
#include "../src/core/features.h"
#include "../src/core/util.h"
#include "../src/core/menu.h"
#include "../src/games/game.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* Select + Triangle + Left: no other combo of the suite's. */
#define FL_MASK (PAD_SELECT | PAD_TRIANGLE | PAD_LEFT)

/* At most this many writes of one FEATURE_SET are compared with the combo's. */
#define FL_WRITES 64

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

static void press(u32 mask)
{
	host_set_pad(0);
	tick(2);
	host_set_pad(mask);
	tick(2);
	host_set_pad(0);
	tick(2);
}

/* The running game's Fast loads id, or -1 when game.h names none or names another row. */
static int fast_loads_id(void)
{
	const struct game_api *g = session_game();
	const struct game_describe *dsc;
	u8 i;

	if (g == NULL || g->fast_loads == 0 || g->describe == NULL) return -1;
	dsc = g->describe();
	for (i = 0; i < dsc->nfeatures; i++) {
		const struct feature_desc *f = &dsc->features[i];
		if (f->id != g->fast_loads - 1) continue;
		if (f->kind != FEATURE_TOGGLE || strcmp(f->label, "Fast loads") != 0) return -1;
		if ((f->flags & FEATURE_FLAG_WRITES_CODE) == 0) return -1;
		return f->id;
	}
	return -1;
}

static int is_on(int id)
{
	return (features_toggle_state() & ((u64)1 << id)) != 0;
}

/* The write log as (address, length) pairs, at most FL_WRITES of them. */
static u32 take_log(u32 *addr, u32 *len)
{
	u32 n = host_write_log_count(), k;

	if (n > FL_WRITES) n = FL_WRITES;
	for (k = 0; k < n; k++) host_write_log_at(k, &addr[k], &len[k]);
	return n;
}

/* Whether every write of `a` is in `b` too, in the same order (b may have others between). */
static int writes_within(const u32 *aa, const u32 *al, u32 na, const u32 *ba, const u32 *bl, u32 nb)
{
	u32 i = 0, k;

	for (k = 0; k < nb && i < na; k++) {
		if (ba[k] == aa[i] && bl[k] == al[i]) i++;
	}
	return i == na;
}

static void fast_loads_combo(const char *title, const char *name)
{
	static u32 sa[FL_WRITES], sl[FL_WRITES], ca[FL_WRITES], cl[FL_WRITES];
	u32 ns, nc, n0;
	char what[160];
	int id;

	snprintf(what, sizeof(what), "%s boots", name);
	check(boot(title), what);
	id = fast_loads_id();
	snprintf(what, sizeof(what), "%s names its Fast loads toggle for the combo (id %d)", name, id);
	check(id >= 0, what);
	if (id < 0) {
		quit();
		return;
	}

	/* What the client's checkbox writes, on and off. */
	host_write_log_reset();
	check(features_set((u8)id, 1) == ST_OK && is_on(id), "the checkbox's FEATURE_SET turns fast loads on");
	ns = take_log(sa, sl);
	check(ns > 0, "and writes the game's fast-load words");
	features_set((u8)id, 0);
	check(!is_on(id), "and off again");

	/* The combo, from off. */
	n0 = host_notify_count();
	host_write_log_reset();
	press(FL_MASK);
	nc = take_log(ca, cl);
	check(is_on(id) && host_notify_count() == n0 + 1 && strcmp(host_notify_last(), "Fast loads on") == 0,
	      "the combo turns them on, and the console says \"Fast loads on\"");
	check(writes_within(sa, sl, ns, ca, cl, nc), "with the very writes the checkbox makes");
	n0 = host_notify_count();
	press(FL_MASK);
	check(!is_on(id) && host_notify_count() == n0 + 1 && strcmp(host_notify_last(), "Fast loads off") == 0,
	      "again: off, and \"Fast loads off\"");

	/* The switch over every combo holds this one off too. */
	config_set_combo_enabled(0);
	tick(2);
	n0 = host_notify_count();
	press(FL_MASK);
	check(!is_on(id) && host_notify_count() == n0, "with the combos switched off it does nothing");
	config_set_combo_enabled(1);
	tick(2);
	press(FL_MASK);
	check(is_on(id), "switched back on, it works");
	press(FL_MASK);
	check(quit(), "quit");
}

void test_combo_fastloads(void)
{
	u8 info[SESSION_INFO_SIZE];
	int id;
	u32 n0;

	group("fast-loads combo: the action");
	check(COMBO_TOGGLE_FAST_LOADS == 6 && COMBO_COUNT == 7, "action 6, of seven");
	check(config_get("combo.fastloads") == NULL && config_combo(COMBO_TOGGLE_FAST_LOADS) == 0,
	      "no mask out of the box");
	check(config_set_combo(COMBO_TOGGLE_FAST_LOADS, FL_MASK) == ST_OK &&
	      config_combo(COMBO_TOGGLE_FAST_LOADS) == FL_MASK &&
	      config_get("combo.fastloads") != NULL, "COMBO_SET stores it as combo.fastloads");
	check(config_set_combo(COMBO_COUNT, FL_MASK) == ST_BAD_ARG, "an eighth action is BAD_ARG");
	/* The client's own menu combo is out of the way; menu installs are not the point here. */
	menu_set_install_delay_us(600000000ull);

	group("fast-loads combo: Ratchet & Clank");
	fast_loads_combo("NPEA00385", "RaC1");
	group("fast-loads combo: Going Commando");
	fast_loads_combo("NPEA00386", "RaC2");
	group("fast-loads combo: Up Your Arsenal");
	fast_loads_combo("NPEA00387", "UYA");
	group("fast-loads combo: Deadlocked");
	fast_loads_combo("NPEA00423", "Deadlocked");

	/* RPCS3 without the code switches: the checkbox's UNSUPPORTED, said on the console. */
	group("fast-loads combo: under RPCS3");
	host_set_can_patch_code(0);
	check(boot("NPEA00423"), "Deadlocked boots under RPCS3");
	tick(8);
	session_info_copy(info, sizeof(info));
	id = fast_loads_id();
	check(id >= 0 && (info[24] & SESSION_FLAG_NO_CODE_PATCHES) != 0 && (info[24] & SESSION_FLAG_CODE_SWITCHES) == 0,
	      "no code patches and no code switches");
	check(id >= 0 && features_set((u8)id, 1) == ST_UNSUPPORTED, "the checkbox's FEATURE_SET is UNSUPPORTED");
	n0 = host_notify_count();
	host_write_log_reset();
	press(FL_MASK);
	check(id >= 0 && !is_on(id) && host_notify_count() == n0 + 1 &&
	      strstr(host_notify_last(), "RPCS3") != NULL && strncmp(host_notify_last(), "Fast loads: ", 12) == 0,
	      "the combo is refused the same way, and the console says it needs qwark's RPCS3 patches");
	check(quit(), "quit");
	host_set_can_patch_code(1);

	/* A game that names no toggle for it says so: the tests' own game names none. */
	group("fast-loads combo: a game with no Fast loads");
	check(boot("TEST00001"), "the tests' own game boots");
	n0 = host_notify_count();
	press(FL_MASK);
	check(host_notify_count() == n0 + 1 && strcmp(host_notify_last(), "Fast loads: not in this game") == 0,
	      "the combo says it has nothing to flip");
	check(quit(), "quit");

	check(config_set_combo(COMBO_TOGGLE_FAST_LOADS, 0) == ST_OK && config_combo(COMBO_TOGGLE_FAST_LOADS) == 0,
	      "COMBO_SET 0 turns it off again");
	menu_set_install_delay_us(3000000ull);
}
