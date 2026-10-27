/*
 * The host build's private seam, between the common host code and whichever
 * backend is linked into the executable. Not part of the platform seam: nothing
 * under src/core or src/games sees this file.
 *
 *   plat_host.c     everything a host build needs whatever it talks to:
 *                   threads, mutexes, semaphores, sockets, files, directories,
 *                   time, log, trace and page memory
 *   backend_fake.c  the fake console qwark-host.exe runs: fake process memory
 *                   and the boot/quit/poke/pad commands host_main.c takes
 *   backend_pine.c  RPCS3 over PINE, which qwark-rpcs3.exe runs
 *
 * Exactly one backend is linked at a time: both define the same plat_* game and
 * memory entry points.
 */
#ifndef QWARK_PLAT_HOST_H
#define QWARK_PLAT_HOST_H

#include "../plat.h"

/* ------------------------------------------------- common host, plat_host.c */

/*
 * Brings up the parts of the platform that have nothing to do with the game:
 * winsock, the scheduler tick and the root directory. A backend's plat_init
 * calls this first.
 *
 * `path` is an explicit root and wins when it is neither NULL nor empty;
 * otherwise the root is <directory of the executable>/<name>.
 */
int  host_common_init(const char *path, const char *name);
void host_common_shutdown(void);

/* Where /dev_hdd0 is mapped on disk. */
const char *host_root(void);

/* "HH:MM:SS" for the log and notify lines. */
void host_stamp(char *out, u32 cap);

/* -------------------------------------------- the fake console, backend_fake.c */

/*
 * Pretends a game booted: allocates a fresh PID, sets the title, and seeds the
 * fake memory so the registered game's fingerprint matches at its declared
 * address. An unregistered title still boots, it just has nothing to seed.
 */
void host_boot(const char *title);

/*
 * The same, for a title that hosts more than one game. BCES01503 is RaC1, RaC2
 * or RaC3 depending on which disc executable is mapped, so `which` picks the
 * candidate to seed: "rac1".."rac4", "1".."4", or "dl" for Deadlocked. NULL or
 * an unrecognised name seeds the first candidate, which is what host_boot does.
 */
void host_boot_as(const char *title, const char *which);

/* Pretends the game exited. */
void host_quit(void);

/* Writes the running game's pad mask, big-endian, so combos can fire. */
int  host_set_pad(u32 mask);

int  host_poke(u32 addr, const u8 *data, u32 len);
int  host_peek(u32 addr, u8 *out, u32 len);

/*
 * One byte of the fake game, to read or write in place (the page is made if it
 * is not there). For the console menu's renderer under test; backend_fake.c only.
 */
u8  *host_mem_ptr(u32 addr);

/* How many reads the core has made of the fake process, for the boot-window test. */
u32  host_mem_reads(void);

/* How many times the core has asked the fake XMB for the running title. */
u32  host_title_calls(void);
u32  host_pid_calls(void);
u32  host_presence_calls(void);
#ifdef QWARK_TEST
void host_advance_time(u64 us);
void host_freeze_time(int on);
/* 1.4.1: how many folders have been opened for a walk since the program started (plat_dir_open). */
u32  host_dir_opens(void);

/*
 * Build 56: a USB drive pulled out mid-copy. Every path under `prefix` (say
 * "/dev_usb000") goes on working for `after_reads` more file reads from files
 * opened under it, and then that read and every later one fails, and every
 * open, folder open and stat under it fails as if nothing were there. 0 pulls
 * it at once; NULL puts it back.
 */
void host_pull_drive(const char *prefix, u32 after_reads);

/*
 * Build 57: a console's cellFs, which is less kind than fopen, for the files
 * under `prefix` (say "/dev_hdd0/qwark/config.txt"). Each field 0 leaves that
 * part as fopen has it; NULL for either argument puts everything back.
 *   read_chunk   every read hands back at most this many bytes, as cellFsRead
 *                may, so a reader that does not go on reading gets part of it
 *   open_fails   the next this many opens fail, the file there or not, as a
 *                busy drive, an FTP server holding the file or too many open
 *                files make cellFsOpen fail
 *   write_cut    a file opened for writing takes this many bytes in all; the
 *                write that would go past it stores what fits and fails
 *   close_fails  the next this many closes of a file opened for writing report
 *                a failure, the file closed all the same
 */
struct host_file_faults {
	u32 read_chunk;
	u32 open_fails;
	u32 write_cut;
	u32 close_fails;
};
void host_set_file_faults(const char *prefix, const struct host_file_faults *faults);

/*
 * The release check (test/test_release.c): counts every use of a path - an
 * open, a folder walk, a look, a make, a delete or a rename - under each of up
 * to four `prefixes`, from now on. NULL or 0 stops watching.
 */
void host_watch_paths(const char * const *prefixes, u32 n);
u32  host_watch_hits(u32 index);
#endif

void host_fail_writes(u32 addr, u32 count);

/*
 * The next `count` reads of the fake process fail, wherever they are, the way a
 * stalled emulator answers; 0 lets them through again.
 */
void host_fail_reads(u32 count);

/*
 * The writes the core has made since host_write_log_reset, oldest first: where
 * each landed and how many bytes it was. The count keeps going past the 64 kept.
 */
void host_write_log_reset(void);
u32  host_write_log_count(void);
int  host_write_log_at(u32 index, u32 *addr, u32 *len);

/*
 * The console notifications plat_notify has shown: how many since the program
 * started, and the text of the latest ("" before the first). Only
 * backend_fake.c defines these.
 */
u32  host_notify_count(void);
const char *host_notify_last(void);

/*
 * Test hooks. The fake backend answers 1 and 0 the way the PS3 does; forcing
 * them lets the unit tests drive the core's code-patch gating without a second
 * executable. Only backend_fake.c defines these.
 */
void host_set_can_patch_code(int on);
void host_set_emulator(int on);

/*
 * Build 54. The figure plat_vsh_memory answers in the simulator, in bytes; a
 * total of 0 (the start) answers -1, as under RPCS3. backend_fake.c only.
 */
void host_set_vsh_memory(u32 free_bytes, u32 total_bytes);

/*
 * Revision 1.24. The fake XMB's signed-in user (plat_user_id; 0 nobody), and
 * whether the console has users at all (plat_has_users; 0 is RPCS3's answer).
 * backend_fake.c only.
 */
void host_set_user(u32 user);
void host_set_has_users(int on);

/*
 * Revision 1.26, game modules. The simulator's loader (backend_fake.c) takes
 * a "module" file of its own making: a SELF header the core reads its pages
 * from, then a line naming the game, the build and the build ID, and,
 * optionally, a load failure to answer with (`fail`, a CELL error, 0 none).
 * host_write_fake_module writes one at `path` on the fake HDD; a `pages` of
 * 0 writes a file with no SELF header at all. Loading a module takes its pages
 * off the fake VSH's free memory (host_set_vsh_memory) and unloading gives
 * them back, and an unloaded module's vtable is overwritten with 0xA5 bytes,
 * so anything the core still holds of it fails at once.
 */
int  host_write_fake_module(const char *path, u8 game, u32 build, u32 build_id, u8 pages, u32 fail);
u32  host_fake_modules_loaded(void);
u32  host_fake_module_loads(void);

#endif /* QWARK_PLAT_HOST_H */
