/*
 * How the games are built into this binary (revision 1.26, game modules;
 * src/games/module.h has the whole story). Nothing is included here: a game's
 * source reads this first of all (by way of src/games/core_import.h), before
 * any header could declare a core function or define an inline one.
 *
 *   QWARK_GAME_MODULES  1 when the core reaches its games through core_api:
 *                       on a console, a test build whose games are SPRX files
 *                       of their own (src/core/gamemod.c); on a PC, the host
 *                       builds, whose games stay linked in but go through the
 *                       same table, and whose simulator can stand modules in
 *                       for them. 0 is a release module, whose games are
 *                       linked in and called directly, as before revision 1.26.
 *                       The makefile sets it to QWARK_DEV unless told
 *                       otherwise; `make release` pins it to 0.
 *   QWARK_GAME_MODULE   1 while this object is going into a game module
 *                       (the makefile's `games`)
 *   QWARK_GAME_IMPORT   the games of this binary call the core through
 *                       core_api's wrappers: either of the two above
 */
#ifndef QWARK_BUILDMODES_H
#define QWARK_BUILDMODES_H

#ifndef QWARK_DEV
#define QWARK_DEV               0
#endif

#ifndef QWARK_GAME_MODULES
#ifdef QWARK_HOST
#define QWARK_GAME_MODULES      1
#else
#define QWARK_GAME_MODULES      QWARK_DEV
#endif
#endif

#ifndef QWARK_GAME_MODULE
#define QWARK_GAME_MODULE       0
#endif

#define QWARK_GAME_IMPORT (QWARK_GAME_MODULES || QWARK_GAME_MODULE)

/*
 * Q, the core's context in a game module (src/games/module.h): r14, a global
 * register variable, which gcc takes only ahead of every function a unit
 * defines, inline ones in headers included, so it is declared here.
 */
#if QWARK_GAME_MODULE && !defined(QWARK_HOST)
struct qwark_ctx;
register struct qwark_ctx *Q __asm__("r14");
#endif

#endif /* QWARK_BUILDMODES_H */
