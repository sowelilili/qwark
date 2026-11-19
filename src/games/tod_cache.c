/*
 * Tools of Destruction: the cutscene cache's three features (revision 1.29;
 * PROTOCOL.md 5.17; REPORT-tod-level-cache.md sections 2, 5.A, 10
 * and 11): Cache cutscenes, its auto on reset and Disable cutscene caching,
 * which write the level warmer (g_levelPrefetch) and the FIOS block table
 * (tod.h has the layouts). Its state is tod_game.c's (struct tod_cc_state).
 *
 * The level cache's debug pages that shared this file (DEBUG_TEXT, 0x00E8)
 * are retired: the op is never reused (PROTOCOL.md 5.16).
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"
#include "../core/proto.h"
#include "../core/util.h"

#include <string.h>

/* A NUL-ended string of the game's at `at`, at most cap - 1 bytes; "" when it cannot be read. */
static void rd_str(u32 at, char *dst, u32 cap)
{
	u32 n = 0;

	dst[0] = 0;
	if (at == 0 || cap < 2) return;
	while (n + 1 < cap) {
		u32 piece = cap - 1 - n;

		if (piece > 32u) piece = 32u;
		if (mem_read(at + n, dst + n, piece) != ST_OK) break;
		while (piece-- > 0) {
			if (dst[n] == 0) return;
			n++;
		}
	}
	dst[n] = 0;
}

/*
 * game.toc's level table (TodLevelArchives[count], sorted by directory name):
 * the entry for directory `dir` into rec, and its address, or 0 when the
 * table cannot be read or has no such entry. Its pointers are heap addresses,
 * so it is looked up each time.
 */
static u32 level_entry(const char *dir, u8 rec[TOD_LA_STRIDE])
{
	char name[48];
	u32 toc = 0, table = 0, count = 0, i;

	if (mem_read_u32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, &toc) != ST_OK || toc == 0 || (toc & 3u)) return 0;
	if (mem_read_u32(toc + TOD_TOC_LEVELS, &table) != ST_OK || table == 0 || (table & 3u)) return 0;
	if (mem_read_u32(table - 4u, &count) != ST_OK || count == 0 || count > TOD_LA_MAX) return 0;
	for (i = 0; i < count; i++) {
		if (mem_read(table + i * TOD_LA_STRIDE, rec, TOD_LA_STRIDE) != ST_OK) return 0;
		rd_str(be32_get(rec), name, sizeof(name));
		if (qstreq(name, dir)) return table + i * TOD_LA_STRIDE;
	}
	return 0;
}

/* ===================================== the cutscene cache, revision 1.29 */

/*
 * Cache cutscenes (19), Cache cutscenes on reset (20) and Disable cutscene
 * caching (21); tod.h has what the game does, why each write is safe, and the
 * status word's layout, REPORT-tod-level-cache.md sections 2.3, 5.A and 10
 * the evidence. Everything here is a data write, so it works the same on a
 * console and under RPCS3 (where cutscenes load in well under a second
 * anyway). It never blocks: the tick looks at the warmer every TOD_CC_EVERY
 * ticks, one 0x58-byte read, and the warmer does the reading inside the game,
 * 256 KB a frame, while the game plays on.
 *
 * A request goes ahead of the game's own warm-up, which in a run is the
 * current planet's sound, dialogue and textures and the next planet's
 * level_cached: about 380 MiB that a runner never stays long enough on
 * Metropolis to see read. The planets go in the order a run needs them:
 * Cobalia's sound first (its opening cinematic, l02_s03_wake_rt, plays as a
 * run arrives from Metropolis), then Stratus City's, then Fastoon's, about
 * 182 MiB in all; a level load hands what is left to the next level. A load
 * into a planet whose sound is not in yet is too late for that planet alone:
 * it is dropped and the rest goes on, and the request ends saying so. Its
 * state is tod_game.c's (struct tod_cc_state, the lent area in a module),
 * cleared as a session enters, as the cache is at every boot.
 */

static const char * const cc_dir[TOD_CC_NPLANETS]  = { "fastoon", "stratus city", "cobalia" };
static const char * const cc_name[TOD_CC_NPLANETS] = { "Fastoon", "Stratus City", "Cobalia" };
/* Each planet's LEVEL_: a load into it before its sound is in is too late. */
static const s32 cc_level[TOD_CC_NPLANETS] = { 3, 2, TOD_LEVEL_COBALIA };
/* Each planet's FAILED reason when it was too late. */
static const u8 cc_late_why[TOD_CC_NPLANETS] = { TOD_CC_WHY_LATE_FN, TOD_CC_WHY_LATE_SC, TOD_CC_WHY_LATE_CB };

/*
 * streaming_sound.dat in each planet's level_uncached.psarc, from the psarc's
 * own TOC (checked against the PSN copy's files, 2026-11-01). The .jp twin
 * follows it with the same length (2026-11-19, the same TOCs: Cobalia's at
 * 69,919,408, Stratus City's at 66,944,500, Fastoon's at 52,633,177), and a
 * console set to Japanese reads it instead (tod.h, TOD_SYSTEM_LANGUAGE). A
 * planet's level_uncached.toc is only in memory while that planet is loaded,
 * so these are this version's numbers; game.toc's size of the archive is
 * checked against the end before a range goes to the warmer.
 */
static const u32 cc_sound_at[TOD_CC_NPLANETS]  = { 7769u, 9716u, 10928u };
static const u32 cc_sound_len[TOD_CC_NPLANETS] = { 52625408u, 66934784u, 69908480u };

/* game.toc's directory for each LEVEL_, tod.c's order: what the FIOS cache's paths name. */
static const char * const cc_level_dir[TOD_NPLANETS] = {
	"metropolis", "cobalia", "stratus city", "fastoon", "space combat i",
	"imperial fight fest", "apogee space station", "pirate base", "space combat ii",
	"rykan v", "sargasso", "iris", "zordoom prison", "space combat iii",
	"kerchu city", "slags_fleet", "cragmite ruins", "meridian city", "fastoon_return"
};

static u32 cc_secs(u32 ticks)
{
	return (ticks + TOD_CC_TICK_HZ / 2u) / TOD_CC_TICK_HZ;
}

static int cc_active(const struct tod_cc_state *cc)
{
	return cc->state == TOD_CC_WAITING || cc->state == TOD_CC_CACHING;
}

/* In gameplay on a level: what the warmer needs and what a range is handed over in. */
static int cc_in_play(const struct tod_as_state *as, int loading)
{
	return as->cur_state == TOD_STATE_GAMEPLAY && as->cur_lua != 0 && as->cur_front == 0 && !loading;
}

/* What was read, in tenths of a MiB/s over the ticks it was read in: 0 for nothing read. */
static u8 cc_rate(const struct tod_cc_state *cc)
{
	u64 r;

	if (cc->bytes == 0) return 0;
	if (cc->rd_ticks == 0) return 255;
	r = (u64)cc->bytes * 10u * TOD_CC_TICK_HZ / ((u64)cc->rd_ticks << 20);
	if (r == 0) r = 1;
	return (u8)(r > 255u ? 255u : r);
}

static void cc_end(struct tod_cc_state *cc, u8 state, u8 detail)
{
	static const char * const why[] = {
		"", "the title came first", "game.toc's level table cannot be read",
		"the warmer stopped reading", "Disable cutscene caching is on", "the game cannot be read",
		"the game is going", "too late for Stratus City", "too late for Fastoon", "too late for Cobalia",
		"the game reads a sound file qwark does not know"
	};

	cc->secs = cc_secs(cc->ticks);
	cc->state = state;
	cc->detail = detail;
	cc->pct = state == TOD_CC_DONE ? cc_rate(cc) : 0u;
	cc->desc = 0;
	cc->borrow = 0;
	cc->in_hand = 0;
	cc->sc_active = 0;
	cc->seq++;
	if (state == TOD_CC_DONE)
		plat_log("tod: Cache cutscenes: Cobalia, Stratus City and Fastoon are cached (%s, %u s, %u KiB read in %u ticks)",
		         cc->jp ? "streaming_sound.jp.dat" : "streaming_sound.dat", cc->secs, cc->bytes >> 10, cc->rd_ticks);
	else
		plat_log("tod: Cache cutscenes: ended, %s", why[detail < sizeof(why) / sizeof(why[0]) ? detail : 0]);
}

/* The planet to read next, other than `not`: the one a run needs first, Cobalia, then Stratus City, then Fastoon. */
static u8 cc_first_pending(const struct tod_cc_state *cc, u8 not)
{
	u8 p = TOD_CC_NPLANETS;

	while (p-- > 0)
		if ((cc->pending & (1u << p)) && not != p) return p;
	return TOD_CC_NO_PLANET;
}

/*
 * Each planet's .uncached descriptor in game.toc's level table (heap
 * addresses, so looked up at each request and each load) and its whole sound
 * range: the sound file the game reads, LevelPrefetch_setup's choice
 * (revision 1.34, tod.h): the game's own from its warmer's slot 0 once
 * seen, and until then its language's, the .jp twin for Japanese. 0, or why
 * not.
 */
static u8 cc_plan(struct tod_cc_state *cc)
{
	u8 rec[TOD_LA_STRIDE];
	u32 at, lang = 0, p;

	if (cc->planned) return 0;
	if (cc->jp_game != TOD_CC_JP_UNKNOWN) {
		cc->jp = cc->jp_game;
	} else {
		if (mem_read_u32(TOD_SYSTEM_LANGUAGE, &lang) != ST_OK) return TOD_CC_WHY_READ;
		cc->jp = (u8)(lang == TOD_LANG_JAPANESE);
	}
	for (p = 0; p < TOD_CC_NPLANETS; p++) {
		u32 s = cc_sound_at[p], e = cc_sound_at[p] + cc_sound_len[p];

		at = level_entry(cc_dir[p], rec);
		if (at == 0) return TOD_CC_WHY_TABLE;
		if (cc->jp) {
			s = e;
			e += cc_sound_len[p];
		}
		if (be32_get(rec + TOD_LA_UNCACHED) == 0 || e > be32_get(rec + TOD_LA_UNCACHED + 4u)) return TOD_CC_WHY_TABLE;
		cc->pdesc[p] = at + TOD_LA_UNCACHED;
		cc->pfs[p] = s;
		cc->pfe[p] = e;
	}
	cc->planned = 1;
	return 0;
}

#define CC_SOUND_ODD 0xFEu   /* a streaming_sound.<x>.dat that is neither of the two */

/*
 * The game's own choice of the sound file for the level it is on (revision
 * 1.34): the path of the descriptor in the warmer's slot 0, which
 * LevelPrefetch_setup took from the level's TOC by the language. 1 the .jp
 * twin, 0 streaming_sound.dat, CC_SOUND_ODD a streaming_sound file of
 * another name, TOD_CC_JP_UNKNOWN anything else (no slot 0, a path that
 * cannot be read, one of ours in a slot borrowed from it).
 */
static u8 cc_game_sound(const u8 *pf)
{
	char path[TOD_CC_PATH_LEN];
	u32 desc = be32_get(pf + TOD_PF_FILE), str = 0, n, i, base = 0;

	if (desc == 0 || (desc & 3u) != 0 || mem_read_u32(desc, &str) != ST_OK || str == 0) return TOD_CC_JP_UNKNOWN;
	rd_str(str, path, sizeof(path));
	n = qstrlen(path);
	for (i = 0; i < n; i++) if (path[i] == '/') base = i;
	if (n - base < sizeof(TOD_SOUND_STEM) - 1u || memcmp(path + base, TOD_SOUND_STEM, sizeof(TOD_SOUND_STEM) - 1u) != 0)
		return TOD_CC_JP_UNKNOWN;
	if (qstreq(path + base, TOD_SOUND_NAME)) return 0;
	if (qstreq(path + base, TOD_SOUND_NAME_JP)) return 1;
	return CC_SOUND_ODD;
}

/* What is left of planet p's range. */
static void cc_range(const struct tod_cc_state *cc, u8 p, u32 *start, u32 *end)
{
	*start = cc->pfs[p];
	*end = cc->pfe[p];
	if (cc->need_hi[p] != 0) {
		if (cc->need_lo[p] > *start) *start = cc->need_lo[p];
		if (cc->need_hi[p] < *end) *end = cc->need_hi[p];
	}
}

static u8 cc_pct_at(const struct tod_cc_state *cc, u32 off)
{
	u32 s = cc->pfs[cc->planet], e = cc->pfe[cc->planet];

	if (off <= s) return 0;
	if (off >= e) return 100;
	return (u8)((u64)(off - s) * 100u / (e - s));
}

/* Planet p is the one queued or read now. */
static void cc_take(struct tod_cc_state *cc, u8 p)
{
	cc->planet = p;
	cc->desc = cc->pdesc[p];
	cc_range(cc, p, &cc->start, &cc->end);
	cc->in_hand = 0;
	cc->queued = 0;
	cc->next = TOD_CC_NO_PLANET;
	cc->stall = 0;
	cc->state = TOD_CC_CACHING;
	cc->detail = (u8)(p + 1u);
	cc->pct = cc_pct_at(cc, cc->start);
}

static void cc_planet_in(struct tod_cc_state *cc, u8 p)
{
	cc->pending &= (u8)~(1u << p);
	cc->need_lo[p] = 0;
	cc->need_hi[p] = 0;
	plat_log("tod: Cache cutscenes: %s's sound is in", cc_name[p]);
}

/* The warmer let `planet` go short of its end: what is left starts on the block it was in. */
static void cc_left_at(struct tod_cc_state *cc)
{
	u32 at = cc->last_off & ~(TOD_CC_BLOCK - 1u);

	if (!cc->in_hand || at <= cc->start || at >= cc->end) return;
	cc->need_lo[cc->planet] = at;
	cc->need_hi[cc->planet] = cc->end;
}

/* The offset in the warmer's hands, of ours: what it read, counted inside our range only. */
static int cc_progress(struct tod_cc_state *cc, u32 off)
{
	if (off > cc->end) off = cc->end;
	if (off > cc->last_off) {
		if (off > cc->start) cc->bytes += off - (cc->last_off > cc->start ? cc->last_off : cc->start);
		cc->last_off = off;
		cc->stall = 0;
		return 1;
	}
	if (off < cc->last_off) cc->last_off = off;   /* an offset of ours the callback wrote over */
	return 0;
}

/* ------------------------------------------- the request's block-table walk */

/* What a path handle names for the request: 1 + the planet whose level_uncached.psarc it is, or 0. */
static u8 sc_verdict(struct tod_cc_state *cc, u32 handle)
{
	char path[TOD_CC_PATH_LEN], want[TOD_CC_PATH_LEN];
	u32 str = 0, j, p;
	u8 v = 0;

	for (j = 0; j < cc->ev_nh; j++) if (cc->ev_handle[j] == handle) return cc->ev_verdict[j];
	if (cc->ev_strs != 0 && handle >= 1u && handle <= cc->ev_npaths &&
	    mem_read_u32(cc->ev_strs + (handle - 1u) * 4u, &str) == ST_OK) {
		rd_str(str, path, sizeof(path));
		for (p = 0; p < TOD_CC_NPLANETS && v == 0; p++) {
			qstrcpy(want, sizeof(want), "/packed/levels/");
			qstrcat(want, sizeof(want), cc_dir[p]);
			qstrcat(want, sizeof(want), "/level_uncached.psarc");
			if (qstreq(path, want)) v = (u8)(p + 1u);
		}
	}
	if (cc->ev_nh < TOD_CC_HANDLES) {
		cc->ev_handle[cc->ev_nh] = handle;
		cc->ev_verdict[cc->ev_nh] = v;
		cc->ev_nh++;
	}
	return v;
}

/*
 * The request's walk over the block table, at the request and at each level
 * load: which 1 MiB blocks of each planet's range the cache holds already, so
 * that only what is not is read. It shares Disable's walk fields, and the two
 * never run together (Disable ends a request and keeps any from starting).
 * Without the table, or the level table, nothing is known and the whole
 * range is read.
 */
static void sc_begin(struct tod_cc_state *cc)
{
	u32 cache = 0, n = 0, blocks = 0, paths = 0, bsize = 0, p;

	cc->sc_active = 0;
	cc->ev_active = 0;
	memset(cc->sc_map, 0, sizeof(cc->sc_map));
	memset(cc->sc_count, 0, sizeof(cc->sc_count));
	if (cc_plan(cc) != 0) return;
	if (mem_read_u32(TOD_FIOS_CACHE, &cache) != ST_OK || cache == 0 || (cache & 3u)) return;
	if (mem_read_u32(cache + TOD_FC_NBLOCKS, &n) != ST_OK || mem_read_u32(cache + TOD_FC_BLOCKS, &blocks) != ST_OK ||
	    mem_read_u32(cache + TOD_FC_BLOCK_SIZE, &bsize) != ST_OK)
		return;
	if (n == 0 || n > TOD_FC_MAX_BLOCKS || blocks == 0 || (blocks & 3u) || bsize < 0x10000u) return;
	if (mem_read_u32(TOD_FIOS_PATHS, &paths) != ST_OK || paths == 0 || (paths & 3u) ||
	    mem_read_u32(paths + TOD_FPATH_STRS, &cc->ev_strs) != ST_OK ||
	    mem_read_u32(paths + TOD_FPATH_COUNT, &cc->ev_npaths) != ST_OK)
		return;
	for (p = 0; p < TOD_CC_NPLANETS; p++) {
		u32 first = cc->pfs[p] / bsize, count = (cc->pfe[p] - 1u) / bsize - first + 1u;

		cc->sc_first[p] = first;
		cc->sc_count[p] = count <= TOD_CC_MAP_WORDS * 32u ? count : 0u;
	}
	cc->sc_bsize = bsize;
	cc->ev_blocks = blocks;
	cc->ev_n = n;
	cc->ev_i = 0;
	cc->ev_nh = 0;
	cc->sc_active = 1;
}

/* The walk's end: each planet's first and last block not in the cache bound what is left of it. */
static void sc_finish(struct tod_cc_state *cc)
{
	u32 p, i, lo, hi;

	cc->sc_active = 0;
	for (p = 0; p < TOD_CC_NPLANETS; p++) {
		u32 n = cc->sc_count[p], i0 = n, i1 = 0;

		if (!(cc->pending & (1u << p)) || n == 0) continue;
		for (i = 0; i < n; i++) {
			if (cc->sc_map[p][i >> 5] & (1u << (i & 31u))) continue;
			if (i0 == n) i0 = i;
			i1 = i;
		}
		if (i0 == n) {
			cc->pending &= (u8)~(1u << p);
			cc->need_lo[p] = 0;
			cc->need_hi[p] = 0;
			plat_log("tod: Cache cutscenes: %s's sound is in the cache already", cc_name[p]);
			continue;
		}
		lo = (cc->sc_first[p] + i0) * cc->sc_bsize;
		hi = (cc->sc_first[p] + i1 + 1u) * cc->sc_bsize;
		cc->need_lo[p] = lo > cc->pfs[p] ? lo : cc->pfs[p];
		cc->need_hi[p] = hi < cc->pfe[p] ? hi : cc->pfe[p];
		plat_log("tod: Cache cutscenes: %s's sound, %u of its %u blocks to read", cc_name[p], i1 - i0 + 1u, n);
	}
}

/* One piece of the walk, TOD_CC_PIECE descriptors a tick: a filling block counts as in. */
static void sc_step(struct tod_cc_state *cc)
{
	u8 buf[TOD_CC_PIECE * TOD_FC_DESC_LEN];
	u32 piece = cc->ev_n - cc->ev_i < TOD_CC_PIECE ? cc->ev_n - cc->ev_i : TOD_CC_PIECE, k;

	if (mem_read(cc->ev_blocks + cc->ev_i * TOD_FC_DESC_LEN, buf, piece * TOD_FC_DESC_LEN) != ST_OK) {
		cc->sc_active = 0;   /* nothing known: the whole ranges */
		return;
	}
	for (k = 0; k < piece; k++) {
		const u8 *d = buf + k * TOD_FC_DESC_LEN;
		u64 use = be64_get(d + TOD_FC_LAST_USE), off = be64_get(d + TOD_FC_OFFSET);
		u32 blk, rel;
		u8 v, p;

		if (use == 0 || use == TOD_FC_FREE || (off >> 32) != 0) continue;
		v = sc_verdict(cc, be32_get(d + TOD_FC_PATH));
		if (v == 0) continue;
		p = (u8)(v - 1u);
		blk = (u32)off / cc->sc_bsize;
		if (blk < cc->sc_first[p]) continue;
		rel = blk - cc->sc_first[p];
		if (rel >= cc->sc_count[p]) continue;
		cc->sc_map[p][rel >> 5] |= 1u << (rel & 31u);
	}
	cc->ev_i += piece;
	if (cc->ev_i >= cc->ev_n) sc_finish(cc);
}

/* ------------------------------------------------ handing ranges over */

static void cc_request(struct tod_cc_state *cc, int is_auto, int in_play)
{
	cc->state = TOD_CC_WAITING;
	cc->detail = in_play ? TOD_CC_WAIT_WARMER : TOD_CC_WAIT_PLAY;
	cc->planet = TOD_CC_NPLANETS - 1u;
	cc->pct = 0;
	cc->ticks = 0;
	cc->seen_play = (u8)(in_play != 0);
	cc->is_auto = (u8)(is_auto != 0);
	cc->read_fails = 0;
	cc->stall = 0;
	cc->wait = 0;
	cc->desc = 0;
	cc->pending = TOD_CC_ALL;
	cc->late = 0;
	cc->planned = 0;
	cc->jp_game = TOD_CC_JP_UNKNOWN;
	cc->sound_looked = 0;
	cc->in_hand = 0;
	cc->borrow = 0;
	cc->queued = 0;
	cc->bytes = 0;
	cc->rd_ticks = 0;
	memset(cc->need_lo, 0, sizeof(cc->need_lo));
	memset(cc->need_hi, 0, sizeof(cc->need_hi));
	sc_begin(cc);
}

/*
 * The warmer idle: `p`'s range with the single-range form. chunkSize, offset
 * and fileEnd in one write, then curFile and state, then readReady last:
 * LevelPrefetch_update does nothing while readReady is 0, which it is
 * whenever the warmer is idle.
 */
static void cc_single(struct tod_cc_state *cc, u8 p)
{
	u8 w[12];
	u32 chunk;

	cc_take(cc, p);
	chunk = cc->end - cc->start < TOD_PF_CHUNK_MAX ? cc->end - cc->start : TOD_PF_CHUNK_MAX;
	be32_put(w, chunk);
	be32_put(w + 4, cc->start);
	be32_put(w + 8, cc->end);
	if (mem_write(TOD_PREFETCH + TOD_PF_CHUNK, w, 12) != ST_OK) {
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	be32_put(w, cc->desc);
	be32_put(w + 4, TOD_PF_NFILES);
	if (mem_write(TOD_PREFETCH + TOD_PF_CUR, w, 8) != ST_OK) {
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	if (mem_write_u32(TOD_PREFETCH + TOD_PF_READY, 1) != ST_OK) {
		mem_write_u32(TOD_PREFETCH + TOD_PF_CUR, 0);   /* the warmer idle as it was */
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	cc->in_hand = 1;
	cc->last_off = cc->start;
	plat_log("tod: Cache cutscenes: %s's sound, bytes %u to %u, the warmer idle", cc_name[p], cc->start, cc->end);
}

/* A slot's file and size, the step's next take. */
static int cc_put_slot(u32 k, u32 desc, u32 size)
{
	return mem_write_u32(TOD_PREFETCH + TOD_PF_SIZE + k * 4u, size) == ST_OK &&
	       mem_write_u32(TOD_PREFETCH + TOD_PF_FILE + k * 4u, desc) == ST_OK;
}

/*
 * Ahead of the game's own: the warmer reads its slot k (state k + 1, curFile
 * file[k]) with more than TOD_CC_MARGIN of it left. `p`'s descriptor and end
 * go into slot k, state back to k, and the slot's own read is cut
 * TOD_CC_GRACE past where it is, so the step takes ours next, from 0. A cut
 * the offset has already reached is undone (the slot read whole, then ours,
 * then the game's next slot).
 */
static void cc_borrow(struct tod_cc_state *cc, u8 p, const u8 *pf)
{
	u32 k = be32_get(pf + TOD_PF_STATE) - 1u, off = be32_get(pf + TOD_PF_OFFSET), fend = be32_get(pf + TOD_PF_END);
	u32 cut = off + TOD_CC_GRACE, now = 0;

	cc_take(cc, p);
	cc->slot = (u8)k;
	cc->slot_desc = be32_get(pf + TOD_PF_CUR);
	cc->slot_size = be32_get(pf + TOD_PF_SIZE + k * 4u);
	cc->slot_cut = 0;
	cc->last_off = off;   /* the slot's own offset, until ours is in hand */
	if (!cc_put_slot(k, cc->desc, cc->end) || mem_write_u32(TOD_PREFETCH + TOD_PF_STATE, k) != ST_OK) {
		cc_put_slot(k, cc->slot_desc, cc->slot_size);
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	cc->borrow = 1;
	if (cut < fend && mem_write_u32(TOD_PREFETCH + TOD_PF_END, cut) == ST_OK) {
		if (mem_read_u32(TOD_PREFETCH + TOD_PF_OFFSET, &now) == ST_OK && now < cut) cc->slot_cut = cut;
		else mem_write_u32(TOD_PREFETCH + TOD_PF_END, fend);
	}
	plat_log("tod: Cache cutscenes: %s's sound, bytes %u to %u, ahead of the warmer's slot %u (cut at %u)",
	         cc_name[p], cc->start, cc->end, k, cc->slot_cut);
}

/*
 * Ours in hand in the borrowed slot, with room before its end: the slot gets
 * what follows it, the next planet or the game's own file, and state k again
 * so that the step takes it. The game's file needs state k only if its read
 * was cut; whole, the warmer goes on to its next slot.
 */
static void cc_queue_next(struct tod_cc_state *cc)
{
	u8 n = cc_first_pending(cc, cc->planet);
	u32 s, e;
	int ok;

	if (n != TOD_CC_NO_PLANET) {
		cc_range(cc, n, &s, &e);
		ok = cc_put_slot(cc->slot, cc->pdesc[n], e) && mem_write_u32(TOD_PREFETCH + TOD_PF_STATE, cc->slot) == ST_OK;
	} else {
		ok = cc_put_slot(cc->slot, cc->slot_desc, cc->slot_size) &&
		     (cc->slot_cut == 0 || mem_write_u32(TOD_PREFETCH + TOD_PF_STATE, cc->slot) == ST_OK);
	}
	cc->next = n;
	cc->queued = 1;
	if (!ok) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
}

/* Give the slot back if it still holds one of ours (a load's LevelPrefetch_setup fills it anew). */
static void cc_unborrow(struct tod_cc_state *cc, const u8 *pf)
{
	u32 f;

	if (!cc->borrow) return;
	cc->borrow = 0;
	f = be32_get(pf + TOD_PF_FILE + cc->slot * 4u);
	if (f != 0 && (f == cc->pdesc[0] || f == cc->pdesc[1] || f == cc->pdesc[2])) {
		cc_put_slot(cc->slot, cc->slot_desc, cc->slot_size);
		plat_log("tod: Cache cutscenes: slot %u given back unread", (u32)cc->slot);
	}
}

/* The warmer idle: every file started, none in hand, none in flight, and the game letting it run. */
static int cc_idle(const u8 *pf)
{
	return be32_get(pf + TOD_PF_STATE) == TOD_PF_NFILES && be32_get(pf + TOD_PF_CUR) == 0 &&
	       be32_get(pf + TOD_PF_BUSY) == 0 && pf[TOD_PF_ENABLED] != 0;
}

/* The warmer on one of its own slots with room to go ahead of it. */
static int cc_borrowable(const u8 *pf)
{
	u32 st = be32_get(pf + TOD_PF_STATE), cur = be32_get(pf + TOD_PF_CUR);
	u32 off = be32_get(pf + TOD_PF_OFFSET), end = be32_get(pf + TOD_PF_END);

	return st >= 1u && st <= TOD_PF_NFILES && cur != 0 && cur == be32_get(pf + TOD_PF_FILE + (st - 1u) * 4u) &&
	       be32_get(pf + TOD_PF_CHUNK) != 0 && end > off && end - off > TOD_CC_MARGIN;
}

/*
 * Every planet in or too late: done, or failed with the first late planet's
 * reason (Cobalia's before Stratus City's before Fastoon's) when one was.
 */
static void cc_finish(struct tod_cc_state *cc)
{
	u8 p = TOD_CC_NPLANETS;

	while (p-- > 0) {
		if (cc->late & (1u << p)) {
			cc_end(cc, TOD_CC_FAILED, cc_late_why[p]);
			return;
		}
	}
	cc_end(cc, TOD_CC_DONE, cc->jp ? TOD_CC_DONE_JP : 0u);
}

/* Waiting: a range to the warmer, ahead of its own slot or with it idle. */
static void cc_wait(struct tod_cc_state *cc, const u8 *pf, int play)
{
	u8 p, why;

	if (cc->pending == 0) {
		cc_finish(cc);
		return;
	}
	if (!play) {
		cc->detail = TOD_CC_WAIT_PLAY;
		return;
	}
	cc->detail = TOD_CC_WAIT_WARMER;
	if (cc->sc_active) return;   /* a moment: the block table first */
	/*
	 * Once a level, before a range goes: the sound file the game itself
	 * reads. One qwark does not know fails the request rather than caching
	 * a file the cutscenes never read; the other of the two plans again,
	 * with a new walk of the block table for its blocks.
	 */
	if (!cc->sound_looked) {
		u8 v = cc_game_sound(pf);

		cc->sound_looked = 1;
		if (v == CC_SOUND_ODD) {
			cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_SOUND);
			return;
		}
		if (v != TOD_CC_JP_UNKNOWN) {
			cc->jp_game = v;
			if (cc->planned && v != cc->jp) {
				plat_log("tod: Cache cutscenes: the game reads %s, not what its language says: planned again",
				         v ? "streaming_sound.jp.dat" : "streaming_sound.dat");
				cc->planned = 0;
				memset(cc->need_lo, 0, sizeof(cc->need_lo));
				memset(cc->need_hi, 0, sizeof(cc->need_hi));
				sc_begin(cc);
				return;
			}
		}
	}
	why = cc_plan(cc);
	if (why != 0) {
		cc_end(cc, TOD_CC_FAILED, why);
		return;
	}
	p = cc_first_pending(cc, TOD_CC_NO_PLANET);
	if (cc_idle(pf)) cc_single(cc, p);
	else if (cc_borrowable(pf)) cc_borrow(cc, p, pf);
}

/*
 * Following ours. Returns 0 when the request is waiting again (the caller
 * looks for the next range at once), 1 otherwise.
 */
static int cc_follow(struct tod_cc_state *cc, const u8 *pf, int play)
{
	u32 cur = be32_get(pf + TOD_PF_CUR), st = be32_get(pf + TOD_PF_STATE);
	u32 off = be32_get(pf + TOD_PF_OFFSET), end = be32_get(pf + TOD_PF_END);
	int on = play && pf[TOD_PF_ENABLED] != 0;

	if (cur == cc->desc) {
		if (!cc->in_hand) {
			cc->in_hand = 1;
			cc->last_off = 0;   /* a slot's file is read from 0 */
		}
		if (!cc_progress(cc, off) && on) {
			cc->stall += TOD_CC_EVERY;
			if (cc->stall >= TOD_CC_STALL_TICKS) {
				cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_STALLED);
				return 1;
			}
		}
		if (on && cc->last_off >= cc->start) cc->rd_ticks += TOD_CC_EVERY;
		cc->pct = cc_pct_at(cc, off);
		/* From 0 on to where what is left starts, at least 256 KB before its end. */
		if (off < cc->start && cc->start - off >= TOD_CC_BLOCK && cc->start + TOD_PF_CHUNK_MAX <= end)
			mem_write_u32(TOD_PREFETCH + TOD_PF_OFFSET, cc->start);
		if (cc->borrow && !cc->queued && st == cc->slot + 1u && end > off && end - off > TOD_CC_MARGIN)
			cc_queue_next(cc);
		return 1;
	}

	if (cc->in_hand) {
		/*
		 * The warmer let ours go: a step at its end (a load the tick sees
		 * first). Only the end of ours makes the single-range form idle, or
		 * the borrowed slot's step take what was queued after it; anything
		 * else is a step too if ours was all but read, or the warmer set up
		 * anew under it.
		 */
		u32 after = cc->slot + 1u + (cc->next == TOD_CC_NO_PLANET && cc->slot_cut == 0 ? 1u : 0u);

		if ((cur == 0 && st == TOD_PF_NFILES) ||
		    (cc->borrow && cc->queued && cur != 0 && st == after && st <= TOD_PF_NFILES &&
		     cur == be32_get(pf + TOD_PF_FILE + (st - 1u) * 4u)) ||
		    cc->last_off >= cc->end || cc->end - cc->last_off <= TOD_CC_SLACK) {
			if (cc->last_off < cc->end)
				cc->bytes += cc->end - (cc->last_off > cc->start ? cc->last_off : cc->start);
			cc_planet_in(cc, cc->planet);
		} else {
			cc_left_at(cc);
			plat_log("tod: Cache cutscenes: %s's sound let go at %u of %u", cc_name[cc->planet], cc->last_off, cc->end);
		}
		cc->in_hand = 0;
		if (cc->borrow && cc->queued && cc->next != TOD_CC_NO_PLANET && cur == cc->pdesc[cc->next] &&
		    st == cc->slot + 1u && (cc->pending & (1u << cc->next))) {
			cc_take(cc, cc->next);   /* the next planet, from the same slot */
			return 1;
		}
		if (cc->borrow && cur == cc->slot_desc && st == cc->slot + 1u) {
			/* The game's own file again, from 0: on to where it was cut. */
			if (cc->slot_cut != 0 && off < cc->slot_cut && cc->slot_cut + TOD_PF_CHUNK_MAX <= end)
				mem_write_u32(TOD_PREFETCH + TOD_PF_OFFSET, cc->slot_cut);
			cc->borrow = 0;
		}
		cc_unborrow(cc, pf);
		cc->state = TOD_CC_WAITING;
		return 0;
	}

	/* Queued, not yet taken: the slot's own read runs on to its cut. */
	if (cc->borrow && cur == cc->slot_desc && st == cc->slot) {
		if (off != cc->last_off) {
			cc->last_off = off;
			cc->stall = 0;
		} else if (on) {
			cc->stall += TOD_CC_EVERY;
			if (cc->stall >= TOD_CC_STALL_TICKS) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_STALLED);
		}
		return 1;
	}

	/* Neither: the warmer was set up anew under it, or let it go unread. */
	cc_unborrow(cc, pf);
	cc->state = TOD_CC_WAITING;
	return 0;
}

/* One look at the warmer: follow the range it has, or hand it the next. */
static void cc_look(struct tod_cc_state *cc, const struct tod_as_state *as, int loading)
{
	u8 pf[TOD_PF_LEN];
	int play = cc_in_play(as, loading);

	if (mem_read(TOD_PREFETCH, pf, sizeof(pf)) != ST_OK) {
		if (++cc->read_fails >= TOD_CC_READ_FAILS) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	cc->read_fails = 0;
	if (play) cc->seen_play = 1;
	if (cc->state == TOD_CC_CACHING && cc_follow(cc, pf, play)) return;
	if (cc->state == TOD_CC_WAITING) cc_wait(cc, pf, play);
}

/*
 * A level load has begun under a request: LevelPrefetch_setup will give the
 * warmer the new level's files, so what was in hand is let go (no write: the
 * setup fills every slot anew) and what is left goes ahead on the new level.
 * A load into a planet whose sound is not in yet is too late for that one:
 * it is dropped, the rest goes on, and the end says so.
 */
static void cc_load(struct tod_cc_state *cc, s32 level)
{
	u32 p;

	if (cc->state == TOD_CC_CACHING) cc_left_at(cc);
	cc->state = TOD_CC_WAITING;
	cc->detail = TOD_CC_WAIT_PLAY;
	cc->in_hand = 0;
	cc->borrow = 0;
	cc->queued = 0;
	cc->desc = 0;
	for (p = 0; p < TOD_CC_NPLANETS; p++) {
		if ((cc->pending & (1u << p)) && level == cc_level[p]) {
			cc->pending &= (u8)~(1u << p);
			cc->late |= (u8)(1u << p);
			plat_log("tod: Cache cutscenes: the run reached %s before its sound was in: too late for it", cc_name[p]);
		}
	}
	if (cc->pending == 0) {
		cc_finish(cc);
		return;
	}
	cc->planned = 0;
	cc->sound_looked = 0;
	sc_begin(cc);
	plat_log("tod: Cache cutscenes: a level load, the rest goes ahead on the next level");
}

/* ------------------------------------------- Disable cutscene caching */

/*
 * What a path handle names, learnt once per walk: 1 the planet's
 * level_uncached.psarc, 2 global_uncached.psarc on a walk for Metropolis,
 * whose sound the game serves from there, 0 anything else.
 */
static u8 ev_verdict(struct tod_cc_state *cc, u32 handle)
{
	char path[TOD_CC_PATH_LEN];
	u32 str = 0, j;
	u8 v = 0;

	for (j = 0; j < cc->ev_nh; j++) if (cc->ev_handle[j] == handle) return cc->ev_verdict[j];
	if (cc->ev_strs != 0 && handle >= 1u && handle <= cc->ev_npaths &&
	    mem_read_u32(cc->ev_strs + (handle - 1u) * 4u, &str) == ST_OK) {
		rd_str(str, path, sizeof(path));
		if (qstreq(path, cc->ev_path)) v = 1;
		else if (cc->ev_metro && qstreq(path, TOD_GLOBAL_UNCACHED)) v = 2;
	}
	if (cc->ev_nh < TOD_CC_HANDLES) {
		cc->ev_handle[cc->ev_nh] = handle;
		cc->ev_verdict[cc->ev_nh] = v;
		cc->ev_nh++;
	}
	return v;
}

/* A level load into `level` has begun: the walk over the block table starts. */
static void ev_begin(struct tod_cc_state *cc, s32 level)
{
	u32 cache = 0, n = 0, blocks = 0, paths = 0;

	cc->ev_active = 0;
	cc->sc_active = 0;
	if (level < 0 || level >= (s32)TOD_NPLANETS) return;
	if (mem_read_u32(TOD_FIOS_CACHE, &cache) != ST_OK || cache == 0 || (cache & 3u)) return;
	if (mem_read_u32(cache + TOD_FC_NBLOCKS, &n) != ST_OK || mem_read_u32(cache + TOD_FC_BLOCKS, &blocks) != ST_OK) return;
	if (n == 0 || n > TOD_FC_MAX_BLOCKS || blocks == 0 || (blocks & 3u)) return;
	cc->ev_strs = 0;
	cc->ev_npaths = 0;
	if (mem_read_u32(TOD_FIOS_PATHS, &paths) != ST_OK || paths == 0 || (paths & 3u) ||
	    mem_read_u32(paths + TOD_FPATH_STRS, &cc->ev_strs) != ST_OK ||
	    mem_read_u32(paths + TOD_FPATH_COUNT, &cc->ev_npaths) != ST_OK)
		return;

	qstrcpy(cc->ev_path, sizeof(cc->ev_path), "/packed/levels/");
	qstrcat(cc->ev_path, sizeof(cc->ev_path), cc_level_dir[level]);
	qstrcat(cc->ev_path, sizeof(cc->ev_path), "/level_uncached.psarc");
	cc->ev_metro = (u8)(level == TOD_LEVEL_METROPOLIS);
	cc->ev_blocks = blocks;
	cc->ev_n = n;
	cc->ev_i = 0;
	cc->ev_nh = 0;
	cc->ev_freed = 0;
	cc->ev_missed = 0;
	cc->ev_active = 1;
}

/*
 * One piece of the walk, TOD_CC_PIECE descriptors, on each tick of the load:
 * every block of the planet's sound archive that holds data (not free, not
 * filling) gets lastUse 2, which FiosSchedulerCache_findBlock skips and
 * claimBlock takes first. The piece is read again after its writes; a block
 * whose lastUse a FIOS hit refreshed in between is counted as missed. Nothing
 * reads a planet's level_uncached.psarc during its own load, which is why the
 * walk only runs then, and stops when the load does.
 */
static void ev_step(struct tod_cc_state *cc, int loading)
{
	u8 buf[TOD_CC_PIECE * TOD_FC_DESC_LEN], two[8];
	u32 piece, k, at, victims = 0;

	if (!loading || !cc->evict_on) {
		plat_log("tod: Disable cutscene caching: the load ended at block %u of %u", cc->ev_i, cc->ev_n);
		cc->ev_active = 0;
		return;
	}
	piece = cc->ev_n - cc->ev_i < TOD_CC_PIECE ? cc->ev_n - cc->ev_i : TOD_CC_PIECE;
	at = cc->ev_blocks + cc->ev_i * TOD_FC_DESC_LEN;
	if (mem_read(at, buf, piece * TOD_FC_DESC_LEN) != ST_OK) {
		cc->ev_active = 0;
		return;
	}
	be64_put(two, TOD_FC_FREE);
	for (k = 0; k < piece; k++) {
		const u8 *d = buf + k * TOD_FC_DESC_LEN;
		u64 use = be64_get(d + TOD_FC_LAST_USE);
		u8 v;

		if (use == 0 || use == TOD_FC_FREE || use == TOD_FC_FILLING) continue;
		v = ev_verdict(cc, be32_get(d + TOD_FC_PATH));
		if (v == 0) continue;
		if (v == 2 && be64_get(d + TOD_FC_OFFSET) >= TOD_METRO_SOUND_END) continue;
		if (mem_write(at + k * TOD_FC_DESC_LEN + TOD_FC_LAST_USE, two, 8) != ST_OK) continue;
		victims |= 1u << k;
		cc->ev_freed++;
	}
	if (victims != 0 && mem_read(at, buf, piece * TOD_FC_DESC_LEN) == ST_OK) {
		for (k = 0; k < piece; k++)
			if ((victims & (1u << k)) && be64_get(buf + k * TOD_FC_DESC_LEN + TOD_FC_LAST_USE) != TOD_FC_FREE)
				cc->ev_missed++;
	}
	cc->ev_i += piece;
	if (cc->ev_i >= cc->ev_n) {
		plat_log("tod: Disable cutscene caching: %u blocks of %s freed, %u missed",
		         (u32)cc->ev_freed, cc->ev_path, (u32)cc->ev_missed);
		cc->ev_active = 0;
	}
}

/* --------------------------------------------------------- the seam */

void tod_cc_forget(struct tod_cc_state *cc)
{
	memset(cc, 0, sizeof(*cc));
	cc->jp_game = TOD_CC_JP_UNKNOWN;
}

void tod_cc_tick(struct tod_cc_state *cc, const struct tod_as_state *as)
{
	int loading = tod_level_loading(as->cur_job, as->cur_tier), began;

	if (!cc->primed) {
		cc->primed = 1;
		cc->loading = (u8)loading;
		return;
	}
	began = loading && !cc->loading;
	cc->loading = (u8)loading;

	if (cc_active(cc)) cc->ticks++;

	/* The title ends a request that has seen gameplay; one made at the title waits through the load out of it. */
	if (cc_active(cc) && as->cur_front != 0 && (cc->seen_play || cc->state == TOD_CC_CACHING))
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_LEVEL);

	if (began) {
		if (cc->evict_on) ev_begin(cc, as->cur_level);
		/* A new run's load: the autosplitter's RESET, by the same two tests. */
		if (cc->auto_on && !cc->evict_on && tod_fresh_run_load(as->cur_level)) {
			cc_request(cc, 1, 0);
			plat_log("tod: Cache cutscenes on reset: a new run, ahead of Metropolis's warmer");
		} else if (cc_active(cc)) {
			cc_load(cc, as->cur_level);
		}
	}
	if (cc->ev_active) ev_step(cc, loading);
	if (cc->sc_active) sc_step(cc);

	if (cc_active(cc)) {
		if (cc->wait != 0) {
			cc->wait--;
		} else {
			cc->wait = TOD_CC_EVERY - 1u;
			cc_look(cc, as, loading);
		}
	}
}

void tod_cc_quit(struct tod_cc_state *cc)
{
	if (cc_active(cc)) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_QUIT);
	cc->ev_active = 0;
	cc->sc_active = 0;
}

/*
 * Cache cutscenes: a request, which goes ahead of the warmer's own work in
 * gameplay and carries on through level loads. One at a time (BUSY while one
 * runs); refused while Disable cutscene caching is on, which would free the
 * blocks again at the planet's load.
 */
int tod_cc_trigger(struct tod_cc_state *cc, const struct tod_as_state *as)
{
	if (cc_active(cc)) return ST_BUSY;
	if (cc->evict_on) {
		cc->ticks = 0;
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_DISABLED);
		return ST_BUSY;
	}
	cc_request(cc, 0, cc_in_play(as, cc->loading));
	plat_log("tod: Cache cutscenes: asked for");
	return ST_OK;
}

/*
 * The two toggles, qwark's own state (not LIVE, so either may come back at
 * boot). Disable cutscene caching ends a request under way (a range already
 * in the warmer's slot is read, and freed again at its planet's load) and
 * keeps the auto from asking.
 */
int tod_cc_set(struct tod_cc_state *cc, u8 id, int on)
{
	switch (id) {
	case TF_CACHE_ON_RESET:
		cc->auto_on = (u8)(on != 0);
		return ST_OK;
	case TF_NO_CUTSCENE_CACHE:
		cc->evict_on = (u8)(on != 0);
		if (on && cc_active(cc)) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_DISABLED);
		return ST_OK;
	default:
		return ST_NOT_FOUND;
	}
}

u32 tod_cc_status(const struct tod_cc_state *cc)
{
	u32 secs = cc_active(cc) ? cc_secs(cc->ticks) : cc->secs;

	if (secs > 255u) secs = 255u;
	return ((u32)cc->state << 28) | ((u32)(cc->detail & 15u) << 24) | ((u32)cc->pct << 16) | (secs << 8) | cc->seq;
}

/*
 * The console menu's row under Cache cutscenes: a value of nineteen characters.
 * Revision 1.34: and, as MENU_STATUS_LINE, the client's line under the button
 * (ACTION_STATUS), worded as the client worded it from its own copy of the
 * word's layout until then.
 */
u8 tod_cc_menu_status(u8 id, u32 word, u8 form, char *out, u32 cap)
{
	static const char * const why[] = {
		"Failed", "Title came first", "No level table", "Warmer stopped", "Caching disabled",
		"Game not readable", "Game closed", "Too late: Stratus", "Too late: Fastoon", "Too late: Cobalia",
		"Unknown sound file"
	};
	static const char * const why_line[] = {
		"unknown reason", "the title screen came before it finished", "the game's level table could not be read",
		"the game stopped reading", "Disable cutscene caching is on", "the game could not be read",
		"the game closed"
	};
	static const char * const late[] = { "Stratus City", "Fastoon", "Cobalia" };
	u32 detail = (word >> 24) & 15u, pct = (word >> 16) & 0xFFu, secs = (word >> 8) & 0xFFu;
	const char *planet = detail == 3u ? "Cobalia " : detail == 2u ? "Stratus City " : "Fastoon ";
	int line = form == MENU_STATUS_LINE;

	out[0] = 0;
	if (id != TF_CACHE_CUTSCENES) return MENU_TONE_NONE;
	switch (word >> 28) {
	case TOD_CC_WAITING:
		if (line)
			qstrcpy(out, cap, detail == TOD_CC_WAIT_WARMER
			        ? "Cutscene cache: waiting for the game's level caching to start..."
			        : "Cutscene cache: waiting for gameplay...");
		else
			qstrcpy(out, cap, detail == TOD_CC_WAIT_WARMER ? "Waiting for warmer" : "Waiting");
		return MENU_TONE_BUSY;
	case TOD_CC_CACHING:
		if (line) qstrcpy(out, cap, "Caching cutscenes: ");
		if (line && (detail == 0 || detail > 3u)) qstrcat(out, cap, "a planet ");
		else qstrcat(out, cap, planet);
		qfmt_u32(out + qstrlen(out), cap - qstrlen(out), pct);
		qstrcat(out, cap, "%");
		return MENU_TONE_BUSY;
	case TOD_CC_DONE:
		/*
		 * "Done (104s)", the seconds alone (2026-11-02, the user's call: the
		 * client's line keeps the read rate), or nothing read: every block
		 * was in already.
		 */
		if (pct == 0) {
			qstrcpy(out, cap, line ? "Cutscenes already cached (all three planets" : "Already cached");
			if (line) qstrcat(out, cap, (detail & TOD_CC_DONE_JP) ? ", Japanese audio)" : ")");
			return MENU_TONE_GOOD;
		}
		qstrcpy(out, cap, line ? "Cutscenes cached (" : "Done (");
		qfmt_u32(out + qstrlen(out), cap - qstrlen(out), secs);
		if (line) {
			/* The read rate in tenths of a MiB/s, as "13.9": no float printf here. */
			qstrcat(out, cap, " s, ");
			qfmt_u32(out + qstrlen(out), cap - qstrlen(out), pct / 10u);
			qstrcat(out, cap, ".");
			qfmt_u32(out + qstrlen(out), cap - qstrlen(out), pct % 10u);
			/* Revision 1.34: which files, when they were the Japanese ones. */
			qstrcat(out, cap, (detail & TOD_CC_DONE_JP) ? " MiB/s, Japanese audio)" : " MiB/s)");
		} else {
			qstrcat(out, cap, "s)");
		}
		return MENU_TONE_GOOD;
	case TOD_CC_FAILED:
		if (!line) {
			qstrcpy(out, cap, why[detail < sizeof(why) / sizeof(why[0]) ? detail : 0]);
		} else if (detail >= 7u && detail <= 9u) {
			qstrcpy(out, cap, "Cutscene cache: too late for ");
			qstrcat(out, cap, late[detail - 7u]);
			qstrcat(out, cap, ", the run got there first");
		} else if (detail == TOD_CC_WHY_SOUND) {
			qstrcpy(out, cap, "Cutscene caching stopped: the game's cutscene sound file is not one qwark knows");
		} else {
			qstrcpy(out, cap, "Cutscene caching stopped: ");
			qstrcat(out, cap, why_line[detail < sizeof(why_line) / sizeof(why_line[0]) ? detail : 0]);
		}
		return MENU_TONE_BAD;
	default:
		if (!line) qstrcpy(out, cap, "Not this boot");
		return MENU_TONE_NONE;
	}
}
