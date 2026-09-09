/*
 * The PS3 implementation of the platform seam.
 *
 * Game memory goes through PS3MAPI, files through cellFs, notifications through
 * vshNotify, and everything else through plain lv2 syscalls. Nothing in here is
 * verified on hardware.
 */
#include "types.h"
#define QWARK_PLAT_TYPES_PROVIDED
#include "../plat.h"
#include "../plat_net.h"

#include "qwark_ps3.h"
#include "vsh.h"
#include "process.h"
#include "printf.h"

#include <cell/cell_fs.h>
#include <sys/tty.h>
#include <stdio.h>
#include <string.h>

/*
 * The channel Target Manager / ProDG surface as the console TTY. If a line
 * never shows there, try SYS_TTYP1: some setups route the debug console to 1.
 */
#define QWARK_TTY_CHANNEL SYS_TTYP0

#define QWARK_LOG_PATH "/dev_hdd0/qwark/qwark.log"

/* --------------------------------------------------------------- lifecycle */

int plat_init(void)
{
	return 0;
}

void plat_shutdown(void)
{
}

/* ----------------------------------------------------------- console state */

int plat_game_running(void)
{
	return IS_INGAME ? 1 : 0;
}

u32 plat_game_pid(void)
{
	if (!IS_INGAME) return 0;
	return (u32)GetGameProcessID();
}

int plat_game_title(char out[16])
{
	out[0] = 0;

	if (!get_game_info()) return 0;

	snprintf(out, 16, "%s", _game_TitleID);
	return out[0] != 0;
}

/*
 * Real hardware executes what is in memory, so an instruction patch is a plain
 * write and everything that depends on one works.
 */
int plat_can_patch_code(void)
{
	return 1;
}

int plat_is_emulator(void)
{
	return 0;
}

/* ------------------------------------------------------------ game memory */

int plat_mem_read(u32 pid, u32 addr, void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (ps3mapi_get_memory(pid, addr, (char *)buf, len) < 0) return -1;
	return 0;
}

int plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len)
{
	if (len == 0 || len > PLAT_MEM_MAX) return -1;
	if (ps3mapi_patch_process(pid, addr, (const char *)buf, (int)len) < 0) return -1;
	return 0;
}

/* -------------------------------------------------------------------- RSX */

/*
 * The same syscall webMAN MOD performs for /xmb.ps3$rsx_pause: lv2 syscall 674
 * (0x2A2), sys_rsx_context_attribute, with the magic context id and 2 to pause /
 * 3 to continue. Lifted from webMAN-MOD include/feat/xmb_savebmp.h's
 * rsx_fifo_pause(), which include/cmd/xmb_browser.h calls for that command.
 */
void plat_rsx_pause(int pause)
{
	system_call_6(0x2A2, 0x55555555ULL, (u64)(pause ? 2 : 3), 0, 0, 0, 0);
	(void)p1;
}

/* --------------------------------------------------------- notify and log */

void plat_notify(const char *msg)
{
	vshNotify_WithIcon(ICON_GAME, msg);
}

static int g_log_on = 1;

void plat_log_enable(int on)
{
	g_log_on = on ? 1 : 0;
}

/*
 * TTY only. No file, no mutex, no allocation: safe from module_start and from
 * the earliest boot steps. sys_tty_write appends its own nothing, so we add the
 * newline ourselves. A length count is required but ignored here.
 */
void plat_trace(const char *msg)
{
	unsigned int wrote = 0;
	unsigned int len = 0;
	static const char nl = 0x0A;

	if (msg == NULL) return;

	while (msg[len] != 0 && len < 240) len++;

	sys_tty_write(QWARK_TTY_CHANNEL, msg, len, &wrote);
	sys_tty_write(QWARK_TTY_CHANNEL, &nl, 1, &wrote);
}

void plat_log(const char *fmt, ...)
{
	char line[256];
	va_list args;
	int n;
	int fd = 0;

	if (!g_log_on) return;

	va_start(args, fmt);
	n = vsnprintf(line, sizeof(line) - 2, fmt, args);
	va_end(args);

	if (n <= 0) return;
	if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;

	/* Mirror every log line to the TTY before the file, so a fault during the
	 * file write still leaves the line on the target console. */
	line[n] = 0;
	plat_trace(line);

	line[n] = 0x0A;
	n++;

	if (cellFsOpen(QWARK_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
	               &fd, NULL, 0) != CELL_OK) {
		return;
	}

	cellFsWrite(fd, line, (uint64_t)n, NULL);
	cellFsClose(fd);
}

/* ------------------------------------------------------------------- time */

u64 plat_time_us(void)
{
	return (u64)sys_time_get_system_time();
}

void plat_sleep_us(u32 us)
{
	sys_timer_usleep(us);
}

void plat_yield(void)
{
	sys_ppu_thread_yield();
}

/* ---------------------------------------------------------------- threads */

#define PLAT_MAX_THREADS 16

struct thread_desc {
	plat_thread_fn fn;
	void *arg;
	u8 used;
};

static struct thread_desc g_tdesc[PLAT_MAX_THREADS];

/*
 * The slot is a hand-off, nothing more: it carries fn and arg from the creator
 * to the new thread and is free again the moment the new thread has read them.
 *
 * It has to be released here rather than after fn returns, because every thread
 * body in qwark ends in plat_thread_exit() and so never comes back. Holding the
 * slot for the life of the thread leaked one per client connection and, after
 * PLAT_MAX_THREADS of them, no further client could be accepted.
 */
static void thread_entry(u64 arg)
{
	u32 i = (u32)arg;
	plat_thread_fn fn = NULL;
	void *fn_arg = NULL;

	if (i < PLAT_MAX_THREADS) {
		fn = g_tdesc[i].fn;
		fn_arg = g_tdesc[i].arg;
		g_tdesc[i].fn = NULL;
		g_tdesc[i].arg = NULL;
		g_tdesc[i].used = 0;
	}

	if (fn != NULL) fn(fn_arg);

	sys_ppu_thread_exit(0);
}

static int thread_spawn(plat_thread_t *out, plat_thread_fn fn, void *arg,
                        u32 stack_size, const char *name, u64 create_flags)
{
	sys_ppu_thread_t id = SYS_PPU_THREAD_NONE;
	int slot = -1;
	int i;

	for (i = 0; i < PLAT_MAX_THREADS; i++) {
		if (!g_tdesc[i].used) { slot = i; break; }
	}
	if (slot < 0) return -1;

	g_tdesc[slot].fn = fn;
	g_tdesc[slot].arg = arg;
	g_tdesc[slot].used = 1;

	if (sys_ppu_thread_create(&id, thread_entry, (u64)(u32)slot, THREAD_PRIO,
	                          stack_size, create_flags,
	                          (char *)name) != CELL_OK) {
		g_tdesc[slot].used = 0;
		return -1;
	}

	if (out) *out = (plat_thread_t)id;
	return 0;
}

int plat_thread_create(plat_thread_t *out, plat_thread_fn fn, void *arg,
                       u32 stack_size, const char *name)
{
	return thread_spawn(out, fn, arg, stack_size, name,
	                    SYS_PPU_THREAD_CREATE_JOINABLE);
}

int plat_thread_create_detached(plat_thread_fn fn, void *arg,
                                u32 stack_size, const char *name)
{
	return thread_spawn(NULL, fn, arg, stack_size, name,
	                    SYS_PPU_THREAD_CREATE_NORMAL);
}

int plat_thread_join(plat_thread_t t)
{
	u64 exit_code = 0;

	if (t == PLAT_THREAD_NONE) return 0;
	return sys_ppu_thread_join((sys_ppu_thread_t)t, &exit_code) == CELL_OK ? 0 : -1;
}

void plat_thread_exit(void)
{
	sys_ppu_thread_exit(0);
}

/* ------------------------------------------------------- mutex, semaphore */

int plat_mutex_init(plat_mutex_t *m)
{
	sys_mutex_attribute_t attr;
	sys_mutex_t *id = (sys_mutex_t *)m->opaque;

	sys_mutex_attribute_initialize(attr);

	if (sys_mutex_create(id, &attr) != CELL_OK) {
		*id = 0;
		return -1;
	}
	return 0;
}

void plat_mutex_destroy(plat_mutex_t *m)
{
	sys_mutex_t *id = (sys_mutex_t *)m->opaque;
	if (*id != 0) sys_mutex_destroy(*id);
	*id = 0;
}

void plat_mutex_lock(plat_mutex_t *m)
{
	sys_mutex_t *id = (sys_mutex_t *)m->opaque;
	if (*id != 0) sys_mutex_lock(*id, 0);
}

void plat_mutex_unlock(plat_mutex_t *m)
{
	sys_mutex_t *id = (sys_mutex_t *)m->opaque;
	if (*id != 0) sys_mutex_unlock(*id);
}

int plat_sem_init(plat_sem_t *s, u32 initial)
{
	sys_semaphore_attribute_t attr;
	sys_semaphore_t *id = (sys_semaphore_t *)s->opaque;

	sys_semaphore_attribute_initialize(attr);

	if (sys_semaphore_create(id, &attr, (sys_semaphore_value_t)initial, 0x7FFFFFFF) != CELL_OK) {
		*id = 0;
		return -1;
	}
	return 0;
}

void plat_sem_destroy(plat_sem_t *s)
{
	sys_semaphore_t *id = (sys_semaphore_t *)s->opaque;
	if (*id != 0) sys_semaphore_destroy(*id);
	*id = 0;
}

void plat_sem_post(plat_sem_t *s)
{
	sys_semaphore_t *id = (sys_semaphore_t *)s->opaque;
	if (*id != 0) sys_semaphore_post(*id, 1);
}

void plat_sem_wait(plat_sem_t *s)
{
	sys_semaphore_t *id = (sys_semaphore_t *)s->opaque;
	if (*id != 0) sys_semaphore_wait(*id, 0);
}

/* ------------------------------------------------------------------ files */

int plat_file_open(const char *path, int mode, plat_file_t *out)
{
	int fd = 0;
	int flags = (mode == PLAT_OPEN_WRITE)
	          ? (CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC)
	          : CELL_FS_O_RDONLY;

	*out = PLAT_FILE_INVALID;

	if (cellFsOpen(path, flags, &fd, NULL, 0) != CELL_OK) return -1;

	if (mode == PLAT_OPEN_WRITE) cellFsChmod(path, 0100000 | 0666);

	*out = fd;
	return 0;
}

int plat_file_read(plat_file_t f, void *buf, u32 len, u32 *nread)
{
	uint64_t got = 0;

	if (nread) *nread = 0;
	if (f == PLAT_FILE_INVALID) return -1;
	if (len == 0) return 0;

	if (cellFsRead(f, buf, (uint64_t)len, &got) != CELL_OK) return -1;

	if (nread) *nread = (u32)got;
	return 0;
}

int plat_file_write(plat_file_t f, const void *buf, u32 len)
{
	uint64_t written = 0;

	if (f == PLAT_FILE_INVALID) return -1;
	if (len == 0) return 0;

	if (cellFsWrite(f, buf, (uint64_t)len, &written) != CELL_OK) return -1;
	return written == (uint64_t)len ? 0 : -1;
}

int plat_file_close(plat_file_t f)
{
	if (f == PLAT_FILE_INVALID) return -1;
	return cellFsClose(f) == CELL_OK ? 0 : -1;
}

int plat_file_unlink(const char *path)
{
	return cellFsUnlink(path) == CELL_OK ? 0 : -1;
}

int plat_dir_create(const char *path)
{
	return cellFsMkdir(path, 0777) == CELL_OK ? 0 : -1;
}

int plat_dir_remove(const char *path)
{
	return cellFsRmdir(path) == CELL_OK ? 0 : -1;
}

int plat_path_exists(const char *path, int *is_dir, u64 *size)
{
	CellFsStat sb;

	if (cellFsStat(path, &sb) != CELL_OK) return 0;

	if (is_dir) *is_dir = ((sb.st_mode & CELL_FS_S_IFMT) == CELL_FS_S_IFDIR) ? 1 : 0;
	if (size)   *size = (u64)sb.st_size;
	return 1;
}

int plat_dir_open(const char *path, plat_dir_t *d)
{
	int fd = 0;

	d->handle = -1;
	d->p = NULL;
	d->path[0] = 0;

	if (cellFsOpendir(path, &fd) != CELL_OK) return -1;

	d->handle = fd;
	snprintf(d->path, sizeof(d->path), "%s", path);
	return 0;
}

int plat_dir_next(plat_dir_t *d, struct plat_dirent *e)
{
	CellFsDirent entry;
	uint64_t nread = 0;
	char child[512];

	if (d->handle < 0) return -1;

	for (;;) {
		memset(&entry, 0, sizeof(entry));
		if (cellFsReaddir(d->handle, &entry, &nread) != CELL_OK) return -1;
		if (nread == 0) return 0;

		/* "." and ".." are never reported, so every caller sees the same thing. */
		if (entry.d_name[0] == '.' &&
		    (entry.d_name[1] == 0 || (entry.d_name[1] == '.' && entry.d_name[2] == 0))) {
			continue;
		}
		break;
	}

	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", entry.d_name);
	e->is_dir = (entry.d_type == CELL_FS_TYPE_DIRECTORY) ? 1 : 0;

	/* cellFsReaddir does not carry the size, so stat the entry for DIR_LIST. */
	snprintf(child, sizeof(child), "%s/%s", d->path, e->name);
	plat_path_exists(child, NULL, &e->size);

	return 1;
}

void plat_dir_close(plat_dir_t *d)
{
	if (d->handle >= 0) cellFsClosedir(d->handle);
	d->handle = -1;
}

/* ------------------------------------------------------------ page memory */

void *plat_alloc_pages(u32 size)
{
	sys_addr_t addr = 0;
	u32 rounded = (size + 0xFFFFu) & ~0xFFFFu;

	if (sys_memory_allocate(rounded, SYS_MEMORY_PAGE_SIZE_64K, &addr) != CELL_OK)
		return NULL;

	return (void *)(u32)addr;
}

void plat_free_pages(void *p)
{
	if (p != NULL) sys_memory_free((sys_addr_t)(u32)p);
}

/* ------------------------------------------------------------------- misc */

u32 plat_user_id(void)
{
	return (u32)xusers()->GetCurrentUserNumber();
}

/* ---------------------------------------------------------------- sockets */

int plat_socket_close(int s)
{
	if (s < 0) return -1;
	return socketclose(s);
}

void plat_socket_shutdown(int s)
{
	if (s >= 0) shutdown(s, SHUT_RDWR);
}

int plat_net_errno(void)
{
	return sys_net_errno;
}

int plat_net_would_retry(int err)
{
	return err == SYS_NET_EINTR;
}
