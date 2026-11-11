/*
 * The installer's work: see install.h for the order of things and why.
 * Static buffers only, so a run asks the console for no memory.
 */
#include "install.h"
#include "ifs.h"
#include "qrle.h"

#include <stdio.h>
#include <string.h>

#define CHUNK 65536u

static unsigned char g_chunk[CHUNK];
static char g_list[INST_LIST_MAX + 1];
static char g_new[BOOTLIST_OUT_MAX(INST_LIST_MAX)];

/* The staged names and the moved-aside names, beside each final one. */
#define TMP(p) p ".tmp"
#define OLD(p) p ".old"

/* ------------------------------------------------------------------ CRC */

static unsigned int crc32_add(unsigned int crc, const unsigned char *p, uint32_t len)
{
	uint32_t i;
	int k;

	crc = ~crc;
	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* ------------------------------------------------------------------ helpers */

/* Only for what a wrong answer cannot hurt: the USB lists, which only add a line to the message. */
static int exists(const char *path)
{
	return ifs_stat(path, NULL, NULL) == 0;
}

/* Whether path is there: 0 and *there, or the error when the console cannot say. */
static int probe(const char *path, int *there)
{
	int e = ifs_stat(path, NULL, NULL);

	*there = e == 0;
	return (e == 0 || e == IFS_ENOENT) ? 0 : e;
}

static int is_dir(const char *path)
{
	int dir = 0;
	return ifs_stat(path, &dir, NULL) == 0 && dir;
}

static int fail(inst_result *r, int step, int err, const char *why)
{
	r->ok = 0;
	r->step = step;
	r->err = err;
	r->why = why;
	return -1;
}

/* A whole small file into buf. -2: larger than cap. */
static int read_whole(const char *path, char *buf, uint32_t cap, uint32_t *len, int *err)
{
	ifs_fd fd;
	uint32_t got;
	char extra;
	int e;

	*len = 0;
	if ((e = ifs_open_read(path, &fd)) != 0) { *err = e; return -1; }
	for (;;) {
		uint32_t room = cap - *len;

		/* Full: one byte more tells a file that is too large from one that just fits. */
		if (room == 0) {
			e = ifs_read(fd, &extra, 1, &got);
			ifs_close(fd);
			if (e != 0) { *err = e; return -1; }
			return got ? -2 : 0;
		}
		if ((e = ifs_read(fd, buf + *len, room, &got)) != 0) { ifs_close(fd); *err = e; return -1; }
		if (got == 0) break;
		*len += got;
	}
	ifs_close(fd);
	return 0;
}

/* Reads a file back and answers its length and CRC. */
static int file_crc(const char *path, uint32_t *len, unsigned int *crc)
{
	ifs_fd fd;
	uint32_t got;
	int e;

	*len = 0;
	*crc = 0;
	if ((e = ifs_open_read(path, &fd)) != 0) return e;
	for (;;) {
		if ((e = ifs_read(fd, g_chunk, CHUNK, &got)) != 0) { ifs_close(fd); return e; }
		if (got == 0) break;
		*crc = crc32_add(*crc, g_chunk, got);
		*len += got;
		if (*len > INST_FILE_MAX) { ifs_close(fd); return -1; }
	}
	return ifs_close(fd);
}

/*
 * src to dst, then dst read back and held against what was read from src.
 * The checks tell the user which file, so they only answer an error code.
 */
static int copy_verified(const char *src, const char *dst, const char **why)
{
	ifs_fd in, out;
	uint32_t got, total = 0, back_len;
	unsigned int crc = 0, back_crc;
	int e;

	*why = NULL;
	if ((e = ifs_open_read(src, &in)) != 0) return e;
	if ((e = ifs_open_write(dst, &out)) != 0) { ifs_close(in); return e; }

	for (;;) {
		if ((e = ifs_read(in, g_chunk, CHUNK, &got)) != 0) break;
		if (got == 0) break;
		crc = crc32_add(crc, g_chunk, got);
		total += got;
		if ((e = ifs_write(out, g_chunk, got)) != 0) break;
	}
	ifs_close(in);
	if (e == 0) e = ifs_sync(out);
	if (ifs_close(out) != 0 && e == 0) e = -1;
	if (e != 0) return e;

	if ((e = file_crc(dst, &back_len, &back_crc)) != 0) return e;
	if (back_len != total || back_crc != crc) {
		*why = "the copy did not read back the same";
		return -1;
	}
	return 0;
}

/* data to path, read back the same way. */
static int write_verified(const char *path, const char *data, uint32_t len, const char **why)
{
	ifs_fd out;
	uint32_t back_len;
	unsigned int back_crc;
	int e;

	*why = NULL;
	if ((e = ifs_open_write(path, &out)) != 0) return e;
	e = len ? ifs_write(out, data, len) : 0;
	if (e == 0) e = ifs_sync(out);
	if (ifs_close(out) != 0 && e == 0) e = -1;
	if (e != 0) return e;

	if ((e = file_crc(path, &back_len, &back_crc)) != 0) return e;
	if (back_len != len || back_crc != crc32_add(0, (const unsigned char *)data, len)) {
		*why = "the file did not read back the same";
		return -1;
	}
	return 0;
}

/* The first n bytes of a file, which must have at least n; its size too. -1: missing or shorter. */
static int read_head(const char *path, unsigned char *head, uint32_t n, uint64_t *size)
{
	ifs_fd fd;
	uint32_t got;
	int dir = 0, e;

	if (ifs_stat(path, &dir, size) != 0 || dir) return -1;
	if ((e = ifs_open_read(path, &fd)) != 0) return e;
	e = ifs_read(fd, head, n, &got);
	ifs_close(fd);
	if (e != 0) return e;
	return got == n ? 0 : -1;
}

/* ------------------------------------------------------------------ the package */

static int check_package(const char *usrdir, char *sprx, char *pak, size_t cap, inst_result *r)
{
	unsigned char head[32];
	uint64_t size = 0;
	unsigned int build;
	int e;

	snprintf(sprx, cap, "%s/qwark.sprx", usrdir);
	snprintf(pak, cap, "%s/ui.pak", usrdir);

	/* A signed module starts "SCE\0"; anything else would not load at boot. */
	if ((e = read_head(sprx, head, 4, &size)) != 0)
		return fail(r, INST_STEP_PACKAGE_SPRX, e, e == -1 ? "it is missing or empty" : NULL);
	if (memcmp(head, "SCE\0", 4) != 0 || size > INST_FILE_MAX)
		return fail(r, INST_STEP_PACKAGE_SPRX, 0, "it is not a signed module");

	/* The pak's header: "QPAK", and at 8 the build it was made for, big-endian (src/core/uipak.h). */
	if ((e = read_head(pak, head, 32, &size)) != 0)
		return fail(r, INST_STEP_PACKAGE_PAK, e, e == -1 ? "it is missing or cut short" : NULL);
	if (memcmp(head, "QPAK", 4) != 0 || size > INST_FILE_MAX)
		return fail(r, INST_STEP_PACKAGE_PAK, 0, "it is not a ui.pak");
	build = ((unsigned int)head[8] << 24) | ((unsigned int)head[9] << 16) | ((unsigned int)head[10] << 8) | head[11];
	if (build != (unsigned int)INST_QWARK_BUILD)
		return fail(r, INST_STEP_PACKAGE_PAK, 0, "it is another build's");
	return 0;
}

/*
 * Revision 1.26: the game modules the package carries, any of the five: each
 * a signed module. *n says how many, and src[i] is "" for one it does not carry.
 */
/* Each game module's file name, by game id less one. */
static const char *const g_game_names[INST_GAMES] = {
	INST_GAME_NAME_1, INST_GAME_NAME_2, INST_GAME_NAME_3, INST_GAME_NAME_4, INST_GAME_NAME_5
};

static int check_games(const char *usrdir, char src[INST_GAMES][512], int *n, inst_result *r)
{
	unsigned char head[4];
	uint64_t size = 0;
	int g, there, e;

	*n = 0;
	for (g = 0; g < INST_GAMES; g++) {
		snprintf(src[g], 512, "%s/%s", usrdir, g_game_names[g]);
		if ((e = probe(src[g], &there)) != 0) {
			r->game = g + 1;
			return fail(r, INST_STEP_PACKAGE_GAME, e, NULL);
		}
		if (!there) { src[g][0] = '\0'; continue; }
		if ((e = read_head(src[g], head, 4, &size)) != 0 || memcmp(head, "SCE\0", 4) != 0 ||
		    size > INST_FILE_MAX) {
			r->game = g + 1;
			return fail(r, INST_STEP_PACKAGE_GAME, e > 0 ? e : 0, "it is not a signed module");
		}
		(*n)++;
	}
	return 0;
}

/* ------------------------------------------------------------------ the moves */

/*
 * final <- tmp. What was at final goes to old first, and comes back if tmp
 * cannot follow it; *lost says that it could not. *had says whether there was
 * anything to move aside. A rename never replaces a file here, so old is
 * cleared first, but only when final is there to take its place: with no
 * final, a .old is an interrupted run's list, the only copy of it on the HDD
 * until this run has put its own in place, and inst_run deletes it then.
 */
static int move_in(const char *final, const char *tmp, const char *old, int *had, int *lost)
{
	int e;

	*lost = 0;
	if ((e = probe(final, had)) != 0) return e;
	if (*had) {
		ifs_unlink(old);
		if ((e = ifs_rename(final, old)) != 0) return e;
	}
	if ((e = ifs_rename(tmp, final)) != 0) {
		if (*had && ifs_rename(old, final) != 0) *lost = 1;
		return e;
	}
	return 0;
}

/* Undoes a move_in that succeeded. */
static int move_out(const char *final, const char *old, int had)
{
	if (had) {
		ifs_unlink(final);
		return ifs_rename(old, final);
	}
	return ifs_unlink(final);
}

typedef struct move {
	const char *final, *tmp, *old;
	int step;
	int staged;     /* there is a .tmp to move */
	int done;       /* moved */
	int had;        /* something was moved aside */
} move;

/* The staged files go; a second try, so one refused call leaves nothing behind. */
static void drop_tmps(move *m, int n)
{
	int i, there;

	for (i = 0; i < n; i++) {
		if (ifs_unlink(m[i].tmp) != 0 && (probe(m[i].tmp, &there) != 0 || there)) ifs_unlink(m[i].tmp);
	}
}

/* ------------------------------------------------------------------ run */

/* The moves' order, and what is in each. */
#define M_BACKUP 0
#define M_GAME   1                       /* .. M_GAME + INST_GAMES - 1 */
#define M_PAK    (M_GAME + INST_GAMES)
#define M_SPRX   (M_PAK + 1)
#define M_LIST   (M_SPRX + 1)
#define M_COUNT  (M_LIST + 1)

static char g_src_game[INST_GAMES][512];

static void install_library(const char *usrdir, inst_result *r);

void inst_run(const char *usrdir, inst_result *r)
{
	char src_sprx[512], src_pak[512];
	int ngames = 0;
	const char *why = NULL;
	uint32_t old_len = 0;
	bootlist_plan plan;
	const char *new_text;
	size_t new_len;
	int e = 0, rc, i, list_changes, there, lost;

	/*
	 * In the order they are moved: the list never names a module that is not in
	 * place, and the game modules (revision 1.26) are in before the module that
	 * loads them.
	 */
	move m[M_COUNT] = {
		{ INST_LIST_BAK, TMP(INST_LIST_BAK), OLD(INST_LIST_BAK), INST_STEP_PLACE_BACKUP, 0, 0, 0 },
		{ INST_GAME_1,   TMP(INST_GAME_1),   OLD(INST_GAME_1),   INST_STEP_PLACE_GAME,   0, 0, 0 },
		{ INST_GAME_2,   TMP(INST_GAME_2),   OLD(INST_GAME_2),   INST_STEP_PLACE_GAME,   0, 0, 0 },
		{ INST_GAME_3,   TMP(INST_GAME_3),   OLD(INST_GAME_3),   INST_STEP_PLACE_GAME,   0, 0, 0 },
		{ INST_GAME_4,   TMP(INST_GAME_4),   OLD(INST_GAME_4),   INST_STEP_PLACE_GAME,   0, 0, 0 },
		{ INST_GAME_5,   TMP(INST_GAME_5),   OLD(INST_GAME_5),   INST_STEP_PLACE_GAME,   0, 0, 0 },
		{ INST_PAK,      TMP(INST_PAK),      OLD(INST_PAK),      INST_STEP_PLACE_PAK,    0, 0, 0 },
		{ INST_SPRX,     TMP(INST_SPRX),     OLD(INST_SPRX),     INST_STEP_PLACE_SPRX,   0, 0, 0 },
		{ INST_LIST,     TMP(INST_LIST),     OLD(INST_LIST),     INST_STEP_PLACE_LIST,   0, 0, 0 },
	};

	memset(r, 0, sizeof(*r));
	r->usb_list = exists(INST_USB0_LIST) || exists(INST_USB1_LIST);

	/* 1. The package, and the list as it is. */
	if (check_package(usrdir, src_sprx, src_pak, sizeof(src_sprx), r) != 0) return;
	if (check_games(usrdir, g_src_game, &ngames, r) != 0) return;

	if ((e = probe(INST_LIST, &there)) != 0) {
		fail(r, INST_STEP_READ_LIST, e, NULL);
		return;
	}
	if (there) {
		r->list_existed = 1;
		rc = read_whole(INST_LIST, g_list, INST_LIST_MAX, &old_len, &e);
	} else {
		/* An earlier run may have stopped between moving the list aside and moving the new one in. */
		if ((e = probe(OLD(INST_LIST), &there)) != 0) {
			fail(r, INST_STEP_READ_LIST, e, NULL);
			return;
		}
		r->list_existed = there;
		r->recovered = there;
		rc = there ? read_whole(OLD(INST_LIST), g_list, INST_LIST_MAX, &old_len, &e) : 0;
	}
	if (rc == -2) {
		fail(r, INST_STEP_READ_LIST, 0, "it is over 64 KB, so it is not a plugin list");
		return;
	}
	if (rc != 0) {
		fail(r, INST_STEP_READ_LIST, e, NULL);
		return;
	}

	bootlist_plan_edit(g_list, old_len, g_new, sizeof(g_new), &plan);
	r->outcome = plan.outcome;
	r->lines = plan.lines;
	if (plan.outcome == BOOTLIST_ALREADY_FIRST) {
		/* Left as it is, unless an interrupted run took it away: then it goes back as it was. */
		new_text = g_list;
		new_len = old_len;
		list_changes = r->recovered;
	} else {
		new_text = g_new;
		new_len = plan.len;
		list_changes = 1;
	}

	/* 2. Everything new, beside its final name. */
	ifs_mkdir(INST_PLUGINS_DIR);
	ifs_mkdir(INST_QWARK_DIR);
	if (!is_dir(INST_PLUGINS_DIR) || !is_dir(INST_QWARK_DIR)) {
		fail(r, INST_STEP_FOLDERS, 0, "a folder could not be made");
		return;
	}

	drop_tmps(m, M_COUNT);

	for (i = 0; i < INST_GAMES; i++) {
		if (g_src_game[i][0] == '\0') continue;
		if ((e = copy_verified(g_src_game[i], m[M_GAME + i].tmp, &why)) != 0) {
			r->game = i + 1;
			fail(r, INST_STEP_COPY_GAME, why ? 0 : e, why);
			drop_tmps(m, M_COUNT);
			return;
		}
		m[M_GAME + i].staged = 1;
	}

	if ((e = copy_verified(src_sprx, m[M_SPRX].tmp, &why)) != 0) {
		fail(r, INST_STEP_COPY_SPRX, why ? 0 : e, why);
		drop_tmps(m, M_COUNT);
		return;
	}
	m[M_SPRX].staged = 1;

	if ((e = copy_verified(src_pak, m[M_PAK].tmp, &why)) != 0) {
		fail(r, INST_STEP_COPY_PAK, why ? 0 : e, why);
		drop_tmps(m, M_COUNT);
		return;
	}
	m[M_PAK].staged = 1;

	if (list_changes) {
		if (r->list_existed) {
			if ((e = write_verified(m[M_BACKUP].tmp, g_list, old_len, &why)) != 0) {
				fail(r, INST_STEP_WRITE_BACKUP, why ? 0 : e, why);
				drop_tmps(m, M_COUNT);
				return;
			}
			m[M_BACKUP].staged = 1;
		}
		if ((e = write_verified(m[M_LIST].tmp, new_text, (uint32_t)new_len, &why)) != 0) {
			fail(r, INST_STEP_WRITE_LIST, why ? 0 : e, why);
			drop_tmps(m, M_COUNT);
			return;
		}
		m[M_LIST].staged = 1;
	}

	/* 3. The moves, and on a failure every one of them undone, newest first. */
	for (i = 0; i < M_COUNT; i++) {
		if (!m[i].staged) continue;
		if ((e = move_in(m[i].final, m[i].tmp, m[i].old, &m[i].had, &lost)) != 0) {
			if (i >= M_GAME && i < M_GAME + INST_GAMES) r->game = i - M_GAME + 1;
			fail(r, m[i].step, e, NULL);
			r->undo_failed = lost;
			while (--i >= 0) {
				if (m[i].done && move_out(m[i].final, m[i].old, m[i].had) != 0) r->undo_failed = 1;
			}
			drop_tmps(m, M_COUNT);
			return;
		}
		m[i].done = 1;
	}

	/*
	 * Done. The list, the pak and the backup that were replaced go; the module
	 * that was replaced stays as qwark.sprx.old, as after the PC client's
	 * update. A boot_plugins.txt.old an interrupted run left goes too, now that
	 * the list is in place again.
	 */
	ifs_unlink(OLD(INST_LIST));
	ifs_unlink(OLD(INST_PAK));
	ifs_unlink(OLD(INST_LIST_BAK));
	for (i = 0; i < INST_GAMES; i++) {
		if (m[M_GAME + i].done) ifs_unlink(m[M_GAME + i].old);
	}

	r->ok = 1;
	r->games = ngames;
	r->backed_up = m[M_BACKUP].done;

	/* 4. The library, now that qwark is in: whatever happens to it, qwark stays installed. */
	install_library(usrdir, r);
}

/* ------------------------------------------------------------------ the library */

/*
 * The mods and premade savefiles the package carries (install.h, INST_LIBRARY_NAME):
 * after qwark is in, only what is missing, each one whole or not at all.
 */
static unsigned char g_lib[INST_LIB_MAX];
static uint32_t g_lib_len, g_lib_count, g_lib_data;
static void (*g_progress)(void);

void inst_set_progress(void (*fn)(void))
{
	g_progress = fn;
}

static void progress(void)
{
	if (g_progress) g_progress();
}

static uint32_t be32_at(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct lib_entry {
	int kind, code, group;
	uint32_t offset, len, size, crc;
	const char *path;       /* under /dev_hdd0/qwark; NUL-terminated inside the table once it is checked */
} lib_entry;

static void lib_get(uint32_t i, lib_entry *e)
{
	const unsigned char *t = g_lib + INST_LIB_HEAD + i * INST_LIB_ENTRY;

	e->kind = t[0];
	e->code = t[1];
	e->group = ((int)t[2] << 8) | t[3];
	e->offset = be32_at(t + 4);
	e->len = be32_at(t + 8);
	e->size = be32_at(t + 12);
	e->crc = be32_at(t + 16);
	e->path = (const char *)t + 32;
}

/*
 * A path the library may write: printable ASCII, '/' between names, no empty
 * name and none starting with a dot (so no ".." either), no '\' or ':', each
 * name max bytes at most. Answers how many names it has, or 0.
 */
static int path_names(const char *p, uint32_t max)
{
	int names = 0;
	uint32_t len = 0;

	for (;; p++) {
		if (*p == '/' || *p == '\0') {
			if (len == 0) return 0;
			names++;
			len = 0;
			if (*p == '\0') return names;
			continue;
		}
		if (len == 0 && *p == '.') return 0;
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E || *p == '\\' || *p == ':') return 0;
		if (++len > max) return 0;
	}
}

/* The length of a path's first k names, without the '/' after them. */
static uint32_t names_len(const char *p, int k)
{
	uint32_t i;

	for (i = 0; p[i] != '\0'; i++) {
		if (p[i] == '/' && --k == 0) return i;
	}
	return i;
}

static int ends_with(const char *s, const char *tail)
{
	size_t n = strlen(s), t = strlen(tail);
	return n > t && strcmp(s + n - t, tail) == 0;
}

/*
 * The header and the table: every entry's data inside the data, every path
 * one the library may write, a mod's files together and in one folder.
 * Nothing at all is written for a library that fails this.
 */
static int lib_check_table(void)
{
	uint32_t i, table, data_len, root = 0;
	lib_entry e;
	const char *mod_root = NULL;
	int last_group = -1;

	if (g_lib_len < INST_LIB_HEAD || memcmp(g_lib, "QLIB", 4) != 0 || g_lib[4] != 0 || g_lib[5] != 1)
		return -1;
	g_lib_count = ((uint32_t)g_lib[6] << 8) | g_lib[7];
	table = g_lib_count * INST_LIB_ENTRY;
	data_len = be32_at(g_lib + 12);
	if (g_lib_len - INST_LIB_HEAD < table || g_lib_len - INST_LIB_HEAD - table != data_len) return -1;
	if (crc32_add(0, g_lib + INST_LIB_HEAD, table) != be32_at(g_lib + 8)) return -1;
	g_lib_data = INST_LIB_HEAD + table;

	for (i = 0; i < g_lib_count; i++) {
		int names;

		lib_get(i, &e);
		if (memchr(e.path, '\0', INST_LIB_PATH_MAX) == NULL) return -1;
		if (e.offset > data_len || e.len > data_len - e.offset || e.size > INST_FILE_MAX) return -1;
		if (e.code != INST_LIB_STORED && e.code != INST_LIB_QRLE) return -1;
		if (e.code == INST_LIB_STORED && e.len != e.size) return -1;
		if (e.kind == INST_LIB_SAVE) {
			names = path_names(e.path, INST_SAVE_NAME_MAX);
			if (names != 4 || strncmp(e.path, "savefiles/", 10) != 0 || !ends_with(e.path, ".sav") || e.group != 0)
				return -1;
		} else if (e.kind == INST_LIB_MOD_FILE) {
			names = path_names(e.path, INST_MOD_NAME_MAX);
			if (names < 4 || strncmp(e.path, "mods/", 5) != 0) return -1;
			if (e.group != last_group) {
				/* A new mod: its number follows the last one's, so no mod comes in two parts. */
				if (e.group != last_group + 1) return -1;
				last_group = e.group;
				mod_root = e.path;
				root = names_len(e.path, 3);
			} else if (names_len(e.path, 3) != root || strncmp(e.path, mod_root, root) != 0) {
				return -1;
			}
		} else {
			return -1;
		}
	}
	return 0;
}

/* Where the expanded bytes go: summed always, and written too when write is set. */
typedef struct sink_ctx {
	unsigned int crc;
	uint32_t total;
	int write;
	ifs_fd fd;
} sink_ctx;

static int to_sink(void *ctx, const unsigned char *p, uint32_t n)
{
	sink_ctx *s = (sink_ctx *)ctx;

	s->crc = crc32_add(s->crc, p, n);
	s->total += n;
	return s->write ? ifs_write(s->fd, p, n) : 0;
}

/* Expands an entry into s: 0, -1 for a bad stream, or the sink's error. */
static int lib_expand(const lib_entry *e, sink_ctx *s)
{
	const unsigned char *in = g_lib + g_lib_data + e->offset;
	int serr = 0, rc;

	if (e->code == INST_LIB_STORED) {
		uint32_t done = 0;

		while (done < e->len) {
			uint32_t k = e->len - done > CHUNK ? CHUNK : e->len - done;
			if ((rc = to_sink(s, in + done, k)) != 0) return rc;
			done += k;
		}
		return 0;
	}
	rc = qrle_expand(in, e->len, e->size, g_chunk, CHUNK, to_sink, s, &serr);
	if (rc == QRLE_SINK) return serr ? serr : -1;
	return rc == QRLE_OK ? 0 : -1;
}

/* Whether an entry expands to exactly the bytes its CRC says: the check before anything is written. */
static int lib_entry_ok(const lib_entry *e)
{
	sink_ctx s;

	memset(&s, 0, sizeof(s));
	return lib_expand(e, &s) == 0 && s.total == e->size && s.crc == e->crc;
}

/* An entry's bytes to path, read back and held against its CRC. */
static int lib_write(const lib_entry *e, const char *path, const char **why)
{
	sink_ctx s;
	uint32_t back_len;
	unsigned int back_crc;
	int err;

	*why = NULL;
	memset(&s, 0, sizeof(s));
	s.write = 1;
	if ((err = ifs_open_write(path, &s.fd)) != 0) return err;
	err = lib_expand(e, &s);
	if (err == 0) err = ifs_sync(s.fd);
	if (ifs_close(s.fd) != 0 && err == 0) err = -1;
	if (err != 0) return err;

	if ((err = file_crc(path, &back_len, &back_crc)) != 0) return err;
	if (back_len != e->size || back_crc != e->crc) {
		*why = "the file did not read back the same";
		return -1;
	}
	return 0;
}

/* Every failure is counted; the first is the one the message names. */
static void lib_fail(inst_result *r, int is_mod, int step, int err, const char *why)
{
	if (is_mod) r->mods_failed++;
	else r->saves_failed++;
	if (r->lib_step == INST_LIB_NONE) {
		r->lib_step = step;
		r->lib_err = why ? 0 : err;
		r->lib_why = why;
	}
}

/*
 * The folders a file path needs, below its first three ("", /dev_hdd0 and
 * /dev_hdd0/qwark): each made if it is not there, and each a folder after.
 */
static int make_parents(char *path)
{
	char *s;
	int slashes = 0;

	for (s = path; *s; s++) {
		if (*s != '/' || ++slashes <= 3) continue;
		*s = '\0';
		if (!is_dir(path)) ifs_mkdir(path);
		if (!is_dir(path)) { *s = '/'; return -1; }
		*s = '/';
	}
	return 0;
}

/*
 * Takes a mod's staging folder apart: the files the library puts there, their
 * folders deepest first, then the stage itself. A stage that is not there
 * costs a few refused calls.
 */
static void unstage(const char *stage, uint32_t first, uint32_t end, uint32_t root)
{
	char path[512];
	size_t stage_len = strlen(stage);
	uint32_t i;
	lib_entry e;

	for (i = first; i < end; i++) {
		char *slash;

		lib_get(i, &e);
		snprintf(path, sizeof(path), "%s/%s", stage, e.path + root + 1);
		ifs_unlink(path);
		while ((slash = strrchr(path, '/')) != NULL && (size_t)(slash - path) > stage_len) {
			*slash = '\0';
			ifs_rmdir(path);
		}
	}
	ifs_rmdir(stage);
}

/* One mod, entries first to end - 1: all of them into a stage folder, then the folder into place. */
static void lib_mod(uint32_t first, uint32_t end, inst_result *r)
{
	char folder[512], stage[512], path[512];
	const char *why = NULL;
	uint32_t i, root, title;
	lib_entry e;
	int there, err;

	lib_get(first, &e);
	root = names_len(e.path, 3);      /* "mods/<TITLEID>/<dir>" */
	title = names_len(e.path, 2);     /* "mods/<TITLEID>" */
	snprintf(folder, sizeof(folder), "%s/%.*s", INST_QWARK_DIR, (int)root, e.path);
	snprintf(stage, sizeof(stage), "%s/%.*s", INST_LIB_STAGE, (int)(root - title - 1), e.path + title + 1);

	/* Already there, whoever put it there and whatever is in it: left exactly as it is. */
	if ((err = probe(folder, &there)) != 0) { lib_fail(r, 1, INST_LIB_LOOK, err, NULL); return; }
	if (there) { r->mods_skipped++; return; }

	for (i = first; i < end; i++) {
		lib_get(i, &e);
		if (!lib_entry_ok(&e)) { lib_fail(r, 1, INST_LIB_CHECK, 0, "it is damaged in this package"); return; }
	}

	/* /dev_hdd0/qwark/mods/<TITLEID>, the mod's place. */
	if (make_parents(folder) != 0) { lib_fail(r, 1, INST_LIB_FOLDER, 0, "a folder could not be made"); return; }

	/* An interrupted run's stage of this mod goes first. */
	unstage(stage, first, end, root);
	for (i = first; i < end; i++) {
		lib_get(i, &e);
		snprintf(path, sizeof(path), "%s/%s", stage, e.path + root + 1);
		if (make_parents(path) != 0) {
			unstage(stage, first, end, root);
			lib_fail(r, 1, INST_LIB_FOLDER, 0, "a folder could not be made");
			return;
		}
		if ((err = lib_write(&e, path, &why)) != 0) {
			unstage(stage, first, end, root);
			lib_fail(r, 1, INST_LIB_WRITE, err, why);
			return;
		}
		progress();
	}
	if ((err = ifs_rename(stage, folder)) != 0) {
		unstage(stage, first, end, root);
		lib_fail(r, 1, INST_LIB_PLACE, err, NULL);
		return;
	}
	r->mods++;
}

/* One savefile, and the .sum beside it the client writes (eight lowercase hex digits). */
static void lib_save(const lib_entry *e, inst_result *r)
{
	char final[512], tmp[520], sum[520], text[16];
	const char *why = NULL;
	int there, err;

	snprintf(final, sizeof(final), "%s/%s", INST_QWARK_DIR, e->path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", final);
	snprintf(sum, sizeof(sum), "%s.sum", final);

	/* Already there, whoever put it there and whatever is in it: left exactly as it is. */
	if ((err = probe(final, &there)) != 0) { lib_fail(r, 0, INST_LIB_LOOK, err, NULL); return; }
	if (there) { r->saves_skipped++; return; }
	if (!lib_entry_ok(e)) { lib_fail(r, 0, INST_LIB_CHECK, 0, "it is damaged in this package"); return; }

	/* /dev_hdd0/qwark/savefiles/<TITLEID>/<category>. */
	if (make_parents(final) != 0) { lib_fail(r, 0, INST_LIB_FOLDER, 0, "a folder could not be made"); return; }

	ifs_unlink(tmp);
	if ((err = lib_write(e, tmp, &why)) != 0) {
		ifs_unlink(tmp);
		lib_fail(r, 0, INST_LIB_WRITE, err, why);
		return;
	}
	/* A sum left behind by a file that is gone would give this one the wrong CRC. */
	ifs_unlink(sum);
	if ((err = ifs_rename(tmp, final)) != 0) {
		ifs_unlink(tmp);
		lib_fail(r, 0, INST_LIB_PLACE, err, NULL);
		return;
	}
	/* Only bookkeeping: without it qwark sums the file the first time it lists it, and writes it then. */
	snprintf(text, sizeof(text), "%08x", e->crc);
	if (write_verified(sum, text, 8, &why) != 0) ifs_unlink(sum);
	r->saves++;
	progress();
}

int inst_has_library(const char *usrdir)
{
	char path[512];
	int there = 0;

	snprintf(path, sizeof(path), "%s/%s", usrdir, INST_LIBRARY_NAME);
	return probe(path, &there) == 0 && there;
}

static void install_library(const char *usrdir, inst_result *r)
{
	char path[512];
	uint32_t i, end;
	lib_entry e, next;
	int there, err = 0, rc;

	snprintf(path, sizeof(path), "%s/%s", usrdir, INST_LIBRARY_NAME);
	if ((err = probe(path, &there)) != 0) {
		/* The console cannot say: a library is taken to be there, and could not be read. */
		r->library = 1;
		r->lib_step = INST_LIB_READ;
		r->lib_err = err;
		return;
	}
	if (!there) return;
	r->library = 1;

	rc = read_whole(path, (char *)g_lib, INST_LIB_MAX, &g_lib_len, &err);
	if (rc == -2) {
		r->lib_step = INST_LIB_DAMAGED;
		r->lib_why = "it is too large to be one";
		return;
	}
	if (rc != 0) {
		r->lib_step = INST_LIB_READ;
		r->lib_err = err;
		return;
	}
	if (lib_check_table() != 0) {
		r->lib_step = INST_LIB_DAMAGED;
		r->lib_why = "it is damaged in this package";
		return;
	}

	for (i = 0; i < g_lib_count; i = end) {
		lib_get(i, &e);
		if (e.kind == INST_LIB_SAVE) {
			lib_save(&e, r);
			end = i + 1;
			continue;
		}
		for (end = i + 1; end < g_lib_count; end++) {
			lib_get(end, &next);
			if (next.kind != INST_LIB_MOD_FILE || next.group != e.group) break;
		}
		lib_mod(i, end, r);
	}
	ifs_rmdir(INST_LIB_STAGE);
}

/* ------------------------------------------------------------------ paths */

void inst_usrdir(const char *argv0, char *out, size_t cap)
{
	const char *slash;

	/* The console starts the app as /dev_hdd0/game/<TITLE>/USRDIR/EBOOT.BIN. */
	if (argv0 != NULL && strncmp(argv0, "/dev_", 5) == 0 && (slash = strrchr(argv0, '/')) != NULL &&
	    (size_t)(slash - argv0) < cap) {
		memcpy(out, argv0, (size_t)(slash - argv0));
		out[slash - argv0] = '\0';
		return;
	}
	snprintf(out, cap, "%s", INST_APP_USRDIR);
}

/* ------------------------------------------------------------------ messages */

static void append(char *out, size_t cap, const char *text)
{
	size_t len = strlen(out);
	if (len < cap) snprintf(out + len, cap - len, "%s", text);
}

void inst_confirm_message(int library, char *out, size_t cap)
{
	snprintf(out, cap,
	         "Install qwark build %d?\n\n"
	         "This copies qwark.sprx to " INST_PLUGINS_DIR " and ui.pak to " INST_QWARK_DIR ". "
	         "It puts qwark first in " INST_LIST ". Your other plugins stay in the list.%s",
	         INST_QWARK_BUILD,
	         library ? "\n\nIt also adds the mods and premade savefiles RaCMAN Reloaded comes with. "
	                   "Any you already have are left as they are." : "");
}

static const char *lib_step_name(int step)
{
	switch (step) {
	case INST_LIB_READ:    return "reading " INST_LIBRARY_NAME;
	case INST_LIB_DAMAGED: return INST_LIBRARY_NAME;
	case INST_LIB_CHECK:   return "checking a file";
	case INST_LIB_FOLDER:  return "making a folder";
	case INST_LIB_WRITE:   return "writing a file";
	case INST_LIB_PLACE:   return "moving a file into place";
	case INST_LIB_LOOK:    return "looking for a file";
	default:               return "unknown";
	}
}

/* "1 mod" or "2 mods". */
static void counted(char *out, size_t cap, int n, const char *one)
{
	snprintf(out, cap, "%d %s%s", n, one, n == 1 ? "" : "s");
}

/*
 * The library's lines: what it added, what it left alone, and what it could
 * not add. qwark is installed by then, so a failure here is only a warning.
 * terse leaves out what added and why one failed, for a message that would
 * not fit a dialog otherwise.
 */
static void library_lines(const inst_result *r, int terse, char *out, size_t cap)
{
	char line[200], a[40], b[40];
	int added = !terse && r->mods + r->saves > 0;

	if (added) {
		counted(a, sizeof(a), r->mods, "mod");
		counted(b, sizeof(b), r->saves, "premade savefile");
		snprintf(line, sizeof(line), "\n\nAdded %s and %s.", a, b);
		append(out, cap, line);
	}
	if (r->mods_skipped + r->saves_skipped > 0) {
		counted(a, sizeof(a), r->mods_skipped, "mod");
		counted(b, sizeof(b), r->saves_skipped, "savefile");
		snprintf(line, sizeof(line), "%sSkipped %s and %s that were already installed.", added ? " " : "\n\n", a, b);
		append(out, cap, line);
	}
	if (r->lib_step == INST_LIB_NONE) return;
	if (r->mods_failed + r->saves_failed > 0) {
		counted(a, sizeof(a), r->mods_failed, "mod");
		counted(b, sizeof(b), r->saves_failed, "savefile");
		snprintf(line, sizeof(line), "\n\nNot added: %s and %s", a, b);
	} else {
		snprintf(line, sizeof(line), "\n\nThe mods and savefiles were not added");
	}
	append(out, cap, line);
	if (terse) line[0] = '\0';
	else if (r->lib_why != NULL) snprintf(line, sizeof(line), " (%s: %s)", lib_step_name(r->lib_step), r->lib_why);
	else snprintf(line, sizeof(line), " (%s, error 0x%08x)", lib_step_name(r->lib_step), (unsigned int)r->lib_err);
	append(out, cap, line);
	append(out, cap, ".");
}

static const char *step_name(int step)
{
	switch (step) {
	case INST_STEP_PACKAGE_SPRX: return "qwark.sprx in this package";
	case INST_STEP_PACKAGE_PAK:  return "ui.pak in this package";
	case INST_STEP_READ_LIST:    return "reading boot_plugins.txt";
	case INST_STEP_FOLDERS:      return "making " INST_PLUGINS_DIR " and " INST_QWARK_DIR;
	case INST_STEP_COPY_SPRX:    return "copying qwark.sprx";
	case INST_STEP_COPY_PAK:     return "copying ui.pak";
	case INST_STEP_WRITE_BACKUP: return "saving the old boot_plugins.txt";
	case INST_STEP_WRITE_LIST:   return "writing the new boot_plugins.txt";
	case INST_STEP_PLACE_BACKUP: return "putting the backup in place";
	case INST_STEP_PLACE_PAK:    return "putting ui.pak in place";
	case INST_STEP_PLACE_SPRX:   return "putting qwark.sprx in place";
	case INST_STEP_PLACE_LIST:   return "putting boot_plugins.txt in place";
	case INST_STEP_PACKAGE_GAME: return "a game module in this package";
	case INST_STEP_COPY_GAME:    return "copying a game module";
	case INST_STEP_PLACE_GAME:   return "putting a game module in place";
	default:                     return "unknown";
	}
}

static void result_message(const inst_result *r, int hen, int terse, char *out, size_t cap)
{
	char line[160];

	if (cap == 0) return;
	out[0] = '\0';

	if (!r->ok) {
		snprintf(out, cap, "qwark was not installed.\n\nThis step failed: %s", step_name(r->step));
		if (r->game >= 1 && r->game <= INST_GAMES) {
			snprintf(line, sizeof(line), " (%s)", g_game_names[r->game - 1]);
			append(out, cap, line);
		}
		if (r->why != NULL) {
			snprintf(line, sizeof(line), ": %s.", r->why);
		} else {
			snprintf(line, sizeof(line), " (error 0x%08x).", (unsigned int)r->err);
		}
		append(out, cap, line);
		if (r->undo_failed) {
			append(out, cap, "\n\nSome files could not be put back. Check " INST_LIST
			                 " before you restart. The old list is in " INST_LIST_BAK ".");
		} else {
			append(out, cap, "\n\nNothing on your console was changed.");
		}
		return;
	}

	snprintf(out, cap, "qwark build %d is installed.\n\n", INST_QWARK_BUILD);
	if (r->games > 0 && !terse) {
		snprintf(line, sizeof(line), "Its %d game modules are in " INST_QWARK_DIR ".\n\n", r->games);
		append(out, cap, line);
	}
	append(out, cap, r->outcome == BOOTLIST_ALREADY_FIRST
	                 ? "boot_plugins.txt already started with qwark."
	                 : "qwark is now first in boot_plugins.txt.");
	if (r->backed_up && !terse) append(out, cap, " The old list is in " INST_LIST_BAK ".");

	if (r->lines > BOOTLIST_HEN_LIMIT) {
		snprintf(line, sizeof(line),
		         "\n\nThe list has %d plugins. Your console loads only the first %d.",
		         r->lines, BOOTLIST_HEN_LIMIT);
		append(out, cap, line);
	}
	if (r->usb_list) {
		append(out, cap, "\n\nYour console has a USB drive attached. The console reads a "
		                 "boot_plugins.txt on the USB drive first, so qwark will not load. "
		                 "Remove that file.");
	}
	library_lines(r, terse, out, cap);
	if (hen) append(out, cap, "\n\nAfter the restart, enable HEN. qwark loads with it.");

	append(out, cap, "\n\nqwark loads when your console starts. Restart now?");
}

/*
 * A dialog takes INST_MESSAGE_MAX bytes. With every warning at once the whole
 * message would not fit, so it is said again tersely: without the game
 * modules' line, the backup's place, what the library added and why a part of
 * it failed. The warnings and the question at the end always stay.
 */
void inst_result_message(const inst_result *r, int hen, char *out, size_t cap)
{
	size_t room = cap < INST_MESSAGE_MAX ? cap : INST_MESSAGE_MAX;

	if (cap == 0) return;
	result_message(r, hen, 0, out, cap);
	if (strlen(out) + 1 >= room) result_message(r, hen, 1, out, cap);
}

void inst_restart_failed_message(unsigned int error, char *out, size_t cap)
{
	snprintf(out, cap,
	         "Your console did not restart (error 0x%08x).\n\n"
	         "qwark is installed. Restart your console from the XMB to load it.",
	         error);
}
