/*
 * Build 54: the VSH's free memory (PROTOCOL.md 5.1.1, VSH_MEMORY), on the fake
 * console, whose plat_vsh_memory answers whatever host_set_vsh_memory says and
 * nothing at all until it has been given a figure.
 *
 * The readings and what they keep - the latest, the lowest since the module
 * started with when, where and in which game, the figure at start - VSH_MEMORY's
 * reply byte for byte, SessionInfo flags bit5, the ten-second period, the log
 * lines (counted, with the figure the last one carried), the game's own lowest
 * at its QUITTING, and no reading in a launch. The wire itself is smoke's.
 */
#include "../src/core/proto.h"
#include "../src/core/session.h"
#include "../src/core/menu.h"
#include "../src/games/game.h"
#include "../src/games/menuhelper_bins.h"
#include "../src/plat/plat.h"
#include "../src/plat/host/plat_host.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

#define KB 1024u

/* session.c's VSHMEM_EVERY: ten seconds of ticks. */
#define VSHMEM_EVERY_TICKS 1200

static void step(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		host_advance_time(8334);
		session_step_once();
	}
}

static int step_until(u8 want, int max)
{
	int i;
	for (i = 0; i < max && session_state() != want; i++) step(1);
	return session_state() == want;
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

/* VSH_MEMORY's reply, decoded. */
struct vm {
	u32 free, total, low, start, age, low_age;
	u8  low_at, low_game, pad0, pad1;
};

static int reply(struct vm *v)
{
	u8 b[VSH_MEMORY_SIZE + 4];

	memset(b, 0xEE, sizeof(b));
	memset(v, 0, sizeof(*v));
	if (session_vsh_memory(b, sizeof(b)) != VSH_MEMORY_SIZE) return 0;
	v->free = be32_get(b);
	v->total = be32_get(b + 4);
	v->low = be32_get(b + 8);
	v->start = be32_get(b + 12);
	v->age = be32_get(b + 16);
	v->low_age = be32_get(b + 20);
	v->low_at = b[24];
	v->low_game = b[25];
	v->pad0 = b[26];
	v->pad1 = b[27];
	/* Nothing past the 28 bytes is touched. */
	return b[VSH_MEMORY_SIZE] == 0xEE;
}

/* SessionInfo flags as the next publish has them. */
static u8 info_flags(void)
{
	u8 b[SESSION_INFO_SIZE];

	step(4);
	session_info_copy(b, sizeof(b));
	return b[24];
}

/* A ten-second reading of `free_kb` now, as the simulator's `vshmem` command takes one. */
static void periodic(u32 free_kb)
{
	host_set_vsh_memory(free_kb * KB, 32768u * KB);
	session_vsh_memory_read(VSHMEM_AT_PERIODIC);
}

static void test_nothing_to_read(void)
{
	struct vm v;
	u8 small[VSH_MEMORY_SIZE];
	u32 lines;

	group("VSH memory: a platform that cannot read it");

	check(quit(), "the session is at the XMB");
	host_set_vsh_memory(0, 0);
	session_vsh_memory_forget();
	lines = session_vsh_memory_lines();

	session_vsh_memory_read(VSHMEM_AT_START);
	session_vsh_memory_read(VSHMEM_AT_TICK_THREAD);
	session_vsh_memory_read(VSHMEM_AT_PERIODIC);
	check(!reply(&v), "VSH_MEMORY has nothing to answer (UNSUPPORTED on the wire)");
	check_eq_u64(session_vsh_memory_lines() - lines, 0, "and nothing is logged");
	check((info_flags() & SESSION_FLAG_VSH_MEMORY) == 0, "SessionInfo flags bit5 stays clear");

	step(VSHMEM_EVERY_TICKS + 1);
	check(!reply(&v), "nor does a ten-second reading find anything");

	host_set_vsh_memory(1000 * KB, 2000 * KB);
	session_vsh_memory_read(VSHMEM_AT_PERIODIC);
	check(session_vsh_memory(small, sizeof(small) - 1) == 0, "a reply buffer short of 28 bytes gets nothing");
	session_vsh_memory_forget();
}

static void test_start(void)
{
	struct vm v;
	u32 lines;

	group("VSH memory: the module start's two readings");

	host_freeze_time(1);
	lines = session_vsh_memory_lines();

	host_set_vsh_memory(20480 * KB, 40960 * KB);
	session_vsh_memory_read(VSHMEM_AT_START);
	check(reply(&v), "VSH_MEMORY answers once a reading has succeeded");
	check(v.free == 20480 * KB && v.total == 40960 * KB, "free and total, in bytes");
	check(v.low == 20480 * KB && v.low_at == VSHMEM_AT_START && v.low_game == GAME_NONE,
	      "the first reading is the lowest, taken at START at the XMB");
	check(v.start == 20480 * KB, "and the figure at start");
	check(v.pad0 == 0 && v.pad1 == 0, "the two pad bytes are 0");
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "a line is logged");
	check_eq_u64(session_vsh_memory_line_kb(), 20480, "carrying 20480 KB");

	/* The tick thread's 16 KB stack, as the console would show it. */
	host_set_vsh_memory(20464 * KB, 40960 * KB);
	session_vsh_memory_read(VSHMEM_AT_TICK_THREAD);
	reply(&v);
	check(v.free == 20464 * KB && v.low == 20464 * KB && v.low_at == VSHMEM_AT_TICK_THREAD,
	      "the reading after the tick thread is 16 KB lower and the new lowest");
	check(v.start == 20480 * KB, "the figure at start stays the start's");
	check_eq_u64(session_vsh_memory_lines() - lines, 2, "it is logged too");
	check_eq_u64(session_vsh_memory_line_kb(), 20464, "with its own figure");

	host_advance_time(5000000);
	host_set_vsh_memory(30000 * KB, 40960 * KB);
	session_vsh_memory_read(VSHMEM_AT_PERIODIC);
	host_advance_time(7000000);
	reply(&v);
	check(v.free == 30000 * KB && v.low == 20464 * KB, "a higher reading moves free and not the lowest");
	check_eq_u64(v.age, 7, "the latest reading is 7 s old");
	check_eq_u64(v.low_age, 12, "and the lowest 12 s");
	check_eq_u64(session_vsh_memory_lines() - lines, 2, "a ten-second reading above 1 MB is not logged");
	host_freeze_time(0);

	check((info_flags() & SESSION_FLAG_VSH_MEMORY) != 0, "SessionInfo flags bit5 is set");
}

static void test_period(void)
{
	struct vm v;
	int i;

	group("VSH memory: the ten-second reading");

	check(session_state() == SESSION_XMB, "at the XMB");
	host_set_vsh_memory(25000 * KB, 40960 * KB);
	for (i = 0; i <= VSHMEM_EVERY_TICKS; i++) {
		step(1);
		reply(&v);
		if (v.free == 25000 * KB) break;
	}
	check(v.free == 25000 * KB && i < VSHMEM_EVERY_TICKS, "the XMB is read within 1200 ticks");

	host_set_vsh_memory(26000 * KB, 40960 * KB);
	step(VSHMEM_EVERY_TICKS - 1);
	reply(&v);
	check(v.free == 25000 * KB, "not again for 1199 ticks");
	step(1);
	reply(&v);
	check(v.free == 26000 * KB, "and again on the 1200th");
}

static void test_lows(void)
{
	struct vm v;
	u32 lines = session_vsh_memory_lines();

	group("VSH memory: the low lines");

	periodic(900);
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "a reading under 1 MB is logged as a low");
	check_eq_u64(session_vsh_memory_line_kb(), 900, "carrying 900 KB");
	reply(&v);
	check(v.low == 900 * KB && v.low_at == VSHMEM_AT_PERIODIC, "and is the lowest, taken at a ten-second reading");

	periodic(880);
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "20 KB lower is not a new line");
	reply(&v);
	check(v.low == 880 * KB, "though it is the new lowest");

	periodic(836);
	check_eq_u64(session_vsh_memory_lines() - lines, 2, "64 KB under the last line is");

	periodic(2000);
	periodic(1100);
	check_eq_u64(session_vsh_memory_lines() - lines, 2, "nothing above 1 MB is");
	reply(&v);
	check(v.low == 836 * KB && v.free == 1100 * KB, "the lowest stays, free follows");
}

static void test_games(void)
{
	struct vm v;
	u32 lines;

	group("VSH memory: a game's arrival and its lowest");

	lines = session_vsh_memory_lines();
	host_set_vsh_memory(1500 * KB, 32768 * KB);
	check(boot("NPEA00385"), "RaC1 comes up");
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "its arrival is read and logged");
	check_eq_u64(session_vsh_memory_line_kb(), 1500, "at 1500 KB");
	reply(&v);
	check(v.free == 1500 * KB && v.total == 32768 * KB, "and VSH_MEMORY has that reading");
	check(v.low == 836 * KB && v.low_game == GAME_NONE, "the lowest is still the XMB's");

	periodic(700);
	check_eq_u64(session_vsh_memory_lines() - lines, 2, "a low in the game is logged");
	reply(&v);
	check(v.low == 700 * KB && v.low_at == VSHMEM_AT_PERIODIC && v.low_game == GAME_RAC1,
	      "and is the lowest, in RaC1");
	periodic(1200);

	check(quit(), "the game goes");
	check_eq_u64(session_vsh_memory_lines() - lines, 3, "its QUITTING logs the game's lowest");
	check_eq_u64(session_vsh_memory_line_kb(), 700, "700 KB");

	/* A reading at the XMB is no game's. */
	periodic(600);
	check_eq_u64(session_vsh_memory_lines() - lines, 4, "a new low at the XMB is logged");
	host_set_vsh_memory(5000 * KB, 32768 * KB);
	check(boot("NPEA00385"), "RaC1 again");
	check(quit(), "and out");
	check_eq_u64(session_vsh_memory_lines() - lines, 6, "its arrival and its lowest");
	check_eq_u64(session_vsh_memory_line_kb(), 5000, "which is that game's own, not the XMB's 600 KB");
	reply(&v);
	check(v.low == 600 * KB && v.low_game == GAME_NONE, "the module's lowest is the XMB's 600 KB");

	check(boot("NPEA00385"), "and once more");
	check(quit(), "and out");
	check_eq_u64(session_vsh_memory_line_kb(), 5000, "a game whose only reading was its arrival logs that");
}

static void test_launch(void)
{
	const struct game_api *cands[GAME_MAX_CANDIDATES];
	const struct game_api *g = NULL;
	struct vm v;
	u8 zeros[64];
	int i;

	group("VSH memory: nothing read in a launch");

	if (game_candidates_for_title("NPEA00385", cands, GAME_MAX_CANDIDATES) > 0) g = cands[0];
	check(g != NULL && g->fp_len > 0 && g->fp_len <= sizeof(zeros), "RaC1 has a fingerprint");
	if (g == NULL || g->fp_len == 0 || g->fp_len > sizeof(zeros)) return;
	memset(zeros, 0, sizeof(zeros));

	check(session_state() == SESSION_XMB, "at the XMB");
	periodic(4000);
	host_set_vsh_memory(4100 * KB, 32768 * KB);

	host_freeze_time(1);
	host_boot("NPEA00385");
	for (i = 0; i < VSHMEM_EVERY_TICKS + 100; i++) session_step_once();
	reply(&v);
	check(session_state() == SESSION_BOOTING && v.free == 4000 * KB,
	      "1300 ticks of a launch's quiet second read nothing");
	host_freeze_time(0);

	/* The quiet second over, but the executable not there yet: still BOOTING. */
	host_poke(g->fp_addr, zeros, g->fp_len);
	step(VSHMEM_EVERY_TICKS + 200);
	reply(&v);
	check(session_state() == SESSION_BOOTING && v.free == 4000 * KB,
	      "nor do 1400 ticks of BOOTING after it");

	host_poke(g->fp_addr, g->fp_bytes, g->fp_len);
	check(step_until(SESSION_INGAME, 100), "the fingerprint arrives and the game is up");
	reply(&v);
	check(v.free == 4100 * KB, "and its arrival is read");
	check(quit(), "out again");
}

static void test_menu_install(void)
{
	const struct mh_desc *d = mh_desc_for_game(GAME_RAC4);
	struct vm v;
	u8 raw[4];
	u32 lines;
	int i;

	group("VSH memory: the console menu's install");

	check(d != NULL, "Deadlocked has a renderer");
	if (d == NULL) return;

	session_vsh_memory_forget();
	host_set_vsh_memory(3000 * KB, 32768 * KB);
	check(boot("NPEA00423"), "Deadlocked comes up");
	reply(&v);
	check(v.low == 3000 * KB && v.low_at == VSHMEM_AT_GAME && v.low_game == GAME_RAC4,
	      "after a fresh start, its arrival is the lowest, taken at GAME in Deadlocked");
	check(menu_state() == MENU_IDLE, "its menu is due a few seconds in");

	/* In play: the mode word 0 and no mode change queued. */
	be32_put(raw, 0);
	host_poke(d->mode_addr, raw, 4);
	be32_put(raw, d->pending_none);
	host_poke(d->pending_addr, raw, 4);

	host_set_vsh_memory(2900 * KB, 32768 * KB);
	lines = session_vsh_memory_lines();
	for (i = 0; i < 1000 && menu_state() != MENU_READY; i++) step(1);
	check(menu_state() == MENU_READY, "the renderer goes in");
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "and the tick it went in reads and logs");
	check_eq_u64(session_vsh_memory_line_kb(), 2900, "2900 KB");
	reply(&v);
	check(v.low == 2900 * KB && v.low_at == VSHMEM_AT_MENU, "the lowest now, taken at MENU");

	step(10);
	check_eq_u64(session_vsh_memory_lines() - lines, 1, "once, not every tick it serves");
	check(quit(), "out");
}

void test_vshmem(void)
{
	test_nothing_to_read();
	test_start();
	test_period();
	test_lows();
	test_games();
	test_launch();
	test_menu_install();

	/* As the simulator starts: nothing to read. */
	host_set_vsh_memory(0, 0);
	session_vsh_memory_forget();
}
