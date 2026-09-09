/*
 * The host implementation of the platform seam, minus the game.
 *
 * Threads, mutexes, semaphores, sockets, files, directories, time, logging and
 * page memory: everything a host build needs whatever it is driving. The game
 * half - process state, memory, RSX and notify - comes from whichever backend
 * is linked beside this file, backend_fake.c or backend_pine.c.
 *
 * Windows-specific code is behind _WIN32; the rest is plain POSIX, so a Linux
 * host build is a short step from here.
 */
#include "../plat.h"
#include "../plat_net.h"
#include "plat_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/time.h>
#endif

static char g_root[1024];

/* --------------------------------------------------------------- lifecycle */

/* <directory of the running executable>/<name>, so the root travels with it. */
static void host_find_root(const char *name)
{
#ifdef _WIN32
	char exe[1024];
	DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe) - 1);
	char *slash;

	if (n == 0) { snprintf(g_root, sizeof(g_root), "%s", name); return; }
	exe[n] = 0;

	slash = strrchr(exe, '\\');
	if (slash) *slash = 0;
	else exe[0] = 0;

	snprintf(g_root, sizeof(g_root), "%s\\%s", exe, name);
#else
	snprintf(g_root, sizeof(g_root), "./%s", name);
#endif
}

const char *host_root(void)
{
	return g_root;
}

/* Maps an absolute console path onto the host root. */
static void host_path(const char *in, char *out, u32 cap)
{
	u32 i;

	snprintf(out, cap, "%s%s", g_root, in);

#ifdef _WIN32
	for (i = 0; out[i] != 0 && i < cap; i++) {
		if (out[i] == '/') out[i] = '\\';
	}
#else
	(void)i;
#endif
}

static void host_mkdir_raw(const char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

/* Creates the root and every parent of `path` under it. */
static void host_mkdir_parents(char *path)
{
	u32 i;

	for (i = 1; path[i] != 0; i++) {
#ifdef _WIN32
		if (path[i] != '\\') continue;
#else
		if (path[i] != '/') continue;
#endif
		path[i] = 0;
		host_mkdir_raw(path);
#ifdef _WIN32
		path[i] = '\\';
#else
		path[i] = '/';
#endif
	}
}

int host_common_init(const char *path, const char *name)
{
	if (path != NULL && path[0] != 0) {
		snprintf(g_root, sizeof(g_root), "%s", path);
	} else {
		host_find_root(name != NULL ? name : "qwark-host-root");
	}
	host_mkdir_raw(g_root);

#ifdef _WIN32
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
	}

	/* The default 15.6 ms scheduler tick would run the 120 Hz loop at ~64 Hz. */
	timeBeginPeriod(1);
#endif

	return 0;
}

void host_common_shutdown(void)
{
#ifdef _WIN32
	timeEndPeriod(1);
	WSACleanup();
#endif
}

/* --------------------------------------------------------- notify and log */

void host_stamp(char *out, u32 cap)
{
	time_t now = time(NULL);
	struct tm tmv;

#ifdef _WIN32
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif

	snprintf(out, cap, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static int g_log_on = 1;

void plat_log_enable(int on)
{
	g_log_on = on ? 1 : 0;
}

void plat_trace(const char *msg)
{
	char ts[16];
	if (msg == NULL) return;
	host_stamp(ts, sizeof(ts));
	fprintf(stderr, "[%s] trace: %s\n", ts, msg);
	fflush(stderr);
}

void plat_log(const char *fmt, ...)
{
	char ts[16];
	va_list args;

	if (!g_log_on) return;

	host_stamp(ts, sizeof(ts));
	fprintf(stderr, "[%s] ", ts);

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fputc('\n', stderr);
	fflush(stderr);
}

/* ------------------------------------------------------------------- time */

u64 plat_time_us(void)
{
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;

	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);

	return (u64)((now.QuadPart * 1000000ULL) / (u64)freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000ULL + (u64)(ts.tv_nsec / 1000);
#endif
}

void plat_sleep_us(u32 us)
{
#ifdef _WIN32
	Sleep(us / 1000 ? us / 1000 : 1);
#else
	usleep(us);
#endif
}

void plat_yield(void)
{
#ifdef _WIN32
	SwitchToThread();
#else
	sched_yield();
#endif
}

/* ---------------------------------------------------------------- threads */

#define HOST_MAX_THREADS 32

struct host_thread {
	pthread_t tid;
	plat_thread_fn fn;
	void *arg;
	int used;
	int detached;
};

static struct host_thread g_threads[HOST_MAX_THREADS];
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * A joinable slot belongs to whoever joins it; a detached one is released here,
 * so a client connection thread does not hold a slot for ever the way it used
 * to (HOST_MAX_THREADS connections and the simulator stopped accepting).
 */
static void *thread_trampoline(void *arg)
{
	struct host_thread *t = (struct host_thread *)arg;

	t->fn(t->arg);

	if (t->detached) {
		pthread_mutex_lock(&g_thread_lock);
		t->used = 0;
		pthread_mutex_unlock(&g_thread_lock);
	}
	return NULL;
}

static int thread_spawn(plat_thread_t *out, plat_thread_fn fn, void *arg,
                        u32 stack_size, int detached)
{
	int slot = -1;
	int i;
	pthread_attr_t attr;

	pthread_mutex_lock(&g_thread_lock);
	for (i = 0; i < HOST_MAX_THREADS; i++) {
		if (!g_threads[i].used) { slot = i; g_threads[i].used = 1; break; }
	}
	pthread_mutex_unlock(&g_thread_lock);

	if (slot < 0) return -1;

	g_threads[slot].fn = fn;
	g_threads[slot].arg = arg;
	g_threads[slot].detached = detached;

	pthread_attr_init(&attr);
	if (stack_size >= 65536) pthread_attr_setstacksize(&attr, stack_size);
	if (detached) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	if (pthread_create(&g_threads[slot].tid, &attr, thread_trampoline,
	                   &g_threads[slot]) != 0) {
		pthread_attr_destroy(&attr);
		g_threads[slot].used = 0;
		return -1;
	}
	pthread_attr_destroy(&attr);

	if (out) *out = (plat_thread_t)(slot + 1);
	return 0;
}

int plat_thread_create(plat_thread_t *out, plat_thread_fn fn, void *arg,
                       u32 stack_size, const char *name)
{
	(void)name;
	return thread_spawn(out, fn, arg, stack_size, 0);
}

int plat_thread_create_detached(plat_thread_fn fn, void *arg,
                                u32 stack_size, const char *name)
{
	(void)name;
	return thread_spawn(NULL, fn, arg, stack_size, 1);
}

int plat_thread_join(plat_thread_t t)
{
	u32 slot;

	if (t == PLAT_THREAD_NONE) return 0;
	slot = (u32)t - 1;
	if (slot >= HOST_MAX_THREADS || !g_threads[slot].used) return -1;

	pthread_join(g_threads[slot].tid, NULL);
	g_threads[slot].used = 0;
	return 0;
}

void plat_thread_exit(void)
{
	/* The trampoline returns and pthreads winds the thread down for us. */
}

/* ------------------------------------------------------- mutex, semaphore */

int plat_mutex_init(plat_mutex_t *m)
{
	pthread_mutex_t *p = (pthread_mutex_t *)m->opaque;
	return pthread_mutex_init(p, NULL) == 0 ? 0 : -1;
}

void plat_mutex_destroy(plat_mutex_t *m)
{
	pthread_mutex_destroy((pthread_mutex_t *)m->opaque);
}

void plat_mutex_lock(plat_mutex_t *m)
{
	pthread_mutex_lock((pthread_mutex_t *)m->opaque);
}

void plat_mutex_unlock(plat_mutex_t *m)
{
	pthread_mutex_unlock((pthread_mutex_t *)m->opaque);
}

struct host_sem {
	pthread_mutex_t m;
	pthread_cond_t  c;
	unsigned count;
};

int plat_sem_init(plat_sem_t *s, u32 initial)
{
	struct host_sem *sem = (struct host_sem *)s->opaque;

	if (sizeof(struct host_sem) > sizeof(s->opaque)) return -1;

	if (pthread_mutex_init(&sem->m, NULL) != 0) return -1;
	if (pthread_cond_init(&sem->c, NULL) != 0) return -1;
	sem->count = initial;
	return 0;
}

void plat_sem_destroy(plat_sem_t *s)
{
	struct host_sem *sem = (struct host_sem *)s->opaque;
	pthread_cond_destroy(&sem->c);
	pthread_mutex_destroy(&sem->m);
}

void plat_sem_post(plat_sem_t *s)
{
	struct host_sem *sem = (struct host_sem *)s->opaque;

	pthread_mutex_lock(&sem->m);
	sem->count++;
	pthread_cond_signal(&sem->c);
	pthread_mutex_unlock(&sem->m);
}

void plat_sem_wait(plat_sem_t *s)
{
	struct host_sem *sem = (struct host_sem *)s->opaque;

	pthread_mutex_lock(&sem->m);
	while (sem->count == 0) pthread_cond_wait(&sem->c, &sem->m);
	sem->count--;
	pthread_mutex_unlock(&sem->m);
}

/* ------------------------------------------------------------------ files */

#define HOST_FILE_SLOTS 32

static FILE *g_open_files[HOST_FILE_SLOTS];

int plat_file_open(const char *path, int mode, plat_file_t *out)
{
	char real[1024];
	FILE *f;
	int i;

	*out = PLAT_FILE_INVALID;

	host_path(path, real, sizeof(real));

	if (mode == PLAT_OPEN_WRITE) {
		char parents[1024];
		snprintf(parents, sizeof(parents), "%s", real);
		host_mkdir_parents(parents);
	}

	f = fopen(real, mode == PLAT_OPEN_WRITE ? "wb" : "rb");
	if (f == NULL) return -1;

	for (i = 0; i < HOST_FILE_SLOTS; i++) {
		if (g_open_files[i] == NULL) {
			g_open_files[i] = f;
			*out = i;
			return 0;
		}
	}

	fclose(f);
	return -1;
}

int plat_file_read(plat_file_t f, void *buf, u32 len, u32 *nread)
{
	size_t got;

	if (nread) *nread = 0;
	if (f < 0 || f >= HOST_FILE_SLOTS || g_open_files[f] == NULL) return -1;
	if (len == 0) return 0;

	got = fread(buf, 1, len, g_open_files[f]);
	if (nread) *nread = (u32)got;
	return 0;
}

int plat_file_write(plat_file_t f, const void *buf, u32 len)
{
	if (f < 0 || f >= HOST_FILE_SLOTS || g_open_files[f] == NULL) return -1;
	if (len == 0) return 0;
	return fwrite(buf, 1, len, g_open_files[f]) == len ? 0 : -1;
}

int plat_file_close(plat_file_t f)
{
	if (f < 0 || f >= HOST_FILE_SLOTS || g_open_files[f] == NULL) return -1;
	fclose(g_open_files[f]);
	g_open_files[f] = NULL;
	return 0;
}

int plat_file_unlink(const char *path)
{
	char real[1024];
	host_path(path, real, sizeof(real));
	return remove(real) == 0 ? 0 : -1;
}

int plat_dir_create(const char *path)
{
	char real[1024];
	host_path(path, real, sizeof(real));
	host_mkdir_parents(real);
	host_mkdir_raw(real);
	return plat_path_exists(path, NULL, NULL) ? 0 : -1;
}

int plat_dir_remove(const char *path)
{
	char real[1024];
	host_path(path, real, sizeof(real));
#ifdef _WIN32
	return _rmdir(real) == 0 ? 0 : -1;
#else
	return rmdir(real) == 0 ? 0 : -1;
#endif
}

int plat_path_exists(const char *path, int *is_dir, u64 *size)
{
	char real[1024];
	struct stat sb;

	host_path(path, real, sizeof(real));
	if (stat(real, &sb) != 0) return 0;

	if (is_dir) *is_dir = (sb.st_mode & S_IFDIR) ? 1 : 0;
	if (size)   *size = (u64)sb.st_size;
	return 1;
}

/*
 * plat_dir_t only carries an int and a void*, so the platform-specific handle
 * (a HANDLE is 64-bit on Win64) lives in a small heap struct hung off `p`.
 */
struct host_dir {
#ifdef _WIN32
	HANDLE h;
	WIN32_FIND_DATAA pending;
	int has_pending;
#else
	DIR *dir;
#endif
};

int plat_dir_open(const char *path, plat_dir_t *d)
{
	char real[1024];
	struct host_dir *hd;

	memset(d, 0, sizeof(*d));
	d->handle = -1;
	snprintf(d->path, sizeof(d->path), "%s", path);

	host_path(path, real, sizeof(real));
	if (!plat_path_exists(path, NULL, NULL)) return -1;

	hd = (struct host_dir *)calloc(1, sizeof(struct host_dir));
	if (hd == NULL) return -1;

#ifdef _WIN32
	{
		char pattern[1024];
		snprintf(pattern, sizeof(pattern), "%s\\*", real);

		hd->h = FindFirstFileA(pattern, &hd->pending);
		if (hd->h == INVALID_HANDLE_VALUE) { free(hd); return -1; }
		hd->has_pending = 1;
	}
#else
	hd->dir = opendir(real);
	if (hd->dir == NULL) { free(hd); return -1; }
#endif

	d->p = hd;
	d->handle = 1;
	return 0;
}

int plat_dir_next(plat_dir_t *d, struct plat_dirent *e)
{
	struct host_dir *hd = (struct host_dir *)d->p;

	if (hd == NULL) return -1;

	memset(e, 0, sizeof(*e));

#ifdef _WIN32
	for (;;) {
		char child[1024];

		if (!hd->has_pending) {
			if (!FindNextFileA(hd->h, &hd->pending)) return 0;
		}
		hd->has_pending = 0;

		if (strcmp(hd->pending.cFileName, ".") == 0 ||
		    strcmp(hd->pending.cFileName, "..") == 0) {
			continue;
		}

		snprintf(e->name, sizeof(e->name), "%s", hd->pending.cFileName);
		e->is_dir = (hd->pending.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
		e->size = ((u64)hd->pending.nFileSizeHigh << 32) | (u64)hd->pending.nFileSizeLow;
		(void)child;
		return 1;
	}
#else
	for (;;) {
		struct dirent *de = readdir(hd->dir);
		char child[1024];

		if (de == NULL) return 0;
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

		snprintf(e->name, sizeof(e->name), "%s", de->d_name);

		snprintf(child, sizeof(child), "%s/%s", d->path, e->name);
		plat_path_exists(child, &e->is_dir, &e->size);
		return 1;
	}
#endif
}

void plat_dir_close(plat_dir_t *d)
{
	struct host_dir *hd = (struct host_dir *)d->p;

	if (hd == NULL) return;

#ifdef _WIN32
	if (hd->h != INVALID_HANDLE_VALUE) FindClose(hd->h);
#else
	if (hd->dir != NULL) closedir(hd->dir);
#endif

	free(hd);
	d->p = NULL;
	d->handle = -1;
}

/* ------------------------------------------------------------ page memory */

void *plat_alloc_pages(u32 size)
{
	return calloc(1, size);
}

void plat_free_pages(void *p)
{
	free(p);
}

/* ------------------------------------------------------------------- misc */

u32 plat_user_id(void)
{
	return 1;
}

/* ---------------------------------------------------------------- sockets */

int plat_socket_close(int s)
{
	if (s < 0) return -1;
#ifdef _WIN32
	return closesocket((SOCKET)s);
#else
	return close(s);
#endif
}

void plat_socket_shutdown(int s)
{
	if (s < 0) return;
#ifdef _WIN32
	shutdown((SOCKET)s, PLAT_SHUT_RDWR);
#else
	shutdown(s, PLAT_SHUT_RDWR);
#endif
}

int plat_net_errno(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

int plat_net_would_retry(int err)
{
#ifdef _WIN32
	return err == WSAEINTR;
#else
	return err == EINTR;
#endif
}
