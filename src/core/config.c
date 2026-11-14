#include "config.h"
#include "util.h"
#include "savefile.h"
#include "../plat/plat.h"

#include <string.h>

/* CONFIG_POOL_BYTES is in config.h, with what it is sized for. */

/*
 * positions/<game>.txt. A line is "<planet>.<slot> = <hex>\n": a planet index,
 * which is a u8 and so at most three digits, a dot, a slot digit (there are
 * eight), " = ", two hex digits per blob byte, and the newline. The blob is 40
 * bytes in RaC4 and 30 in the other three, and QWARK_MAX_BLOB, the most any game
 * may store, is 64.
 *
 * So the file this table can write is POS_MAX_ENTRIES of the widest line, and
 * that is what the buffer is, rather than the round 8192 it was: a full table of
 * maximum-length blobs came to 8769 bytes, which the old buffer could not read
 * back, and a positions file that does not fit is a file whose every slot is
 * silently dropped. What the four games actually write is 89 bytes a line, so a
 * full table of RaC4 positions is 5.6 KB and always was.
 */
#define POS_MAX_ENTRIES    64
#define POS_LINE_MAX       (3 + 1 + 1 + 3 + 2 * QWARK_MAX_BLOB + 1)
#define POS_TEXT_MAX       (POS_MAX_ENTRIES * POS_LINE_MAX + 2)

/*
 * Build 56: the key store (config.h, CONFIG_POOL_BYTES). Every key of both
 * files is one entry in g_pool: a byte naming the store, the key and its NUL,
 * the value and its NUL, packed one after another in the order they were
 * first put. A store is the file its keys go back to: STORE_CONSOLE is
 * config.txt, STORE_PROFILE the active profile's config.txt.
 *
 * Every write is under the core lock (util.h's rule for the text buffer, which
 * the config ops and the tick thread keep). A read outside it can see a value
 * move under it; every walk stops at the pool's end, and the pool is followed
 * by three bytes that are always 0, so that costs a stale answer and never a
 * read past the end.
 */
#define STORE_CONSOLE 0
#define STORE_PROFILE 1
#define KV_NONE       0xFFFFFFFFu
/*
 * Build 57: set on an entry's store byte while store_save has written it, and
 * cleared again before store_save returns, so nothing else ever sees it.
 */
#define KV_SAVED      0x80

static char g_pool[CONFIG_POOL_BYTES + 3];
static u32  g_pool_used;
/*
 * A store whose file did not all fit, or did not all read: it is never written
 * back, so the part that was not read is never cut out of the file.
 */
static u8   g_over[2];
static u32  g_version;

#if QWARK_PREVIEW
/* Revision 1.24. The active profile (config.h, the resolver). */
static struct {
	u8   active;    /* per-user keys come from STORE_PROFILE */
	u8   exists;    /* its folder and config.txt have been made */
	u8   seq;       /* SessionInfo byte 30 */
	u8   pad;
	u32  user;      /* the user it is, 0 when none is active */
	char folder[PROFILE_FOLDER_MAX];
} g_prof;
#endif /* QWARK_PREVIEW: a module without the preview features has no profile, and every key is config.txt's (config.h) */

static int pos_load_file(void);
/* The positions' title (rac1 .. rac4), which the slot table further down is of. */
static char g_pos_title[16];

/*
 * config.txt and the positions file are read into the core's shared text buffer
 * (util.h), under the core lock like every other user of it, rather than into
 * buffers of their own. gnu99, so no _Static_assert: a cap that outgrew that
 * buffer fails the build here rather than running off the end of it.
 *
 * Since build 56 config.txt's cap is the buffer itself, QTEXT_BYTES: the file
 * the pool can write is the pool's bytes plus one more a line (" = " and the
 * newline where the pool has a store byte and two NULs), and an entry is at
 * least four pool bytes, so the file is at most 5/4 of the pool. Every write
 * also keeps the old file's lines the store does not hold (store_save): while
 * a profile is active, the console's per-user lines, which came out of the
 * same pool when it went active. A file that has grown past the buffer even so
 * is refused whole (ST_FULL) and never cut down.
 */
typedef char pos_text_fits[POS_TEXT_MAX <= QTEXT_BYTES ? 1 : -1];
typedef char config_text_fits[CONFIG_POOL_BYTES / 4 * 5 < QTEXT_BYTES - 1 ? 1 : -1];

static const char * const g_combo_key[COMBO_COUNT] = {
	"combo.save",
	"combo.load",
	"combo.die",
	"combo.planet",
	"combo.setaside",
	"combo.menu",       /* revision 1.22, COMBO_OPEN_MENU */
	"combo.fastloads"   /* revision 1.23, COMBO_TOGGLE_FAST_LOADS */
};

/* Revision 1.12. The switch under those five, and in the same scope as them. */
static const char g_combo_enabled_key[] = "combo.enabled";

/* ------------------------------------------------------------------ store */

/* The entry after the one at `off`. */
static u32 kv_next(u32 off)
{
	off += 1 + qstrlen(g_pool + off + 1) + 1;
	return off + qstrlen(g_pool + off) + 1;
}

static u32 kv_find(u8 store, const char *key)
{
	u32 off;

	for (off = 0; off < g_pool_used && off < CONFIG_POOL_BYTES; off = kv_next(off)) {
		if (((u8)g_pool[off] & ~KV_SAVED) == store + 1 && qstreq(g_pool + off + 1, key)) return off;
	}
	return KV_NONE;
}

static const char *kv_get(u8 store, const char *key)
{
	u32 off = kv_find(store, key);
	return off == KV_NONE ? NULL : g_pool + off + 2 + qstrlen(g_pool + off + 1);
}

/*
 * The `cut` bytes at `at` replaced by `n` bytes of `src`, the rest of the pool
 * moved up or down behind them. ST_FULL, with nothing changed, when that would
 * run past the pool. What a shrink leaves past the end is old entries, NULs
 * and all, and the guard bytes are never written.
 */
static int pool_splice(u32 at, u32 cut, const char *src, u32 n)
{
	u32 used = g_pool_used;

	if (used - cut + n > CONFIG_POOL_BYTES) return ST_FULL;
	memmove(g_pool + at + n, g_pool + at + cut, used - at - cut);
	memcpy(g_pool + at, src, n);
	g_pool_used = used - cut + n;
	g_version++;
	return ST_OK;
}

/* Every key of one store out of the pool. */
static void kv_clear(u8 store)
{
	u32 off = 0;

	while (off < g_pool_used) {
		if (((u8)g_pool[off] & ~KV_SAVED) == store + 1) pool_splice(off, kv_next(off) - off, "", 0);
		else off = kv_next(off);
	}
}

/*
 * A key and value are cut at CONFIG_KEY_MAX - 1 and CONFIG_VAL_MAX - 1
 * characters, as the slot table cut them. A new key goes at the end; a key
 * already there keeps its place, so the file keeps its order. ST_FULL, with
 * nothing changed, when the pool has no room, or for a store that did not load
 * whole (g_over), whose file is never written again.
 */
static int kv_put(u8 store, const char *key, const char *value)
{
	char e[1 + CONFIG_KEY_MAX + CONFIG_VAL_MAX];
	u32 n;
	u32 off;

	if (g_over[store]) return ST_FULL;
	e[0] = (char)(store + 1);
	qstrcpy(e + 1, CONFIG_KEY_MAX, key);
	n = 1 + qstrlen(e + 1) + 1;
	qstrcpy(e + n, CONFIG_VAL_MAX, value);
	n += qstrlen(e + n) + 1;

	off = kv_find(store, e + 1);
	if (off == KV_NONE) return pool_splice(g_pool_used, 0, e, n);
	return pool_splice(off, kv_next(off) - off, e, n);
}

/* ------------------------------------------------------------ key scopes */

#if QWARK_PREVIEW

static int has_prefix(const char *s, const char *p)
{
	while (*p != 0) {
		if (*s != *p) return 0;
		s++;
		p++;
	}
	return 1;
}

/*
 * "<TITLEID>.auto.<slug>" and "<TITLEID>.mod.<dirname>.auto": nine capitals
 * and digits, the title id every key of the kind starts with (feature_key and
 * mod_key, below), then ".auto." or ".mod.". The NUL fails the character test,
 * so a short key stops there.
 */
static int title_auto_key(const char *key)
{
	int i;

	for (i = 0; i < 9; i++) {
		char c = key[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
	}
	return has_prefix(key + 9, ".auto.") || has_prefix(key + 9, ".mod.");
}

int config_key_scope(const char *key)
{
	int i;

	if (key == NULL) return CONFIG_SCOPE_CONSOLE;
	for (i = 0; i < COMBO_COUNT; i++) {
		if (qstreq(key, g_combo_key[i])) return CONFIG_SCOPE_COMBO;
	}
	if (has_prefix(key, "selected.") || has_prefix(key, "manip.") ||
	    has_prefix(key, "profile.") || qstreq(key, "menu.help") ||
	    qstreq(key, "run.warnings") || qstreq(key, "reboot.apply") || title_auto_key(key))
		return CONFIG_SCOPE_USER;
	return CONFIG_SCOPE_CONSOLE;
}
#endif /* QWARK_PREVIEW */

/*
 * A per-user key of an active profile is the profile's alone: one the profile
 * does not hold is unset, whatever the console's file says, so a profile made
 * fresh reads every default. A combo is the profile's override when it holds
 * one, and the console's otherwise.
 */
const char *config_get(const char *key)
{
#if QWARK_PREVIEW
	if (g_prof.active) {
		int scope = config_key_scope(key);
		if (scope == CONFIG_SCOPE_USER) return kv_get(STORE_PROFILE, key);
		if (scope == CONFIG_SCOPE_COMBO) {
			const char *v = kv_get(STORE_PROFILE, key);
			if (v != NULL) return v;
		}
	}
#endif
	return kv_get(STORE_CONSOLE, key);
}

u32 config_version(void)
{
	return g_version;
}

static int global_save(void);
#if QWARK_PREVIEW
static int profile_save(const char *drop);
static int profile_make(void);
#endif

/*
 * Every write lands in the active profile's file when the key is a per-user
 * one, making the profile first if it has never been made, and in the
 * console's file otherwise. COMBO_SET's masks are the console's: an override
 * goes through config_set_combo_override.
 */
int config_set(const char *key, const char *value)
{
	int rc;

#if QWARK_PREVIEW
	if (g_prof.active && config_key_scope(key) == CONFIG_SCOPE_USER) {
		rc = profile_make();
		if (rc != ST_OK) return rc;
		rc = kv_put(STORE_PROFILE, key, value);
		if (rc != ST_OK) return rc;
		return profile_save(NULL);
	}
#endif

	rc = kv_put(STORE_CONSOLE, key, value);
	if (rc != ST_OK) return rc;
	return global_save();
}

u32 config_get_u32(const char *key, u32 fallback)
{
	const char *v = config_get(key);
	int ok = 0;
	u32 out;

	if (v == NULL) return fallback;
	out = qparse_u32(v, &ok);
	return ok ? out : fallback;
}

int config_set_u32(const char *key, u32 value)
{
	char buf[16];
	qfmt_u32(buf, sizeof(buf), value);
	return config_set(key, buf);
}

/* -------------------------------------------------------------- load/save */

/*
 * Build 56. A file is written as "<path>.new" and then put in the old one's
 * place, so a write cut off part of the way leaves the last whole file rather
 * than half of the new one. cellFs will not rename onto a file that exists, so
 * the old one goes first; a read that finds no file but a ".new" takes that
 * (qfile_read below), which is all the gap between the two can leave.
 */
#define FILE_PATH_MAX 128

static void new_path(const char *path, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, path);
	qstrcat(out, cap, ".new");
}

static int file_begin(const char *path, plat_file_t *f)
{
	char tmp[FILE_PATH_MAX];

	new_path(path, tmp, sizeof(tmp));
	return plat_file_open(tmp, PLAT_OPEN_WRITE, f) == 0 ? ST_OK : ST_IO_ERROR;
}

/*
 * Closes the ".new" and puts it in place, or throws it away when `ok` is not.
 * Build 57: a close that fails is a write that may not all be on the disk, so
 * it throws the ".new" away too, and the old file stays.
 */
static int file_end(const char *path, plat_file_t f, int ok)
{
	char tmp[FILE_PATH_MAX];

	if (plat_file_close(f) != 0) ok = 0;
	new_path(path, tmp, sizeof(tmp));
	if (!ok) {
		plat_file_unlink(tmp);
		return ST_IO_ERROR;
	}
	plat_file_unlink(path);
	return plat_file_rename(tmp, path) == 0 ? ST_OK : ST_IO_ERROR;
}

/*
 * qread_file, and the ".new" a cut-off write left when there is no file. The
 * ".new" stays where it is: the next write of the file replaces it.
 *
 * Build 57: qread_file answers ST_NOT_FOUND for any open that fails, and a
 * console's cellFsOpen fails for more than a missing file (too many open, the
 * drive busy, an FTP server holding it). A file that is there but would not
 * open is tried once more and is then ST_IO_ERROR, never "no file": no file
 * loads as an empty store that may be written, and the next write would have
 * replaced every setting with the one it made.
 */
static int read_one(const char *path, char *buf, u32 cap, u32 *len)
{
	int rc = qread_file(path, buf, cap, len);

	if (rc != ST_NOT_FOUND || !plat_path_exists(path, NULL, NULL)) return rc;
	rc = qread_file(path, buf, cap, len);
	if (rc != ST_NOT_FOUND || !plat_path_exists(path, NULL, NULL)) return rc;
	return ST_IO_ERROR;
}

static int qfile_read(const char *path, char *buf, u32 cap, u32 *len)
{
	char tmp[FILE_PATH_MAX];
	int rc = read_one(path, buf, cap, len);

	if (rc != ST_NOT_FOUND) return rc;
	new_path(path, tmp, sizeof(tmp));
	return read_one(tmp, buf, cap, len);
}

/*
 * Which of a file's keys a load keeps. LOAD_ALL every one; while a profile is
 * active the console's store is LOAD_CONSOLE_SIDE, everything but the per-user
 * keys, which stay in config.txt (store_save keeps every line the store does
 * not hold), and a COPY is LOAD_USER_SIDE, the per-user keys alone.
 */
#define LOAD_ALL          0
#define LOAD_CONSOLE_SIDE 1
#define LOAD_USER_SIDE    2

/*
 * One "key = value" file into a store, which is emptied first whatever the
 * file is, read whole into the core's text buffer like the positions file.
 * ST_NOT_FOUND for no file. A file the pool (or that buffer) cannot hold all
 * of answers ST_FULL, one that does not read ST_IO_ERROR, and either leaves
 * the store marked (g_over) so that it is never written back over the file.
 */
static int store_load(u8 store, const char *path, int which)
{
	char *text = qtext();
	char *cursor;
	char *line;
	int rc;

	kv_clear(store);
	g_over[store] = 0;

	rc = qfile_read(path, text, QTEXT_BYTES, NULL);
	if (rc == ST_NOT_FOUND) return rc;
	if (rc != ST_OK) {
		g_over[store] = 1;
		return rc;
	}

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *eq;
		char *key;
		char *value;

		line = qtrim(line);
		if (line[0] == '#') continue;

		eq = line;
		while (*eq != 0 && *eq != '=') eq++;
		if (*eq != '=') continue;

		*eq = 0;
		key = qtrim(line);
		value = qtrim(eq + 1);
		if (key[0] == 0) continue;
		/*
		 * Build 57: a key or value longer than the store keeps (a hand edit's)
		 * is not held cut short, which would write it back cut: it is not
		 * held at all, reads as unset, and store_save keeps its line as it is.
		 */
		if (qstrlen(key) >= CONFIG_KEY_MAX || qstrlen(value) >= CONFIG_VAL_MAX) continue;
#if QWARK_PREVIEW
		if (which != LOAD_ALL &&
		    (config_key_scope(key) == CONFIG_SCOPE_USER) != (which == LOAD_USER_SIDE))
			continue;
#else
		(void)which;
#endif
		if (kv_put(store, key, value) != ST_OK) g_over[store] = 1;
	}
	return g_over[store] ? ST_FULL : ST_OK;
}

/*
 * What store_save sends to the ".new": staged here a piece at a time, under
 * the core lock like the rest of the store. g_out_rc is the first thing that
 * went wrong, a write that failed (ST_IO_ERROR) or a file grown past what a
 * read takes back whole (ST_FULL), and nothing more is written once it is set.
 */
#define OUT_STAGE 512

static char        g_out[OUT_STAGE];
static u32         g_out_n;
static u32         g_out_total;
static int         g_out_rc;
static plat_file_t g_out_f;

static void out_flush(void)
{
	if (g_out_n != 0 && g_out_rc == ST_OK && plat_file_write(g_out_f, g_out, g_out_n) != 0)
		g_out_rc = ST_IO_ERROR;
	g_out_n = 0;
}

static void out_put(const char *s, u32 n)
{
	if (g_out_rc != ST_OK) return;
	g_out_total += n;
	if (g_out_total >= QTEXT_BYTES) {
		g_out_rc = ST_FULL;
		return;
	}
	while (n > 0) {
		u32 take = OUT_STAGE - g_out_n;
		if (take > n) take = n;
		memcpy(g_out + g_out_n, s, take);
		g_out_n += take;
		s += take;
		n -= take;
		if (g_out_n == OUT_STAGE) out_flush();
	}
}

static void out_str(const char *s)
{
	out_put(s, qstrlen(s));
}

/* An entry of the pool as its file has it, "key = value". */
static void out_entry(u32 off)
{
	out_str(g_pool + off + 1);
	out_str(" = ");
	out_str(g_pool + off + 2 + qstrlen(g_pool + off + 1));
	out_str("\n");
}

/*
 * The key of one line of a file, as store_load reads it, into `key`: 0 for a
 * line that is not a key the store can hold (blank, a comment, no '=', or a
 * key too long for it). The line itself is not changed.
 */
static int line_key(const char *line, char *key)
{
	const char *s = line;
	const char *e;
	u32 n;

	while (*s == ' ' || *s == '\t') s++;
	if (*s == 0 || *s == '#') return 0;
	e = s;
	while (*e != 0 && *e != '=') e++;
	if (*e != '=') return 0;
	while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
	n = (u32)(e - s);
	if (n == 0 || n >= CONFIG_KEY_MAX) return 0;
	memcpy(key, s, n);
	key[n] = 0;
	return 1;
}

/*
 * A store into its file. Build 57: a write is the file it replaces with the
 * store's keys put in, never the store alone, so no write takes out of a file
 * anything the module does not hold. The old file (a cut-off write's ".new"
 * when there is no other, qfile_read) is read into the core's text buffer and
 * its lines go out in their order: a key the store holds with the store's
 * value, the first time it comes (a second line for it is the same key, and
 * goes); every other line as it was, which is a comment, a blank, a key too
 * long to hold, a key this store does not have (while a profile is active,
 * config.txt's per-user keys, LOAD_CONSOLE_SIDE), or one a load did not keep.
 * The store's keys the file did not have follow, in the store's order. CR LF
 * and a last line with no newline read as any other line, and go out with LF.
 * `drop` names the one key to take out of the file, which the store has just
 * let go of (a combo override removed), or is NULL.
 *
 * The file is written to "<path>.new" in pieces and put in place only when
 * every piece and the close went through (file_end). ST_FULL, and the file as
 * it was, for a store that did not load whole (g_over), an old file too big
 * to read whole, or a new one that would be; an old file there but not
 * readable answers its status and is left as it is.
 */
static int store_save(u8 store, const char *path, const char *drop)
{
	char *text = qtext();
	char *cursor = text;
	char *line;
	char key[CONFIG_KEY_MAX];
	u32 off;
	int rc;

	if (g_over[store]) return ST_FULL;

	rc = qfile_read(path, text, QTEXT_BYTES, NULL);
	if (rc == ST_NOT_FOUND) text[0] = 0;
	else if (rc != ST_OK) return rc;

	if (file_begin(path, &g_out_f) != ST_OK) return ST_IO_ERROR;
	g_out_n = 0;
	g_out_total = 0;
	g_out_rc = ST_OK;

	while ((line = qnext_line(&cursor)) != NULL) {
		if (line_key(line, key)) {
			off = kv_find(store, key);
			if (off != KV_NONE) {
				if (((u8)g_pool[off] & KV_SAVED) == 0) {
					out_entry(off);
					g_pool[off] = (char)((u8)g_pool[off] | KV_SAVED);
				}
				continue;
			}
			if (drop != NULL && qstreq(key, drop)) continue;
		}
		out_str(line);
		out_str("\n");
	}

	for (off = 0; off < g_pool_used; off = kv_next(off)) {
		if ((u8)g_pool[off] == (u8)(store + 1)) out_entry(off);
		g_pool[off] = (char)((u8)g_pool[off] & ~KV_SAVED);
	}
	out_flush();

	rc = file_end(path, g_out_f, g_out_rc == ST_OK);
	return g_out_rc != ST_OK ? g_out_rc : rc;
}

#if QWARK_PREVIEW
static void profile_path(const char *folder, const char *leaf, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, QWARK_PROFILEDIR);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, folder);
	if (leaf != NULL) {
		qstrcat(out, cap, "/");
		qstrcat(out, cap, leaf);
	}
}

#endif /* QWARK_PREVIEW */

/*
 * config.txt. While a profile is active its per-user lines are the console's
 * own settings, which the store does not hold (LOAD_CONSOLE_SIDE), and the
 * write keeps them as it keeps every line the store does not hold.
 */
static int global_save(void)
{
	return store_save(STORE_CONSOLE, QWARK_CONFIG, NULL);
}

#if QWARK_PREVIEW

/* The active profile's config.txt; `drop` as store_save's. */
static int profile_save(const char *drop)
{
	char path[FILE_PATH_MAX];

	if (!g_prof.active || !g_prof.exists) return ST_OK;
	profile_path(g_prof.folder, "config.txt", path, sizeof(path));
	return store_save(STORE_PROFILE, path, drop);
}

/*
 * The active profile's config.txt into its store; nothing for one never made.
 * One the pool cannot hold all of reads what fits and is never written back.
 */
static void profile_load(void)
{
	char path[FILE_PATH_MAX];

	kv_clear(STORE_PROFILE);
	g_over[STORE_PROFILE] = 0;
	if (!g_prof.active || !g_prof.exists) return;
	profile_path(g_prof.folder, "config.txt", path, sizeof(path));
	store_load(STORE_PROFILE, path, LOAD_ALL);
}
#endif /* QWARK_PREVIEW */

int config_load(void)
{
	int rc;

#if QWARK_PREVIEW
	rc = store_load(STORE_CONSOLE, QWARK_CONFIG, g_prof.active ? LOAD_CONSOLE_SIDE : LOAD_ALL);

	/*
	 * Revision 1.24. CONFIG_RELOAD re-reads the active profile with the
	 * console's file, and says so in the profile sequence. Whether the switch
	 * itself changed by hand is the XMB's to act on (session.c), never a game's.
	 */
	if (g_prof.active) {
		profile_load();
		g_prof.seq++;
	}
#else
	rc = store_load(STORE_CONSOLE, QWARK_CONFIG, LOAD_ALL);
#endif

	/*
	 * No file, or one that did not read: the defaults apply. A file the pool
	 * could not hold all of still applies what it read, and answers ST_FULL.
	 */
	if (rc != ST_OK && rc != ST_FULL) {
		plat_log_enable(1);
		return rc;
	}

	/*
	 * Every log line costs a cellFs open, write and close, so a user who does
	 * not want that pays nothing for it. Default on, since the module is new.
	 */
	plat_log_enable((int)config_get_u32("log", 1));

	/* savefile_helper = 0: never write the savefile helper into a game. */
	savefile_set_enabled((int)config_get_u32("savefile_helper", 1));

	return rc;
}

/* Both files: the console's, and the active profile's when it has been made. */
int config_save(void)
{
#if QWARK_PREVIEW
	int rc = global_save();
	int rc2 = profile_save(NULL);
	return rc != ST_OK ? rc : rc2;
#else
	return global_save();
#endif
}

int config_init(void)
{
	plat_dir_create("/dev_hdd0/qwark");
	plat_dir_create(QWARK_POSDIR);
	plat_dir_create(QWARK_MODSDIR);
	plat_dir_create(QWARK_SAVEDIR);
	plat_trace("qwark:     config dirs ok");

	config_load();      /* a missing file is fine, we start empty */
	plat_trace("qwark:     config file read");
	return ST_OK;
}

/* ----------------------------------------------------------------- combos */

/*
 * Revision 1.22: the menu combo is the one with a mask of its own when the file
 * has never named it, COMBO_MENU_DEFAULT. A 0 written there turns it off like
 * any other, and stays off.
 */
u32 config_combo(u8 action)
{
	if (action >= COMBO_COUNT) return 0;
	return config_get_u32(g_combo_key[action],
	                      action == COMBO_OPEN_MENU ? COMBO_MENU_DEFAULT : 0);
}

int config_set_combo(u8 action, u32 mask)
{
	char buf[16];

	if (action >= COMBO_COUNT) return ST_BAD_ARG;

	buf[0] = '0';
	buf[1] = 'x';
	qfmt_hex(buf + 2, sizeof(buf) - 2, mask, 8);
	return config_set(g_combo_key[action], buf);
}

/*
 * Revision 1.12. A config.txt written before this switch existed, or one a user
 * has never touched it in, has no such key, and combos fire: the switch is only
 * ever off because somebody turned it off, and it stays off until somebody
 * turns it back on. Written as a plain 0 or 1, the way every other flag in the
 * file is, so it reads the same by hand as through COMBO_ENABLE.
 */
int config_combo_enabled(void)
{
	return config_get_u32(g_combo_enabled_key, 1) != 0;
}

int config_set_combo_enabled(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32(g_combo_enabled_key, on);
}

/*
 * Revision 1.22. `menu.enabled`, the console menu's own switch: 1 for a file
 * that has never mentioned it. Nothing on the wire writes it; a user edits it,
 * and CONFIG_RELOAD or a console reboot picks it up.
 */
int config_menu_enabled(void)
{
	return config_get_u32("menu.enabled", 1) != 0;
}

/*
 * Revision 1.23. `menu.help`: 1 for a file that has never mentioned it, as the
 * menu took the Help row before the key existed. MENU_HELP writes it, a plain
 * 0 or 1 like `combo.enabled`, so it reads the same by hand.
 */
int config_menu_help(void)
{
	return config_get_u32("menu.help", 1) != 0;
}

int config_set_menu_help(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32("menu.help", on);
}

/*
 * Revision 1.30. `run.warnings`: 1 for a file that has never
 * mentioned it, so a run start with cheats or mods on says so out of the box.
 * RUN_WARN writes it. Revision 1.33: 2 is the cheats line alone. 0 is off and
 * 2 cheats only; any other number in the file reads as 1, as every number but
 * 0 did before 2 meant anything, so a file written by hand keeps warning.
 */
u8 config_run_warnings(void)
{
	u32 v = config_get_u32("run.warnings", RUN_WARN_ALL);
	return (u8)(v == RUN_WARN_NONE ? RUN_WARN_NONE : v == RUN_WARN_CHEATS_ONLY ? RUN_WARN_CHEATS_ONLY : RUN_WARN_ALL);
}

int config_set_run_warnings(u8 mode)
{
	if (mode > RUN_WARN_CHEATS_ONLY) return ST_BAD_ARG;
	return config_set_u32("run.warnings", mode);
}

/*
 * Revision 1.33. `reboot.apply`, Apply toggles on game reboot: 1, Ask, for a
 * file that has never mentioned it, as every build before it left the
 * previous-session record for a client to offer; 0 Always and 2 Never. Any
 * other number in the file reads as Ask. A test build of the revision had a
 * `boot.apply` that meant something else; it is not read, and a line of it
 * left in a file is kept as any unknown key is.
 */
u8 config_reboot_apply(void)
{
	u32 v = config_get_u32("reboot.apply", REBOOT_APPLY_ASK);
	return (u8)(v == REBOOT_APPLY_ALWAYS || v == REBOOT_APPLY_NEVER ? v : REBOOT_APPLY_ASK);
}

int config_set_reboot_apply(u8 mode)
{
	if (mode > REBOOT_APPLY_NEVER) return ST_BAD_ARG;
	return config_set_u32("reboot.apply", mode);
}

/*
 * Revision 1.32. `telemetry.hz`: since revision 1.33 60 for a file that has
 * never mentioned it, and for a hand-edited value that is none of the three
 * (30 before it, as every build before 1.32 sent). Console-wide: it is how
 * often this console talks to the PCs, so a user signing in at the XMB does
 * not change it (config_key_scope leaves it on the console's side, as it does
 * any key it does not name).
 */
#define TELEMETRY_HZ_DEFAULT 60

static int telemetry_hz_ok(u32 hz)
{
	return hz == 30 || hz == 60 || hz == 120;
}

u8 config_telemetry_hz(void)
{
	u32 hz = config_get_u32("telemetry.hz", TELEMETRY_HZ_DEFAULT);
	return (u8)(telemetry_hz_ok(hz) ? hz : TELEMETRY_HZ_DEFAULT);
}

int config_set_telemetry_hz(u8 hz)
{
	if (!telemetry_hz_ok(hz)) return ST_BAD_ARG;
	return config_set_u32("telemetry.hz", hz);
}

/* -------------------------------------------------------------- selection */

u8 config_selected_slot(void)
{
	u32 v = config_get_u32("selected.slot", 0);
	return (u8)(v < QWARK_POS_SLOTS ? v : 0);
}

void config_set_selected_slot(u8 slot)
{
	config_set_u32("selected.slot", slot);
}

/*
 * Build 53. "selected.<game>.<field>", beside the manips' "manip.<game>.*": the
 * longest is "selected.rac4.planet_flags", 26 of CONFIG_KEY_MAX.
 */
static void selected_key(const char *game, const char *field, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, "selected.");
	qstrcat(out, cap, game);
	qstrcat(out, cap, ".");
	qstrcat(out, cap, field);
}

/*
 * The game's own key whenever the file holds one, as a relabelled toggle's auto
 * flag answers from its new label (config_feature_auto_former): a value that
 * does not parse is 0 then, not the old key's. Until then the key the console
 * shared before build 53, `former`, and 0 where neither is there.
 */
static u8 selected_get(const char *game, const char *field, const char *former)
{
	char key[CONFIG_KEY_MAX];

	if (game == NULL) return 0;

	selected_key(game, field, key, sizeof(key));
	if (config_get(key) != NULL) return (u8)config_get_u32(key, 0);
	return (u8)config_get_u32(former, 0);
}

u8 config_selected_planet(const char *game)
{
	return selected_get(game, "planet", "selected.planet");
}

u8 config_selected_planet_flags(const char *game)
{
	return selected_get(game, "planet_flags", "selected.planet_flags");
}

/*
 * Both of the game's own keys, every time, so a game that has made one
 * selection never reads either half of it from the old pair again. The old pair
 * is left as it is: it is what the games with no selection of their own still
 * answer.
 */
int config_set_selected_planet(const char *game, u8 planet, u8 flags)
{
	char key[CONFIG_KEY_MAX];
	int rc;

	if (game == NULL) return ST_BAD_ARG;

	selected_key(game, "planet", key, sizeof(key));
	rc = config_set_u32(key, planet);
	if (rc != ST_OK) return rc;

	selected_key(game, "planet_flags", key, sizeof(key));
	return config_set_u32(key, flags);
}

/* ------------------------------------------------------------- auto flags */

static void feature_key(const char *title, const char *label, char *out, u32 cap)
{
	char slug[48];

	qslug(label, slug, sizeof(slug));

	out[0] = 0;
	qstrcat(out, cap, title);
	qstrcat(out, cap, ".auto.");
	qstrcat(out, cap, slug);
}

static void mod_key(const char *title, const char *dirname, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, title);
	qstrcat(out, cap, ".mod.");
	qstrcat(out, cap, dirname);
	qstrcat(out, cap, ".auto");
}

u32 config_feature_auto(const char *title, const char *label)
{
	return config_feature_auto_default(title, label, 0);
}

/* Revision 1.24: the key's number as it stands, which a COLOR's re-apply needs. */
u32 config_feature_auto_default(const char *title, const char *label, int fallback)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	return config_get_u32(key, fallback != 0);
}

u32 config_feature_auto_former(const char *title, const char *label, const char *former,
                               int fallback)
{
	char key[CONFIG_KEY_MAX];

	if (former != NULL) {
		feature_key(title, label, key, sizeof(key));
		if (config_get(key) == NULL)
			return config_feature_auto_default(title, former, fallback);
	}
	return config_feature_auto_default(title, label, fallback);
}

int config_set_feature_auto(const char *title, const char *label, u32 value)
{
	char key[CONFIG_KEY_MAX];
	feature_key(title, label, key, sizeof(key));
	return config_set_u32(key, value);
}

int config_mod_auto(const char *title, const char *dirname)
{
	char key[CONFIG_KEY_MAX];
	mod_key(title, dirname, key, sizeof(key));
	return config_get_u32(key, 0) != 0;
}

int config_set_mod_auto(const char *title, const char *dirname, int on)
{
	char key[CONFIG_KEY_MAX];
	mod_key(title, dirname, key, sizeof(key));
	return config_set_u32(key, on ? 1 : 0);
}

/* ---------------------------------------------------------- manip options */

static void manip_key(const char *game, const char *field, char *out, u32 cap)
{
	out[0] = 0;
	qstrcat(out, cap, "manip.");
	qstrcat(out, cap, game);
	qstrcat(out, cap, ".");
	qstrcat(out, cap, field);
}

u32 config_manip(const char *game, const char *field, u32 fallback)
{
	char key[CONFIG_KEY_MAX];
	manip_key(game, field, key, sizeof(key));
	return config_get_u32(key, fallback);
}

int config_set_manip(const char *game, const char *field, u32 value)
{
	char key[CONFIG_KEY_MAX];
	manip_key(game, field, key, sizeof(key));
	return config_set_u32(key, value);
}

/* --------------------------------------------------------------- profiles */

/* A module built with QWARK_PREVIEW 0 (proto.h) leaves all of this out; config.h has its stand-ins. */
#if QWARK_PREVIEW

/* The XMB's user numbers are eight digits, 00000001 on; 0 and a negative answer are nobody. */
static int user_valid(u32 user)
{
	return user != 0 && user <= 99999999u;
}

/* Eight digits, leading zeros: the way /dev_hdd0/home names the user's folder. */
static void user_digits(u32 user, char *out, u32 cap)
{
	char num[12];
	u32 n = qfmt_u32(num, sizeof(num), user);

	out[0] = 0;
	for (; n < 8; n++) qstrcat(out, cap, "0");
	qstrcat(out, cap, num);
}

void config_profile_folder_for(u32 user, char *out, u32 cap)
{
	char digits[12];

	out[0] = 0;
	if (!user_valid(user)) return;
	user_digits(user, digits, sizeof(digits));
	qstrcat(out, cap, "user-");
	qstrcat(out, cap, digits);
}

int config_profile_exists(u32 user)
{
	char folder[PROFILE_FOLDER_MAX];
	char path[FILE_PATH_MAX];

	config_profile_folder_for(user, folder, sizeof(folder));
	if (folder[0] == 0) return 0;
	profile_path(folder, "config.txt", path, sizeof(path));
	return plat_path_exists(path, NULL, NULL);
}

/*
 * The active profile's folder, its config.txt and its positions/, made the
 * first time anything is written into a profile that has none: it starts
 * empty, every per-user key at its default. Nothing for a profile that exists
 * or while none is active.
 */
static int profile_make(void)
{
	char path[FILE_PATH_MAX];

	if (!g_prof.active || g_prof.exists) return ST_OK;

	plat_dir_create(QWARK_PROFILEDIR);
	profile_path(g_prof.folder, NULL, path, sizeof(path));
	plat_dir_create(path);
	profile_path(g_prof.folder, "positions", path, sizeof(path));
	plat_dir_create(path);

	g_prof.exists = 1;
	if (profile_save(NULL) != ST_OK) {
		g_prof.exists = 0;
		return ST_IO_ERROR;
	}
	return ST_OK;
}

/*
 * Build 56. What a setup writes into a profile's folder: the four position
 * files and config.txt, which is what makes a profile made.
 */
static const char * const g_prof_files[5] = {
	"positions/rac1.txt", "positions/rac2.txt", "positions/rac3.txt", "positions/rac4.txt",
	"config.txt"
};

/*
 * A setup that could not finish takes away every file it wrote, so no profile
 * is left behind half-copied or empty and the next time the switch goes on
 * the question is asked again. The empty folders it made can stay: a profile
 * is made when its config.txt is there.
 */
static void profile_unmake(void)
{
	char path[FILE_PATH_MAX];
	int i;

	for (i = 0; i < 5; i++) {
		profile_path(g_prof.folder, g_prof_files[i], path, sizeof(path));
		plat_file_unlink(path);
	}
	g_prof.exists = 0;
}

/*
 * PROFILE_ENABLE's COPY, into the profile just made: every per-user key of the
 * console's config.txt, read from the file (while a profile is active they are
 * not in the pool), and the console's four position files. The combos are not
 * copied: the console's are in force for every profile until it overrides one.
 * A position file that is not there, or does not read, copies as no slots.
 * Anything but ST_OK, and the caller takes the profile away again.
 */
static int profile_copy_shared(void)
{
	char keep[sizeof(g_pos_title)];
	int rc;
	int i;

	rc = store_load(STORE_PROFILE, QWARK_CONFIG, LOAD_USER_SIDE);
	if (rc == ST_NOT_FOUND) rc = ST_OK;
	if (rc == ST_OK) rc = profile_save(NULL);

	/*
	 * The slots, a game at a time, through the slot table itself: read with the
	 * console's folder in force and written with the profile's. The resolver
	 * reads the running game's back once this is done.
	 */
	qstrcpy(keep, sizeof(keep), g_pos_title);
	for (i = 0; i < 4 && rc == ST_OK; i++) {
		qstrcpy(g_pos_title, 5, g_prof_files[i] + 10);   /* "positions/rac1.txt": "rac1" */
		g_prof.active = 0;
		pos_load_file();
		g_prof.active = 1;
		rc = pos_save_file();
	}
	qstrcpy(g_pos_title, sizeof(g_pos_title), keep);
	return rc;
}

int config_profiles_enabled(void)
{
	return config_get_u32("profiles.enabled", 0) != 0;
}

int config_set_profiles_enabled(u8 on)
{
	if (on > 1) return ST_BAD_ARG;
	return config_set_u32("profiles.enabled", on);
}

/*
 * The resolver's work (config.h), for the switch as the pool has it: a
 * PROFILE_ENABLE has written it first. `*changed` is 1 when the active profile
 * changed, or was made, or a setup failed part-way. The status is the
 * setup's: on anything but ST_OK no profile is active and nothing the setup
 * began is left.
 *
 * The first profile to go active takes the console's per-user keys out of the
 * pool: they are config.txt's and stay in it (store_save keeps them), so the
 * pool holds one set of per-user keys, whichever is in use. The last to go
 * reads them back from the file.
 */
static int profile_switch(u32 user, u8 setup, int *changed)
{
	int want = plat_has_users() && config_profiles_enabled() && user_valid(user);
	int make = want && setup != PROFILE_SETUP_NONE && !config_profile_exists(user);
	int was = g_prof.active;
	int rc = ST_OK;

	*changed = 0;
	if (!make && want == was && (!want || user == g_prof.user)) return ST_OK;
	*changed = 1;

	/* The slots go back to the file they came from before the folder changes. */
	pos_save_file();

	g_prof.active = 0;
	if (want) {
		/* config.txt's per-user keys out of the pool: the file keeps them. */
		if (!was) store_load(STORE_CONSOLE, QWARK_CONFIG, LOAD_CONSOLE_SIDE);
		g_prof.active = 1;
		g_prof.user = user;
		config_profile_folder_for(user, g_prof.folder, sizeof(g_prof.folder));
		g_prof.exists = (u8)config_profile_exists(user);

		if (make) {
			rc = profile_make();
			if (rc == ST_OK && setup == PROFILE_SETUP_COPY) rc = profile_copy_shared();
			if (rc != ST_OK) {
				profile_unmake();
				g_prof.active = 0;
			}
		}
	}
	profile_load();   /* the store emptied, and the active profile's file read */
	if (!g_prof.active) {
		g_prof.user = 0;
		g_prof.folder[0] = 0;
		if (was || rc != ST_OK) store_load(STORE_CONSOLE, QWARK_CONFIG, LOAD_ALL);
	}

	pos_load_file();
	g_prof.seq++;
	g_version++;

	plat_log("qwark: profile %s, %d", g_prof.active ? g_prof.folder : "off", rc);
	return rc;
}

int config_profile_resolve(u32 user, u8 setup)
{
	int changed;

	profile_switch(user, setup, &changed);
	return changed;
}

int config_profiles_enable(u8 on, u8 setup, u32 user, int *changed)
{
	const char *was = config_profiles_enabled() ? "1" : "0";
	int rc;

	/* The switch first: a config.txt with no room for it changes nothing at all. */
	*changed = 0;
	rc = config_set_u32("profiles.enabled", on);
	if (rc == ST_OK) rc = profile_switch(user, setup, changed);
	if (rc != ST_OK) {
		/*
		 * Nothing of a setup that did not finish is left, so the switch goes
		 * back as well, in config.txt once the setup had begun.
		 */
		kv_put(STORE_CONSOLE, "profiles.enabled", was);
		if (*changed) global_save();
	}
	return rc;
}

int config_profile_active(void)        { return g_prof.active; }
u32 config_profile_user(void)          { return g_prof.user; }
u8  config_profile_seq(void)           { return g_prof.seq; }

u8 config_profile_flags(void)
{
	u8 flags = 0;

	if (!plat_has_users()) return 0;
	flags |= SESSION_PROFILE_SUPPORTED;
	if (config_profiles_enabled()) flags |= SESSION_PROFILE_ON;
	if (g_prof.active)             flags |= SESSION_PROFILE_ACTIVE;
	return flags;
}

int config_combo_overridden(u8 action)
{
	if (action >= COMBO_COUNT || !g_prof.active) return 0;
	return kv_find(STORE_PROFILE, g_combo_key[action]) != KV_NONE;
}

int config_set_combo_override(u8 action, int set, u32 mask)
{
	char buf[16];
	int rc;

	if (action >= COMBO_COUNT) return ST_BAD_ARG;
	if (!g_prof.active) return ST_NOT_FOUND;

	if (!set) {
		u32 off = kv_find(STORE_PROFILE, g_combo_key[action]);
		if (off == KV_NONE) return ST_OK;
		pool_splice(off, kv_next(off) - off, "", 0);
		return profile_save(g_combo_key[action]);
	}

	rc = profile_make();
	if (rc != ST_OK) return rc;

	buf[0] = '0';
	buf[1] = 'x';
	qfmt_hex(buf + 2, sizeof(buf) - 2, mask, 8);
	rc = kv_put(STORE_PROFILE, g_combo_key[action], buf);
	if (rc != ST_OK) return rc;
	return profile_save(NULL);
}

#endif /* QWARK_PREVIEW */

/* -------------------------------------------------------------- positions */

/*
 * A planet is a u8 everywhere it is asked for, so it is one here too: a
 * hand-edited line naming a planet past 255 used to take a slot it could never
 * be fetched from again, and is skipped instead.
 */
struct pos_entry {
	u8  planet;
	u8  slot;
	u8  len;
	u8  used;
	u8  blob[QWARK_MAX_BLOB];
};

static struct pos_entry g_pos[POS_MAX_ENTRIES];

/*
 * Build 57: the positions file is there but did not read (or is too big to),
 * so the table is empty without the file being so, and it is never written
 * over: the next store would have left the file that one slot.
 */
static u8 g_pos_bad;

/*
 * Revision 1.24: the slots are the active profile's, in its own positions/
 * folder, and the console's while none is active.
 */
static void pos_dir(char *out, u32 cap)
{
#if QWARK_PREVIEW
	if (g_prof.active) {
		profile_path(g_prof.folder, "positions", out, cap);
		return;
	}
#endif
	out[0] = 0;
	qstrcat(out, cap, QWARK_POSDIR);
}

static void pos_path(char *out, u32 cap)
{
	pos_dir(out, cap);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, g_pos_title);
	qstrcat(out, cap, ".txt");
}

static int pos_load_file(void)
{
	char path[128];
	char *text = qtext();
	char *cursor;
	char *line;
	int rc;

	memset(g_pos, 0, sizeof(g_pos));
	g_pos_bad = 0;

	if (g_pos_title[0] == 0) return ST_OK;

	pos_path(path, sizeof(path));
	rc = qfile_read(path, text, POS_TEXT_MAX, NULL);
	if (rc != ST_OK) {
		if (rc != ST_NOT_FOUND) g_pos_bad = 1;
		return ST_OK;
	}

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL) {
		char *eq;
		char *dot;
		char *key;
		char *value;
		u32 planet;
		u32 slot;
		int ok = 0;
		int i;

		line = qtrim(line);
		if (line[0] == 0 || line[0] == '#') continue;

		eq = line;
		while (*eq != 0 && *eq != '=') eq++;
		if (*eq != '=') continue;
		*eq = 0;
		key = qtrim(line);
		value = qtrim(eq + 1);

		dot = key;
		while (*dot != 0 && *dot != '.') dot++;
		if (*dot != '.') continue;
		*dot = 0;

		planet = qparse_u32(key, &ok);
		if (!ok || planet > 0xFFu) continue;
		slot = qparse_u32(dot + 1, &ok);
		if (!ok || slot >= QWARK_POS_SLOTS) continue;

		for (i = 0; i < POS_MAX_ENTRIES; i++) {
			if (g_pos[i].used) continue;
			g_pos[i].planet = (u8)planet;
			g_pos[i].slot   = (u8)slot;
			g_pos[i].len    = (u8)qhex_to_bytes(value, g_pos[i].blob, QWARK_MAX_BLOB);
			g_pos[i].used   = 1;
			break;
		}
	}

	return ST_OK;
}

int pos_save_file(void)
{
	char path[128];
	plat_file_t f;
	int i;

	int ok = 1;

	if (g_pos_title[0] == 0) return ST_OK;
	if (g_pos_bad) return ST_IO_ERROR;
#if QWARK_PREVIEW
	/* A profile never made has no folder, and nothing to save: a store makes it. */
	if (g_prof.active && !g_prof.exists) return ST_OK;
#endif

	pos_path(path, sizeof(path));
	if (file_begin(path, &f) != ST_OK) return ST_IO_ERROR;

	for (i = 0; i < POS_MAX_ENTRIES && ok; i++) {
		char line[16 + 2 * QWARK_MAX_BLOB + 8];
		char hex[2 * QWARK_MAX_BLOB + 2];
		char num[12];

		if (!g_pos[i].used) continue;

		qbytes_to_hex(g_pos[i].blob, g_pos[i].len, hex, sizeof(hex));

		line[0] = 0;
		qfmt_u32(num, sizeof(num), g_pos[i].planet);
		qstrcat(line, sizeof(line), num);
		qstrcat(line, sizeof(line), ".");
		qfmt_u32(num, sizeof(num), g_pos[i].slot);
		qstrcat(line, sizeof(line), num);
		qstrcat(line, sizeof(line), " = ");
		qstrcat(line, sizeof(line), hex);
		qstrcat(line, sizeof(line), "\n");

		if (plat_file_write(f, line, qstrlen(line)) != 0) ok = 0;
	}

	return file_end(path, f, ok);
}

int pos_use_title(const char *title)
{
	if (title != NULL && qstreq(g_pos_title, title)) return ST_OK;

	if (g_pos_title[0] != 0) pos_save_file();

	qstrcpy(g_pos_title, sizeof(g_pos_title), title ? title : "");
	return pos_load_file();
}

static struct pos_entry *pos_find(u8 planet, u8 slot)
{
	int i;
	for (i = 0; i < POS_MAX_ENTRIES; i++) {
		if (g_pos[i].used && g_pos[i].planet == planet && g_pos[i].slot == slot)
			return &g_pos[i];
	}
	return NULL;
}

int pos_store(u8 planet, u8 slot, const u8 *blob, u8 len)
{
	struct pos_entry *e;
	int i;

	if (slot >= QWARK_POS_SLOTS || len > QWARK_MAX_BLOB) return ST_BAD_ARG;

#if QWARK_PREVIEW
	/* Revision 1.24: a slot stored into a profile never made makes it. */
	{
		int rc = profile_make();
		if (rc != ST_OK) return rc;
	}
#endif

	e = pos_find(planet, slot);
	if (e == NULL) {
		for (i = 0; i < POS_MAX_ENTRIES; i++) {
			if (!g_pos[i].used) { e = &g_pos[i]; break; }
		}
		if (e == NULL) return ST_FULL;
		e->used   = 1;
		e->planet = planet;
		e->slot   = slot;
	}

	memset(e->blob, 0, sizeof(e->blob));
	memcpy(e->blob, blob, len);
	e->len = len;

	return pos_save_file();
}

int pos_fetch(u8 planet, u8 slot, u8 *blob, u8 *len)
{
	struct pos_entry *e = pos_find(planet, slot);

	if (e == NULL) return ST_NOT_FOUND;

	memcpy(blob, e->blob, e->len);
	*len = e->len;
	return ST_OK;
}

int pos_clear(u8 planet, u8 slot)
{
	struct pos_entry *e = pos_find(planet, slot);

	if (e == NULL) return ST_NOT_FOUND;

	memset(e, 0, sizeof(*e));
	return pos_save_file();
}
