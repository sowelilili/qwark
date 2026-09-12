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

/* How many reads the core has made of the fake process, for the boot-window test. */
u32  host_mem_reads(void);

/* How many times the core has asked the fake VSH what it is running. */
u32  host_vsh_calls(void);

/* Page allocations still live, which an idle connection must hold none of. */
u32  host_live_pages(void);

/* The fake process's PRX module count, which a test walks up as a game would. */
void host_set_module_count(int n);

/*
 * Test hooks. The fake backend answers 1 and 0 the way the PS3 does; forcing
 * them lets the unit tests drive the core's code-patch gating without a second
 * executable. Only backend_fake.c defines these.
 */
void host_set_can_patch_code(int on);
void host_set_emulator(int on);

#endif /* QWARK_PLAT_HOST_H */
