/*
 * The running game's code: linked in, or a game module loaded behind the
 * memory gate (gamemod.h; PROTOCOL.md 5.1.2 and 9.11; SPEC-game-modules.md,
 * design A1).
 *
 * The gate (the user's design, 2026-10-27). A load takes the module's pages
 * (one, 64 KB, for every module the build lets out: tools/gamemodule.py) and,
 * for a moment, about one page more (the PRX probe's transient). The VSH's
 * free memory swings by about 448 KB on and off by itself in a game, on CFW
 * and on HEN alike, and dips furthest as a game launches. So a module is
 * loaded only when the *lowest* free figure over the last `game.window_ms`
 * (3 s), sampled ten times a second from the moment the session is INGAME,
 * less those two, still leaves `game.floor_kb` (128 KB). A launch's dip is in
 * the window, so the gate waits it out by itself; a console that never has the
 * room keeps waiting, says so once after 30 s, and loads the moment it has.
 * Every decision is logged with its figures.
 *
 * While the game is not here the session is INGAME with no game: the memory,
 * watch, freeze and patch ops, the connection and the console menu's own pages
 * work, every game op answers UNSUPPORTED, and SessionInfo flags2 says why
 * (GAME_WAITING, GAME_MODULE_BAD). What cannot wait for a module lives in the
 * title index (src/games/titles.h) and the session.
 */
#include "gamemod.h"
#include "config.h"
#include "mem.h"
#include "session.h"
#include "util.h"
#include "proto.h"
#include "../games/game.h"
#include "../games/titles.h"
#include "../games/module.h"
#include "../games/core_api.h"

#include <string.h>

/* ------------------------------------------------------ the context, Q */

#if QWARK_GAME_IMPORT
#if QWARK_GAME_MODULES && !defined(QWARK_HOST)
/*
 * A test module: the lent state area, which only a game module uses (a linked
 * game keeps its statics where they always were).
 */
static u64 g_state_area[QWARK_GAME_STATE_BYTES / 8];
#define STATE_AREA ((void *)g_state_area)
#else
#define STATE_AREA NULL
#endif

/*
 * The context every game reaches the core through. On the console its address
 * goes into r14 as each of the core's threads starts (plat_ps3.c); on a PC, Q
 * points at it.
 */
struct qwark_ctx g_qwark_ctx = { &core_api_table, STATE_AREA };
#ifdef QWARK_HOST
struct qwark_ctx *Q = &g_qwark_ctx;
#endif
#endif /* QWARK_GAME_IMPORT */

/* ------------------------------------------------------------- the state */

#define SAMPLES       64u
#define SAMPLE_EVERY  12u            /* ticks: ten readings a second */
#define LOG_EVERY_US  10000000ull    /* a waiting line at most every ten seconds */

static struct {
	u8  state;                       /* GM_* */
	u8  game_id;                     /* the title's game */
	u8  pages;                       /* the module's, from its header; 0 unknown */
	u8  notified;                    /* the 30 s notification went this wait */
	u8  told_bad;                    /* the bad-module notification went this session */
	u8  look;                        /* a client wrote the module's file: look again */
	u8  refusal;                     /* QWARK_MOD_REFUSE_* of an OTHER */
	u8  logged_wait;                 /* the first waiting line went */
	const struct game_title *title;
	const struct game_api *game;     /* what arrived */
	u64 wait_since;
	u64 next_log;
	u32 error;                       /* a FAILED's code */
	u32 load_us;
	u32 module_build_id;
	u32 min_kb, now_kb, need_kb, floor_kb, window_ms;
	u32 tick;
} g_gm;

/* The module lv2 has: one at a time. -1 for none. */
static int g_prx = -1;
static u8  g_prx_game;               /* whose it is */
static const struct game_api *g_prx_api;

#if QWARK_GAME_MODULES
/* The gate's readings, a ring: when each was taken, in ms, and the KB free. */
static struct { u32 ms; u32 kb; } g_samp[SAMPLES];
#endif
static u32 g_nsamp, g_samp_head;

#ifdef QWARK_HOST
static int g_modules_on;
void gamemod_set_modules(int on) { g_modules_on = on ? 1 : 0; }
int  gamemod_modules(void) { return g_modules_on; }
#elif QWARK_GAME_MODULES
int  gamemod_modules(void) { return 1; }
#else
int  gamemod_modules(void) { return 0; }
#endif

const struct game_api *gamemod_game(void) { return g_gm.game; }

/* ------------------------------------------------------------- the figures */

static u32 cfg_floor_kb(void)
{
	return config_get_u32("game.floor_kb", GM_FLOOR_KB_DEFAULT);
}

static u32 cfg_window_ms(void)
{
	u32 w = config_get_u32("game.window_ms", GM_WINDOW_MS_DEFAULT);
	return w > 60000u ? 60000u : w;
}

static int cfg_keep(void)
{
	return config_get_u32("game.keep", 0) != 0;
}

static void module_path(char *out, u32 cap, int dot_new)
{
	title_module_path(g_gm.game_id, out, cap, dot_new);
}

static const char *module_name(void)
{
	static char name[16];
	char path[48];

	module_path(path, sizeof(path), 0);
	qstrcpy(name, sizeof(name), path + sizeof(GAME_MODULE_DIR) - 1);
	return name;
}

/* --------------------------------------------------------- the begin */

int gamemod_begin(const struct game_title *t)
{
	g_gm.title = t;
	g_gm.game = NULL;
	g_gm.game_id = t != NULL ? t->game_id : GAME_NONE;
	g_gm.pages = 0;
	g_gm.notified = 0;
	g_gm.told_bad = 0;
	g_gm.logged_wait = 0;
	g_gm.refusal = 0;
	g_gm.error = 0;
	g_gm.min_kb = g_gm.now_kb = g_gm.need_kb = 0;
	g_gm.floor_kb = cfg_floor_kb();
	g_gm.window_ms = cfg_window_ms();
	g_nsamp = 0;
	g_samp_head = 0;

	if (t == NULL) {
		g_gm.state = GM_NONE;
		return GM_ARRIVED_NOTHING;
	}

	if (!gamemod_modules()) {
		/* Linked in: here from the session's first tick, as before modules. */
		g_gm.game = t->linked;
		if (g_gm.game != NULL && g_gm.game->init != NULL) g_gm.game->init();
		g_gm.state = g_gm.game != NULL ? GM_LINKED : GM_NONE;
		return g_gm.game != NULL ? GM_ARRIVED_GAME : GM_ARRIVED_NOTHING;
	}

	/*
	 * A module kept over the XMB (`game.keep`), for this very game: it is
	 * still loaded and started, so nothing is allocated and nothing waits.
	 * Its state area is as the last session left it, as a linked game's is.
	 */
	if (g_prx >= 0 && g_prx_game == g_gm.game_id && g_prx_api != NULL) {
		g_gm.game = g_prx_api;
		g_gm.state = GM_LOADED;
		plat_log("game module: %s kept from the last session, nothing to load", module_name());
		return GM_ARRIVED_GAME;
	}

	g_gm.state = GM_CHECKING;
	return GM_ARRIVED_NOTHING;
}

#if QWARK_GAME_MODULES
/* ------------------------------------------------------------ the file */

/*
 * How many 64 KB pages the module's LOAD segments take, out of its SELF
 * header, which scetool leaves in the clear: the SCE header, then the SELF
 * header with the ELF header's and the program headers' offsets. 0 when the
 * file is not one.
 */
static u32 rd64lo(const u8 *p) { return be32_get(p) != 0 ? 0xFFFFFFFFu : be32_get(p + 4); }

static u8 self_pages(const u8 *h, u32 n)
{
	u32 elf, ph, phnum, i, pages = 0;

	if (n < 0x70 || h[0] != 'S' || h[1] != 'C' || h[2] != 'E' || h[3] != 0) return 0;
	elf = rd64lo(h + 0x30);
	ph = rd64lo(h + 0x38);
	if (elf > n - 64 || h[elf] != 0x7F || h[elf + 1] != 'E' || h[elf + 2] != 'L' || h[elf + 3] != 'F')
		return 0;
	phnum = be16_get(h + elf + 0x38);
	if (phnum == 0 || phnum > 8 || ph > n || phnum * 56u > n - ph) return 0;
	for (i = 0; i < phnum; i++) {
		const u8 *p = h + ph + i * 56u;
		u32 memsz = rd64lo(p + 40);
		if (be32_get(p) != 1) continue;          /* PT_LOAD */
		if (memsz == 0xFFFFFFFFu) return 0;
		pages += (memsz + 0xFFFFu) >> 16;
	}
	return pages == 0 || pages > 16 ? 0 : (u8)pages;
}

/* A module an update left as game-<n>.sprx.new takes the old one's place. */
static void take_new(void)
{
	char path[48], next[48];

	module_path(next, sizeof(next), 1);
	if (!plat_path_exists(next, NULL, NULL)) return;
	module_path(path, sizeof(path), 0);
	plat_file_unlink(path);
	if (plat_file_rename(next, path) == 0) plat_log("game module: took %s.new, which an update left", module_name());
	else plat_log("game module: could not put %s.new in place", module_name());
}

/* The file's verdict: WAITING with its pages, MISSING or FAILED. */
static void look(void)
{
	char path[48];
	plat_file_t f;
	u8 *h = qscratch();
	u32 got = 0;

	take_new();
	module_path(path, sizeof(path), 0);
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) {
		g_gm.state = GM_MISSING;
		plat_log("game module: no %s on the console; this game has no game features this session", module_name());
		return;
	}
	if (plat_file_read(f, h, 1024, &got) != 0) got = 0;
	plat_file_close(f);

	g_gm.pages = self_pages(h, got);
	if (g_gm.pages == 0) {
		g_gm.state = GM_FAILED;
		g_gm.error = 0;
		plat_log("game module: %s is not a signed module (%d bytes read); no game features this session",
		         module_name(), (int)got);
		return;
	}
	g_gm.need_kb = (u32)g_gm.pages * 64u + GM_TRANSIENT_KB;
	g_gm.state = GM_WAITING;
	g_gm.wait_since = plat_time_us();
	g_gm.next_log = 0;
	plat_log("game module: %s is %d page(s); it needs %d KB free plus the %d KB floor, "
	         "lowest over %d ms", module_name(), (int)g_gm.pages, (int)g_gm.need_kb,
	         (int)g_gm.floor_kb, (int)g_gm.window_ms);
}

/* ------------------------------------------------------------ the gate */

static void sample(u32 now_ms)
{
	u32 fr = 0, total = 0;

	if (plat_vsh_memory(&fr, &total) != 0) return;
	g_samp[g_samp_head].ms = now_ms;
	g_samp[g_samp_head].kb = fr >> 10;
	g_samp_head = (g_samp_head + 1u) % SAMPLES;
	if (g_nsamp < SAMPLES) g_nsamp++;
	g_gm.now_kb = fr >> 10;
}

/*
 * The lowest reading of the last window, and whether the readings reach back
 * over all of it. 0 readings: nothing is known.
 */
static int window_min(u32 now_ms, u32 *min_kb)
{
	u32 i, oldest = now_ms, lo = 0xFFFFFFFFu, n = 0;

	for (i = 0; i < g_nsamp; i++) {
		u32 at = g_samp[i].ms;
		if (now_ms - at > g_gm.window_ms) continue;
		if (g_samp[i].kb < lo) lo = g_samp[i].kb;
		if (now_ms - at > now_ms - oldest) oldest = at;
		n++;
	}
	*min_kb = n ? lo : 0;
	if (n == 0) return 0;
	/* Full when a reading in it is as old as the window, give or take one sample's gap. */
	return now_ms - oldest + 150u >= g_gm.window_ms;
}

static void notify_waiting(void)
{
	char msg[160], kb[12];

	qstrcpy(msg, sizeof(msg), "Game support is waiting for memory: your console has ");
	qfmt_u32(kb, sizeof(kb), g_gm.min_kb);
	qstrcat(msg, sizeof(msg), kb);
	qstrcat(msg, sizeof(msg), " KB free and needs ");
	qfmt_u32(kb, sizeof(kb), g_gm.need_kb + g_gm.floor_kb);
	qstrcat(msg, sizeof(msg), kb);
	qstrcat(msg, sizeof(msg), " KB. It loads by itself when there is room.");
	plat_notify(msg);
}

static void notify_bad(const char *why)
{
	char msg[160];

	if (g_gm.told_bad) return;
	g_gm.told_bad = 1;
	qstrcpy(msg, sizeof(msg), "Game support: ");
	qstrcat(msg, sizeof(msg), module_name());
	qstrcat(msg, sizeof(msg), why);
	plat_notify(msg);
}

/* --------------------------------------------------------- load, unload */

static struct qwark_mod_args g_args;

static void drop_prx(const char *why)
{
	int modres = 0, rc_stop, rc_unload;

	if (g_prx < 0) return;
	memset(&g_args, 0, sizeof(g_args));
	g_args.magic = QWARK_MOD_MAGIC;
	g_args.size = sizeof(g_args);
	rc_stop = plat_prx_stop(g_prx, &g_args, sizeof(g_args), &modres);
	rc_unload = plat_prx_unload(g_prx);
	plat_log("game module: game-%d.sprx unloaded (%s): stop 0x%08X, unload 0x%08X",
	         (int)g_prx_game, why, (unsigned)rc_stop, (unsigned)rc_unload);
	g_prx = -1;
	g_prx_game = GAME_NONE;
	g_prx_api = NULL;
}

/* Answers 1 when the game arrived. */
static int load(void)
{
	char path[48];
	u64 t0, t1;
	u32 before = 0, total = 0;
	int id, rc, modres = 0;
	const struct game_api *g;

	module_path(path, sizeof(path), 0);
	if (plat_vsh_memory(&before, &total) != 0) before = 0;

	/* What a module finds: a zeroed state area, and the patch pool its init() draws on. */
	if (g_qwark_ctx.state != NULL) memset(g_qwark_ctx.state, 0, QWARK_GAME_STATE_BYTES);
	patch_pool_reset();

	t0 = plat_time_us();
	id = plat_prx_load(path);
	if (id < 0) {
		g_gm.state = GM_FAILED;
		g_gm.error = (u32)id;
		plat_log("game module: lv2 would not load %s: 0x%08X, %d KB free", module_name(),
		         (unsigned)id, (int)(before >> 10));
		notify_bad(" would not load on your console. RaCMAN can send it again.");
		return 0;
	}

	memset(&g_args, 0, sizeof(g_args));
	g_args.magic = QWARK_MOD_MAGIC;
	g_args.size = sizeof(g_args);
	g_args.build = QWARK_BUILD;
	g_args.build_id = QWARK_BUILD_ID;
	g_args.api_size = sizeof(struct core_api);
	g_args.state_size = g_qwark_ctx.state != NULL ? QWARK_GAME_STATE_BYTES : 0xFFFFFFFFu;
	g_args.game_id = g_gm.game_id;
	rc = plat_prx_start(id, &g_args, sizeof(g_args), &modres);
	t1 = plat_time_us();
	g_gm.load_us = (u32)(t1 - t0);
	g = g_args.game;

	if (rc != 0 || modres != 0 || g == NULL) {
		/* lv2 drops a module whose start does not answer RESIDENT; make sure it went. */
		plat_prx_unload(id);
		g_gm.refusal = g_args.refusal;
		g_gm.module_build_id = g_args.module_build_id;
		if (g_args.refusal == QWARK_MOD_REFUSE_BUILD || g_args.refusal == QWARK_MOD_REFUSE_ID ||
		    g_args.refusal == QWARK_MOD_REFUSE_API) {
			g_gm.state = GM_OTHER;
			plat_log("game module: %s is from another build (refusal %d, its ID 0x%08X, ours 0x%08X)",
			         module_name(), (int)g_args.refusal, (unsigned)g_args.module_build_id,
			         (unsigned)QWARK_BUILD_ID);
			notify_bad(" is from another build. RaCMAN can send the right one.");
		} else {
			g_gm.state = GM_FAILED;
			g_gm.error = rc != 0 ? (u32)rc : (u32)(0x80000000u | g_args.refusal);
			plat_log("game module: %s would not start: 0x%08X, module answered %d, refusal %d",
			         module_name(), (unsigned)rc, modres, (int)g_args.refusal);
			notify_bad(" would not start on your console. RaCMAN can send it again.");
		}
		return 0;
	}

	/* What it handed back must be its own: a vtable inside the module, for this game. */
	if (plat_prx_contains(id, g, sizeof(*g)) == 0 || g->game_id != g_gm.game_id) {
		g_prx = id;
		g_prx_game = g_gm.game_id;
		drop_prx("not this game's");
		g_gm.state = GM_FAILED;
		g_gm.error = 0x80000100u;
		plat_log("game module: %s answered with a table that is not its own game's", module_name());
		notify_bad(" is not this game's module. RaCMAN can send it again.");
		return 0;
	}

	g_prx = id;
	g_prx_game = g_gm.game_id;
	g_prx_api = g;
	g_gm.game = g;
	g_gm.state = GM_LOADED;
	{
		u32 after = 0;
		if (plat_vsh_memory(&after, &total) != 0) after = 0;
		plat_log("game module: %s loaded in %d ms (id 0x%08X), %d KB free before, %d KB after",
		         module_name(), (int)(g_gm.load_us / 1000u), (unsigned)id, (int)(before >> 10),
		         (int)(after >> 10));
	}
	if (g->init != NULL) g->init();
	return 1;
}

static int gate(void)
{
	u64 now = plat_time_us();
	u32 now_ms = (u32)(now / 1000u);
	u32 total = 0, fr = 0;
	int full;

	if ((g_gm.tick++ % SAMPLE_EVERY) != 0) return 0;
	if (plat_vsh_memory(&fr, &total) != 0) {
		/* Nothing to judge by: nothing on a console fails this; load, and say so. */
		plat_log("game module: no reading of the VSH's memory; loading %s", module_name());
		return load();
	}
	sample(now_ms);
	full = window_min(now_ms, &g_gm.min_kb);
	if (!full) return 0;

	if (g_gm.min_kb >= g_gm.need_kb + g_gm.floor_kb) {
		plat_log("game module: loading %s: %d KB free at the lowest over %d ms, %d KB now; "
		         "it needs %d KB and leaves %d KB over the %d KB floor, after %d ms of waiting",
		         module_name(), (int)g_gm.min_kb, (int)g_gm.window_ms, (int)g_gm.now_kb,
		         (int)g_gm.need_kb, (int)(g_gm.min_kb - g_gm.need_kb - g_gm.floor_kb),
		         (int)g_gm.floor_kb, (int)((now - g_gm.wait_since) / 1000u));
		return load();
	}

	if (!g_gm.logged_wait || now >= g_gm.next_log) {
		g_gm.logged_wait = 1;
		g_gm.next_log = now + LOG_EVERY_US;
		plat_log("game module: %s waits for memory: %d KB free at the lowest over %d ms (%d KB now), "
		         "it needs %d KB plus the %d KB floor; waiting %d ms so far",
		         module_name(), (int)g_gm.min_kb, (int)g_gm.window_ms, (int)g_gm.now_kb,
		         (int)g_gm.need_kb, (int)g_gm.floor_kb, (int)((now - g_gm.wait_since) / 1000u));
	}
	if (!g_gm.notified && now - g_gm.wait_since >= GM_NOTIFY_US) {
		g_gm.notified = 1;
		plat_log("game module: still waiting after %d s; the console is told once",
		         (int)((now - g_gm.wait_since) / 1000000u));
		notify_waiting();
	}
	return 0;
}
#endif /* QWARK_GAME_MODULES */

int gamemod_tick(void)
{
#if QWARK_GAME_MODULES
	int arrived = 0;

	switch (g_gm.state) {
	case GM_CHECKING:
		/* A module kept for another game goes before this one's is counted. */
		if (g_prx >= 0 && g_prx_game != g_gm.game_id) drop_prx("another game boots");
		look();
		if (g_gm.state == GM_MISSING) {
			notify_bad(" is missing on your console. RaCMAN can send it.");
			return GM_ARRIVED_NONE;
		}
		if (g_gm.state == GM_FAILED) {
			notify_bad(" is damaged on your console. RaCMAN can send it again.");
			return GM_ARRIVED_NONE;
		}
		return GM_ARRIVED_NOTHING;

	case GM_WAITING:
		arrived = gate();
		if (arrived) return GM_ARRIVED_GAME;
		return g_gm.state == GM_WAITING ? GM_ARRIVED_NOTHING : GM_ARRIVED_NONE;

	case GM_MISSING:
	case GM_FAILED:
	case GM_OTHER:
		/* A client wrote the file: look at it again, and wait for memory again. */
		if (!g_gm.look) return GM_ARRIVED_NOTHING;
		g_gm.look = 0;
		g_gm.told_bad = 0;
		g_gm.state = GM_CHECKING;
		plat_log("game module: %s was written; looking again", module_name());
		return GM_ARRIVED_NOTHING;

	default:
		return GM_ARRIVED_NOTHING;
	}
#else
	return GM_ARRIVED_NOTHING;
#endif
}

u8 gamemod_names(const char *path)
{
	u8 n;

	if (path == NULL) return 0;
	for (n = 1; n <= GAME_RAC4; n++) {
		char mine[48];
		u32 len;

		title_module_path(n, mine, sizeof(mine), 0);
		len = qstrlen(mine);
		if (qstrneq(path, mine, len) && (path[len] == 0 || qstreq(path + len, ".new"))) return n;
	}
	return 0;
}

void gamemod_touch_game(u8 game_id)
{
	if (game_id != GAME_NONE && game_id == g_gm.game_id) g_gm.look = 1;
}

int gamemod_leaving(void)
{
	/*
	 * A module's game is dropped by every holder whether or not the module
	 * stays loaded (game.keep): the next session takes it up again, under its
	 * own arrival, so nothing keeps a pointer over the XMB. A linked game's
	 * holders keep theirs, as they always have.
	 */
	int module = g_gm.state == GM_LOADED;

	g_gm.game = NULL;
	g_gm.state = GM_NONE;
	g_gm.title = NULL;
	g_gm.game_id = GAME_NONE;
	return module;
}

void gamemod_unload(void)
{
#if QWARK_GAME_MODULES
	if (g_prx < 0) return;
	if (cfg_keep()) {
		plat_log("game module: game-%d.sprx kept over the XMB (game.keep = 1)", (int)g_prx_game);
		return;
	}
	drop_prx("the game quit");
#endif
}

void gamemod_shutdown(void)
{
#if QWARK_GAME_MODULES
	drop_prx("qwark stops");
#endif
}

u8 gamemod_flags2(void)
{
	switch (g_gm.state) {
	case GM_CHECKING:
	case GM_WAITING:
		return SESSION_FLAG2_GAME_WAITING;
	case GM_MISSING:
	case GM_FAILED:
	case GM_OTHER:
		return SESSION_FLAG2_GAME_MODULE_BAD;
	default:
		return 0;
	}
}

u32 gamemod_info(u8 *out, u32 cap)
{
	u64 now = plat_time_us();

	if (cap < GAME_MODULE_SIZE) return 0;
	memset(out, 0, GAME_MODULE_SIZE);
	out[0] = g_gm.state;
	out[1] = g_gm.game_id;
	out[2] = g_gm.pages;
	out[3] = (u8)((cfg_keep() ? 1 : 0) | (g_gm.notified ? 2 : 0) | (gamemod_modules() ? 4 : 0) |
	              (g_prx >= 0 ? 8 : 0));
	be32_put(out + 4, g_gm.min_kb);
	be32_put(out + 8, g_gm.now_kb);
	be32_put(out + 12, g_gm.need_kb);
	be32_put(out + 16, g_gm.floor_kb);
	be32_put(out + 20, g_gm.window_ms);
	be32_put(out + 24, g_gm.state == GM_WAITING ? (u32)((now - g_gm.wait_since) / 1000u) : 0);
	be32_put(out + 28, g_gm.state == GM_OTHER ? (u32)g_gm.refusal : g_gm.error);
	be32_put(out + 32, g_gm.load_us);
	be32_put(out + 36, g_gm.module_build_id);
	return GAME_MODULE_SIZE;
}
