/*
 * Tools of Destruction: the level cache's debug pages (revision 1.29,
 * pending; DEBUG_TEXT, PROTOCOL.md 5.16; REPORT-tod-level-cache.md sections 2
 * and 9). Read only: nothing in this file writes the game's memory.
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
 * No state of its own: everything lives on the tick thread's stack for one
 * call (about 2 KB) and in the core's scratch buffer.
 */
#include "core_import.h"   /* first: src/games/module.h */
#include "tod.h"
#include "../core/mem.h"
#include "../core/proto.h"
#include "../core/util.h"

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

/* The archive's size, when `path` is one of the game's level archives (game.toc's table). */
static u32 level_archive_size(const char *path)
{
	static const char prefix[] = "/packed/levels/";
	static const char * const kinds[3] = {
		"level_cached.psarc", "level_uncached.psarc", "level_textures.psarc"
	};
	char dir[48], name[48];
	u32 toc = 0, table = 0, count = 0, i, k, slash = 0, p = sizeof(prefix) - 1;
	u8 rec[TOD_LA_STRIDE];

	for (i = 0; i < p; i++) if (path[i] != prefix[i]) return 0;
	while (path[p + slash] != 0 && path[p + slash] != '/') slash++;
	if (path[p + slash] != '/' || slash == 0 || slash >= sizeof(dir)) return 0;
	for (i = 0; i < slash; i++) dir[i] = path[p + i];
	dir[slash] = 0;
	for (k = 0; k < 3; k++) if (qstreq(path + p + slash + 1, kinds[k])) break;
	if (k == 3) return 0;

	if (mem_read_u32(TOD_ARCHIVE_GAME + TOD_ARCHIVE_TOC, &toc) != ST_OK || toc == 0 || (toc & 3u)) return 0;
	if (mem_read_u32(toc + TOD_TOC_LEVELS, &table) != ST_OK || table == 0 || (table & 3u)) return 0;
	if (mem_read_u32(table - 4u, &count) != ST_OK || count == 0 || count > TOD_LA_MAX) return 0;
	for (i = 0; i < count; i++) {
		if (mem_read(table + i * TOD_LA_STRIDE, rec, sizeof(rec)) != ST_OK) return 0;
		rd_str(be32_get(rec), name, sizeof(name));
		if (qstreq(name, dir)) return be32_get(rec + 4u + k * 0x14u + 4u);
	}
	return 0;
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
