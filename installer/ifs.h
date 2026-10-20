/*
 * The few file operations the installer makes, so that install.c builds for the
 * console (ifs_ps3.c, over CellFs) and for the host tests (test/ifs_host.c, over
 * a folder that stands in for the console's devices, with failures to order).
 *
 * Every call answers 0 for success and anything else for failure: on the
 * console that is the CellFs error code, which the installer's message shows.
 */
#ifndef QWARK_INSTALLER_IFS_H
#define QWARK_INSTALLER_IFS_H

#include <stdint.h>

typedef int ifs_fd;

int ifs_open_read(const char *path, ifs_fd *fd);
/* Creates the file, or empties the one that is there. */
int ifs_open_write(const char *path, ifs_fd *fd);
/* Reads up to len bytes; *got 0 is the end of the file. */
int ifs_read(ifs_fd fd, void *buf, uint32_t len, uint32_t *got);
/* Writes all of buf, or fails. */
int ifs_write(ifs_fd fd, const void *buf, uint32_t len);
int ifs_sync(ifs_fd fd);
int ifs_close(ifs_fd fd);

/*
 * Whether path exists: 0 when it does, with whether it is a folder and its
 * size, IFS_ENOENT when it does not, and any other code when the console could
 * not say. The installer never takes "could not say" for "not there": a list
 * it failed to see would otherwise be replaced by one with only qwark on it.
 */
int ifs_stat(const char *path, int *is_dir, uint64_t *size);

/* CELL_FS_ENOENT (cell/fs/cell_fs_errno.h), which ifs_host.c answers too. */
#define IFS_ENOENT ((int)0x80010006u)
int ifs_mkdir(const char *path);
int ifs_rename(const char *from, const char *to);
int ifs_unlink(const char *path);

#endif
