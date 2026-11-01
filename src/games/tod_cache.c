/*
 * Tools of Destruction: the level cache's debug pages (revision 1.29,
 * pending; DEBUG_TEXT, PROTOCOL.md 5.16; REPORT-tod-level-cache.md sections 2
 * and 9), which only read, and, at the end of the file, the cutscene cache's
 * three features (Cache cutscenes, its auto on reset, Disable cutscene
 * caching), which share the pages' level table and block table code and
 * write the warmer and the block table (tod.h).
 *
 *   page 0, "Now"    what loads right now: the game state, the level warmer
 *                    (g_levelPrefetch) and its gates, what an opening
 *                    cutscene is waiting for, and the save thread. A dozen
 *                    small reads, so a client may ask for it a few times a
 *                    second.
 *   page 1, "Cache"  the FIOS block cache on /dev_hdd1, summed per file and
 *                    ordered by the newest block, last to be evicted first.
 *                    The block table is 70 KB, read in qscratch-sized
 *                    pieces, so it is SLOW: asked for on the user's word only.
 *
 * Each page is plain text the client shows as it is, so the client keeps no
 * address and no layout of the game's. A value that cannot be read says so in
 * the text; the page itself still answers OK.
 *
 * The pages keep no state of their own: everything lives on the tick
 * thread's stack for one call (about 2 KB) and in the core's scratch buffer.
 * The cutscene cache's state is tod_game.c's (struct tod_cc_state).
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"
#include "../core/proto.h"
#include "../core/util.h"

#include <string.h>

/* ------------------------------------------------------------- the text */

struct tbuf {
	char *p;
	u32 cap;
	u32 n;
};

static void t_str(struct tbuf *t, const char *s)
{
	while (*s != 0 && t->n + 1 < t->cap) t->p[t->n++] = *s++;
}

static void t_u32(struct tbuf *t, u32 v)
{
	char d[12];

	qfmt_u32(d, sizeof(d), v);
	t_str(t, d);
}

static void t_s32(struct tbuf *t, s32 v)
{
	if (v < 0) {
		t_str(t, "-");
		t_u32(t, (u32)(-(v + 1)) + 1u);
	} else t_u32(t, (u32)v);
}

static void t_hex(struct tbuf *t, u32 v)
{
	static const char hx[] = "0123456789ABCDEF";
	char d[11];
	int i;

	d[0] = '0';
	d[1] = 'x';
	for (i = 0; i < 8; i++) d[2 + i] = hx[(v >> (28 - 4 * i)) & 15u];
	d[10] = 0;
	t_str(t, d);
}

/* `whole` and one or two decimals: v hundredths, or tenths when `digits` is 1. */
static void t_fixed(struct tbuf *t, u32 v, u32 digits)
{
	u32 div = digits == 1 ? 10u : 100u;
	u32 frac = v % div;

	t_u32(t, v / div);
	t_str(t, ".");
	if (digits == 2 && frac < 10) t_str(t, "0");
	t_u32(t, frac);
}

/* Bytes as MiB with one decimal. */
static void t_mib(struct tbuf *t, u32 bytes)
{
	t_fixed(t, (u32)(((u64)bytes * 10u + 0x80000u) >> 20), 1);
	t_str(t, " MiB");
}

static void t_nl(struct tbuf *t)
{
	t_str(t, "\n");
}

/* An f32 of seconds as hundredths, 0 for a negative or not-a-number. */
static u32 secs_cs(u32 bits)
{
	f32 v = f32_from_bits(bits);

	if (!(v > 0.0f)) return 0;
	if (v > 100000.0f) return 10000000u;
	return (u32)(v * 100.0f + 0.5f);
}

/* ------------------------------------------------------------- the reads */

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

static u32 rd32(u32 at, int *ok)
{
	u32 v = 0;

	if (mem_read_u32(at, &v) != ST_OK) *ok = 0;
	return v;
}

static u8 rd8(u32 at, int *ok)
{
	u8 v = 0;

	if (mem_read_u8(at, &v) != ST_OK) *ok = 0;
	return v;
}

/* A TodFileDesc's path and size; 0 when there is none. */
static int rd_file(u32 desc, char *path, u32 cap, u32 *size)
{
	u8 b[8];

	path[0] = 0;
	*size = 0;
	if (desc == 0 || (desc & 3u) != 0) return 0;
	if (mem_read(desc, b, sizeof(b)) != ST_OK) return 0;
	*size = be32_get(b + 4);
	rd_str(be32_get(b), path, cap);
	return 1;
}

/* ---------------------------------------------------------- page 0, Now */

static const char *game_state_name(s32 s)
{
	switch (s) {
	case 3:  return "a level load";
	case 4:  return "gameplay";
	case 7:  return "an FMV";
	case 9:  return "a cinematic";
	case 11: return "the credits";
	default: return NULL;
	}
}

static const char *movie_state_name(u32 s)
{
	switch (s) {
	case 0:  return "idle";
	case 1:  return "black, waiting to start";
	case 2:  return "playing";
	case 3:  return "fading back in";
	case 4:  return "done";
	default: return "?";
	}
}

static const char *save_state_name(u32 s)
{
	switch (s) {
	case 0:  return "not up";
	case 1:  return "idle";
	case 2:  return "saving";
	case 3:  return "loading";
	case 4:  return "deleting";
	case TOD_SS_HELD: return "held by qwark";
	default: return "?";
	}
}

static const char * const pf_slot_names[TOD_PF_NFILES] = {
	"sound", "dialogue", "textures", "next level"
};

static void page_now(struct tbuf *t)
{
	u8 pf[TOD_PF_LEN];
	u8 mp[TOD_MP_LOOK_LEN];
	char path[96];
	int ok = 1, pf_ok;
	s32 gs;
	u32 level, i, cur, state, size, fade_cs, sm_state, sm_op, queued;
	const char *name;

	gs = (s32)rd32(TOD_GAME_STATE, &ok);
	level = rd32(TOD_CURRENT_LEVEL, &ok);
	t_str(t, "Game state ");
	t_s32(t, gs);
	name = game_state_name(gs);
	if (name != NULL) {
		t_str(t, " (");
		t_str(t, name);
		t_str(t, ")");
	}
	t_str(t, ", level ");
	t_u32(t, level);
	if (level < tod_facts.nplanets) {
		t_str(t, " (");
		t_str(t, tod_facts.planet_names[level]);
		t_str(t, ")");
	}
	t_nl(t);

	/* The warmer. */
	pf_ok = mem_read(TOD_PREFETCH, pf, sizeof(pf)) == ST_OK;
	t_nl(t);
	if (!pf_ok) {
		t_str(t, "Warmer: cannot be read\n");
	} else {
		u8 gl = rd8(TOD_PF_GATE_LEVEL, &ok), gm = rd8(TOD_PF_GATE_MOVIE, &ok), pend = rd8(TOD_PF_PENDING, &ok);
		u32 off = be32_get(pf + TOD_PF_OFFSET), end = be32_get(pf + TOD_PF_END);

		cur = be32_get(pf + TOD_PF_CUR);
		state = be32_get(pf + TOD_PF_STATE);
		t_str(t, "Warmer: ");
		t_str(t, pf[TOD_PF_ENABLED] ? "on" : "off");
		t_str(t, " (level gate ");
		t_u32(t, gl);
		t_str(t, ", movie gate ");
		t_u32(t, gm);
		t_str(t, ", pending ");
		t_u32(t, pend);
		t_str(t, ")\n");
		if (cur == 0) {
			t_str(t, state >= TOD_PF_NFILES ? "  idle: every file read\n" : "  not started\n");
		} else {
			rd_file(cur, path, sizeof(path), &size);
			t_str(t, "  file ");
			t_u32(t, state);
			t_str(t, " of 4: ");
			t_str(t, path[0] != 0 ? path : "?");
			t_nl(t);
			t_str(t, "  at ");
			t_mib(t, off);
			t_str(t, " of ");
			t_mib(t, end);
			if (end > 0) {
				t_str(t, " (");
				t_u32(t, (u32)((u64)(off > end ? end : off) * 100u / end));
				t_str(t, "%)");
			}
			if (be32_get(pf + TOD_PF_BUSY) != 0) t_str(t, ", a read in flight");
			t_nl(t);
		}
		for (i = 0; i < TOD_PF_NFILES; i++) {
			u32 desc = be32_get(pf + TOD_PF_FILE + i * 4u);

			t_str(t, "  ");
			t_u32(t, i + 1u);
			t_str(t, " ");
			t_str(t, pf_slot_names[i]);
			t_str(t, ": ");
			if (!rd_file(desc, path, sizeof(path), &size)) {
				t_str(t, "none\n");
				continue;
			}
			t_mib(t, be32_get(pf + TOD_PF_SIZE + i * 4u));
			t_str(t, "  ");
			t_str(t, path);
			t_nl(t);
		}
	}

	/* What a cutscene waits for. */
	t_nl(t);
	if (mem_read(TOD_MOVIE_PLAYER + TOD_MP_STATE, mp, sizeof(mp)) != ST_OK) {
		t_str(t, "Cutscene: cannot be read\n");
	} else {
		u32 mstate = be32_get(mp), movie = be32_get(mp + (TOD_MP_MOVIE - TOD_MP_STATE));
		u32 rec = 0, nameptr = 0;

		t_str(t, "Cutscene: ");
		path[0] = 0;
		if (movie < TOD_NMOVIES && mem_read_u32(TOD_MOVIE_RECORDS + movie * 4u, &rec) == ST_OK &&
		    rec > 4u && mem_read_u32(rec - 4u, &nameptr) == ST_OK)
			rd_str(nameptr, path, 40);
		if (mstate == 0) {
			t_str(t, "none");
		} else {
			t_str(t, path[0] != 0 ? path : "?");
		}
		t_str(t, ", player ");
		t_u32(t, mstate);
		t_str(t, " (");
		t_str(t, movie_state_name(mstate));
		t_str(t, ")\n");
		queued = rd32(TOD_MOVIE_QUEUE_N, &ok);
		sm_state = rd32(TOD_SAVE_MANAGER + TOD_SM_STATE, &ok);
		sm_op = rd32(TOD_SAVE_MANAGER + TOD_SM_LAST_OP, &ok);
		if (mstate == 1) {
			fade_cs = secs_cs(rd32(TOD_MOVIE_PLAYER + TOD_MP_FADE_TIMER, &ok));
			t_str(t, "  waited ");
			t_fixed(t, fade_cs, 2);
			t_str(t, " s; the sounds' limit is ");
			t_fixed(t, TOD_PREROLL_LIMIT_CS, 2);
			t_str(t, " s");
			if (fade_cs > TOD_PREROLL_LIMIT_CS) t_str(t, " (passed)");
			t_str(t, ", ");
			t_u32(t, rd32(TOD_CINEMATIC + TOD_CINE_SOUNDS, &ok));
			t_str(t, " sounds pre-rolling\n");
			if (sm_state == 2 && sm_op == TOD_SAVE_OP_AUTO)
				t_str(t, "  held: the autosave is running\n");
		}
		if (queued != 0) {
			t_str(t, "  queued after it: ");
			t_u32(t, queued);
			t_nl(t);
		}
		t_str(t, "Save thread: ");
		t_str(t, save_state_name(sm_state));
		if (sm_state == 2) {
			t_str(t, sm_op == TOD_SAVE_OP_AUTO ? " (autosave)" : " (op ");
			if (sm_op != TOD_SAVE_OP_AUTO) {
				t_u32(t, sm_op);
				t_str(t, ")");
			}
		}
		t_nl(t);
	}
	if (!ok) t_str(t, "(some values could not be read)\n");
}

/* -------------------------------------------------------- page 1, Cache */

#define CACHE_FILES   40u     /* files summed; any past that are counted, not named */
#define CACHE_PIECE   (QSCRATCH_BYTES / TOD_FC_DESC_LEN)
#define NS_PER_CS     10000000u

/* How much older `old` is than `now`, hundredths of a second, at most a day. */
static u32 age_cs(u64 now, u64 old)
{
	u64 d = now > old ? (now - old) / NS_PER_CS : 0;

	return d > 8640000u ? 8640000u : (u32)d;
}

struct cache_file {
	u32 handle;
	u32 blocks;
	u32 lo_mib;     /* the lowest and highest block's offset in its file, MiB */
	u32 hi_mib;
	u64 newest;
	u64 oldest;
};

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

/* The archive's size, when `path` is one of the game's level archives (game.toc's table). */
static u32 level_archive_size(const char *path)
{
	static const char prefix[] = "/packed/levels/";
	static const char * const kinds[3] = {
		"level_cached.psarc", "level_uncached.psarc", "level_textures.psarc"
	};
	char dir[48];
	u32 i, k, slash = 0, p = sizeof(prefix) - 1;
	u8 rec[TOD_LA_STRIDE];

	for (i = 0; i < p; i++) if (path[i] != prefix[i]) return 0;
	while (path[p + slash] != 0 && path[p + slash] != '/') slash++;
	if (path[p + slash] != '/' || slash == 0 || slash >= sizeof(dir)) return 0;
	for (i = 0; i < slash; i++) dir[i] = path[p + i];
	dir[slash] = 0;
	for (k = 0; k < 3; k++) if (qstreq(path + p + slash + 1, kinds[k])) break;
	if (k == 3) return 0;

	if (level_entry(dir, rec) == 0) return 0;
	return be32_get(rec + 4u + k * 0x14u + 4u);
}

static void page_cache(struct tbuf *t)
{
	struct cache_file files[CACHE_FILES];
	char path[96];
	u32 cache = 0, blocks = 0, n = 0, bsize = 0, paths = 0, strs = 0, npaths = 0;
	u32 i, j, nfiles = 0, nfree = 0, nfill = 0, nused = 0, other_blocks = 0;
	u64 newest = 0, oldest = 0;
	u8 *buf = qscratch();

	if (mem_read_u32(TOD_FIOS_CACHE, &cache) != ST_OK || cache == 0 || (cache & 3u)) {
		t_str(t, "The cache is not set up (g_fiosSchedulerCache is 0).\n");
		return;
	}
	if (mem_read_u32(cache + TOD_FC_NBLOCKS, &n) != ST_OK || mem_read_u32(cache + TOD_FC_BLOCKS, &blocks) != ST_OK ||
	    mem_read_u32(cache + TOD_FC_BLOCK_SIZE, &bsize) != ST_OK) {
		t_str(t, "The cache object cannot be read.\n");
		return;
	}
	if (n == 0 || n > TOD_FC_MAX_BLOCKS || blocks == 0 || (blocks & 3u)) {
		t_str(t, "The block table does not look right: ");
		t_u32(t, n);
		t_str(t, " blocks at ");
		t_hex(t, blocks);
		t_nl(t);
		return;
	}

	for (i = 0; i < n; i += CACHE_PIECE) {
		u32 piece = n - i < CACHE_PIECE ? n - i : CACHE_PIECE, k;

		if (mem_read(blocks + i * TOD_FC_DESC_LEN, buf, piece * TOD_FC_DESC_LEN) != ST_OK) {
			t_str(t, "The block table cannot be read at block ");
			t_u32(t, i);
			t_nl(t);
			return;
		}
		for (k = 0; k < piece; k++) {
			const u8 *d = buf + k * TOD_FC_DESC_LEN;
			u64 use = be64_get(d + TOD_FC_LAST_USE);
			u32 handle = be32_get(d + TOD_FC_PATH);
			u32 mib = (u32)(be64_get(d + TOD_FC_OFFSET) >> 20);

			if (use == TOD_FC_FREE || use == 0) { nfree++; continue; }
			if (use == TOD_FC_FILLING) { nfill++; continue; }
			nused++;
			if (use > newest) newest = use;
			if (oldest == 0 || use < oldest) oldest = use;
			for (j = 0; j < nfiles; j++) if (files[j].handle == handle) break;
			if (j == nfiles) {
				if (nfiles == CACHE_FILES) { other_blocks++; continue; }
				files[j].handle = handle;
				files[j].blocks = 0;
				files[j].lo_mib = mib;
				files[j].hi_mib = mib;
				files[j].newest = use;
				files[j].oldest = use;
				nfiles++;
			}
			files[j].blocks++;
			if (mib < files[j].lo_mib) files[j].lo_mib = mib;
			if (mib > files[j].hi_mib) files[j].hi_mib = mib;
			if (use > files[j].newest) files[j].newest = use;
			if (use < files[j].oldest) files[j].oldest = use;
		}
	}

	/* Newest first: the order eviction takes them in, backwards. */
	for (i = 1; i < nfiles; i++) {
		struct cache_file f = files[i];

		for (j = i; j > 0 && files[j - 1].newest < f.newest; j--) files[j] = files[j - 1];
		files[j] = f;
	}

	t_u32(t, nused);
	t_str(t, " of ");
	t_u32(t, n);
	t_str(t, " blocks in use (");
	t_mib(t, bsize);
	t_str(t, " each), ");
	t_u32(t, nfree);
	t_str(t, " free");
	if (nfill != 0) {
		t_str(t, ", ");
		t_u32(t, nfill);
		t_str(t, " filling");
	}
	t_nl(t);
	if (nused != 0) {
		t_str(t, "The oldest block, next to go once none is free, is ");
		t_fixed(t, age_cs(newest, oldest), 2);
		t_str(t, " s older than the newest.\n");
	}
	t_str(t, "\nNewest first. blocks / of the file, newest and oldest block's age (s), offsets (MiB), path:\n");

	if (mem_read_u32(TOD_FIOS_PATHS, &paths) == ST_OK && paths != 0 && (paths & 3u) == 0) {
		if (mem_read_u32(paths + TOD_FPATH_STRS, &strs) != ST_OK) strs = 0;
		if (mem_read_u32(paths + TOD_FPATH_COUNT, &npaths) != ST_OK) npaths = 0;
	}
	for (i = 0; i < nfiles; i++) {
		const struct cache_file *f = &files[i];
		u32 str = 0, size = 0;

		path[0] = 0;
		if (strs != 0 && f->handle >= 1u && f->handle <= npaths &&
		    mem_read_u32(strs + (f->handle - 1u) * 4u, &str) == ST_OK)
			rd_str(str, path, sizeof(path));
		t_str(t, "  ");
		t_u32(t, f->blocks);
		if (path[0] != 0 && bsize != 0) size = level_archive_size(path);
		if (size != 0) {
			t_str(t, " / ");
			t_u32(t, (size + bsize - 1u) / bsize);
		}
		t_str(t, "  ");
		t_fixed(t, age_cs(newest, f->newest), 2);
		t_str(t, " .. ");
		t_fixed(t, age_cs(newest, f->oldest), 2);
		t_str(t, "  ");
		t_u32(t, f->lo_mib);
		t_str(t, "..");
		t_u32(t, f->hi_mib);
		t_str(t, "  ");
		if (path[0] != 0) t_str(t, path);
		else {
			t_str(t, "(path ");
			t_u32(t, f->handle);
			t_str(t, ")");
		}
		t_nl(t);
	}
	if (other_blocks != 0) {
		t_str(t, "  and ");
		t_u32(t, other_blocks);
		t_str(t, " blocks of other files\n");
	}
}

/* --------------------------------------------------------------- the op */

/* A title in its fixed field, the rest zeroed. */
static void put_title(u8 *dst, const char *src)
{
	u32 i = 0;

	while (i < DEBUG_TITLE_LEN && src[i] != 0) { dst[i] = (u8)src[i]; i++; }
	while (i < DEBUG_TITLE_LEN) dst[i++] = 0;
}

static const char * const page_titles[TOD_DEBUG_NPAGES] = { "Now", "Cache" };
static const u8 page_flags[TOD_DEBUG_NPAGES] = { 0, DEBUG_PAGE_SLOW };

int tod_debug_text(u8 page, u8 *out, u32 cap, u32 *len)
{
	struct tbuf t;

	*len = 0;
	if (page == DEBUG_TEXT_LIST) {
		u32 i, need = 1u + TOD_DEBUG_NPAGES * (1u + DEBUG_TITLE_LEN);

		if (cap < need) return ST_FULL;
		out[0] = (u8)TOD_DEBUG_NPAGES;
		for (i = 0; i < TOD_DEBUG_NPAGES; i++) {
			out[1 + i * (1u + DEBUG_TITLE_LEN)] = page_flags[i];
			put_title(out + 2 + i * (1u + DEBUG_TITLE_LEN), page_titles[i]);
		}
		*len = need;
		return ST_OK;
	}
	if (page >= TOD_DEBUG_NPAGES) return ST_BAD_ARG;
	if (cap < DEBUG_TEXT_HEAD + 64u) return ST_FULL;

	t.p = (char *)out + DEBUG_TEXT_HEAD;
	t.cap = cap - DEBUG_TEXT_HEAD;
	if (t.cap > 0xFFFFu) t.cap = 0xFFFFu;
	t.n = 0;
	if (page == TOD_DEBUG_NOW) page_now(&t);
	else page_cache(&t);

	be16_put(out, (u16)t.n);
	be16_put(out + 2, 0);
	*len = DEBUG_TEXT_HEAD + t.n;
	return ST_OK;
}

/* ===================================== the cutscene cache, revision 1.29 */

/*
 * Cache cutscenes (19), Cache cutscenes on reset (20) and Disable cutscene
 * caching (21); tod.h has what the game does and the status word's layout,
 * REPORT-tod-level-cache.md sections 5.A and 10 the evidence. Everything here
 * is a data write, so it works the same on a console and under RPCS3 (where
 * cutscenes load in well under a second anyway). It never blocks: the tick
 * looks at the warmer every TOD_CC_EVERY ticks, one 0x58-byte read, and the
 * warmer does the reading inside the game, 256 KB a frame, while the game
 * plays on. Its state is tod_game.c's (struct tod_cc_state, the lent area in
 * a module), cleared as a session enters, as the cache is at every boot.
 */

static const char * const cc_dir[TOD_CC_NPLANETS]  = { "fastoon", "stratus city" };
static const char * const cc_name[TOD_CC_NPLANETS] = { "Fastoon", "Stratus City" };

/*
 * streaming_sound.dat in each planet's level_uncached.psarc, from the psarc's
 * own TOC (checked against the PSN copy's files, 2026-11-01). The .jp twin
 * follows it with the same length. A planet's level_uncached.toc is only in
 * memory while that planet is loaded, so these are this version's numbers;
 * game.toc's size of the archive is checked against the end before a range
 * goes to the warmer.
 */
static const u32 cc_sound_at[TOD_CC_NPLANETS]  = { 7769u, 9716u };
static const u32 cc_sound_len[TOD_CC_NPLANETS] = { 52625408u, 66934784u };

/* game.toc's directory for each LEVEL_, tod.c's order: what the FIOS cache's paths name. */
static const char * const cc_level_dir[TOD_NPLANETS] = {
	"metropolis", "cobalia", "stratus city", "fastoon", "space combat i",
	"imperial fight fest", "apogee space station", "pirate base", "space combat ii",
	"rykan v", "sargasso", "iris", "zordoom prison", "space combat iii",
	"kerchu city", "slags_fleet", "cragmite ruins", "meridian city", "fastoon_return"
};

static u32 cc_secs(u32 ticks)
{
	return (ticks + 60u) / 120u;
}

static int cc_active(const struct tod_cc_state *cc)
{
	return cc->state == TOD_CC_WAITING || cc->state == TOD_CC_CACHING;
}

/* In gameplay on a level: what the warmer needs and what the range is handed over in. */
static int cc_in_play(const struct tod_as_state *as, int loading)
{
	return as->cur_state == TOD_STATE_GAMEPLAY && as->cur_lua != 0 && as->cur_front == 0 && !loading;
}

static void cc_end(struct tod_cc_state *cc, u8 state, u8 detail)
{
	static const char * const why[] = {
		"", "a level load or the title came first", "game.toc's level table cannot be read",
		"the warmer stopped reading", "Disable cutscene caching is on", "the game cannot be read",
		"the game is going"
	};

	cc->secs = cc_secs(cc->ticks);
	cc->state = state;
	cc->detail = detail;
	cc->pct = state == TOD_CC_DONE ? 100u : 0u;
	cc->desc = 0;
	cc->seq++;
	if (state == TOD_CC_DONE)
		plat_log("tod: Cache cutscenes: Fastoon and Stratus City are cached (%u s)", cc->secs);
	else
		plat_log("tod: Cache cutscenes: stopped, %s", why[detail < sizeof(why) / sizeof(why[0]) ? detail : 0]);
}

static void cc_request(struct tod_cc_state *cc, int is_auto, int in_play)
{
	cc->state = TOD_CC_WAITING;
	cc->detail = in_play ? TOD_CC_WAIT_WARMER : TOD_CC_WAIT_PLAY;
	cc->planet = 0;
	cc->pct = 0;
	cc->ticks = 0;
	cc->seen_play = (u8)(in_play != 0);
	cc->is_auto = (u8)(is_auto != 0);
	cc->read_fails = 0;
	cc->stall = 0;
	cc->wait = 0;
	cc->desc = 0;
}

/*
 * The warmer is idle: `cc->planet`'s sound to it, with the single-range form.
 * The planet's entry is looked up in game.toc each time (heap addresses).
 * chunkSize, offset and fileEnd in one write, then curFile and state, then
 * readReady last: LevelPrefetch_update does nothing while readReady is 0,
 * which it is whenever the warmer is idle.
 */
static void cc_start(struct tod_cc_state *cc)
{
	u8 rec[TOD_LA_STRIDE], w[12];
	u32 at, size, path, param = 0, start = 0, end, chunk;
	u8 p = cc->planet;

	at = level_entry(cc_dir[p], rec);
	if (at == 0) {
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_TABLE);
		return;
	}
	path = be32_get(rec + TOD_LA_UNCACHED);
	size = be32_get(rec + TOD_LA_UNCACHED + 4u);
	end = cc_sound_at[p] + cc_sound_len[p];
	/* LevelPrefetch_setup's own choice of the sound file: the .jp twin when the parameter is 2. */
	if (mem_read_u32(TOD_SOUND_PARAM, &param) == ST_OK && param == TOD_SOUND_PARAM_JP) {
		start = end;
		end += cc_sound_len[p];
	}
	if (path == 0 || end > size) {
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_TABLE);
		return;
	}

	chunk = end - start < TOD_PF_CHUNK_MAX ? end - start : TOD_PF_CHUNK_MAX;
	be32_put(w, chunk);
	be32_put(w + 4, start);
	be32_put(w + 8, end);
	if (mem_write(TOD_PREFETCH + TOD_PF_CHUNK, w, 12) != ST_OK) {
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	be32_put(w, at + TOD_LA_UNCACHED);
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

	cc->state = TOD_CC_CACHING;
	cc->detail = (u8)(p + 1u);
	cc->pct = 0;
	cc->desc = at + TOD_LA_UNCACHED;
	cc->start = start;
	cc->end = end;
	cc->last_off = start;
	cc->stall = 0;
	plat_log("tod: Cache cutscenes: %s's sound, bytes %u to %u of its level_uncached.psarc", cc_name[p], start, end);
}

/* The warmer idle: every file started, none in hand, none in flight, and the game letting it run. */
static int cc_idle(const u8 *pf)
{
	return be32_get(pf + TOD_PF_STATE) == TOD_PF_NFILES && be32_get(pf + TOD_PF_CUR) == 0 &&
	       be32_get(pf + TOD_PF_BUSY) == 0 && pf[TOD_PF_ENABLED] != 0;
}

/* One look at the warmer: start the next range when it is idle, follow the one it reads. */
static void cc_look(struct tod_cc_state *cc, const struct tod_as_state *as, int loading)
{
	u8 pf[TOD_PF_LEN];
	int play = cc_in_play(as, loading);
	u32 cur, off;

	if (mem_read(TOD_PREFETCH, pf, sizeof(pf)) != ST_OK) {
		if (++cc->read_fails >= TOD_CC_READ_FAILS) cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_READ);
		return;
	}
	cc->read_fails = 0;
	cur = be32_get(pf + TOD_PF_CUR);

	if (cc->state == TOD_CC_CACHING) {
		if (cur == cc->desc) {
			off = be32_get(pf + TOD_PF_OFFSET);
			if (off != cc->last_off) {
				cc->last_off = off;
				cc->stall = 0;
			} else if (play && pf[TOD_PF_ENABLED] != 0) {
				cc->stall += TOD_CC_EVERY;
				if (cc->stall >= TOD_CC_STALL_TICKS) {
					cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_STALLED);
					return;
				}
			}
			if (off > cc->end) off = cc->end;
			cc->pct = (u8)(off <= cc->start ? 0u : (u32)((u64)(off - cc->start) * 100u / (cc->end - cc->start)));
			return;
		}
		/* Not ours: LevelPrefetch_setup has run (a level load the tick missed). */
		if (cur != 0 || be32_get(pf + TOD_PF_STATE) != TOD_PF_NFILES) {
			cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_LEVEL);
			return;
		}
		/* curFile back to 0 with state 4: the range is in. */
		plat_log("tod: Cache cutscenes: %s's sound is in", cc_name[cc->planet]);
		if (++cc->planet >= TOD_CC_NPLANETS) {
			cc_end(cc, TOD_CC_DONE, 0);
			return;
		}
		cc->state = TOD_CC_WAITING;
		cc->pct = 0;
	}

	/* Waiting. */
	if (!play) {
		cc->detail = TOD_CC_WAIT_PLAY;
		return;
	}
	cc->seen_play = 1;
	if (!cc_idle(pf)) {
		cc->detail = TOD_CC_WAIT_WARMER;
		return;
	}
	cc_start(cc);
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

	/*
	 * A level load or the title ends a range in hand, and a request that has
	 * seen gameplay: LevelPrefetch_setup gives the warmer the next level's
	 * files. The auto's own request, made as its load began, waits on.
	 */
	if ((cc->state == TOD_CC_CACHING && (loading || as->cur_front != 0)) ||
	    (cc->state == TOD_CC_WAITING && cc->seen_play && (loading || as->cur_front != 0)))
		cc_end(cc, TOD_CC_FAILED, TOD_CC_WHY_LEVEL);

	if (began) {
		if (cc->evict_on) ev_begin(cc, as->cur_level);
		/* A new run's load: the autosplitter's RESET, by the same two tests. */
		if (cc->auto_on && !cc->evict_on && tod_fresh_run_load(as->cur_level)) {
			cc_request(cc, 1, 0);
			plat_log("tod: Cache cutscenes on reset: a new run, waiting for Metropolis's warmer");
		}
	}
	if (cc->ev_active) ev_step(cc, loading);

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
}

/*
 * Cache cutscenes: a request, which waits for gameplay and the warmer. One at
 * a time (BUSY while one runs); refused while Disable cutscene caching is on,
 * which would free the blocks again at the planet's load.
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
 * boot). Disable cutscene caching ends a request under way and keeps the
 * auto from asking.
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

/* The console menu's row under Cache cutscenes: a value of nineteen characters. */
void tod_cc_menu_status(u8 id, u32 word, char *out, u32 cap)
{
	static const char * const why[] = {
		"Failed", "Level changed", "No level table", "Warmer stopped", "Caching disabled",
		"Game not readable", "Game closed"
	};
	u32 detail = (word >> 24) & 15u, pct = (word >> 16) & 0xFFu, secs = (word >> 8) & 0xFFu;

	out[0] = 0;
	if (id != TF_CACHE_CUTSCENES) return;
	switch (word >> 28) {
	case TOD_CC_WAITING:
		qstrcpy(out, cap, detail == TOD_CC_WAIT_WARMER ? "Waiting for warmer" : "Waiting");
		return;
	case TOD_CC_CACHING:
		qstrcpy(out, cap, detail == 2u ? "Stratus City " : "Fastoon ");
		qfmt_u32(out + qstrlen(out), cap - qstrlen(out), pct);
		qstrcat(out, cap, "%");
		return;
	case TOD_CC_DONE:
		qstrcpy(out, cap, "Done, ");
		qfmt_u32(out + qstrlen(out), cap - qstrlen(out), secs);
		qstrcat(out, cap, " s");
		return;
	case TOD_CC_FAILED:
		qstrcpy(out, cap, why[detail < sizeof(why) / sizeof(why[0]) ? detail : 0]);
		return;
	default:
		qstrcpy(out, cap, "Not this boot");
		return;
	}
}
