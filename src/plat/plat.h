/*
 * qwark platform seam.
 *
 * Everything under src/core/ and src/games/ talks to the outside world through
 * this header and plat_net.h. No PS3 (or Windows) header is ever visible past
 * this line: src/plat/ps3/plat_ps3.c and src/plat/host/plat_host.c implement it.
 */
#ifndef QWARK_PLAT_H
#define QWARK_PLAT_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

/*
 * The PS3 build already has these names from src/plat/ps3/types.h. That file is
 * included by the VSH glue, so plat_ps3.c defines QWARK_PLAT_TYPES_PROVIDED
 * before including us to avoid a duplicate typedef (gcc 4.1 rejects those).
 */
#ifndef QWARK_PLAT_TYPES_PROVIDED
typedef int8_t   s8;
typedef uint8_t  u8;
typedef int16_t  s16;
typedef uint16_t u16;
typedef int32_t  s32;
typedef uint32_t u32;
typedef int64_t  s64;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;
#endif

/* ------------------------------------------------------------------ life cycle */

int  plat_init(void);
void plat_shutdown(void);

/* ------------------------------------------------------------ console and game */

/* Non-zero while the console is running a game (VSH IS_INGAME). */
int  plat_game_running(void);

/* The game process id, 0 when there is no game process. */
u32  plat_game_pid(void);

/* NUL-terminated title id ("NPEA00385") into out. Returns 1 on success. */
int  plat_game_title(char out[16]);

/*
 * Whether writing an instruction word into the running game actually changes
 * what the game executes.
 *
 * 1 on the PS3 and in the host simulator. 0 under RPCS3: the emulator recompiles
 * PPU code and keeps running the translated block, so the word changes in memory
 * and the game carries on executing the old instruction. Rather than pretend a
 * patch worked, the core refuses everything that depends on one - patch_apply,
 * FEATURE_SET on a WRITES_CODE feature, a mod with patch words or caves - and
 * the games skip their embedded helpers. See PROTOCOL.md, SessionInfo flags
 * bit2 NO_CODE_PATCHES.
 */
int  plat_can_patch_code(void);

/*
 * Non-zero when the "console" is an emulator rather than real hardware, so a
 * client can say so and soften what it expects. SessionInfo flags bit1.
 */
int  plat_is_emulator(void);

/*
 * How many 120 Hz ticks the session leaves a newly appeared game process alone
 * before it reads a single byte of it, this platform's answer.
 *
 * On a console this is a safety number, not a nicety: the process id appears
 * when the VSH hands over, which is before the game has finished building
 * itself, and reading it in that state panics the machine. Everywhere else the
 * process is a fake or an emulator's and there is nothing to protect, so the
 * wait is short enough not to be felt. config.txt's `boot_delay_ms` overrides
 * whatever this says.
 */
u32  plat_boot_settle_ticks(void);

/* --------------------------------------------------------------- game memory */

#define PLAT_MEM_MAX 65536u

/* Both return 0 on success, negative on failure. len must be <= PLAT_MEM_MAX. */
int  plat_mem_read(u32 pid, u32 addr, void *buf, u32 len);
int  plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len);

/* --------------------------------------------------------------------- RSX */

/* pause != 0 pauses the RSX FIFO, 0 resumes it. */
void plat_rsx_pause(int pause);

/* ------------------------------------------------------------ notify and log */

void plat_notify(const char *msg);

/* printf2 semantics: integers and strings only, never a float conversion. */
void plat_log(const char *fmt, ...);

/*
 * Every log line is a file open, write and close on the PS3, so config's
 * `log = 0` silences it. Logging is on until config says otherwise.
 */
void plat_log_enable(int on);

/*
 * A bare, dependency-free trace line for bring-up. On the PS3 it is a single
 * sys_tty_write to channel 0, which is what Target Manager / ProDG shows, and
 * nothing else: no file, no mutex, no allocation. It is therefore the one log
 * call that is safe from the module entry point and from the earliest boot
 * steps, before any kernel object exists. On the host it goes to stderr.
 */
void plat_trace(const char *msg);

/* -------------------------------------------------------------------- time */

u64  plat_time_us(void);
void plat_sleep_us(u32 us);
void plat_yield(void);

/* ------------------------------------------------------------------ threads */

typedef u64 plat_thread_t;
#define PLAT_THREAD_NONE ((plat_thread_t)0)

typedef void (*plat_thread_fn)(void *arg);

int  plat_thread_create(plat_thread_t *out, plat_thread_fn fn, void *arg,
                        u32 stack_size, const char *name);

/*
 * A thread nobody will ever join: the platform releases it the moment it
 * returns, so it can never hold up a module unload and its stack is not kept
 * waiting for a join that is not coming. Client connection threads are created
 * this way, exactly as Ratchetron creates its (SYS_PPU_THREAD_CREATE_NORMAL).
 */
int  plat_thread_create_detached(plat_thread_fn fn, void *arg,
                                 u32 stack_size, const char *name);

int  plat_thread_join(plat_thread_t t);
void plat_thread_exit(void);

/*
 * Opaque, sized so a POSIX pthread_mutex_t / pthread_cond_t pair fits with room
 * to spare. Never inspect the contents outside the platform file.
 */
typedef struct plat_mutex { u64 opaque[16]; } plat_mutex_t;
typedef struct plat_sem   { u64 opaque[24]; } plat_sem_t;

int  plat_mutex_init(plat_mutex_t *m);
void plat_mutex_destroy(plat_mutex_t *m);
void plat_mutex_lock(plat_mutex_t *m);
void plat_mutex_unlock(plat_mutex_t *m);

int  plat_sem_init(plat_sem_t *s, u32 initial);
void plat_sem_destroy(plat_sem_t *s);
void plat_sem_post(plat_sem_t *s);
void plat_sem_wait(plat_sem_t *s);

/* -------------------------------------------------------------------- files */

typedef int plat_file_t;
#define PLAT_FILE_INVALID (-1)

#define PLAT_OPEN_READ  0
#define PLAT_OPEN_WRITE 1   /* create and truncate */

int  plat_file_open(const char *path, int mode, plat_file_t *out);
int  plat_file_read(plat_file_t f, void *buf, u32 len, u32 *nread);
int  plat_file_write(plat_file_t f, const void *buf, u32 len);
int  plat_file_close(plat_file_t f);
int  plat_file_unlink(const char *path);

/*
 * Moves a file within the same filesystem. cellFsRename on the console, rename()
 * on the host, and both of those differ about a destination that already exists:
 * POSIX replaces it silently, Windows and cellFs refuse. Nothing here settles
 * that, so a caller that cares checks first, which is what FILE_RENAME does.
 */
int  plat_file_rename(const char *from, const char *to);

int  plat_dir_create(const char *path);   /* one level; parents must exist */
int  plat_dir_remove(const char *path);   /* must be empty */

/* is_dir and size may be NULL. Returns 1 when the path exists. */
int  plat_path_exists(const char *path, int *is_dir, u64 *size);

struct plat_dirent {
	char name[256];
	int  is_dir;
	u64  size;
};

typedef struct plat_dir {
	int   handle;
	void *p;
	char  path[512];
} plat_dir_t;

int  plat_dir_open(const char *path, plat_dir_t *d);
int  plat_dir_next(plat_dir_t *d, struct plat_dirent *e); /* 1 entry, 0 end, <0 error */
void plat_dir_close(plat_dir_t *d);

/* -------------------------------------------------------------- page memory */

/*
 * A large, page-aligned block. On the PS3 this is sys_memory_allocate with 64 KB
 * pages, the way Ratchetron gets its per-connection transfer buffer; on the host
 * it is malloc. Used once per connection, never in the tick loop.
 */
void *plat_alloc_pages(u32 size);
void  plat_free_pages(void *p);

/* --------------------------------------------------------------------- misc */

u32  plat_user_id(void);

/* ------------------------------------------------------- big-endian helpers */

/*
 * The wire is big-endian and so is the PS3's memory; the host simulator is not.
 * Nothing anywhere memcpy's a native integer onto the wire, it goes through here.
 */

static __inline u16 be16_get(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u16)(((u16)b[0] << 8) | (u16)b[1]);
}

static __inline u32 be32_get(const void *p)
{
	const u8 *b = (const u8 *)p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static __inline u64 be64_get(const void *p)
{
	const u8 *b = (const u8 *)p;
	return ((u64)be32_get(b) << 32) | (u64)be32_get(b + 4);
}

static __inline void be16_put(void *p, u16 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v >> 8);
	b[1] = (u8)v;
}

static __inline void be32_put(void *p, u32 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v >> 24);
	b[1] = (u8)(v >> 16);
	b[2] = (u8)(v >> 8);
	b[3] = (u8)v;
}

static __inline void be64_put(void *p, u64 v)
{
	be32_put(p, (u32)(v >> 32));
	be32_put((u8 *)p + 4, (u32)v);
}

/* IEEE 754 single precision, big-endian, through a u32 so no aliasing games. */
static __inline f32 bef32_get(const void *p)
{
	union { u32 i; f32 f; } u;
	u.i = be32_get(p);
	return u.f;
}

static __inline void bef32_put(void *p, f32 v)
{
	union { u32 i; f32 f; } u;
	u.f = v;
	be32_put(p, u.i);
}

static __inline u32 f32_bits(f32 v)
{
	union { u32 i; f32 f; } u;
	u.f = v;
	return u.i;
}

static __inline f32 f32_from_bits(u32 v)
{
	union { u32 i; f32 f; } u;
	u.i = v;
	return u.f;
}

/*
 * Right-aligns a big-endian value of 1, 2, 4 or 8 bytes into a u64 (the shape
 * watches and freezes use on the wire), and the reverse.
 */
static __inline u64 be_get_sized(const void *p, u32 size)
{
	switch (size) {
	case 1: return (u64)(*(const u8 *)p);
	case 2: return (u64)be16_get(p);
	case 4: return (u64)be32_get(p);
	case 8: return be64_get(p);
	default: return 0;
	}
}

static __inline void be_put_sized(void *p, u32 size, u64 v)
{
	switch (size) {
	case 1: *(u8 *)p = (u8)v; break;
	case 2: be16_put(p, (u16)v); break;
	case 4: be32_put(p, (u32)v); break;
	case 8: be64_put(p, v); break;
	default: break;
	}
}

#endif /* QWARK_PLAT_H */
