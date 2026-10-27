/*
 * Game modules and the memory gate (revision 1.26; src/core/gamemod.c,
 * PROTOCOL.md 5.1.2 and 9.11), against the simulator's loader: a module file
 * on the fake HDD, its pages taken off the fake VSH's memory while it is
 * loaded, and its vtable overwritten with 0xA5 bytes once it is unloaded, so a
 * pointer the core kept past it faults here instead of reaching the game that
 * is still linked in.
 *
 * Every test turns modules on and leaves them off again, so the rest of the
 * suite runs the linked games it always ran.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/gamemod.h"
#include "../src/core/features.h"
#include "../src/core/manips.h"
#include "../src/core/mem.h"
#include "../src/core/config.h"
#include "../src/core/autosplit.h"
#include "../src/core/util.h"
#include "../src/games/game.h"
#include "../src/games/titles.h"
#include "../src/games/rac1.h"
#include "../src/games/rac4.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define KB 1024u
#define TICK_US 8334u
#define TICKS_PER_S 120

static void step(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(TICK_US);
		session_step_once();
	}
}

static int step_until(u8 want, int max)
{
	int i;
	for (i = 0; i < max && session_state() != want; i++) step(1);
	return session_state() == want;
}

/* Until a game's code is here (a module's arrival), at most `max` ticks. Answers the ticks taken, -1 never. */
static int until_game(int max)
{
	int i;
	for (i = 0; i < max; i++) {
		if (session_game() != NULL) return i;
		step(1);
	}
	return session_game() != NULL ? max : -1;
}

static int boot(const char *title)
{
	host_boot(title);
	return step_until(SESSION_INGAME, 4000);
}

static int quit(void)
{
	host_quit();
	return step_until(SESSION_XMB, 2000);
}

struct gm {
	u8 state, game, pages, flags;
	u32 lowest, now, need, floor, window, waited, error, load_us, module_id;
};

static void info(struct gm *g)
{
	u8 b[GAME_MODULE_SIZE + 8];

	memset(g, 0, sizeof(*g));
	check_eq_u64(gamemod_info(b, sizeof(b)), GAME_MODULE_SIZE, "GAME_MODULE is 40 bytes");
	g->state = b[0]; g->game = b[1]; g->pages = b[2]; g->flags = b[3];
	g->lowest = be32_get(b + 4); g->now = be32_get(b + 8); g->need = be32_get(b + 12);
	g->floor = be32_get(b + 16); g->window = be32_get(b + 20); g->waited = be32_get(b + 24);
	g->error = be32_get(b + 28); g->load_us = be32_get(b + 32); g->module_id = be32_get(b + 36);
}

/* SessionInfo as a client reads it, published every fourth tick. */
static u8 info_byte(u32 at)
{
	u8 b[SESSION_INFO_SIZE];

	step(4);
	session_info_copy(b, sizeof(b));
	return b[at];
}

static void module(u8 game)
{
	char path[48];

	title_module_path(game, path, sizeof(path), 0);
	host_write_fake_module(path, game, QWARK_BUILD, QWARK_BUILD_ID, 1, 0);
}

static void module_gone(u8 game)
{
	char path[48];

	title_module_path(game, path, sizeof(path), 0);
	plat_file_unlink(path);
}

static int feature_patches(void)
{
	u32 i, n = 0;
	for (i = 0; i < patch_count(); i++) {
		const struct patch_def *d = patch_at(i);
		if (d != NULL && d->kind == PATCH_KIND_FEATURE) n++;
	}
	return (int)n;
}

static void begin(void)
{
	config_set("game.window_ms", "3000");
	config_set("game.floor_kb", "128");
	config_set("game.keep", "0");
	host_set_vsh_memory(1000 * KB, 20480 * KB);
	gamemod_set_modules(1);
	module(GAME_RAC1);
	module(GAME_RAC4);
}

static void end(void)
{
	if (session_state() != SESSION_XMB) quit();
	gamemod_set_modules(0);
	config_set("game.keep", "0");
	module_gone(GAME_RAC1);
	module_gone(GAME_RAC4);
	host_set_vsh_memory(0, 0);
	session_vsh_memory_forget();
}

/* ------------------------------------------------------------------ tests */

static void test_linked(void)
{
	struct gm g;

	group("game modules: a linked game is there from the session's first tick");
	gamemod_set_modules(0);
	check(boot("NPEA00385"), "RaC1 boots");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC1, "its code is here at INGAME");
	info(&g);
	check(g.state == GM_LINKED && g.game == GAME_RAC1 && (g.flags & 4) == 0, "GAME_MODULE says LINKED");
	check_eq_u64(info_byte(29) & (SESSION_FLAG2_GAME_MODULE_BAD | SESSION_FLAG2_GAME_WAITING), 0,
	             "and neither flags2 bit is set");
	check(quit(), "quits");
}

static void test_wait_then_load(void)
{
	struct gm g;
	int t;
	u32 free_now = 0, total = 0;

	group("game modules: the gate waits out its window, then loads");
	begin();
	check(boot("NPEA00385"), "RaC1 reaches INGAME on its fingerprint");
	check(session_game() == NULL, "with no game code yet");
	check_eq_u64(info_byte(3), GAME_NONE, "the game byte reads none while it waits");
	check((info_byte(29) & SESSION_FLAG2_GAME_WAITING) != 0, "flags2 bit6 GAME_WAITING");
	info(&g);
	check(g.state == GM_WAITING && g.game == GAME_RAC1 && g.pages == 1, "GAME_MODULE: WAITING, RaC1, one page");
	check(g.need == 128 && g.floor == 128 && g.window == 3000, "needs 128 KB over the 128 KB floor, 3000 ms");
	check(features_describe(qscratch(), 4096, &total) == ST_UNSUPPORTED, "DESCRIBE answers UNSUPPORTED meanwhile");
	{
		u8 b[4];
		check(mem_read(0x00100000, b, 4) == ST_OK, "and the memory tools work");
	}
	t = until_game(5 * TICKS_PER_S);
	check(t > (int)(2.5 * TICKS_PER_S) && t < (int)(3.6 * TICKS_PER_S), "the module loads a window into the session");
	info(&g);
	check(g.state == GM_LOADED && g.lowest == 1000 && (g.flags & 8) != 0, "LOADED, at 1000 KB the lowest");
	check(session_game() != NULL && session_game()->game_id == GAME_RAC1, "RaC1's code arrived");
	check_eq_u64(info_byte(3), GAME_RAC1, "the game byte names it now");
	check_eq_u64(info_byte(29) & SESSION_FLAG2_GAME_WAITING, 0, "and bit6 is down");
	check(features_describe(qscratch(), 4096, &total) == ST_OK, "DESCRIBE answers");
	check_eq_u64(host_fake_modules_loaded(), 1, "one module is loaded");
	plat_vsh_memory(&free_now, &total);
	check_eq_u64(free_now, 936 * KB, "and its page came off the VSH's free memory");
	end();
}

static void test_gate_holds(void)
{
	struct gm g;
	u32 notes;
	int i, t;

	group("game modules: too little memory holds the load back, says so once, and loads when there is room");
	begin();
	host_set_vsh_memory(200 * KB, 20480 * KB);
	notes = host_notify_count();
	check(boot("NPEA00385"), "RaC1 boots");
	step(10 * TICKS_PER_S);
	info(&g);
	check(g.state == GM_WAITING && g.lowest == 200 && g.waited >= 9000, "still waiting at 200 KB, 10 s in");
	check(session_game() == NULL, "no game code");
	check_eq_u64(host_notify_count() - notes, 0, "nothing on screen yet");
	step(21 * TICKS_PER_S);
	check_eq_u64(host_notify_count() - notes, 1, "30 s in, one notification");
	check(qstreq(host_notify_last(), "qwark: couldn't load game module (200KB free / 256KB needed)"),
	      "naming the KB free and needed");
	info(&g);
	check((g.flags & 2) != 0, "GAME_MODULE says it went");
	step(30 * TICKS_PER_S);
	check_eq_u64(host_notify_count() - notes, 1, "and only one");

	/* Room again: the low readings age out of the window first. */
	host_set_vsh_memory(1000 * KB, 20480 * KB);
	t = until_game(6 * TICKS_PER_S);
	check(t > (int)(2.5 * TICKS_PER_S) && t < (int)(3.6 * TICKS_PER_S),
	      "it loads once the window holds no low reading");

	/* The swing: the lowest of the window decides, not the latest reading. */
	check(quit(), "quit");
	check_eq_u64(host_fake_modules_loaded(), 0, "unloaded");
	check(boot("NPEA00385"), "again");
	for (i = 0; i < 8 * TICKS_PER_S && session_game() == NULL; i++) {
		host_set_vsh_memory(((i / TICKS_PER_S) % 2 ? 250u : 1500u) * KB, 20480 * KB);
		step(1);
	}
	check(session_game() == NULL, "a swing down to 250 KB each second keeps it waiting (250 < 128 + 128)");
	info(&g);
	check(g.lowest == 250, "the lowest over the window is 250 KB");
	for (i = 0; i < 8 * TICKS_PER_S && session_game() == NULL; i++) {
		host_set_vsh_memory(((i / TICKS_PER_S) % 2 ? 300u : 1500u) * KB, 20480 * KB);
		step(1);
	}
	check(session_game() != NULL, "one down to 300 KB lets it through");
	end();
}

static void test_missing_and_written(void)
{
	struct gm g;
	char path[48];
	u32 notes;
	int t;

	group("game modules: a missing module, then a client writes it");
	begin();
	module_gone(GAME_RAC1);
	notes = host_notify_count();
	check(boot("NPEA00385"), "RaC1 boots");
	step(2);
	info(&g);
	check(g.state == GM_MISSING && g.game == GAME_RAC1, "GAME_MODULE: MISSING");
	check((info_byte(29) & SESSION_FLAG2_GAME_MODULE_BAD) != 0, "flags2 bit5 GAME_MODULE_BAD");
	check(session_game() == NULL && info_byte(3) == GAME_NONE, "no game");
	check_eq_u64(host_notify_count() - notes, 1, "one notification");
	check(strstr(host_notify_last(), "rac1.sprx is missing on your console") != NULL, "which says so");
	{
		u8 b[4];
		check(mem_read(0x00100000, b, 4) == ST_OK, "the memory tools work");
	}

	title_module_path(GAME_RAC1, path, sizeof(path), 0);
	host_write_fake_module(path, GAME_RAC1, QWARK_BUILD, QWARK_BUILD_ID, 1, 0);
	gamemod_touch_game(gamemod_names(path));
	t = until_game(5 * TICKS_PER_S);
	check(t > 0, "the written module loads in the same session");
	check_eq_u64(info_byte(29) & SESSION_FLAG2_GAME_MODULE_BAD, 0, "and bit5 is down");
	check_eq_u64(gamemod_names("/dev_hdd0/qwark/rac4.sprx.new"), GAME_RAC4, "rac4.sprx.new is Deadlocked's");
	check_eq_u64(gamemod_names("/dev_hdd0/qwark/rac4.sprx.old"), 0, "and .old nobody's");
	check_eq_u64(gamemod_names("/dev_hdd0/qwark/rac2.sprx"), GAME_RAC2, "rac2.sprx is RaC2's");
	check_eq_u64(gamemod_names("/dev_hdd0/qwark/game-1.sprx"), 0, "and the old game-1.sprx nobody's");
	check(qstreq(title_module_name(GAME_RAC1), "rac1.sprx") && qstreq(title_module_name(GAME_RAC3), "rac3.sprx"),
	      "each module is named for its game");
	check(title_module_name(GAME_NONE) == NULL && title_module_name(GAME_RAC4 + 1) == NULL,
	      "and no game, or one past the four, has none");
	title_module_path(GAME_RAC4, path, sizeof(path), 1);
	check(qstreq(path, "/dev_hdd0/qwark/rac4.sprx.new"), "rac4.sprx's update is rac4.sprx.new");
	title_module_path(GAME_NONE, path, sizeof(path), 0);
	check(path[0] == 0, "and no game has no path");
	end();
}

static void test_bad_modules(void)
{
	struct gm g;
	char path[48];

	group("game modules: another build's, one lv2 refuses, one that is no module");
	begin();
	title_module_path(GAME_RAC1, path, sizeof(path), 0);

	host_write_fake_module(path, GAME_RAC1, QWARK_BUILD, QWARK_BUILD_ID ^ 0x11111111u, 1, 0);
	check(boot("NPEA00385"), "RaC1 boots");
	until_game(5 * TICKS_PER_S);
	info(&g);
	check(g.state == GM_OTHER && g.error == QWARK_MOD_REFUSE_ID && g.module_id == (QWARK_BUILD_ID ^ 0x11111111u),
	      "OTHER, refused for its build ID, which it names");
	check((info_byte(29) & SESSION_FLAG2_GAME_MODULE_BAD) != 0 && session_game() == NULL, "bit5, no game");
	check_eq_u64(host_fake_modules_loaded(), 0, "and lv2 does not keep it");
	check(quit(), "quit");

	host_write_fake_module(path, GAME_RAC1, QWARK_BUILD + 1, QWARK_BUILD_ID, 1, 0);
	check(boot("NPEA00385"), "again, another build number");
	until_game(5 * TICKS_PER_S);
	info(&g);
	check(g.state == GM_OTHER && g.error == QWARK_MOD_REFUSE_BUILD, "OTHER, its build number");
	check(quit(), "quit");

	host_write_fake_module(path, GAME_RAC1, QWARK_BUILD, QWARK_BUILD_ID, 1, 0x80010004u);
	check(boot("NPEA00385"), "again, one lv2 will not load");
	until_game(5 * TICKS_PER_S);
	info(&g);
	check(g.state == GM_FAILED && g.error == 0x80010004u, "FAILED, with lv2's error");
	check(quit(), "quit");

	host_write_fake_module(path, GAME_RAC1, QWARK_BUILD, QWARK_BUILD_ID, 0, 0);
	check(boot("NPEA00385"), "again, a file that is no module");
	step(2);
	info(&g);
	check(g.state == GM_FAILED && g.pages == 0, "FAILED at once, never waiting");
	end();
}

static void test_unload_and_audit(void)
{
	u32 len = 0;

	group("game modules: unloaded at the XMB, and nothing of it kept");
	begin();
	check(boot("NPEA00385") && until_game(5 * TICKS_PER_S) >= 0, "RaC1's module in");
	check(features_set(F_INFINITE_AMMO, 1) == ST_OK, "a code-patch toggle on");
	check(feature_patches() > 0, "the patch table holds its def");
	check(manips_supported(), "the manips have the game's table");
	check(quit(), "quit");
	check_eq_u64(host_fake_modules_loaded(), 0, "the module is unloaded");
	check(session_game() == NULL, "the session let go");
	check(features_describe(qscratch(), 4096, &len) == ST_UNSUPPORTED, "the feature registry let go");
	check(!manips_supported(), "so did the manips");
	check_eq_u64(feature_patches(), 0, "and the patch table, of the toggle's def");
	check((features_toggle_state() & ((u64)1 << F_INFINITE_AMMO)) != 0, "the toggle's state is the core's and stays");

	/* The same game again: the record is built at once, offered when the game arrives. */
	check(boot("NPEA00385"), "RaC1 again");
	check(!session_previous()->pending, "nothing offered while the module waits");
	check(until_game(5 * TICKS_PER_S) >= 0, "the module back");
	check(session_previous()->pending && (session_previous()->toggles & ((u64)1 << F_INFINITE_AMMO)) != 0,
	      "then the record offers the toggle");
	session_previous_dismiss();
	end();
}

static void test_switch_and_keep(void)
{
	u32 loads;

	group("game modules: a game switch, and game.keep");
	begin();
	check(boot("NPEA00385") && until_game(5 * TICKS_PER_S) >= 0, "RaC1's module in");
	check(quit(), "quit");
	check(boot("NPEA00423") && until_game(5 * TICKS_PER_S) >= 0, "Deadlocked's module in");
	check(session_game()->game_id == GAME_RAC4, "Deadlocked's code");
	check_eq_u64(host_fake_modules_loaded(), 1, "one module loaded, not two");
	check(quit(), "quit");

	config_set("game.keep", "1");
	check(boot("NPEA00385") && until_game(5 * TICKS_PER_S) >= 0, "RaC1's module in, kept");
	check(quit(), "quit");
	check_eq_u64(host_fake_modules_loaded(), 1, "game.keep keeps it over the XMB");
	check(session_game() == NULL && features_describe(qscratch(), 4096, &loads) == ST_UNSUPPORTED,
	      "though nothing holds it there");
	loads = host_fake_module_loads();
	check(boot("NPEA00385"), "RaC1 again");
	check(session_game() != NULL, "its code is here at INGAME, nothing waited for");
	check_eq_u64(host_fake_module_loads(), loads, "and nothing loaded");
	check(quit(), "quit");
	check(boot("NPEA00423") && until_game(5 * TICKS_PER_S) >= 0, "Deadlocked");
	check_eq_u64(host_fake_modules_loaded(), 1, "the kept RaC1 module went first");
	end();
}

static u32 events_since(u32 seq, u8 kind, u8 code)
{
	u8 *b = qscratch();
	u32 n, i, hits = 0;

	if (autosplit_encode_events(seq, b, 4096) < 5) return 0;
	n = b[4];
	for (i = 0; i < n; i++) {
		const u8 *e = b + 5 + i * AUTOSPLIT_EVENT_SIZE;
		if (e[8] == kind && e[9] == code) hits++;
	}
	return hits;
}

static void test_pause_pair(void)
{
	u32 seq, w = 0;
	u8 ff = 0xFF, b = 0;

	group("game modules: Deadlocked's quit pause and its resume keep their time without the module");
	begin();
	check(boot("NPEA00423") && until_game(5 * TICKS_PER_S) >= 0, "Deadlocked's module in");
	/* A quit an earlier test left open is resumed first: its logo. */
	host_poke(RAC4_LOADING_VAL, &ff, 1);
	step(2);
	seq = autosplit_latest_seq();
	check(quit(), "quit to the XMB");
	check_eq_u64(events_since(seq, AUTOSPLIT_PAUSE, R4_AS_QUIT), 1, "the PAUSE went as it left");

	host_set_vsh_memory(200 * KB, 20480 * KB);
	seq = autosplit_latest_seq();
	check(boot("NPEA00423"), "Deadlocked again, with no room for its module");
	check(session_game() == NULL, "no game code");
	mem_read_u32(RAC4_QUIT_HOOK_ADDR, &w);
	check_eq_u64(w, 0x386000FFu, "the quit hook went in anyway, the core's boot patch");
	step(TICKS_PER_S);
	check_eq_u64(events_since(seq, AUTOSPLIT_RESUME, R4_AS_QUIT), 0, "no RESUME before the logo");
	host_poke(RAC4_LOADING_VAL, &ff, 1);
	step(1);
	check_eq_u64(events_since(seq, AUTOSPLIT_RESUME, R4_AS_QUIT), 1, "the RESUME the tick the logo is up");
	host_peek(RAC4_LOADING_VAL, &b, 1);
	check_eq_u64(b, 0, "and the byte is down again");
	check(session_game() == NULL, "still with no module");
	step(TICKS_PER_S);
	check_eq_u64(events_since(seq, AUTOSPLIT_RESUME, R4_AS_QUIT), 1, "only once");
	end();
}

void test_gamemod(void)
{
	test_linked();
	test_wait_then_load();
	test_gate_holds();
	test_missing_and_written();
	test_bad_modules();
	test_unload_and_audit();
	test_switch_and_keep();
	test_pause_pair();
}
