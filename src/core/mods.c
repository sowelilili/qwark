#include "mods.h"
#include "config.h"
#include "session.h"
#include "util.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * MOD_WORD_POOL, MOD_CAVE_POOL and MOD_TEXT_MAX are in mods.h, next to the
 * mod_entry that indexes them.
 *
 * A cave file is streamed into the game a MOD_CAVE_CHUNK at a time out of the
 * shared scratch buffer; it used to have a 64 KB buffer all to itself. A cave
 * region is memory nothing branches to until the mod's patch words go in, and
 * those go in as one batch after every cave byte has landed (see
 * mods_load_index), so how many pieces the cave arrives in is invisible to the
 * running game and 4 KB is as good as 64 KB. The largest cave in the shipped
 * library is 2968 bytes and the loop handles a cave of any size.
 */
#define MOD_CAVE_CHUNK  4096u
#define MOD_PATH_MAX    512

/*
 * gnu99, so no _Static_assert: a chunk bigger than the shared buffer fails the
 * build here rather than running off the end of it.
 */
typedef char mods_cave_chunk_fits[MOD_CAVE_CHUNK <= QSCRATCH_BYTES ? 1 : -1];

struct mod_cave {
	u32  addr;
	char file[64];
};

static struct mod_entry  g_mods[QWARK_MAX_MODS];
static u32               g_nmods;

static struct patch_word g_words[MOD_WORD_POOL];
static u32               g_originals[MOD_WORD_POOL];
static u16               g_words_used;

static struct mod_cave   g_caves[MOD_CAVE_POOL];
static u16               g_caves_used;

static char g_title[16];

/*
 * Build 49, the presence look (mods.h). `g_look_on` is whether this session
 * looks at all: set at INGAME on a platform that refuses code patches, cleared
 * when the session leaves the game. The cursor says how far the look has got:
 * which mod, which part of it (cave 0 .. ncaves-1, then its words at ncaves),
 * and how far into that part, in bytes for a cave and in words for the words.
 *
 * Build 51: only in the emulator builds. A console never looks, so on the PS3
 * every mod's `look` stays MOD_LOOK_NONE and arming it is nothing.
 */
#ifdef QWARK_EMULATOR
static u8  g_look_on;
static u32 g_look_mod;
static u16 g_look_part;
static u32 g_look_at;

static void look_arm_all(void);
#else
static __inline void look_arm_all(void) { }
#endif

/*
 * patch.txt is read into the core's shared text buffer (util.h) rather than into
 * one of its own: every read of it happens under the core lock and nothing holds
 * it across a return. MOD_TEXT_MAX is still the size a patch.txt is refused at.
 */
typedef char mods_text_fits[MOD_TEXT_MAX <= QTEXT_BYTES ? 1 : -1];

/* --------------------------------------------------------------- helpers */

static void mod_dir_path(char *out, u32 cap, const char *dirname, const char *leaf)
{
	out[0] = 0;
	qstrcat(out, cap, QWARK_MODSDIR);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, g_title);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, dirname);
	if (leaf != NULL) {
		qstrcat(out, cap, "/");
		qstrcat(out, cap, leaf);
	}
}

u32 mods_count(void) { return g_nmods; }

const struct mod_entry *mods_at(u32 index)
{
	if (index >= g_nmods || !g_mods[index].used) return NULL;
	return &g_mods[index];
}

int mods_find(const char *dirname)
{
	u32 i;
	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].used && qstreq(g_mods[i].dirname, dirname)) return (int)i;
	}
	return -1;
}

static int mods_find_by_name(const char *name)
{
	u32 i;
	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].used && qstreq(g_mods[i].name, name)) return (int)i;
	}
	return -1;
}

u32 mods_loaded_mask(void)
{
	u32 mask = 0;
	u32 i;
	for (i = 0; i < g_nmods && i < 32; i++) {
		if (g_mods[i].used && (g_mods[i].flags & MOD_FLAG_LOADED)) mask |= 1u << i;
	}
	return mask;
}

u32 mods_auto_mask(void)
{
	u32 mask = 0;
	u32 i;
	for (i = 0; i < g_nmods && i < 32; i++) {
		if (g_mods[i].used && (g_mods[i].flags & MOD_FLAG_AUTO)) mask |= 1u << i;
	}
	return mask;
}

void mods_forget_loaded(void)
{
	u32 i;
	for (i = 0; i < g_nmods; i++) g_mods[i].flags &= (u8)~MOD_FLAG_LOADED;
}

/* ---------------------------------------------------------------- parsing */

static int line_is_meta(const char *line)
{
	return line[0] == '#' && line[1] == '-';
}

static void parse_meta(struct mod_entry *m, char *line)
{
	char *colon;
	char *key;
	char *value;

	colon = line;
	while (*colon != 0 && *colon != ':') colon++;
	if (*colon != ':') return;

	*colon = 0;
	key   = qtrim(line + 2);
	value = qtrim(colon + 1);

	if (qstreq(key, "name"))             qstrcpy(m->name, sizeof(m->name), value);
	else if (qstreq(key, "version"))     qstrcpy(m->version, sizeof(m->version), value);
	else if (qstreq(key, "author"))      qstrcpy(m->author, sizeof(m->author), value);
	else if (qstreq(key, "depends"))     qstrcpy(m->depends, sizeof(m->depends), value);
	/* "description" is not kept: mods_info reads it back out of patch.txt. */
}

/*
 * Parses one mod folder into `m`. Words and caves are taken from the shared
 * pools; a mod that does not fit is marked parse_error and keeps no storage.
 */
static int parse_mod(struct mod_entry *m, const char *dirname)
{
	char path[MOD_PATH_MAX];
	char *text = qtext();
	char *cursor;
	char *line;
	u16 word_start = g_words_used;
	u16 cave_start = g_caves_used;
	u16 nwords = 0;
	int rc;

	memset(m, 0, sizeof(*m));
	qstrcpy(m->dirname, sizeof(m->dirname), dirname);
	qstrcpy(m->name, sizeof(m->name), dirname);   /* the C# falls back to the folder name */
	m->used = 1;

	mod_dir_path(path, sizeof(path), dirname, "patch.txt");
	rc = qread_file(path, text, MOD_TEXT_MAX, NULL);
	if (rc != ST_OK) {
		m->flags |= MOD_FLAG_PARSE_ERROR;
		return rc;
	}

	/* Pass one: the #- metadata lines. */
	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		if (line_is_meta(line)) parse_meta(m, line);
	}

	/*
	 * qwark.sum, written by the client after an upload: eight hex digits, with or
	 * without an 0x prefix. Always read as hex, never as decimal.
	 */
	{
		char sum[32];
		mod_dir_path(path, sizeof(path), dirname, "qwark.sum");
		if (qread_file(path, sum, sizeof(sum), NULL) == ST_OK) {
			char *trimmed = qtrim(sum);
			u8 raw[4];

			if (trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
				int ok = 0;
				u32 v = qparse_u32(trimmed, &ok);
				if (ok) m->hash = v;
			} else if (qhex_to_bytes(trimmed, raw, 4) == 4) {
				m->hash = be32_get(raw);
			}
		}
	}

	/* Pass two: the patch lines. The metadata pass already consumed the file, so
	 * re-read it: qnext_line writes NULs into the buffer. */
	mod_dir_path(path, sizeof(path), dirname, "patch.txt");
	if (qread_file(path, text, MOD_TEXT_MAX, NULL) != ST_OK) {
		m->flags |= MOD_FLAG_PARSE_ERROR;
		return ST_IO_ERROR;
	}

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *colon;
		char *addr_s;
		char *value_s;
		u32 addr;
		int ok = 0;

		if (qstrlen(line) < 2) continue;
		if (line[0] == '#') continue;      /* comments and metadata */

		colon = line;
		while (*colon != 0 && *colon != ':') colon++;
		if (*colon != ':') continue;
		*colon = 0;

		addr_s  = qtrim(line);
		value_s = qtrim(colon + 1);

		if (qstreq(addr_s, "automation")) {
			m->flags |= MOD_FLAG_NEEDS_LUA;
			continue;
		}

		if (!(addr_s[0] == '0' && (addr_s[1] == 'x' || addr_s[1] == 'X'))) continue;

		addr = qparse_u32(addr_s, &ok);
		if (!ok) continue;

		if (value_s[0] == '0' && (value_s[1] == 'x' || value_s[1] == 'X')) {
			u32 word = qparse_u32(value_s, &ok);
			if (!ok) continue;

			if (word_start + nwords >= MOD_WORD_POOL) {
				m->flags |= MOD_FLAG_PARSE_ERROR;
				g_words_used = word_start;
				g_caves_used = cave_start;
				return ST_FULL;
			}

			g_words[word_start + nwords].addr  = addr;
			g_words[word_start + nwords].value = word;
			nwords++;
		} else {
			if (g_caves_used >= MOD_CAVE_POOL) {
				m->flags |= MOD_FLAG_PARSE_ERROR;
				g_words_used = word_start;
				g_caves_used = cave_start;
				return ST_FULL;
			}
			g_caves[g_caves_used].addr = addr;
			qstrcpy(g_caves[g_caves_used].file, sizeof(g_caves[0].file), value_s);
			g_caves_used++;
		}
	}

	g_words_used = (u16)(word_start + nwords);

	m->cave_first = cave_start;
	m->ncaves     = (u16)(g_caves_used - cave_start);

	m->def.name      = m->name;
	m->def.kind      = PATCH_KIND_MOD;
	m->def.words     = &g_words[word_start];
	m->def.count     = nwords;
	m->def.originals = &g_originals[word_start];

	if (config_mod_auto(g_title, dirname)) m->flags |= MOD_FLAG_AUTO;

	return ST_OK;
}

/* ----------------------------------------------------------------- rescan */

/*
 * Packs the word, originals and cave pools down onto the entries the table
 * still holds, so the space the ones a rescan dropped were using comes back.
 * The pools are bump allocated and a mod owns one slice of each; walking the
 * table in order walks the slices in the order they were taken, so a slice only
 * ever moves down and never onto one that has not moved yet.
 *
 * A kept entry's def is re-pointed at its moved slice. The entry itself does
 * not move, and must not: patch.c's applied table holds the address of a loaded
 * mod's patch_def, and a def that moved would never be matched again to be
 * reverted. The words themselves are copied bytes for bytes, so a mod that is
 * loaded reverts with exactly the originals it captured.
 */
static void mods_pack_pools(void)
{
	u16 wdst = 0;
	u16 cdst = 0;
	u32 i;

	for (i = 0; i < g_nmods; i++) {
		struct mod_entry *m = &g_mods[i];
		u16 wsrc;

		if (!m->used || m->def.words == NULL) continue;

		wsrc = (u16)(m->def.words - (const struct patch_word *)g_words);

		if (m->def.count > 0 && wdst != wsrc) {
			memmove(&g_words[wdst], &g_words[wsrc],
			        (u32)m->def.count * sizeof(g_words[0]));
			memmove(&g_originals[wdst], &g_originals[wsrc],
			        (u32)m->def.count * sizeof(g_originals[0]));
		}
		m->def.words     = &g_words[wdst];
		m->def.originals = &g_originals[wdst];
		wdst = (u16)(wdst + m->def.count);

		if (m->ncaves > 0 && cdst != m->cave_first) {
			memmove(&g_caves[cdst], &g_caves[m->cave_first],
			        (u32)m->ncaves * sizeof(g_caves[0]));
		}
		m->cave_first = cdst;
		cdst = (u16)(cdst + m->ncaves);
	}

	g_words_used = wdst;
	g_caves_used = cdst;
}

/* The lowest free table slot, or QWARK_MAX_MODS when the table is full. */
static u32 mods_free_slot(void)
{
	u32 i;
	for (i = 0; i < QWARK_MAX_MODS; i++) {
		if (!g_mods[i].used) return i;
	}
	return QWARK_MAX_MODS;
}

int mods_rescan(void)
{
	char path[MOD_PATH_MAX];
	plat_dir_t dir;
	struct plat_dirent ent;
	u32 i;

	if (g_title[0] == 0) {
		g_nmods = 0;
		memset(g_mods, 0, sizeof(g_mods));
		g_words_used = 0;
		g_caves_used = 0;
		look_arm_all();
		return ST_OK;
	}

	/*
	 * Every folder is read again, because a rescan is what a client asks for
	 * the moment it has replaced a mod's files and it has to see the new ones:
	 * the new qwark.sum, which is the hash MOD_LIST reports and the only thing
	 * telling that client its upload landed, and the new patch words, which are
	 * what the next MOD_LOAD writes into the game.
	 *
	 * A mod that is loaded right now is the exception. It is in the running
	 * process: its words are in the pool next to the originals it will need to
	 * put back, and patch.c holds the address of its patch_def. So it keeps its
	 * entry, its slot and its slice and stays exactly as it was parsed, and a
	 * client that wants it re-read unloads it first - which is what the mods
	 * panel does, since it only ever uploads a mod it is about to load.
	 */
	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].used && (g_mods[i].flags & MOD_FLAG_LOADED)) continue;
		memset(&g_mods[i], 0, sizeof(g_mods[i]));
	}
	mods_pack_pools();
	while (g_nmods > 0 && !g_mods[g_nmods - 1].used) g_nmods--;

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), g_title);

	if (plat_dir_open(path, &dir) != 0) {   /* no mods installed yet */
		look_arm_all();
		return ST_OK;
	}

	while (plat_dir_next(&dir, &ent) == 1) {
		struct mod_entry tmp;
		u32 slot;

		if (!ent.is_dir) continue;
		if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
		if (mods_find(ent.name) >= 0) continue;   /* a loaded mod, left alone */

		slot = mods_free_slot();
		if (slot >= QWARK_MAX_MODS) break;

		parse_mod(&tmp, ent.name);
		tmp.index = (u8)slot;
		g_mods[slot] = tmp;
		/*
		 * parse_mod pointed def.name at the temporary's own name buffer, which
		 * dies with this loop iteration; the copy in the table has to point at
		 * its own. Without this PATCH_LIST read a dead stack frame for the name.
		 */
		g_mods[slot].def.name = g_mods[slot].name;
		if (slot + 1 > g_nmods) g_nmods = slot + 1;
	}

	plat_dir_close(&dir);

	/*
	 * Build 49. Whatever the last look found was found against the files this
	 * rescan has just replaced, so every mod is looked for again - the ones kept
	 * because they were loaded too, since their folders may have changed under
	 * them all the same.
	 */
	look_arm_all();
	return ST_OK;
}

void mods_set_title(const char *title)
{
	qstrcpy(g_title, sizeof(g_title), title ? title : "");

	g_nmods = 0;
	memset(g_mods, 0, sizeof(g_mods));
	g_words_used = 0;
	g_caves_used = 0;

	mods_rescan();
}

/* ------------------------------------------------------------ load/unload */

static int write_cave(const struct mod_entry *m, const struct mod_cave *cave)
{
	char path[MOD_PATH_MAX];
	plat_file_t f;
	/*
	 * The first MOD_CAVE_CHUNK of the tick thread's shared scratch buffer. A mod
	 * loads out of the command ring, which only the tick thread drains, and the
	 * bytes are gone again by the time this returns: see util.h.
	 */
	u8 *chunk = qscratch();
	u32 offset = 0;

	mod_dir_path(path, sizeof(path), m->dirname, cave->file);
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_NOT_FOUND;

	for (;;) {
		u32 got = 0;
		int rc;

		if (plat_file_read(f, chunk, MOD_CAVE_CHUNK, &got) != 0) {
			plat_file_close(f);
			return ST_IO_ERROR;
		}
		if (got == 0) break;

		rc = mem_write(cave->addr + offset, chunk, got);
		if (rc != ST_OK) {
			plat_file_close(f);
			return rc;
		}
		offset += got;
	}

	plat_file_close(f);
	return offset != 0 ? ST_OK : ST_IO_ERROR;
}

/*
 * The next name out of a "#- depends:" list, without writing into it.
 *
 * `*pos` is a byte offset into `list` and is carried across calls, which is what
 * lets the loader below keep one two-byte cursor per level instead of a
 * 128-byte copy of the list. Separators and empty names are skipped and each
 * name is trimmed, exactly as the old strtok-and-qtrim walk did. Returns 1 with
 * a name in `out`, 0 at the end of the list.
 */
static int depends_next(const char *list, u16 *pos, char *out, u32 cap)
{
	u32 i = *pos;
	u32 start, end, n;

	while (list[i] == ',' || list[i] == ' ' || list[i] == '\t' ||
	       list[i] == '\r' || list[i] == '\n') i++;

	if (list[i] == 0) { *pos = (u16)i; return 0; }

	start = i;
	while (list[i] != 0 && list[i] != ',') i++;
	*pos = (u16)i;

	end = i;
	while (end > start && (list[end - 1] == ' ' || list[end - 1] == '\t' ||
	                       list[end - 1] == '\r' || list[end - 1] == '\n')) end--;

	n = end - start;
	if (n >= cap) n = cap - 1;
	memcpy(out, list + start, n);
	out[n] = 0;
	return 1;
}

/*
 * One level of the load walk. Four bytes, so the whole work list is 136 bytes of
 * static storage rather than the 23 KB of tick-thread stack the recursion used
 * to reserve: load_dependencies and mods_load_index called each other and the
 * pair cost about 700 bytes a level for as many as 33 levels, which is the only
 * reason the tick thread ever needed a 48 KB stack.
 *
 * It is static and therefore shared, which is safe because only the tick thread
 * ever loads a mod: mods_load and mods_apply_mask are reached from the command
 * ring or from enter_ingame, both on that thread, and neither re-enters.
 */
struct load_frame {
	u8  index;
	u8  depth;
	u8  opened;      /* its own checks have passed; its depends list is being walked */
	u16 pos;         /* how far into that list the walk has got */
};

static struct load_frame g_load_stack[QWARK_MAX_MODS + 2];

/* The mod itself, once every dependency of it is in. */
static int mod_write(struct mod_entry *m)
{
	u16 c;
	int rc;

	/*
	 * Caves first, then the words: the words are usually branches into the cave,
	 * so the target exists before anything jumps to it.
	 *
	 * Unloading only puts the patch words back. The cave bytes are left in
	 * memory on purpose: restoring them crashed the game, because the branches
	 * into a cave go away a moment before its bytes would, and anything still
	 * executing there falls off the end. Reverting the branches first and
	 * leaving the cave bytes where they are is safe and is what racman does too.
	 *
	 * Nothing pauses the game meanwhile; racman paused the RSX, which stops the
	 * GPU and leaves every game thread running. The order is the protection, and
	 * patch_apply keeps it among the words too: the real dl-cs and quartu_patch
	 * list a branch ahead of the trampoline it jumps to.
	 */
	for (c = 0; c < m->ncaves; c++) {
		rc = write_cave(m, &g_caves[m->cave_first + c]);
		if (rc != ST_OK) return rc;
	}

	if (m->def.count > 0) {
		rc = patch_apply(&m->def);
		if (rc != ST_OK) {
			/* Keep a failed cleanup unloadable, and never rewrite its caves. */
			if (patch_is_applied(&m->def)) m->flags |= MOD_FLAG_LOADED;
			return rc;
		}
	}

	/* PREVIOUS is not stored, so there is nothing to clear: see mods_list_encode. */
	m->flags |= MOD_FLAG_LOADED;
	return ST_OK;
}

/*
 * Depth-first, dependencies before the mod that names them and in the order the
 * list writes them, stopping at the first failure with that failure's status:
 * the same walk the recursion made, with the explicit work list above standing
 * in for the call stack. The depth limit is unchanged, so a cycle still shows up
 * as a chain longer than QWARK_MAX_MODS levels and is still refused ST_BAD_ARG.
 */
static int mods_load_index(int index, int depth)
{
	int sp = 0;
	int rc = ST_OK;

	if (index < 0 || (u32)index >= g_nmods) return ST_NOT_FOUND;
	if (depth > QWARK_MAX_MODS) return ST_BAD_ARG;

	g_load_stack[0].index  = (u8)index;
	g_load_stack[0].depth  = (u8)depth;
	g_load_stack[0].opened = 0;
	g_load_stack[0].pos    = 0;

	for (;;) {
		struct load_frame *fr = &g_load_stack[sp];
		struct mod_entry *m;
		char name[33];

		if (!fr->opened) {
			if ((u32)fr->index >= g_nmods) { rc = ST_NOT_FOUND; break; }
			if (fr->depth > QWARK_MAX_MODS) { rc = ST_BAD_ARG; break; }

			m = &g_mods[fr->index];
			if (!m->used) { rc = ST_NOT_FOUND; break; }

			if (m->flags & MOD_FLAG_LOADED) {
				rc = m->def.count > 0 ? patch_apply(&m->def) : ST_OK;
				if (rc != ST_OK || sp == 0) break;
				sp--;                      /* nothing to do; back to its dependant */
				continue;
			}
			if (m->flags & MOD_FLAG_PARSE_ERROR) { rc = ST_IO_ERROR; break; }
			/*
			 * Every mod is patch words, code caves, or both, and neither survives
			 * a platform that cannot patch code: the caves would land in memory
			 * nothing ever branches to and the words would change instructions
			 * nobody executes. Refuse the whole mod rather than write half of it.
			 */
			if (!plat_can_patch_code() && (m->def.count > 0 || m->ncaves > 0)) {
				rc = ST_UNSUPPORTED;
				break;
			}
			if (!mem_is_ingame()) { rc = ST_NOT_INGAME; break; }

			fr->opened = 1;
			fr->pos = 0;
		}

		m = &g_mods[fr->index];

		if (depends_next(m->depends, &fr->pos, name, sizeof(name))) {
			int dep = mods_find_by_name(name);

			if (dep < 0) { rc = ST_NOT_FOUND; break; }

			/* The depth check above already bounds this; belt to its braces. */
			if (sp + 1 >= (int)(sizeof(g_load_stack) / sizeof(g_load_stack[0]))) {
				rc = ST_BAD_ARG;
				break;
			}

			sp++;
			g_load_stack[sp].index  = (u8)dep;
			g_load_stack[sp].depth  = (u8)(fr->depth + 1);
			g_load_stack[sp].opened = 0;
			g_load_stack[sp].pos    = 0;
			continue;
		}

		rc = mod_write(m);
		if (rc != ST_OK || sp == 0) break;
		sp--;
	}

	return rc;
}

int mods_load(const char *dirname)
{
	return mods_load_index(mods_find(dirname), 0);
}

int mods_unload(const char *dirname)
{
	int index = mods_find(dirname);
	struct mod_entry *m;

	if (index < 0) return ST_NOT_FOUND;

	m = &g_mods[index];
	if (!(m->flags & MOD_FLAG_LOADED)) return ST_NOT_FOUND;

	/*
	 * The patch words only; the cave bytes stay where they are, because putting
	 * them back crashed the game. patch_revert takes the branches out first.
	 */
	if (m->def.count > 0) {
		int rc = patch_revert(&m->def);
		if (rc != ST_OK) return rc;
	}

	m->flags &= (u8)~MOD_FLAG_LOADED;
	return ST_OK;
}

int mods_set_auto(const char *dirname, int on)
{
	int index = mods_find(dirname);

	if (index < 0) return ST_NOT_FOUND;

	config_set_mod_auto(g_title, dirname, on);

	if (on) g_mods[index].flags |= MOD_FLAG_AUTO;
	else    g_mods[index].flags &= (u8)~MOD_FLAG_AUTO;

	return ST_OK;
}

/*
 * The "#- description:" line, read out of the mod's patch.txt rather than kept
 * in the table: one description at a time, when a user opens one mod, instead of
 * 256 bytes for every mod of every title for the life of the module.
 *
 * It runs on a network thread, under the core lock, which is the same lock
 * mods_rescan takes; the text buffer belongs to whoever holds it. A mod with no
 * description line, or whose patch.txt has gone, answers with an empty string
 * and ST_OK, exactly as an entry with an empty description field used to.
 */
int mods_info(const char *dirname, char *out, u32 cap, u32 *len)
{
	char path[MOD_PATH_MAX];
	char *text = qtext();
	char *cursor;
	char *line;
	int index = mods_find(dirname);

	*len = 0;
	if (cap == 0) return ST_FULL;
	out[0] = 0;
	if (index < 0) return ST_NOT_FOUND;

	mod_dir_path(path, sizeof(path), dirname, "patch.txt");
	if (qread_file(path, text, MOD_TEXT_MAX, NULL) != ST_OK) return ST_OK;

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *colon;

		if (!line_is_meta(line)) continue;

		colon = line;
		while (*colon != 0 && *colon != ':') colon++;
		if (*colon != ':') continue;
		*colon = 0;

		if (!qstreq(qtrim(line + 2), "description")) continue;

		qstrcpy(out, cap > MOD_DESC_MAX ? MOD_DESC_MAX : cap, qtrim(colon + 1));
		break;
	}

	*len = qstrlen(out);
	return ST_OK;
}

void mods_apply_mask(u32 mask)
{
	u32 i;
	for (i = 0; i < g_nmods && i < 32; i++) {
		if ((mask & (1u << i)) == 0) continue;
		mods_load_index((int)i, 0);
	}
}

/* --------------------------------------------------------------- MOD_LIST */

static void put_fixed(u8 *dst, u32 cap, const char *src)
{
	u32 i = 0;
	memset(dst, 0, cap);
	if (src == NULL) return;
	while (i < cap && src[i] != 0) { dst[i] = (u8)src[i]; i++; }
}

/*
 * The flags a row goes out with, which are not quite the flags it is kept with.
 *
 * PREVIOUS is the mod's bit in the previous-session record (SessionInfo's
 * mod_previous), for as long as the mod is not loaded again. Build 49: before
 * it the bit was documented, cleared on a load and never set anywhere, so a
 * client could not have seen it once. Working it out here rather than storing
 * it means a rescan, a REAPPLY and a DISMISS can never leave it stale.
 *
 * Build 49, on a platform that refuses code patches: LOADED is whether the look
 * found the mod in the game, and CHECKING whether it is still to look. qwark's
 * own LOADED stands beside it for the one kind of mod that can still load there,
 * one with no words and no caves, which loads as nothing on any platform and is
 * never looked for, so the two never meet on one row. On a console neither
 * applies and the row is what it always was.
 */
static u8 wire_flags(const struct mod_entry *m, u32 i, u32 previous)
{
	u8 flags = (u8)(m->flags & ~(MOD_FLAG_PREVIOUS | MOD_FLAG_CHECKING));

	if (i < 32 && (previous & (1u << i)) != 0 && (flags & MOD_FLAG_LOADED) == 0)
		flags |= MOD_FLAG_PREVIOUS;

	if (!plat_can_patch_code()) {
		if (m->look == MOD_LOOK_PRESENT) flags |= MOD_FLAG_LOADED;
		if (m->look == MOD_LOOK_PENDING) flags |= MOD_FLAG_CHECKING;
	}

	return flags;
}

int mods_list_encode(u8 *out, u32 cap, u32 *len)
{
	u32 off = 0;
	u32 previous = session_previous()->mods;
	u32 i;
	u8 n = 0;

	*len = 0;
	if (cap < 1 + g_nmods * MOD_WIRE_SIZE) return ST_FULL;

	off = 1;
	for (i = 0; i < g_nmods; i++) {
		const struct mod_entry *m = &g_mods[i];
		if (!m->used) continue;

		out[off + 0] = m->index;
		out[off + 1] = wire_flags(m, i, previous);
		out[off + 2] = 0;
		out[off + 3] = 0;
		be32_put(out + off + 4, m->hash);
		put_fixed(out + off + 8, 32, m->dirname);
		put_fixed(out + off + 40, 32, m->name);
		put_fixed(out + off + 72, 16, m->version);
		put_fixed(out + off + 88, 32, m->author);
		off += MOD_WIRE_SIZE;
		n++;
	}

	out[0] = n;
	*len = off;
	return ST_OK;
}

/* ------------------------------------- MOD_PATCH, revisions 1.16 and 1.19 */

/*
 * MOD_PATCH and the presence look are the emulator builds' alone, to the end of
 * the file (build 51, mods.h).
 */
#ifdef QWARK_EMULATOR

/*
 * How much of a cave file MOD_PATCH reads at a time. It runs on a network
 * thread, which must never borrow the tick thread's scratch buffer, so this is
 * on the stack; a multiple of four, so a chunk is whole rows but for its tail.
 */
#define MOD_PATCH_CHUNK  256u

/* A cave's tail is its last len % 4 bytes: at most three a cave. */
#define MOD_PATCH_TAILS  (MOD_CAVE_POOL * 3u)

/*
 * The reply pages, revision 1.19.
 *
 * One frame holds at most QWARK_MAX_PAYLOAD bytes, and a page is `u16 n,
 * u16 pages, u32 stamp`, n word rows, `u16 nb, u16 pad` and nb byte rows, eight
 * bytes a row either way: MOD_PATCH_PAGE_ROWS, (16384 - 8 - 4) / 8 = 2046 rows
 * a page in all. The layout depends only on the mod's files, so every call lays
 * the whole mod out again and keeps the requested page's window of word rows,
 * while the stamp's CRC runs over every row of the mod.
 *
 * With W word rows and B byte rows, pages = ceil((W + B) / 2046), at least 1.
 * Page k carries word rows [2046 k, min(2046 (k+1), W)); the last page carries
 * every byte row after its words, so it has at most 2046 - B words and may have
 * none. B is at most MOD_PATCH_TAILS, far below a page.
 */
struct patch_out {
	u8  *rows;                          /* where the window's word rows go */
	u32  first;                         /* the mod's index of the window's first row */
	u32  window;                        /* how many rows the window holds */
	u32  n;                             /* word rows laid out so far, the whole mod */
	u32  crc;                           /* CRC-32 state over every word row so far */
	u32  ntail;
	u32  tail_addr[MOD_PATCH_TAILS];
	u8   tail_val[MOD_PATCH_TAILS];
};

/* One word row: into the CRC whatever it is, into the window when it falls there. */
static void patch_row(struct patch_out *p, u32 addr, const u8 value[4])
{
	u8 row[MOD_PATCH_ROW];

	be32_put(row, addr);
	memcpy(row + 4, value, 4);
	p->crc = qcrc32_update(p->crc, row, MOD_PATCH_ROW);
	if (p->n >= p->first && p->n - p->first < p->window)
		memcpy(p->rows + (p->n - p->first) * MOD_PATCH_ROW, row, MOD_PATCH_ROW);
	p->n++;
}

/*
 * One cave as rows: its file four bytes at a time, each as the be32 word the
 * game holds there, and what is left over at the end as tail bytes. The file is
 * streamed whole, exactly as MOD_LOAD streams it into a console's memory, and a
 * missing, unreadable or empty one is IO_ERROR: MOD_LOAD would not load it
 * either, and a patch of half a mod is not one to hand a client.
 */
static int patch_cave(struct patch_out *p, const struct mod_entry *m,
                      const struct mod_cave *cave)
{
	char path[MOD_PATH_MAX];
	u8 buf[MOD_PATCH_CHUNK];
	plat_file_t f;
	u32 have = 0;     /* bytes at the front of buf not yet made into a row */
	u32 at = 0;       /* the cave offset of buf[0] */
	u32 k;
	int rc = ST_OK;

	mod_dir_path(path, sizeof(path), m->dirname, cave->file);
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_IO_ERROR;

	for (;;) {
		u32 got = 0;

		if (plat_file_read(f, buf + have, sizeof(buf) - have, &got) != 0) {
			rc = ST_IO_ERROR;
			break;
		}
		if (got == 0) break;
		have += got;

		for (k = 0; k + 4u <= have; k += 4u)
			patch_row(p, cave->addr + at + k, buf + k);

		at += k;
		have -= k;
		memmove(buf, buf + k, have);
	}

	plat_file_close(f);
	if (rc != ST_OK) return rc;
	if (at + have == 0) return ST_IO_ERROR;

	/* At most three a cave and MOD_CAVE_POOL caves, so this never refuses. */
	for (k = 0; k < have; k++) {
		if (p->ntail >= MOD_PATCH_TAILS) return ST_FULL;
		p->tail_addr[p->ntail] = cave->addr + at + k;
		p->tail_val[p->ntail] = buf[k];
		p->ntail++;
	}
	return ST_OK;
}

/*
 * The whole mod, every word row through patch_row (which keeps the rows in p's
 * window and runs the CRC over all of them) and every byte row into p's tail
 * table, sorted by address at the end. Everything a MOD_PATCH reply holds comes
 * from here, whichever page it is.
 *
 * Every cave in patch.txt order, then every word in patch.txt order: the order a
 * console writes them in, caves before the branches into them, and so the order
 * a patch file lists them in. The dependencies are not folded in; a client walks
 * "#- depends" itself and writes each mod on its own.
 */
static int patch_layout(const struct mod_entry *m, struct patch_out *p)
{
	u32 i, j;
	int rc;

	p->n = 0;
	p->ntail = 0;
	p->crc = qcrc32_start();

	for (i = 0; i < m->ncaves; i++) {
		rc = patch_cave(p, m, &g_caves[m->cave_first + i]);
		if (rc != ST_OK) return rc;
	}

	for (i = 0; i < m->def.count; i++) {
		u8 value[4];

		be32_put(value, m->def.words[i].value);
		patch_row(p, m->def.words[i].addr, value);
	}

	/* The tail bytes in address order, which is also the caves' own order within one. */
	for (i = 1; i < p->ntail; i++) {
		u32 a = p->tail_addr[i];
		u8 v = p->tail_val[i];

		for (j = i; j > 0 && p->tail_addr[j - 1] > a; j--) {
			p->tail_addr[j] = p->tail_addr[j - 1];
			p->tail_val[j] = p->tail_val[j - 1];
		}
		p->tail_addr[j] = a;
		p->tail_val[j] = v;
	}
	return ST_OK;
}

/* One byte row: u32 addr, u8 value, u8 pad[3]. */
static void patch_byte_row(u8 *b, u32 addr, u8 value)
{
	be32_put(b, addr);
	b[4] = value;
	b[5] = 0;
	b[6] = 0;
	b[7] = 0;
}

/*
 * The stamp: the CRC-32 of what one unbounded reply would hold after it - every
 * word row (already in p->crc), `u16 nb, u16 pad`, every byte row. For a mod
 * that fits one page that is exactly the bytes after the stamp, as it was
 * before paging.
 */
static u32 patch_stamp(const struct patch_out *p)
{
	u8 b[MOD_PATCH_BYTE_ROW];
	u32 crc = p->crc;
	u32 i;

	be16_put(b, (u16)p->ntail);
	be16_put(b + 2, 0);
	crc = qcrc32_update(crc, b, MOD_PATCH_BYTES_HEAD);
	for (i = 0; i < p->ntail; i++) {
		patch_byte_row(b, p->tail_addr[i], p->tail_val[i]);
		crc = qcrc32_update(crc, b, MOD_PATCH_BYTE_ROW);
	}
	return qcrc32_finish(crc);
}

/* How many pages `nw` word rows and `nb` byte rows take: at least 1. */
static u32 patch_pages(u32 nw, u32 nb)
{
	u32 rows = nw + nb;
	return rows == 0 ? 1u : (rows + MOD_PATCH_PAGE_ROWS - 1u) / MOD_PATCH_PAGE_ROWS;
}

/* How many word rows page `page` carries, from row page * MOD_PATCH_PAGE_ROWS on. */
static u32 patch_page_count(u32 nw, u32 page)
{
	u32 first = page * MOD_PATCH_PAGE_ROWS;
	u32 count = nw > first ? nw - first : 0u;
	return count > MOD_PATCH_PAGE_ROWS ? MOD_PATCH_PAGE_ROWS : count;
}

int mods_patch_encode(u8 index, u16 page, u8 *out, u32 cap, u32 *len)
{
	/*
	 * Static rather than on the stack: 500 bytes that a network thread's stack
	 * need not carry, and safe because every caller holds the core lock, the
	 * same rule the shared text buffer lives by.
	 */
	static struct patch_out p;
	const struct mod_entry *m;
	u32 pages, count, nb, off, room, i;
	int rc;

	*len = 0;
	if (cap > QWARK_MAX_PAYLOAD) cap = QWARK_MAX_PAYLOAD;

	/*
	 * The refusals MOD_LOAD makes, in its order, and one of its own: a mod
	 * with a Lua automation is only half there as words, and the half that
	 * is not is something no patch can carry.
	 */
	m = mods_at(index);
	if (m == NULL) return ST_NOT_FOUND;
	if (m->flags & MOD_FLAG_PARSE_ERROR) return ST_IO_ERROR;
	if (m->flags & MOD_FLAG_NEEDS_LUA) return ST_UNSUPPORTED;
	if (cap < MOD_PATCH_HEAD + MOD_PATCH_BYTES_HEAD) return ST_FULL;

	/*
	 * The window is the page's word rows, no more than the buffer has room for.
	 * On the wire the buffer is a whole frame and always holds a page; a smaller
	 * one (the tests') is FULL below when the page it asked for does not fit.
	 */
	room = (cap - MOD_PATCH_HEAD - MOD_PATCH_BYTES_HEAD) / MOD_PATCH_ROW;
	p.rows = out + MOD_PATCH_HEAD;
	p.first = (u32)page * MOD_PATCH_PAGE_ROWS;
	p.window = room < MOD_PATCH_PAGE_ROWS ? room : MOD_PATCH_PAGE_ROWS;

	rc = patch_layout(m, &p);
	if (rc != ST_OK) return rc;

	/*
	 * Every byte row goes on the last page, so a mod whose byte rows alone are
	 * more than a page cannot be paged; MOD_PATCH_TAILS keeps that from ever
	 * happening. Nor can `pages` pass its u16, short of a cave of 500 MB.
	 */
	if (p.ntail > MOD_PATCH_PAGE_ROWS) return ST_FULL;
	pages = patch_pages(p.n, p.ntail);
	if (pages > 0xFFFFu) return ST_FULL;
	if ((u32)page >= pages) return ST_BAD_ARG;

	/* The window holds this page's word rows already; the byte rows follow them
	 * on the last page, where patch_pages left them the room. */
	count = patch_page_count(p.n, page);
	nb = ((u32)page == pages - 1u) ? p.ntail : 0u;
	off = MOD_PATCH_HEAD + count * MOD_PATCH_ROW;
	if (off + MOD_PATCH_BYTES_HEAD + nb * MOD_PATCH_BYTE_ROW > cap) return ST_FULL;

	be16_put(out + off, (u16)nb);
	be16_put(out + off + 2, 0);
	off += MOD_PATCH_BYTES_HEAD;
	for (i = 0; i < nb; i++, off += MOD_PATCH_BYTE_ROW)
		patch_byte_row(out + off, p.tail_addr[i], p.tail_val[i]);

	be16_put(out, (u16)count);
	be16_put(out + 2, (u16)pages);
	be32_put(out + 4, patch_stamp(&p));

	*len = off;
	return ST_OK;
}

int mods_patch_request(const u8 *req, u32 reqlen, u8 *out, u32 cap, u32 *len)
{
	*len = 0;
	if (reqlen < 4) return ST_BAD_ARG;
	return mods_patch_encode(req[0], be16_get(req + 2), out, cap, len);
}

#ifdef QWARK_TEST
u32 mods_test_page_split(u32 nw, u32 nb, u32 page, u32 *count)
{
	if (count != NULL) *count = patch_page_count(nw, page);
	return patch_pages(nw, nb);
}
#endif

/* ----------------------------------------------- the presence look, build 49 */

/*
 * Where code cannot be patched - RPCS3, which recompiles PPU code - a mod gets
 * into the game one way: the client writes it from MOD_PATCH into RPCS3's patch
 * file and RPCS3 applies it as it loads the executable, before it compiles a
 * block. qwark's part is to say which mods went in, and it says that the way
 * savefile.c says it for the helper: a mod is there when every byte of every
 * cave and every word reads back exactly as MOD_PATCH hands it out.
 *
 * A patch goes in at load or not at all, so the answer holds for the session:
 * each mod is looked at once, and looked at again only when a new session
 * starts or a rescan has replaced the files it was compared against.
 *
 * The look is spread over ticks, because a word here and a word there is a PINE
 * round trip each and a title can have hundreds: MOD_LOOK_READS reads a tick at
 * most, each of at most MOD_LOOK_CHUNK bytes, so a tick never waits on more
 * than four replies or 8 KB. A run of consecutive words is one read. Every
 * title of the shipped library is looked through in under thirty ticks - bl-mod,
 * whose 103 words fall in 85 runs, is the slowest mod at 22 - and a tick whose
 * read fails stops there and tries again next tick from the same place.
 */
#define MOD_LOOK_READS  4u
#define MOD_LOOK_CHUNK  2048u

/* The memory half and the file half of the scratch buffer. */
typedef char mods_look_fits[2u * MOD_LOOK_CHUNK <= QSCRATCH_BYTES ? 1 : -1];

static void look_arm_all(void)
{
	u32 i;

	g_look_mod = 0;
	g_look_part = 0;
	g_look_at = 0;

	/*
	 * Only a mod MOD_PATCH would describe, and that has something to describe:
	 * one with no words and no caves loads as nothing on any platform, MOD_LOAD
	 * takes it here as it always has, and qwark's own LOADED stays its answer.
	 */
	for (i = 0; i < g_nmods; i++) {
		struct mod_entry *m = &g_mods[i];

		if (g_look_on && m->used &&
		    (m->flags & (MOD_FLAG_PARSE_ERROR | MOD_FLAG_NEEDS_LUA)) == 0 &&
		    (m->ncaves > 0 || m->def.count > 0))
			m->look = MOD_LOOK_PENDING;
		else
			m->look = MOD_LOOK_NONE;
	}
}

void mods_presence_start(void)
{
	g_look_on = plat_can_patch_code() ? 0 : 1;
	look_arm_all();
}

void mods_presence_stop(void)
{
	g_look_on = 0;
	look_arm_all();
}

/*
 * Whether a word from `from` on in the mod's list covers `addr`. A patch is
 * applied in MOD_PATCH's order, caves and then words, so where two of them name
 * the same byte the later one is what the game holds; the look compares that
 * byte against the later one only. No shipped mod does this, but a mod that
 * fixes up its own cave with a word, or names one address twice, is not absent
 * for it.
 */
static int later_word_covers(const struct mod_entry *m, u32 from, u32 addr)
{
	u32 k;

	for (k = from; k < m->def.count; k++) {
		if (addr - m->def.words[k].addr < 4u) return 1;
	}
	return 0;
}

/*
 * Up to `cap` bytes of a cave's file from byte `at`, into `buf`, looping until
 * it has them or the file ends, so fewer than `cap` means the end. There is no
 * seek in the platform layer, so what comes before `at` is read and dropped;
 * caves are a few kilobytes and this only runs on a PC.
 */
static int cave_read_at(const struct mod_entry *m, const struct mod_cave *cave,
                        u32 at, u8 *buf, u32 cap, u32 *got)
{
	char path[MOD_PATH_MAX];
	plat_file_t f;
	u32 skipped = 0;
	int rc = ST_OK;

	*got = 0;
	mod_dir_path(path, sizeof(path), m->dirname, cave->file);
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_NOT_FOUND;

	while (rc == ST_OK && skipped < at) {
		u32 want = at - skipped;
		u32 n = 0;

		if (want > cap) want = cap;
		if (plat_file_read(f, buf, want, &n) != 0) rc = ST_IO_ERROR;
		else if (n == 0) break;
		skipped += n;
	}

	while (rc == ST_OK && skipped == at && *got < cap) {
		u32 n = 0;

		if (plat_file_read(f, buf + *got, cap - *got, &n) != 0) rc = ST_IO_ERROR;
		else if (n == 0) break;
		*got += n;
	}

	plat_file_close(f);
	return rc;
}

/*
 * One read's worth of the look at `m`, from the cursor. 1 when the mod is
 * decided (its `look` says how), 0 when there is more of it, -1 when the read
 * failed and the mod stays PENDING.
 */
static int look_step(struct mod_entry *m)
{
	u8 *mem = qscratch();
	u32 k;

	if (g_look_part < m->ncaves) {
		const struct mod_cave *cave = &g_caves[m->cave_first + g_look_part];
		u8 *file = mem + MOD_LOOK_CHUNK;
		u32 got = 0;

		/*
		 * A cave whose file has gone cannot be compared, and a client cannot
		 * have written it from MOD_PATCH either, which refuses it the same way:
		 * absent, not unknown, since asking again next tick will not bring the
		 * file back. An empty one is refused by MOD_PATCH and MOD_LOAD alike.
		 */
		if (cave_read_at(m, cave, g_look_at, file, MOD_LOOK_CHUNK, &got) != ST_OK ||
		    (got == 0 && g_look_at == 0)) {
			m->look = MOD_LOOK_ABSENT;
			return 1;
		}

		if (got > 0) {
			if (mem_read(cave->addr + g_look_at, mem, got) != ST_OK) return -1;

			for (k = 0; k < got; k++) {
				if (mem[k] == file[k]) continue;
				if (later_word_covers(m, 0, cave->addr + g_look_at + k)) continue;
				m->look = MOD_LOOK_ABSENT;
				return 1;
			}
			g_look_at += got;
		}

		/* A short read is the end of the file, and so of this cave. */
		if (got < MOD_LOOK_CHUNK) {
			g_look_part++;
			g_look_at = 0;
		}
		return 0;
	}

	if (g_look_at < m->def.count) {
		const struct patch_word *w = m->def.words;
		u32 first = g_look_at;
		u32 n = 1;

		while (first + n < m->def.count && n < MOD_LOOK_CHUNK / 4u &&
		       w[first + n].addr == w[first + n - 1].addr + 4u)
			n++;

		if (mem_read(w[first].addr, mem, n * 4u) != ST_OK) return -1;

		for (k = 0; k < n * 4u; k++) {
			u8 want[4];

			be32_put(want, w[first + k / 4u].value);
			if (mem[k] == want[k % 4u]) continue;
			if (later_word_covers(m, first + k / 4u + 1u, w[first + k / 4u].addr + k % 4u))
				continue;
			m->look = MOD_LOOK_ABSENT;
			return 1;
		}
		g_look_at += n;
		if (g_look_at < m->def.count) return 0;
	}

	/* Every cave and every word read back. */
	m->look = MOD_LOOK_PRESENT;
	return 1;
}

void mods_presence_tick(void)
{
	u32 reads = 0;

	if (!g_look_on || plat_can_patch_code() || !mem_is_ingame()) return;

	while (reads < MOD_LOOK_READS) {
		struct mod_entry *m;
		int rc;

		while (g_look_mod < g_nmods &&
		       (!g_mods[g_look_mod].used || g_mods[g_look_mod].look != MOD_LOOK_PENDING)) {
			g_look_mod++;
			g_look_part = 0;
			g_look_at = 0;
		}
		if (g_look_mod >= g_nmods) return;   /* every mod has its answer */

		m = &g_mods[g_look_mod];
		rc = look_step(m);
		reads++;

		if (rc < 0) return;                  /* not answering: again next tick */
		if (rc > 0) {
			if (m->look == MOD_LOOK_PRESENT)
				plat_log("mods: %s is in the game, patched in at load", m->dirname);
			g_look_mod++;
			g_look_part = 0;
			g_look_at = 0;
		}
	}
}

#endif /* QWARK_EMULATOR */
