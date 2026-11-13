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
#include "../../core/buildmodes.h"   /* QWARK_GAME_MODULES, revision 1.26 */
#include "../plat_net.h"

#include "qwark_ps3.h"
#include "vsh.h"
#include "process.h"
#include "printf.h"

#include <cell/cell_fs.h>
#include <sys/tty.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>

/*
 * The channel Target Manager / ProDG surface as the console TTY. If a line
 * never shows there, try SYS_TTYP1: some setups route the debug console to 1.
 */
#define QWARK_TTY_CHANNEL SYS_TTYP0

#define QWARK_LOG_PATH     "/dev_hdd0/qwark/qwark.log"
#define QWARK_LOG_OLD_PATH "/dev_hdd0/qwark/qwark.old.log"

/*
 * The log is appended to and never truncated. Past this size when the module
 * starts, it becomes qwark.old.log, replacing the one before, and a new log
 * begins. A crash is followed by a reboot and so by this check: its last lines
 * are at the end of qwark.log, or of qwark.old.log when this is what moved them.
 */
#define QWARK_LOG_KEEP     (1024u * 1024u)

/* --------------------------------------------------------------- lifecycle */

/*
 * One line at a time into the log. The tick thread and every connection thread
 * log, and without this a line could land in the middle of another. Zero until
 * plat_init makes it, and plat_mutex_lock does nothing on a zero mutex, so a
 * line logged before then simply goes in unlocked.
 */
static plat_mutex_t g_log_mutex;

int plat_init(void)
{
	u64 size = 0;

	if (plat_path_exists(QWARK_LOG_PATH, NULL, &size) && size > QWARK_LOG_KEEP) {
		plat_file_unlink(QWARK_LOG_OLD_PATH);
		plat_file_rename(QWARK_LOG_PATH, QWARK_LOG_OLD_PATH);
	}

	plat_mutex_init(&g_log_mutex);
	return 0;
}

void plat_shutdown(void)
{
}

/* ----------------------------------------------------------- console state */

int plat_game_running(void)
{
	return IS_INGAME != 0;
}

u32 plat_game_pid(void)
{
	return (u32)GetGameProcessID();
}

int plat_game_title(char out[16])
{
	out[0] = 0;

	if (!get_game_info(out)) return 0;

	return out[0] != 0;
}

/*
 * Real hardware executes what is in memory, so an instruction patch is a plain
 * write and everything that depends on one works. plat_can_patch_code() and
 * plat_is_emulator() are the constants 1 and 0 in plat.h here (build 51), so
 * the core's RPCS3 branches fold away rather than asking this file.
 */

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

/* --------------------------------------------------------- notify and log */

void plat_notify(const char *msg)
{
	vshNotify_Game(msg);
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
	va_list args;

	va_start(args, fmt);
	plat_vlog(fmt, args);
	va_end(args);
}

void plat_vlog(const char *fmt, va_list args)
{
	char line[256];
	int n;
	int fd = 0;

	if (!g_log_on) return;

	n = vsnprintf(line, sizeof(line) - 2, fmt, args);

	if (n <= 0) return;
	if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;

	/* Mirror every log line to the TTY before the file, so a fault during the
	 * file write still leaves the line on the target console. */
	line[n] = 0;
	plat_trace(line);

	line[n] = 0x0A;
	n++;

	plat_mutex_lock(&g_log_mutex);
	if (cellFsOpen(QWARK_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
	               &fd, NULL, 0) == CELL_OK) {
		cellFsWrite(fd, line, (uint64_t)n, NULL);
		cellFsClose(fd);
	}
	plat_mutex_unlock(&g_log_mutex);
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
#if QWARK_GAME_MODULES
extern struct qwark_ctx g_qwark_ctx;   /* src/core/gamemod.c */
#endif

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

	/*
	 * Revision 1.26, game modules (src/games/module.h): a test module's
	 * threads carry the core's context in r14, which the core and every game
	 * module are compiled to leave alone (-ffixed-r14), so whichever of them
	 * calls into a module, the module finds it there. lv2 starts a thread
	 * with whatever r14 it likes, so it goes in before anything of qwark runs.
	 */
#if QWARK_GAME_MODULES
	__asm__ volatile ("mr 14, %0" : : "r" (&g_qwark_ctx));
#endif

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

/* plat.h sizes both to the kernel object's id on the PS3, and no larger. */
typedef char plat_mutex_fits[sizeof(sys_mutex_t) <= sizeof(plat_mutex_t) ? 1 : -1];
typedef char plat_sem_fits[sizeof(sys_semaphore_t) <= sizeof(plat_sem_t) ? 1 : -1];

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

int plat_file_read_at(plat_file_t f, u32 off, void *buf, u32 len, u32 *nread)
{
	uint64_t got = 0;

	if (nread) *nread = 0;
	if (f == PLAT_FILE_INVALID) return -1;
	if (len == 0) return 0;

	if (cellFsReadWithOffset(f, (uint64_t)off, buf, (uint64_t)len, &got) != CELL_OK) return -1;

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

int plat_file_rename(const char *from, const char *to)
{
	return cellFsRename(from, to) == CELL_OK ? 0 : -1;
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

/*
 * sys_memory_allocate is gone from the module, as of revision 1.11. The request
 * buffers it used to hand out - three 64 KB pages of the VSH's own pool per
 * request in flight - are two static 16 KB buffers in net.c now, shared by every
 * connection, so nothing here asks the console for memory after the load.
 */

/* ------------------------------------------------------------------- misc */

u32 plat_user_id(void)
{
	return (u32)xusers()->GetCurrentUserNumber();
}

/*
 * Build 54. Syscall 352, the SDK's inline in <sys/memory.h>: the VSH's user
 * memory, total and available, in bytes. webMAN's /cpursx.ps3 shows the same
 * number.
 */
int plat_vsh_memory(u32 *free_bytes, u32 *total_bytes)
{
	sys_memory_info_t mi;

	if (sys_memory_get_user_memory_size(&mi) != CELL_OK) return -1;
	*free_bytes = (u32)mi.available_user_memory;
	*total_bytes = (u32)mi.total_user_memory;
	return 0;
}

/* ------------------------------------------------- the VSH's own pad (probe) */

/*
 * Revision 1.31, the VSH pad probe (src/core/vshpad.c). sys_io's libpad, the
 * VSH's own copy, through the SDK's libio_stub, as webMAN MOD's XMB combos
 * read it (include/feat/vpad.h's pad_read and include/poll/combos.h: GetInfo2,
 * then GetData on a connected port, len > 0 taken as data). The VSH initialised
 * libpad long before any plugin loads, so neither webMAN nor qwark calls
 * cellPadInit (CELL_PAD_ERROR_ALREADY_INITIALIZED at best) or cellPadEnd (which
 * would take the pad away from the VSH). Both calls copy out of libpad's
 * buffers and do not block.
 *
 * libpad keeps one "last read" per port for the whole process, so a len > 0
 * read hands the change to whoever asks first and the next asker in the VSH
 * (webMAN's combo poll) sees len 0 for it: why the core reads only while a
 * client is connected (vshpad.c) and why the probe's notes ask about webMAN.
 */
#include <cell/pad.h>

int plat_vsh_pad_info(struct plat_pad_info *out)
{
	CellPadInfo2 info;
	int rc, i;

	memset(&info, 0, sizeof(info));
	rc = (int)cellPadGetInfo2(&info);
	if (rc != CELL_PAD_OK) return rc == PLAT_PAD_NONE ? -1 : rc;

	out->max_connect = info.max_connect;
	out->now_connect = info.now_connect;
	out->system_info = info.system_info;
	for (i = 0; i < PLAT_PAD_PORTS; i++) {
		out->port_status[i] = info.port_status[i];
		out->device_type[i] = info.device_type[i];
	}
	return 0;
}

int plat_vsh_pad_read(u32 port, struct plat_pad *out)
{
	CellPadData data;
	int rc;

	memset(out, 0, sizeof(*out));
	if (port >= PLAT_PAD_PORTS) return -1;

	data.len = 0;
	rc = (int)cellPadGetData(port, &data);
	if (rc != CELL_PAD_OK) return rc == PLAT_PAD_NONE ? -1 : rc;

	out->len = data.len;
	if (data.len <= 0) return 0;
	out->fresh = 1;
	/* len counts the button[] words filled: 4 for the digital pair, 8 with the sticks. */
	if (data.len > CELL_PAD_BTN_OFFSET_DIGITAL2) {
		out->digital1 = (u8)data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
		out->digital2 = (u8)data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
	}
	if (data.len <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) {
		out->stick[0] = out->stick[1] = out->stick[2] = out->stick[3] = 0x80;
		return 0;
	}
	out->stick[0] = (u8)data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
	out->stick[1] = (u8)data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];
	out->stick[2] = (u8)data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
	out->stick[3] = (u8)data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
	return 0;
}

#if QWARK_GAME_MODULES
/* ------------------------------------------------- game modules (1.26) */

/*
 * liblv2's user-mode sys_prx_* (sysPrxForUser), on the calling thread, the
 * tick: what the PRX probe ran on CFW and HEN (branch prx-probe,
 * src/plat/ps3/prx_ps3.c), every module loaded, relocated, started, called,
 * stopped and unloaded with nothing left behind. No Cobra slot, no start
 * thread of lv2's, no argument page.
 */
#include <sys/prx.h>

int plat_prx_load(const char *path)
{
	return (int)sys_prx_load_module(path, 0, NULL);
}

int plat_prx_start(int id, void *args, u32 size, int *modres)
{
	*modres = 0;
	return sys_prx_start_module((sys_prx_id_t)id, (size_t)size, args, modres, 0, NULL);
}

int plat_prx_stop(int id, void *args, u32 size, int *modres)
{
	*modres = 0;
	return sys_prx_stop_module((sys_prx_id_t)id, (size_t)size, args, modres, 0, NULL);
}

int plat_prx_unload(int id)
{
	return sys_prx_unload_module((sys_prx_id_t)id, 0, NULL);
}

/*
 * The module's segments, as lv2 placed them: what a module's start hands back
 * must lie inside them. Static, not on the tick thread's stack. The module
 * info's start entry reads 0xFFFFFFFF before a start (the probe's first
 * hardware run), so nothing here looks at it.
 */
#define PRX_SEGS 4
static sys_prx_segment_info_t g_prx_segs[PRX_SEGS];
static char g_prx_name[SYS_PRX_MODULE_FILENAME_SIZE];

int plat_prx_contains(int id, const void *p, u32 len)
{
	sys_prx_module_info_t mi;
	u32 a = (u32)p, i, n;

	memset(&mi, 0, sizeof(mi));
	memset(g_prx_segs, 0, sizeof(g_prx_segs));
	mi.size = sizeof(mi);
	mi.filename = (sys_prx_user_pchar_t)(u32)g_prx_name;
	mi.filename_size = sizeof(g_prx_name);
	mi.segments = (sys_prx_user_segment_vector_t)(u32)g_prx_segs;
	mi.segments_num = PRX_SEGS;
	if (sys_prx_get_module_info((sys_prx_id_t)id, 0, &mi) != 0) return -1;

	n = mi.segments_num < PRX_SEGS ? (u32)mi.segments_num : PRX_SEGS;
	for (i = 0; i < n; i++) {
		u32 base = (u32)g_prx_segs[i].base, memsz = (u32)g_prx_segs[i].memsz;
		if (g_prx_segs[i].type != 1 || memsz == 0) continue;   /* LOAD segments only */
		if (a >= base && len <= memsz && a - base <= memsz - len) return 1;
	}
	return 0;
}
#endif /* QWARK_GAME_MODULES */

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

int plat_socket_nonblocking(int s)
{
	int on = 1;
	return setsockopt(s, SOL_SOCKET, SO_NBIO, &on, sizeof(on));
}

int plat_socket_wait(int s, int writing, u32 ms)
{
	fd_set fds;
	struct timeval timeout;
	if (s < 0 || s >= FD_SETSIZE) return -1;
	FD_ZERO(&fds);
	FD_SET(s, &fds);
	timeout.tv_sec = ms / 1000u;
	timeout.tv_usec = (ms % 1000u) * 1000u;
	return socketselect(s + 1, writing ? NULL : &fds, writing ? &fds : NULL, NULL, &timeout);
}

int plat_net_would_retry(int err)
{
	return err == SYS_NET_EINTR || err == SYS_NET_EWOULDBLOCK;
}
