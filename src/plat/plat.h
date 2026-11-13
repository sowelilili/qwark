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

/*
 * Presence is separate from PID discovery so the session can leave the console
 * alone for a second after IS_INGAME, before making any process queries.
 */
int  plat_game_running(void);
u32  plat_game_pid(void); /* 0 when no PID is ready. Call only after presence. */

/* NUL-terminated title id ("NPEA00385") into out. Returns 1 on success. */
int  plat_game_title(char out[16]);

/*
 * QWARK_EMULATOR (build 51) is defined by the host builds, build-host.sh and
 * test/run.sh, and never by the PS3 makefile. Everything that exists only for a
 * platform that refuses code patches - the RPCS3 patch ops, the presence looks,
 * the code switches - is compiled only where it is defined, so the SPRX carries
 * none of it. The host builds are always emulator builds: the simulator stands
 * in for RPCS3 in the tests as well as for a console.
 */
#if defined(QWARK_HOST) && !defined(QWARK_EMULATOR)
#error "the host builds define QWARK_EMULATOR (build-host.sh, test/run.sh)"
#endif

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
 *
 * plat_is_emulator: non-zero when the "console" is an emulator rather than real
 * hardware, so a client can say so and soften what it expects. SessionInfo
 * flags bit1.
 *
 * On the PS3 both are constants, so every branch that exists for RPCS3 folds
 * away at compile time rather than riding along in the module.
 */
#ifdef QWARK_EMULATOR
int  plat_can_patch_code(void);
int  plat_is_emulator(void);
#else
static __inline int plat_can_patch_code(void) { return 1; }
static __inline int plat_is_emulator(void)    { return 0; }
#endif

/* --------------------------------------------------------------- game memory */

#define PLAT_MEM_MAX 65536u

/* Both return 0 on success, negative on failure. len must be <= PLAT_MEM_MAX. */
int  plat_mem_read(u32 pid, u32 addr, void *buf, u32 len);
int  plat_mem_write(u32 pid, u32 addr, const void *buf, u32 len);

/* ------------------------------------------------------------ notify and log */

void plat_notify(const char *msg);

/* printf2 semantics: integers and strings only, never a float conversion. */
void plat_log(const char *fmt, ...);
/* The same with its arguments as a va_list: what a game module's plat_log calls (core_api). */
void plat_vlog(const char *fmt, va_list args);

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
 * Opaque. Never inspect the contents outside the platform file.
 *
 * The host builds size them so a POSIX pthread_mutex_t / pthread_cond_t pair
 * fits with room to spare. The PS3 keeps nothing in one but the kernel object's
 * id, a 32-bit sys_mutex_t or sys_semaphore_t, so there they are that and no
 * more: at the host's sizes the eight mutexes and the command ring's sixteen
 * semaphores were 4 KB of the module's RW segment (SPEC-footprint.md, cut C10).
 * plat_ps3.c checks at build time that the ids fit.
 */
#ifdef QWARK_HOST
typedef struct plat_mutex { u64 opaque[16]; } plat_mutex_t;
typedef struct plat_sem   { u64 opaque[24]; } plat_sem_t;
#else
typedef struct plat_mutex { u32 opaque[1]; } plat_mutex_t;
typedef struct plat_sem   { u32 opaque[1]; } plat_sem_t;
#endif

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
/*
 * Build 55. Reads at an offset from the file's start, wherever the file's own
 * position is: cellFsReadWithOffset on the console. ui.pak's reader takes one
 * game's renderer out of the middle of the file with it (src/core/uipak.c).
 */
int  plat_file_read_at(plat_file_t f, u32 off, void *buf, u32 len, u32 *nread);
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

/*
 * There is no page allocator here any more. Until revision 1.11 a connection
 * took one to hold a request and its reply, because a frame could be 65600
 * bytes; on the console that was sys_memory_allocate out of the VSH's own pool,
 * the same pool every other plugin and the XMB draw on. A frame is 16384 now
 * and net.c keeps two static buffers of that size for every connection to share,
 * so the module asks the console for no memory once it has loaded.
 */

/* --------------------------------------------------------------------- misc */

/*
 * The XMB's signed-in user number, the eight digits of /dev_hdd0/home/<user>:
 * 1 for 00000001. 0, or anything past 99999999, is nobody signed in yet (the
 * VSH answers a negative number at its user-select screen). The PS3 asks the
 * VSH each call (xsetting's GetCurrentUserNumber), so the session asks once a
 * second at the XMB and never in a game.
 */
u32  plat_user_id(void);

/*
 * Revision 1.24, build 56. Whether the console has users to keep settings
 * profiles for (src/core/config.h, the profiles). 1 on the PS3 and in the
 * simulator; 0 under RPCS3, which has one user, so per-user settings are not
 * offered there at all.
 */
#ifdef QWARK_EMULATOR
int  plat_has_users(void);
#else
static __inline int plat_has_users(void) { return 1; }
#endif

/*
 * Build 54. The VSH's own memory, in bytes: what is free of its user memory,
 * and the total. On the PS3 it is lv2 syscall 352,
 * sys_memory_get_user_memory_size, read-only, which answers for the calling
 * process, and a VSH plugin's thread is the VSH's: the memory qwark's image and
 * stacks, every other plugin and the XMB all come out of. Fills both and
 * answers 0, or answers -1 where there is nothing to read: under RPCS3, and in
 * the simulator until its `vshmem` command gives it a figure. The session reads
 * it (session.c, PROTOCOL.md 5.1.1 VSH_MEMORY).
 */
int  plat_vsh_memory(u32 *free_bytes, u32 *total_bytes);

/* ------------------------------------------------- the VSH's own pad (probe) */

/*
 * Revision 1.31, the VSH pad probe (src/core/vshpad.c). The controller as the
 * VSH's own copy of libpad sees it: qwark is a VSH plugin, so on the PS3 these
 * are sys_io's cellPadGetInfo2 and cellPadGetData, the calls webMAN MOD's XMB
 * combos make, against the libpad the VSH has already initialised. Nothing here
 * ever calls cellPadInit or cellPadEnd, which belong to the VSH.
 *
 * plat_vsh_pad_info fills `out` and answers 0, or libpad's (negative) error.
 * plat_vsh_pad_read reads one port: `fresh` 1 when libpad handed over new data
 * (len > 0), 0 for its "same as before" (len 0), when the bytes are not filled.
 * Both answer PLAT_PAD_NONE where there is no such pad to read: the simulator
 * until a test or its `vshpad` command gives it one, and qwark-rpcs3 always.
 */
#define PLAT_PAD_NONE   1
#define PLAT_PAD_PORTS  7    /* CELL_PAD_MAX_PORT_NUM */

struct plat_pad_info {
	u32 max_connect;
	u32 now_connect;
	u32 system_info;                   /* bit0 CELL_PAD_INFO_INTERCEPTED */
	u32 port_status[PLAT_PAD_PORTS];   /* bit0 CELL_PAD_STATUS_CONNECTED */
	u32 device_type[PLAT_PAD_PORTS];
};

struct plat_pad {
	s32 len;        /* libpad's CellPadData len */
	u8  fresh;      /* len > 0: the bytes below are this read's */
	u8  digital1;   /* CELL_PAD_BTN_OFFSET_DIGITAL1: select, L3, R3, start, up, right, down, left */
	u8  digital2;   /* CELL_PAD_BTN_OFFSET_DIGITAL2: L2, R2, L1, R1, triangle, circle, cross, square */
	u8  stick[4];   /* rx, ry, lx, ly as libpad gives them, 0..255, 0x80 the centre */
};

int  plat_vsh_pad_info(struct plat_pad_info *out);
int  plat_vsh_pad_read(u32 port, struct plat_pad *out);

/* ------------------------------------------------- game modules (1.26) */

/*
 * A game module's load, start, stop and unload (src/core/gamemod.c), on the
 * calling thread, the tick. On the PS3 these are liblv2's user-mode sys_prx_*
 * calls, as the PRX probe ran them on CFW and HEN (src/plat/ps3/plat_ps3.c);
 * on a PC the simulator stands a copy of the linked game in for a module
 * (plat_host.c), and qwark-rpcs3 never asks. Each answers what lv2 answers:
 * load an id (>= 0) or a negative CELL error; the others 0 or one.
 * `modres` is what the module's start or stop returned: 0 is
 * SYS_PRX_RESIDENT, 1 SYS_PRX_NO_RESIDENT.
 *
 * plat_prx_contains: 1 when [p, p + len) lies inside the loaded module's
 * segments, 0 when it does not, -1 when that cannot be told. What a module's
 * start hands back is held to it before anything of it is called.
 */
int  plat_prx_load(const char *path);
int  plat_prx_start(int id, void *args, u32 size, int *modres);
int  plat_prx_stop(int id, void *args, u32 size, int *modres);
int  plat_prx_unload(int id);
int  plat_prx_contains(int id, const void *p, u32 len);

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
