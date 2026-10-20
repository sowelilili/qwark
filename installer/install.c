/*
 * The installer's work: see install.h for the order of things and why.
 * Static buffers only, so a run asks the console for no memory.
 */
#include "install.h"
#include "ifs.h"

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

void inst_run(const char *usrdir, inst_result *r)
{
	char src_sprx[512], src_pak[512];
	const char *why = NULL;
	uint32_t old_len = 0;
	bootlist_plan plan;
	const char *new_text;
	size_t new_len;
	int e = 0, rc, i, list_changes, there, lost;

	/* In the order they are moved: the list never names a module that is not in place. */
	move m[4] = {
		{ INST_LIST_BAK, TMP(INST_LIST_BAK), OLD(INST_LIST_BAK), INST_STEP_PLACE_BACKUP, 0, 0, 0 },
		{ INST_PAK,      TMP(INST_PAK),      OLD(INST_PAK),      INST_STEP_PLACE_PAK,    0, 0, 0 },
		{ INST_SPRX,     TMP(INST_SPRX),     OLD(INST_SPRX),     INST_STEP_PLACE_SPRX,   0, 0, 0 },
		{ INST_LIST,     TMP(INST_LIST),     OLD(INST_LIST),     INST_STEP_PLACE_LIST,   0, 0, 0 },
	};

	memset(r, 0, sizeof(*r));
	r->usb_list = exists(INST_USB0_LIST) || exists(INST_USB1_LIST);

	/* 1. The package, and the list as it is. */
	if (check_package(usrdir, src_sprx, src_pak, sizeof(src_sprx), r) != 0) return;

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

	drop_tmps(m, 4);

	if ((e = copy_verified(src_sprx, m[2].tmp, &why)) != 0) {
		fail(r, INST_STEP_COPY_SPRX, why ? 0 : e, why);
		drop_tmps(m, 4);
		return;
	}
	m[2].staged = 1;

	if ((e = copy_verified(src_pak, m[1].tmp, &why)) != 0) {
		fail(r, INST_STEP_COPY_PAK, why ? 0 : e, why);
		drop_tmps(m, 4);
		return;
	}
	m[1].staged = 1;

	if (list_changes) {
		if (r->list_existed) {
			if ((e = write_verified(m[0].tmp, g_list, old_len, &why)) != 0) {
				fail(r, INST_STEP_WRITE_BACKUP, why ? 0 : e, why);
				drop_tmps(m, 4);
				return;
			}
			m[0].staged = 1;
		}
		if ((e = write_verified(m[3].tmp, new_text, (uint32_t)new_len, &why)) != 0) {
			fail(r, INST_STEP_WRITE_LIST, why ? 0 : e, why);
			drop_tmps(m, 4);
			return;
		}
		m[3].staged = 1;
	}

	/* 3. The moves, and on a failure every one of them undone, newest first. */
	for (i = 0; i < 4; i++) {
		if (!m[i].staged) continue;
		if ((e = move_in(m[i].final, m[i].tmp, m[i].old, &m[i].had, &lost)) != 0) {
			fail(r, m[i].step, e, NULL);
			r->undo_failed = lost;
			while (--i >= 0) {
				if (m[i].done && move_out(m[i].final, m[i].old, m[i].had) != 0) r->undo_failed = 1;
			}
			drop_tmps(m, 4);
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

	r->ok = 1;
	r->backed_up = m[0].done;
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

void inst_confirm_message(char *out, size_t cap)
{
	snprintf(out, cap,
	         "Install qwark build %d?\n\n"
	         "This copies qwark.sprx to " INST_PLUGINS_DIR " and ui.pak to " INST_QWARK_DIR ". "
	         "It puts qwark first in " INST_LIST ". Your other plugins stay in the list.",
	         INST_QWARK_BUILD);
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
	default:                     return "unknown";
	}
}

void inst_result_message(const inst_result *r, int hen, char *out, size_t cap)
{
	char line[160];

	if (cap == 0) return;
	out[0] = '\0';

	if (!r->ok) {
		snprintf(out, cap, "qwark was not installed.\n\nThis step failed: %s", step_name(r->step));
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
			append(out, cap, "\n\nNothing on the console was changed.");
		}
		return;
	}

	snprintf(out, cap, "qwark build %d is installed.\n\n", INST_QWARK_BUILD);
	append(out, cap, r->outcome == BOOTLIST_ALREADY_FIRST
	                 ? "boot_plugins.txt already started with qwark."
	                 : "qwark is now first in boot_plugins.txt.");
	if (r->backed_up) append(out, cap, " The old list is in " INST_LIST_BAK ".");

	if (r->lines > BOOTLIST_HEN_LIMIT) {
		snprintf(line, sizeof(line),
		         "\n\nThe list has %d plugins. The console loads only the first %d.",
		         r->lines, BOOTLIST_HEN_LIMIT);
		append(out, cap, line);
	}
	if (r->usb_list) {
		append(out, cap, "\n\nYour console has a USB drive attached. The console reads a "
		                 "boot_plugins.txt on the USB drive first, so qwark will not load. "
		                 "Remove that file.");
	}
	if (hen) append(out, cap, "\n\nAfter the restart, enable HEN. qwark loads with it.");

	append(out, cap, "\n\nqwark loads when the console starts. Restart now?");
}

void inst_restart_failed_message(unsigned int error, char *out, size_t cap)
{
	snprintf(out, cap,
	         "The console did not restart (error 0x%08x).\n\n"
	         "qwark is installed. Restart the console from the XMB to load it.",
	         error);
}
