#include "mods.h"
#include "config.h"
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
static char g_text[MOD_TEXT_MAX];

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
	else if (qstreq(key, "description")) qstrcpy(m->description, sizeof(m->description), value);
	else if (qstreq(key, "depends"))     qstrcpy(m->depends, sizeof(m->depends), value);
}

/*
 * Parses one mod folder into `m`. Words and caves are taken from the shared
 * pools; a mod that does not fit is marked parse_error and keeps no storage.
 */
static int parse_mod(struct mod_entry *m, const char *dirname)
{
	char path[MOD_PATH_MAX];
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
	rc = qread_file(path, g_text, sizeof(g_text), NULL);
	if (rc != ST_OK) {
		m->flags |= MOD_FLAG_PARSE_ERROR;
		return rc;
	}

	/* Pass one: the #- metadata lines. */
	cursor = g_text;
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
	if (qread_file(path, g_text, sizeof(g_text), NULL) != ST_OK) {
		m->flags |= MOD_FLAG_PARSE_ERROR;
		return ST_IO_ERROR;
	}

	cursor = g_text;
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

int mods_rescan(void)
{
	char path[MOD_PATH_MAX];
	plat_dir_t dir;
	struct plat_dirent ent;
	int any_loaded = 0;
	u32 i;

	if (g_title[0] == 0) {
		g_nmods = 0;
		memset(g_mods, 0, sizeof(g_mods));
		g_words_used = 0;
		g_caves_used = 0;
		return ST_OK;
	}

	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].used && (g_mods[i].flags & MOD_FLAG_LOADED)) any_loaded = 1;
	}

	/*
	 * The word and cave pools are bump allocated, so a full re-parse is only
	 * safe when nothing is holding onto a slice of them. With mods loaded we do
	 * what racman's ReloadMods does and only pick up folders we have not seen.
	 */
	if (!any_loaded) {
		g_nmods = 0;
		memset(g_mods, 0, sizeof(g_mods));
		g_words_used = 0;
		g_caves_used = 0;
	}

	path[0] = 0;
	qstrcat(path, sizeof(path), QWARK_MODSDIR);
	qstrcat(path, sizeof(path), "/");
	qstrcat(path, sizeof(path), g_title);

	if (plat_dir_open(path, &dir) != 0) return ST_OK;   /* no mods installed yet */

	while (plat_dir_next(&dir, &ent) == 1) {
		struct mod_entry tmp;

		if (!ent.is_dir) continue;
		if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
		if (g_nmods >= QWARK_MAX_MODS) break;
		if (mods_find(ent.name) >= 0) continue;

		parse_mod(&tmp, ent.name);
		tmp.index = (u8)g_nmods;
		g_mods[g_nmods] = tmp;
		/*
		 * parse_mod pointed def.name at the temporary's own name buffer, which
		 * dies with this loop iteration; the copy in the table has to point at
		 * its own. Without this PATCH_LIST read a dead stack frame for the name.
		 */
		g_mods[g_nmods].def.name = g_mods[g_nmods].name;
		g_nmods++;
	}

	plat_dir_close(&dir);
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

static int mods_load_index(int index, int depth);

static int load_dependencies(struct mod_entry *m, int depth)
{
	char list[MOD_DEPENDS_MAX];
	char *p;

	if (m->depends[0] == 0) return ST_OK;

	qstrcpy(list, sizeof(list), m->depends);
	p = list;

	while (*p != 0) {
		char *start = p;
		char *name;
		int dep;
		int rc;

		while (*p != 0 && *p != ',') p++;
		if (*p == ',') { *p = 0; p++; }

		name = qtrim(start);
		if (name[0] == 0) continue;

		dep = mods_find_by_name(name);
		if (dep < 0) return ST_NOT_FOUND;

		rc = mods_load_index(dep, depth + 1);
		if (rc != ST_OK) return rc;
	}

	return ST_OK;
}

static int mods_load_index(int index, int depth)
{
	struct mod_entry *m;
	u16 c;
	int rc;

	if (index < 0 || (u32)index >= g_nmods) return ST_NOT_FOUND;

	/* A cycle can only show up as unbounded recursion; QWARK_MAX_MODS bounds it. */
	if (depth > QWARK_MAX_MODS) return ST_BAD_ARG;

	m = &g_mods[index];
	if (!m->used) return ST_NOT_FOUND;
	if (m->flags & MOD_FLAG_LOADED)
		return m->def.count > 0 ? patch_apply(&m->def) : ST_OK;
	if (m->flags & MOD_FLAG_PARSE_ERROR) return ST_IO_ERROR;
	/*
	 * Every mod is patch words, code caves, or both, and neither survives a
	 * platform that cannot patch code: the caves would land in memory nothing
	 * ever branches to and the words would change instructions nobody executes.
	 * Refuse the whole mod rather than write half of it.
	 */
	if (!plat_can_patch_code() && (m->def.count > 0 || m->ncaves > 0))
		return ST_UNSUPPORTED;
	if (!mem_is_ingame()) return ST_NOT_INGAME;

	rc = load_dependencies(m, depth);
	if (rc != ST_OK) return rc;

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

	m->flags |= MOD_FLAG_LOADED;
	m->flags &= (u8)~MOD_FLAG_PREVIOUS;
	return ST_OK;
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

int mods_info(const char *dirname, char *out, u32 cap, u32 *len)
{
	int index = mods_find(dirname);

	*len = 0;
	if (index < 0) return ST_NOT_FOUND;

	qstrcpy(out, cap, g_mods[index].description);
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

int mods_list_encode(u8 *out, u32 cap, u32 *len)
{
	u32 off = 0;
	u32 i;
	u8 n = 0;

	*len = 0;
	if (cap < 1 + g_nmods * MOD_WIRE_SIZE) return ST_FULL;

	off = 1;
	for (i = 0; i < g_nmods; i++) {
		const struct mod_entry *m = &g_mods[i];
		if (!m->used) continue;

		out[off + 0] = m->index;
		out[off + 1] = m->flags;
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
