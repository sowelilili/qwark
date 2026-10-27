/*
 * Game modules (SPEC-game-modules.md, design A1; PROTOCOL.md section 9.11).
 *
 * A test build's core carries no game: each game is its own SPRX, named for
 * the game (/dev_hdd0/qwark/rac1.sprx to rac4.sprx, the title index's
 * title_module_name), loaded when that game is running and there is
 * memory for it (src/core/gamemod.c), and unloaded once it quits. A module has
 * one R-X segment and nothing writable: its code, its tables, its descriptors
 * and its TOC, which lv2 relocates at the load. What it changes it keeps in the
 * core's memory, the lent state area, and it calls the core only through
 * core_api (tools/coreapi.py).
 *
 * Three ways the games are built:
 *
 *   QWARK_GAME_MODULE   this object goes into a game module (the PS3 makefile's
 *                       `games`): core calls through core_api, state in the
 *                       lent area, Q in r14
 *   QWARK_GAME_IMPORT   the games reach the core through core_api: a module,
 *                       and the host builds, whose games stay linked in
 *                       (QWARK_GAME_MODULES on a PC) so the seam is tested
 *   neither             a release module: the games linked in, calling the
 *                       core directly, exactly as before game modules
 */
#ifndef QWARK_GAME_MODULE_H
#define QWARK_GAME_MODULE_H

/* First: Q is a register in a game module, declared ahead of every function. */
#include "../core/buildmodes.h"

#include "../plat/plat.h"
#include "../core/proto.h"

struct core_api;

/*
 * The core's context, which a module's code finds through Q: the core's table
 * and the state area it lends. One for the module's life, the core's static.
 */
struct qwark_ctx {
	const struct core_api *api;
	void *state;
};

/*
 * A module has no writable memory to keep a pointer to the core in, and lv2
 * resolves nothing into a read-only segment for it, so on the console the
 * context lives in a register: r14, which every PPU function preserves (it is
 * callee-saved) and which the core and every module are compiled to leave
 * alone (-ffixed-r14). Each of the core's threads puts the context there as it
 * starts (src/plat/ps3/plat_ps3.c, thread_spawn), so whatever thread calls a
 * module finds it. A module's start and stop run inside lv2's own code and
 * must not use it; they only read and write their argument block.
 *
 * On a PC it is an ordinary pointer (src/core/gamemod.c).
 */
#if QWARK_GAME_IMPORT && defined(QWARK_HOST)
extern struct qwark_ctx *Q;
#endif

/*
 * The lent state area: a game's mutable statics, which a module cannot have.
 * The core zeroes it before every load. Its parts sit at fixed offsets: the
 * statics classic.c keeps for every game first, then the game's own, then its
 * panel file's. Each file holds its own struct to its part at compile time.
 * In a build whose games are linked in, each file keeps a static of its own
 * instead, as it always had, and nothing of this area exists.
 */
#define QWARK_STATE_CLASSIC_OFF   0u
#define QWARK_STATE_CLASSIC_MAX   2304u
#define QWARK_STATE_GAME_OFF      2304u
#define QWARK_STATE_GAME_MAX      2048u
#define QWARK_STATE_PANEL_OFF     4352u
#define QWARK_STATE_PANEL_MAX     1792u
#define QWARK_GAME_STATE_BYTES    6144u

#if QWARK_GAME_MODULE
#define QWARK_STATE_AT(type, off) ((type *)((u8 *)Q->state + (off)))
#endif

/*
 * What the core hands sys_prx_start_module, and what the module answers in
 * it. The module checks every IN field against its own and, unless all of them
 * match, answers SYS_PRX_NO_RESIDENT with `refusal` saying which, so lv2 drops
 * it: a module and a core always come out of one `make`.
 */
#define QWARK_MOD_MAGIC         0x51474D44u   /* "QGMD" */

#define QWARK_MOD_REFUSE_NONE   0
#define QWARK_MOD_REFUSE_ARGS   1             /* no block, or not this layout */
#define QWARK_MOD_REFUSE_BUILD  2             /* another QWARK_BUILD */
#define QWARK_MOD_REFUSE_ID     3             /* another build ID */
#define QWARK_MOD_REFUSE_API    4             /* another core_api */
#define QWARK_MOD_REFUSE_STATE  5             /* a state area too small for it */
#define QWARK_MOD_REFUSE_GAME   6             /* another game than the core asked for */

struct qwark_mod_args {
	u32 magic;                      /* IN: QWARK_MOD_MAGIC */
	u32 size;                       /* IN: sizeof(struct qwark_mod_args) */
	u32 build;                      /* IN: QWARK_BUILD */
	u32 build_id;                   /* IN: QWARK_BUILD_ID */
	u32 api_size;                   /* IN: sizeof(struct core_api) */
	u32 state_size;                 /* IN: QWARK_GAME_STATE_BYTES */
	u8  game_id;                    /* IN: the game the core expects */
	u8  refusal;                    /* OUT: QWARK_MOD_REFUSE_* */
	u8  module_game;                /* OUT: the module's own game */
	u8  pad;
	u32 module_build_id;            /* OUT: the module's own build ID */
	const struct game_api *game;    /* OUT: the module's vtable, NULL when it refused */
};

#endif /* QWARK_GAME_MODULE_H */
