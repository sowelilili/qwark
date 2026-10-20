/*
 * ifs.h for the host tests: the console's devices are folders under a root
 * (root/dev_hdd0, root/dev_usb000), and any call can be made to fail, so the
 * tests can stop the installer at every step and look at what it left.
 *
 * Like the console's file system as the installer treats it, a rename never
 * replaces a file: it fails when the target exists, whatever the host does.
 */
#include "ifs_host.h"
#include "../ifs.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define host_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define host_mkdir(p) mkdir((p), 0777)
#endif

#define MAX_FDS 16

static char g_root[1024];
static FILE *g_files[MAX_FDS];
static char g_paths[MAX_FDS][1024];

static int g_calls;          /* every call so far */
static int g_fail_at;        /* the call that fails, 1-based; 0 for none */
static int g_fail_from;      /* this call and every one after it fail; 0 for none */
static int g_fault_op;       /* or every call of this kind ... */
static char g_fault_path[256];   /* ... on a path that holds this */
static char g_corrupt_path[256]; /* writes to a path that holds this lose a bit */

void ifs_host_root(const char *root)
{
	snprintf(g_root, sizeof(g_root), "%s", root);
}

void ifs_host_reset(void)
{
	int i;

	for (i = 0; i < MAX_FDS; i++) {
		if (g_files[i]) fclose(g_files[i]);
		g_files[i] = NULL;
	}
	g_calls = 0;
	g_fail_at = 0;
	g_fail_from = 0;
	g_fault_op = 0;
	g_fault_path[0] = '\0';
	g_corrupt_path[0] = '\0';
}

int ifs_host_calls(void) { return g_calls; }
void ifs_host_fail_at(int call) { g_fail_at = call; }
void ifs_host_fail_from(int call) { g_fail_from = call; }

void ifs_host_fault(int op, const char *path_part)
{
	g_fault_op = op;
	snprintf(g_fault_path, sizeof(g_fault_path), "%s", path_part ? path_part : "");
}

void ifs_host_corrupt(const char *path_part)
{
	snprintf(g_corrupt_path, sizeof(g_corrupt_path), "%s", path_part ? path_part : "");
}

static void host_path(const char *path, char *out, size_t cap)
{
	snprintf(out, cap, "%s%s", g_root, path);
}

/* Whether this call fails: counted, then held against the faults set. */
static int faulted(int op, const char *path)
{
	g_calls++;
	if (g_fail_at && g_calls == g_fail_at) return 1;
	if (g_fail_from && g_calls >= g_fail_from) return 1;
	if (g_fault_op == op && path && g_fault_path[0] && strstr(path, g_fault_path)) return 1;
	return 0;
}

/* The error a faulted call answers: CELL_FS_EIO's value, so the message shows a console-like code. */
#define FAULT_ERR ((int)0x8001002Bu)

static int new_fd(FILE *f, const char *path, ifs_fd *fd)
{
	int i;

	for (i = 0; i < MAX_FDS; i++) {
		if (!g_files[i]) {
			g_files[i] = f;
			snprintf(g_paths[i], sizeof(g_paths[i]), "%s", path);
			*fd = i + 1;
			return 0;
		}
	}
	fclose(f);
	return -1;
}

static FILE *file_of(ifs_fd fd)
{
	return (fd >= 1 && fd <= MAX_FDS) ? g_files[fd - 1] : NULL;
}

static const char *path_of(ifs_fd fd)
{
	return (fd >= 1 && fd <= MAX_FDS && g_files[fd - 1]) ? g_paths[fd - 1] : NULL;
}

int ifs_open_read(const char *path, ifs_fd *fd)
{
	char hp[1300];
	FILE *f;

	if (faulted(IFS_OP_OPEN_READ, path)) return FAULT_ERR;
	host_path(path, hp, sizeof(hp));
	if ((f = fopen(hp, "rb")) == NULL) return errno ? errno : -1;
	return new_fd(f, path, fd);
}

int ifs_open_write(const char *path, ifs_fd *fd)
{
	char hp[1300];
	FILE *f;

	if (faulted(IFS_OP_OPEN_WRITE, path)) return FAULT_ERR;
	host_path(path, hp, sizeof(hp));
	if ((f = fopen(hp, "wb")) == NULL) return errno ? errno : -1;
	return new_fd(f, path, fd);
}

int ifs_read(ifs_fd fd, void *buf, uint32_t len, uint32_t *got)
{
	FILE *f = file_of(fd);
	size_t n;

	*got = 0;
	if (faulted(IFS_OP_READ, path_of(fd))) return FAULT_ERR;
	if (!f) return -1;
	n = fread(buf, 1, len, f);
	if (n == 0 && ferror(f)) return -1;
	*got = (uint32_t)n;
	return 0;
}

int ifs_write(ifs_fd fd, const void *buf, uint32_t len)
{
	FILE *f = file_of(fd);
	const char *path = path_of(fd);

	if (faulted(IFS_OP_WRITE, path)) return FAULT_ERR;
	if (!f) return -1;
	if (len && g_corrupt_path[0] && path && strstr(path, g_corrupt_path)) {
		/* What a bad sector would do: the call succeeds, the bytes are not the ones sent. */
		unsigned char first = (unsigned char)(((const unsigned char *)buf)[0] ^ 0x01u);
		if (fwrite(&first, 1, 1, f) != 1) return -1;
		return fwrite((const unsigned char *)buf + 1, 1, len - 1, f) == len - 1 ? 0 : -1;
	}
	return fwrite(buf, 1, len, f) == len ? 0 : -1;
}

int ifs_sync(ifs_fd fd)
{
	FILE *f = file_of(fd);

	if (faulted(IFS_OP_SYNC, path_of(fd))) return FAULT_ERR;
	return (f && fflush(f) == 0) ? 0 : -1;
}

int ifs_close(ifs_fd fd)
{
	FILE *f = file_of(fd);
	int failed;

	/* A close always lets go of the file, faulted or not, as a real one does. */
	failed = faulted(IFS_OP_CLOSE, path_of(fd));
	if (!f) return -1;
	g_files[fd - 1] = NULL;
	if (fclose(f) != 0) return -1;
	return failed ? FAULT_ERR : 0;
}

int ifs_stat(const char *path, int *is_dir, uint64_t *size)
{
	char hp[1300];
	struct stat st;

	if (faulted(IFS_OP_STAT, path)) return FAULT_ERR;
	host_path(path, hp, sizeof(hp));
	if (stat(hp, &st) != 0) return errno == ENOENT ? IFS_ENOENT : (errno ? errno : -1);
	if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	if (size) *size = (uint64_t)st.st_size;
	return 0;
}

int ifs_mkdir(const char *path)
{
	char hp[1300];

	if (faulted(IFS_OP_MKDIR, path)) return FAULT_ERR;
	host_path(path, hp, sizeof(hp));
	return host_mkdir(hp) == 0 ? 0 : (errno ? errno : -1);
}

int ifs_rename(const char *from, const char *to)
{
	char hf[1300], ht[1300];
	struct stat st;

	if (faulted(IFS_OP_RENAME, to)) return FAULT_ERR;
	host_path(from, hf, sizeof(hf));
	host_path(to, ht, sizeof(ht));
	if (stat(ht, &st) == 0) return EEXIST;
	return rename(hf, ht) == 0 ? 0 : (errno ? errno : -1);
}

int ifs_unlink(const char *path)
{
	char hp[1300];

	if (faulted(IFS_OP_UNLINK, path)) return FAULT_ERR;
	host_path(path, hp, sizeof(hp));
	return remove(hp) == 0 ? 0 : (errno ? errno : -1);
}
