/*
 * The bits of the host simulator that host_main.c drives from stdin. Not part of
 * the platform seam: nothing under src/core or src/games sees this file.
 */
#ifndef QWARK_PLAT_HOST_H
#define QWARK_PLAT_HOST_H

#include "../plat.h"

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

/* Where /dev_hdd0 is mapped on disk. */
const char *host_root(void);

#endif /* QWARK_PLAT_HOST_H */
