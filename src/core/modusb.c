/*
 * Build 56, revision 1.24: mods on a USB drive. modusb.h says what and why;
 * PROTOCOL.md 5.7.2 is the contract.
 */
#include "modusb.h"
#include "mods.h"
#include "mem.h"
#include "config.h"
#include "util.h"
#include "../plat/plat.h"

#include <string.h>

/*
 * Every path this file builds: "/dev_usb000/qwark/mods/" is 23 bytes, a title
 * id at most 15, a folder name 31 and a .bin name 63, so 192 holds the longest
 * with room over; a path that would not fit is refused, never cut.
 */
#define MODUSB_PATH_MAX   192u

/* How much of a file is read at a time: on the stack, on either thread. */
#define MODUSB_CHUNK      512u

/* A .bin name is at most what parse_mod keeps of one (struct mod_cave). */
#define MODUSB_BIN_NAME_MAX 63u

/* Where a take builds its copy, and where the copy it replaces waits to go. */
#define MODUSB_TMP        QWARK_ROOT "/usbtmp"
#define MODUSB_TMP_NEW    QWARK_ROOT "/usbtmp/new"
#define MODUSB_TMP_OLD    QWARK_ROOT "/usbtmp/old"

/* The mount points the console gives USB storage, and the two places on each. */
static const u8 g_ports[] = { 0, 1, 2, 3, 6, 7 };
static const char * const g_roots[] = { "/qwark/mods", "/mods" };

static struct modusb_entry g_usb[MODUSB_MAX];
static u32  g_nusb;
static u8   g_scanned;
static u64  g_scan_us;
static char g_scan_title[16];

/* modusb_prepare's walk over a mod and what it depends on: static, as mods.c's is. */
static u8   g_walk[QWARK_MAX_MODS];

int modusb_enabled(void)
{
	return plat_can_patch_code();
}

u32 modusb_count(void) { return g_nusb; }

const struct modusb_entry *modusb_at(u32 index)
{
	return index < g_nusb ? &g_usb[index] : NULL;
}

int modusb_find(const char *dirname)
{
	u32 i;
	for (i = 0; i < g_nusb; i++) {
		if (qstreq(g_usb[i].dirname, dirname)) return (int)i;
	}
	return -1;
}

static int modusb_find_by_name(const char *name)
{
	u32 i;
	for (i = 0; i < g_nusb; i++) {
		if (qstreq(g_usb[i].name, name)) return (int)i;
	}
	return -1;
}

/* ------------------------------------------------------------- versions */

static int ver_parse(const char *s, u32 part[4])
{
	u32 n = 0;

	part[0] = part[1] = part[2] = part[3] = 0;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == 'v' || *s == 'V') s++;

	for (;;) {
		u32 v = 0;
		u32 digits = 0;

		while (*s >= '0' && *s <= '9') {
			if (v < 100000000u) v = v * 10u + (u32)(*s - '0');
			s++;
			digits++;
		}
		if (digits == 0 || n >= 4) return 0;
		part[n++] = v;
		if (*s != '.') break;
		s++;
	}

	while (*s == ' ' || *s == '\t') s++;
	return *s == 0;
}

int modusb_version_cmp(const char *a, const char *b, int *cmp)
{
	u32 pa[4], pb[4];
	u32 i;

	*cmp = 0;
	if (!ver_parse(a, pa) || !ver_parse(b, pb)) return 0;
	for (i = 0; i < 4; i++) {
		if (pa[i] != pb[i]) {
			*cmp = pa[i] > pb[i] ? 1 : -1;
			break;
		}
	}
	return 1;
}

/* ---------------------------------------------------------------- paths */

/* a + "/" + b into out, 0 (and nothing promised in out) when it would not fit. */
static int path_join(char *out, u32 cap, const char *a, const char *b)
{
	if (qstrlen(a) + 1u + qstrlen(b) + 1u > cap) return 0;
	qstrcpy(out, cap, a);
	qstrcat(out, cap, "/");
	qstrcat(out, cap, b);
	return 1;
}

/* /dev_usb00N<root>/<title>, the folder a title's USB mods are in. */
static int usb_title_path(char *out, u32 cap, u8 port, u8 root, const char *title)
{
	char base[32];

	qstrcpy(base, sizeof(base), "/dev_usb00");
	base[10] = (char)('0' + port);
	base[11] = 0;
	qstrcat(base, sizeof(base), g_roots[root]);
	return path_join(out, cap, base, title);
}

/*
 * A .bin name qwark will read off a USB drive and write under its own folder:
 * relative, no "..", "." or empty part, no '\' or ':', at most 63 bytes. The
 * console's own library is the client's to fill and is not held to this, but a
 * take writes what patch.txt names, so a name that climbs out is never copied.
 */
static int bin_name_ok(const char *name)
{
	u32 n = qstrlen(name);
	u32 i;
	u32 part = 0;

	if (n == 0 || n > MODUSB_BIN_NAME_MAX || name[0] == '/') return 0;
	for (i = 0; i <= n; i++) {
		char c = name[i];

		if (c == '\\' || c == ':') return 0;
		if (c == '/' || c == 0) {
			if (part == 0) return 0;
			if (part == 1 && name[i - 1] == '.') return 0;
			if (part == 2 && name[i - 1] == '.' && name[i - 2] == '.') return 0;
			part = 0;
			continue;
		}
		part++;
	}
	return 1;
}

/* ------------------------------------------------------------ patch.txt */

/*
 * One line of a patch.txt the way the client reads one (ModLibrary.Read), for
 * the two things this file wants: the "#-" metadata into `meta` when it is not
 * NULL, qwark's parse_meta rule, and a .bin name in *bin, returning 1 for one.
 * Writes NULs into the line, as qnext_line has already.
 */
static int line_bin(char *line, struct modusb_entry *meta, char **bin)
{
	char *colon;
	char *key;
	char *value;

	if (line[0] == '#' && line[1] == '-') {
		if (meta == NULL) return 0;
		colon = line;
		while (*colon != 0 && *colon != ':') colon++;
		if (*colon != ':') return 0;
		*colon = 0;
		key = qtrim(line + 2);
		value = qtrim(colon + 1);
		if (qstreq(key, "name"))         qstrcpy(meta->name, sizeof(meta->name), value);
		else if (qstreq(key, "version")) qstrcpy(meta->version, sizeof(meta->version), value);
		else if (qstreq(key, "author"))  qstrcpy(meta->author, sizeof(meta->author), value);
		return 0;
	}

	line = qtrim(line);
	if (qstrlen(line) < 2 || line[0] == '#') return 0;

	colon = line;
	while (*colon != 0 && *colon != ':') colon++;
	if (*colon != ':') return 0;
	*colon = 0;
	key = qtrim(line);
	value = qtrim(colon + 1);

	if (!(key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))) return 0;
	if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) return 0;
	if (value[0] == 0) return 0;
	*bin = value;
	return 1;
}

/* Whether `name` is among the first `n` of `bins`, ignoring case as the client does. */
static int bin_seen(char * const *bins, u32 n, const char *name)
{
	u32 i;
	for (i = 0; i < n; i++) {
		if (qstreq_ci(bins[i], name)) return 1;
	}
	return 0;
}

/* CRC-32 and length of a whole file through `chunk`, into the running state. */
static int file_crc(const char *path, u8 *chunk, u32 *crc, u32 *size, u32 max)
{
	plat_file_t f;
	int rc = ST_OK;

	*size = 0;
	if (plat_file_open(path, PLAT_OPEN_READ, &f) != 0) return ST_NOT_FOUND;
	for (;;) {
		u32 got = 0;

		if (plat_file_read(f, chunk, MODUSB_CHUNK, &got) != 0) { rc = ST_IO_ERROR; break; }
		if (got == 0) break;
		*size += got;
		if (*size > max) { rc = ST_FULL; break; }
		*crc = qcrc32_update(*crc, chunk, got);
	}
	plat_file_close(f);
	return rc;
}

static int write_file(const char *path, const u8 *data, u32 len)
{
	plat_file_t f;
	int rc;

	if (plat_file_open(path, PLAT_OPEN_WRITE, &f) != 0) return ST_IO_ERROR;
	rc = plat_file_write(f, data, len) == 0 ? ST_OK : ST_IO_ERROR;
	if (plat_file_close(f) != 0) rc = ST_IO_ERROR;
	return rc;
}

/* Reads `path` back and compares it with what was written: the CRC and the length. */
static int verify_file(const char *path, u8 *chunk, u32 want_crc, u32 want_len)
{
	u32 crc = qcrc32_start();
	u32 size = 0;

	if (file_crc(path, chunk, &crc, &size, MODUSB_FILE_MAX) != ST_OK) return ST_IO_ERROR;
	return (size == want_len && qcrc32_finish(crc) == want_crc) ? ST_OK : ST_IO_ERROR;
}

/*
 * file_crc's twin for a take: the file is also written to `to` as it goes past,
 * and read back and compared at the end. A read that fails - a drive pulled
 * out - or a file past the bound stops it.
 */
static int copy_file(const char *from, const char *to, u8 *chunk, u32 *crc, u32 *size)
{
	plat_file_t in, out;
	u32 filecrc = qcrc32_start();
	int rc = ST_OK;

	*size = 0;
	if (plat_file_open(from, PLAT_OPEN_READ, &in) != 0) return ST_NOT_FOUND;
	if (plat_file_open(to, PLAT_OPEN_WRITE, &out) != 0) {
		plat_file_close(in);
		return ST_IO_ERROR;
	}

	for (;;) {
		u32 got = 0;

		if (plat_file_read(in, chunk, MODUSB_CHUNK, &got) != 0) { rc = ST_IO_ERROR; break; }
		if (got == 0) break;
		*size += got;
		if (*size > MODUSB_FILE_MAX) { rc = ST_FULL; break; }
		if (plat_file_write(out, chunk, got) != 0) { rc = ST_IO_ERROR; break; }
		filecrc = qcrc32_update(filecrc, chunk, got);
		*crc = qcrc32_update(*crc, chunk, got);
	}

	plat_file_close(in);
	if (plat_file_close(out) != 0 && rc == ST_OK) rc = ST_IO_ERROR;
	return rc == ST_OK ? verify_file(to, chunk, qcrc32_finish(filecrc), *size) : rc;
}

/* The folders a .bin name sits in, made under `root` one level at a time. */
static void make_parents(char *path, u32 at)
{
	for (; path[at] != 0; at++) {
		if (path[at] != '/') continue;
		path[at] = 0;
		plat_dir_create(path);     /* already there is fine; the file open says if not */
		path[at] = '/';
	}
}

static void set_problem(u8 *problem, u8 p)
{
	if (*problem == MODUSB_PROBLEM_NONE) *problem = p;
}

/*
 * Reads a mod folder: the "#-" lines into `e` (when it is not NULL), and the
 * content hash qwark.sum carries - CRC-32 of patch.txt and then of each .bin it
 * names, once each in the order they first appear, a missing one skipped,
 * exactly the client's ComputeHash - with the first problem met. 0 when the
 * folder has no patch.txt and so is no mod at all.
 *
 * With `to` a folder this is a take's copy as well: patch.txt and each .bin
 * are written under `to` as they are read, each read back and compared, and
 * the first problem stops it, so the hash is that of what was read.
 */
static int read_folder(const char *folder, struct modusb_entry *e, u32 *hash, u8 *problem, const char *to)
{
	char path[MODUSB_PATH_MAX];
	char dst[MODUSB_PATH_MAX];
	u8 chunk[MODUSB_CHUNK];
	char *bins[MODUSB_MAX_BINS];
	char *text = qtext();
	char *cursor;
	char *line;
	u32 nbins = 0;
	u32 len = 0;
	u32 total;
	u32 crc;
	int rc;

	*hash = 0;
	*problem = MODUSB_PROBLEM_NONE;

	if (!path_join(path, sizeof(path), folder, "patch.txt")) return 0;
	rc = qread_file(path, text, MOD_TEXT_MAX, &len);
	if (rc == ST_NOT_FOUND && !plat_path_exists(path, NULL, NULL)) return 0;
	if (rc != ST_OK) {
		*problem = rc == ST_FULL ? MODUSB_PROBLEM_PATCH : MODUSB_PROBLEM_READ;
		return 1;
	}

	crc = qcrc32_update(qcrc32_start(), (const u8 *)text, len);
	total = len;

	if (to != NULL && (!path_join(dst, sizeof(dst), to, "patch.txt") ||
	                   write_file(dst, (const u8 *)text, len) != ST_OK ||
	                   verify_file(dst, chunk, qcrc32((const u8 *)text, len), len) != ST_OK)) {
		*problem = MODUSB_PROBLEM_READ;
		return 1;
	}

	cursor = text;
	while ((line = qnext_line(&cursor)) != NULL && (to == NULL || *problem == MODUSB_PROBLEM_NONE)) {
		char *bin = NULL;
		u32 size = 0;

		if (!line_bin(line, e, &bin)) continue;
		if (bin_seen(bins, nbins, bin)) continue;

		if (!bin_name_ok(bin) || !path_join(path, sizeof(path), folder, bin)) {
			set_problem(problem, MODUSB_PROBLEM_NAME);
			continue;
		}
		if (nbins >= MODUSB_MAX_BINS) {
			set_problem(problem, MODUSB_PROBLEM_COUNT);
			continue;
		}
		bins[nbins++] = bin;

		if (to == NULL) {
			rc = file_crc(path, chunk, &crc, &size, MODUSB_FILE_MAX);
		} else if (path_join(dst, sizeof(dst), to, bin)) {
			make_parents(dst, qstrlen(to) + 1u);
			rc = copy_file(path, dst, chunk, &crc, &size);
		} else {
			rc = ST_BAD_ARG;
		}

		total += size;
		if (rc == ST_NOT_FOUND) set_problem(problem, MODUSB_PROBLEM_MISSING);
		else if (rc == ST_FULL || total > MODUSB_TOTAL_MAX) set_problem(problem, MODUSB_PROBLEM_SIZE);
		else if (rc != ST_OK) set_problem(problem, MODUSB_PROBLEM_READ);
	}

	*hash = qcrc32_finish(crc);
	return 1;
}

/* ------------------------------------------------------------------ scan */

/* Which of two copies of one mod a scan keeps: 1 for `cand` over `have`. */
static int better(const struct modusb_entry *cand, const struct modusb_entry *have)
{
	int cmp = 0;

	/* A copy that can be taken over one that cannot... */
	if (have->problem != MODUSB_PROBLEM_NONE && cand->problem == MODUSB_PROBLEM_NONE) return 1;
	if (have->problem == MODUSB_PROBLEM_NONE && cand->problem != MODUSB_PROBLEM_NONE) return 0;
	/*
	 * ...then the newer version. Equal versions are the same mod, and so are two
	 * copies with no version to compare: the first found stays, which is the
	 * lower port, and on one drive qwark/mods before mods.
	 */
	return modusb_version_cmp(cand->version, have->version, &cmp) && cmp > 0;
}

void modusb_scan(int force)
{
	const char *title = mods_title();
	u64 now = plat_time_us();
	u32 p, r, i;

	if (!modusb_enabled() || title[0] == 0) {
		g_nusb = 0;
		g_scanned = 0;
		return;
	}
	if (!force && g_scanned && qstreq(g_scan_title, title) && now - g_scan_us < MODUSB_FRESH_US)
		return;

	g_nusb = 0;

	for (p = 0; p < sizeof(g_ports); p++) {
		for (r = 0; r < 2; r++) {
			char path[MODUSB_PATH_MAX];
			char folder[MODUSB_PATH_MAX];
			plat_dir_t dir;
			struct plat_dirent ent;

			if (!usb_title_path(path, sizeof(path), g_ports[p], (u8)r, title)) continue;
			if (plat_dir_open(path, &dir) != 0) continue;

			while (plat_dir_next(&dir, &ent) == 1) {
				struct modusb_entry tmp;
				int at;

				if (!ent.is_dir) continue;
				if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
				/* A name MOD_LIST and MOD_LOAD cannot carry whole is no use to anyone. */
				if (qstrlen(ent.name) > 31u) continue;
				if (!path_join(folder, sizeof(folder), path, ent.name)) continue;

				memset(&tmp, 0, sizeof(tmp));
				qstrcpy(tmp.dirname, sizeof(tmp.dirname), ent.name);
				qstrcpy(tmp.name, sizeof(tmp.name), ent.name);
				tmp.port = g_ports[p];
				tmp.root = (u8)r;
				if (!read_folder(folder, &tmp, &tmp.hash, &tmp.problem, NULL)) continue;

				at = modusb_find(tmp.dirname);
				if (at >= 0) {
					if (better(&tmp, &g_usb[at])) g_usb[at] = tmp;
				} else if (g_nusb < MODUSB_MAX) {
					g_usb[g_nusb++] = tmp;
				}
			}
			plat_dir_close(&dir);
		}
	}

	/*
	 * A console copy with no qwark.sum (put there by hand rather than by the
	 * client) has hash 0 in its row; its content is worked out here, once a
	 * scan, so SAME and CONFLICT still mean what they say.
	 */
	for (i = 0; i < g_nusb; i++) {
		int idx = mods_find(g_usb[i].dirname);
		const struct mod_entry *m = idx >= 0 ? mods_at((u32)idx) : NULL;
		char base[MODUSB_PATH_MAX];
		char folder[MODUSB_PATH_MAX];
		u8 problem;

		g_usb[i].hdd_hash = 0;
		if (m == NULL || m->hash != 0) continue;
		if (!path_join(base, sizeof(base), QWARK_MODSDIR, title)) continue;
		if (!path_join(folder, sizeof(folder), base, m->dirname)) continue;
		read_folder(folder, NULL, &g_usb[i].hdd_hash, &problem, NULL);
	}

	if (force && g_nusb > 0) plat_log("mods: %d on USB for %s", (int)g_nusb, title);
	qstrcpy(g_scan_title, sizeof(g_scan_title), title);
	g_scan_us = now;
	g_scanned = 1;
}

/* ----------------------------------------------------------------- state */

u8 modusb_state(const struct modusb_entry *u, u8 *flags)
{
	int idx = mods_find(u->dirname);
	const struct mod_entry *m = idx >= 0 ? mods_at((u32)idx) : NULL;
	u32 hdd_hash;
	int cmp = 0;
	int comparable;

	*flags = u->root == MODUSB_ROOT_MODS ? MODUSB_FLAG_ROOT_MODS : 0;

	if (u->problem != MODUSB_PROBLEM_NONE) return MODUSB_BROKEN;
	if (m == NULL) return MODUSB_AVAILABLE;

	comparable = modusb_version_cmp(u->version, m->version, &cmp);
	if (comparable && cmp > 0) {
		if (m->flags & MOD_FLAG_LOADED) *flags |= MODUSB_FLAG_WAITING;
		return MODUSB_NEWER;
	}
	if (comparable && cmp < 0) return MODUSB_OLDER;

	hdd_hash = m->hash != 0 ? m->hash : u->hdd_hash;
	return u->hash == hdd_hash ? MODUSB_SAME : MODUSB_CONFLICT;
}

int modusb_list_encode(u8 *out, u32 cap, u32 *len)
{
	u32 i;
	u32 off = 1;

	*len = 0;
	if (!modusb_enabled()) return ST_UNSUPPORTED;
	modusb_scan(0);
	if (cap < 1u + g_nusb * MODUSB_WIRE_SIZE) return ST_FULL;

	for (i = 0; i < g_nusb; i++) {
		const struct modusb_entry *u = &g_usb[i];
		u8 flags = 0;

		out[off + 1] = modusb_state(u, &flags);
		out[off + 0] = u->port;
		out[off + 2] = flags;
		out[off + 3] = u->problem;
		be32_put(out + off + 4, u->hash);
		qput_fixed(out + off + 8, 32, u->dirname);
		qput_fixed(out + off + 40, 32, u->name);
		qput_fixed(out + off + 72, 16, u->version);
		qput_fixed(out + off + 88, 32, u->author);
		off += MODUSB_WIRE_SIZE;
	}

	out[0] = (u8)g_nusb;
	*len = off;
	return ST_OK;
}

/* ------------------------------------------------------------- the copy */

/*
 * Deletes a folder and everything under it, or a file, with one folder handle
 * and one path at a time however deep it goes: it goes down to the first
 * subfolder it finds, deleting files on the way, and back up once a folder is
 * empty. No recursion, so the stack it costs is the same at any depth; the path
 * buffer is what bounds the depth.
 */
static int tree_delete(const char *root)
{
	char path[MODUSB_PATH_MAX];
	u32 base;
	u32 guard;
	int is_dir = 0;

	if (!plat_path_exists(root, &is_dir, NULL)) return ST_OK;
	if (!is_dir) return plat_file_unlink(root) == 0 ? ST_OK : ST_IO_ERROR;

	qstrcpy(path, sizeof(path), root);
	base = qstrlen(path);

	for (guard = 0; guard < 4096u; guard++) {
		plat_dir_t dir;
		struct plat_dirent ent;
		u32 at = qstrlen(path);
		int down = 0;

		if (plat_dir_open(path, &dir) != 0) return ST_IO_ERROR;
		while (plat_dir_next(&dir, &ent) == 1) {
			if (qstreq(ent.name, ".") || qstreq(ent.name, "..")) continue;
			if (at + 1u + qstrlen(ent.name) + 1u > sizeof(path)) continue;
			path[at] = '/';
			qstrcpy(path + at + 1, sizeof(path) - at - 1, ent.name);
			if (ent.is_dir) {
				down = 1;
				break;
			}
			plat_file_unlink(path);
			path[at] = 0;
		}
		plat_dir_close(&dir);
		if (down) continue;

		if (plat_dir_remove(path) != 0) return ST_IO_ERROR;
		if (at <= base) return ST_OK;
		while (at > base && path[at] != '/') at--;
		path[at] = 0;
	}
	return ST_IO_ERROR;
}

/*
 * Copies USB entry `ui` into the console's library, whole or not at all:
 *
 *   1. read_folder copies patch.txt and every .bin it names into
 *      /dev_hdd0/qwark/usbtmp/new, each read back and compared, within the
 *      bounds, and the content hash of what was read must still be the scan's;
 *   2. qwark.sum is written beside them, as a client's upload writes it;
 *   3. the console's copy, if any, is renamed to usbtmp/old, the new folder
 *      renamed into its place (the old one back if that fails), and the old
 *      one deleted.
 *
 * A pulled drive is a failed open or read in step 1, and the library is never
 * touched. Never a mod that is loaded: its files are what its unload and the
 * presence look go by. `clear_auto` drops an auto-load key the folder name
 * still has from a copy deleted earlier, through the config API, so in the
 * active profile: a mod taken by a load does not come back auto-loading
 * because an old one once did.
 */
static int take(int ui, int clear_auto)
{
	const struct modusb_entry *u = &g_usb[ui];
	const char *title = mods_title();
	char src[MODUSB_PATH_MAX];
	char to[MODUSB_PATH_MAX];
	char sum[12];
	u32 hash = 0;
	u8 problem = MODUSB_PROBLEM_NONE;
	int idx = mods_find(u->dirname);
	int had;

	if (idx >= 0 && (mods_at((u32)idx)->flags & MOD_FLAG_LOADED)) return ST_BUSY;
	if (u->problem != MODUSB_PROBLEM_NONE) return ST_IO_ERROR;

	plat_dir_create(MODUSB_TMP);
	tree_delete(MODUSB_TMP_OLD);
	tree_delete(MODUSB_TMP_NEW);

	/* 1. The copy into the temporary folder. */
	if (!usb_title_path(to, sizeof(to), u->port, u->root, title) ||
	    !path_join(src, sizeof(src), to, u->dirname) ||
	    plat_dir_create(MODUSB_TMP_NEW) != 0 ||
	    !read_folder(src, NULL, &hash, &problem, MODUSB_TMP_NEW) ||
	    problem != MODUSB_PROBLEM_NONE || hash != u->hash)
		goto fail;

	/* 2. qwark.sum, eight lowercase hex digits, as the client writes it. */
	qfmt_hex(sum, sizeof(sum), hash, 8);
	path_join(to, sizeof(to), MODUSB_TMP_NEW, "qwark.sum");
	if (write_file(to, (const u8 *)sum, 8) != ST_OK) goto fail;

	/* 3. The swap. */
	if (!path_join(src, sizeof(src), QWARK_MODSDIR, title) ||
	    !path_join(to, sizeof(to), src, u->dirname))
		goto fail;
	plat_dir_create(src);

	had = plat_path_exists(to, NULL, NULL);
	if (had && plat_file_rename(to, MODUSB_TMP_OLD) != 0) goto fail;
	if (plat_file_rename(MODUSB_TMP_NEW, to) != 0) {
		if (had) plat_file_rename(MODUSB_TMP_OLD, to);
		goto fail;
	}
	tree_delete(MODUSB_TMP_OLD);

	if (clear_auto && config_mod_auto(title, u->dirname)) config_set_mod_auto(title, u->dirname, 0);
	plat_log("mods: took %s %s from usb00%d", u->dirname, u->version, (int)u->port);
	return ST_OK;

fail:
	tree_delete(MODUSB_TMP_NEW);
	/* The drive may have gone or changed: the next list or load looks again. */
	g_scanned = 0;
	plat_log("mods: %s not taken from usb00%d (%d), library unchanged", u->dirname, (int)u->port, (int)problem);
	return ST_IO_ERROR;
}

/* --------------------------------------------------------- the load path */

/*
 * One pass over the mod at console index `idx` and everything it depends on:
 * a newer USB copy of each that is not loaded is taken, and a dependency only
 * USB has is taken; `tried` keeps each USB entry to one go per load, so a copy
 * that fails is not tried again in the next round. 1 when something was taken.
 */
static int prepare_closure(int idx, u32 *tried)
{
	u32 seen = 1u << (u32)idx;
	int sp = 0;
	int changed = 0;

	g_walk[sp++] = (u8)idx;

	while (sp > 0) {
		const struct mod_entry *m = mods_at(g_walk[--sp]);
		char name[33];
		u16 pos = 0;
		int u;

		if (m == NULL) continue;

		u = modusb_find(m->dirname);
		if (u >= 0 && (*tried & (1u << (u32)u)) == 0) {
			u8 flags = 0;
			u8 state = modusb_state(&g_usb[u], &flags);

			*tried |= 1u << (u32)u;
			if (state == MODUSB_NEWER && (flags & MODUSB_FLAG_WAITING) == 0) {
				if (take(u, 0) == ST_OK) changed = 1;
			} else if (state == MODUSB_NEWER || state == MODUSB_OLDER || state == MODUSB_CONFLICT) {
				/* The console's copy stays: loaded (the update waits), newer, or other contents. */
				plat_log("mods: %s keeps %s, USB has %s (%d)", m->dirname, m->version, g_usb[u].version, (int)state);
			}
		}

		while (mods_depends_next(m->depends, &pos, name, sizeof(name))) {
			int d = mods_find_by_name(name);

			if (d >= 0) {
				if ((seen & (1u << (u32)d)) == 0 && sp < (int)sizeof(g_walk)) {
					seen |= 1u << (u32)d;
					g_walk[sp++] = (u8)d;
				}
				continue;
			}

			u = modusb_find_by_name(name);
			if (u < 0 || (*tried & (1u << (u32)u)) != 0) continue;
			*tried |= 1u << (u32)u;
			if (mods_find(g_usb[u].dirname) >= 0) continue;   /* the console has the folder under another name */
			if (take(u, 1) == ST_OK) changed = 1;
		}
	}

	return changed;
}

int modusb_prepare(const char *dirname)
{
	u32 tried = 0;
	u32 round;

	if (!modusb_enabled() || mods_title()[0] == 0 || !mem_is_ingame()) return ST_OK;

	modusb_scan(0);
	if (g_nusb == 0) return ST_OK;

	for (round = 0; round <= QWARK_MAX_MODS; round++) {
		int idx = mods_find(dirname);
		int changed;

		if (idx < 0) {
			int u = modusb_find(dirname);
			int rc;

			if (u < 0 || (tried & (1u << (u32)u)) != 0) return ST_OK;
			tried |= 1u << (u32)u;
			rc = take(u, 1);
			if (rc != ST_OK) return rc;
			changed = 1;
		} else {
			changed = prepare_closure(idx, &tried);
		}

		if (!changed) break;
		mods_rescan();
	}
	return ST_OK;
}

int modusb_install(const char *dirname)
{
	int u;
	int rc;

	if (!modusb_enabled()) return ST_NOT_FOUND;
	modusb_scan(0);
	u = modusb_find(dirname);
	if (u < 0) return ST_NOT_FOUND;
	if (g_usb[u].problem != MODUSB_PROBLEM_NONE) return ST_IO_ERROR;

	rc = take(u, 0);
	if (rc == ST_OK) mods_rescan();
	return rc;
}
