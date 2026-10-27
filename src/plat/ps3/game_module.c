/*
 * A game module's start and stop (revision 1.26; src/games/module.h,
 * PROTOCOL.md 9.11). One source, four modules: the makefile's `games` builds
 * it with QWARK_MODULE_GAME 1 to 4 into game-<n>.sprx, beside that game's own
 * sources, classic.c, its savefile helper's numbers and core_import.c.
 *
 * The core calls sys_prx_start_module on its tick thread with a struct
 * qwark_mod_args of its own. Start checks every field the core filled in
 * against what this module was built with and stays resident only when all of
 * them match: one `make` builds a core and its modules, so anything else is a
 * module from another build, which lv2 then drops again. Start and stop run
 * inside lv2's own calls, where r14 is not the core's context, so neither
 * reaches the core or the state area: they read and write the argument block
 * and nothing else.
 *
 * The module imports nothing, not even the VSH's C library qwark itself uses
 * (src/plat/ps3/libc.c), so the few C library functions the compiler calls
 * for struct copies and clears are here, as plain loops.
 */
#include "../../core/buildmodes.h"   /* first: Q, a register (src/games/module.h) */
#include "../../games/module.h"
#include "../../games/game.h"
#include "../../games/core_api.h"

#include <sys/prx.h>

#if !QWARK_GAME_MODULE || !defined(QWARK_MODULE_GAME)
#error "game_module.c is built by the makefile's games target alone"
#endif

#define QM_PASTE2(a, b, c) a##b##c
#define QM_PASTE(a, b, c)  QM_PASTE2(a, b, c)
#define QM_VTABLE          QM_PASTE(rac, QWARK_MODULE_GAME, _game)
#define QM_NAME            QM_PASTE(qwark_game, QWARK_MODULE_GAME, _)

extern const struct game_api QM_VTABLE;

SYS_MODULE_INFO(QM_NAME, 0, 1, 1);
SYS_MODULE_START(game_module_start);
SYS_MODULE_STOP(game_module_stop);

int game_module_start(size_t args, void *argp);
int game_module_stop(size_t args, void *argp);

/*
 * The module's stamp, for tools/gamemodule.py: the build and the build ID it
 * was made with, behind a marker, in its read-only data (the signed file is
 * encrypted, so the check reads the ELF before it is signed).
 */
const u32 qwark_game_module_stamp[4] = {
	0x51474D53u,   /* "QGMS" */
	QWARK_BUILD, QWARK_BUILD_ID, QWARK_MODULE_GAME
};

int game_module_start(size_t args, void *argp)
{
	struct qwark_mod_args *a = (struct qwark_mod_args *)argp;
	u8 why = QWARK_MOD_REFUSE_NONE;

	if (a == NULL || args < sizeof(*a) || a->magic != QWARK_MOD_MAGIC || a->size != sizeof(*a))
		return SYS_PRX_NO_RESIDENT;

	a->module_game = QWARK_MODULE_GAME;
	/* Read through a volatile, so the stamp is not folded away and the link keeps it. */
	a->module_build_id = ((const volatile u32 *)qwark_game_module_stamp)[2];
	a->game = NULL;

	if (a->build != QWARK_BUILD)                    why = QWARK_MOD_REFUSE_BUILD;
	else if (a->build_id != QWARK_BUILD_ID)         why = QWARK_MOD_REFUSE_ID;
	else if (a->api_size != sizeof(struct core_api)) why = QWARK_MOD_REFUSE_API;
	else if (a->state_size < QWARK_GAME_STATE_BYTES) why = QWARK_MOD_REFUSE_STATE;
	else if (a->game_id != QWARK_MODULE_GAME)       why = QWARK_MOD_REFUSE_GAME;

	a->refusal = why;
	if (why != QWARK_MOD_REFUSE_NONE) return SYS_PRX_NO_RESIDENT;

	a->game = &QM_VTABLE;
	return SYS_PRX_RESIDENT;
}

int game_module_stop(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	return SYS_PRX_STOP_OK;
}

/* ------------------------------------------------- the C library it needs */

void *memset(void *dst, int c, size_t n)
{
	u8 *d = (u8 *)dst;
	while (n--) *d++ = (u8)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;
	while (n--) *d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;
	if (d < s) {
		while (n--) *d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--) *--d = *--s;
	}
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = (const u8 *)a, *y = (const u8 *)b;
	for (; n; n--, x++, y++) {
		if (*x != *y) return (int)*x - (int)*y;
	}
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}
