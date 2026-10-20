/*
 * ifs.h on the console: CellFs, as qwark's own src/plat/ps3/plat_ps3.c uses it.
 * The CellFs error codes come back as they are, for the installer's message.
 */
#include "ifs.h"

#include <cell/cell_fs.h>

int ifs_open_read(const char *path, ifs_fd *fd)
{
	int f = -1;
	int e = cellFsOpen(path, CELL_FS_O_RDONLY, &f, NULL, 0);

	*fd = f;
	return e;
}

int ifs_open_write(const char *path, ifs_fd *fd)
{
	int f = -1;
	int e = cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &f, NULL, 0);

	*fd = f;
	/* The mode qwark gives its own files: a regular file anyone may read and write. */
	if (e == CELL_FS_SUCCEEDED) cellFsChmod(path, CELL_FS_S_IFREG | 0666);
	return e;
}

int ifs_read(ifs_fd fd, void *buf, uint32_t len, uint32_t *got)
{
	uint64_t n = 0;
	int e = cellFsRead(fd, buf, (uint64_t)len, &n);

	*got = (e == CELL_FS_SUCCEEDED) ? (uint32_t)n : 0;
	return e;
}

int ifs_write(ifs_fd fd, const void *buf, uint32_t len)
{
	uint64_t n = 0;
	int e = cellFsWrite(fd, buf, (uint64_t)len, &n);

	if (e != CELL_FS_SUCCEEDED) return e;
	return n == (uint64_t)len ? 0 : -1;
}

int ifs_sync(ifs_fd fd)
{
	return cellFsFsync(fd);
}

int ifs_close(ifs_fd fd)
{
	return cellFsClose(fd);
}

int ifs_stat(const char *path, int *is_dir, uint64_t *size)
{
	CellFsStat st;
	int e = cellFsStat(path, &st);

	if (e != CELL_FS_SUCCEEDED) return e;
	if (is_dir) *is_dir = (st.st_mode & CELL_FS_S_IFMT) == CELL_FS_S_IFDIR;
	if (size) *size = st.st_size;
	return 0;
}

int ifs_mkdir(const char *path)
{
	/* 0777, as qwark makes its own folders: the VSH and webMAN read them too. */
	return cellFsMkdir(path, 0777);
}

int ifs_rename(const char *from, const char *to)
{
	return cellFsRename(from, to);
}

int ifs_unlink(const char *path)
{
	return cellFsUnlink(path);
}
