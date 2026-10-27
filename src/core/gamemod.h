/*
 * The running game's code (revision 1.26, PROTOCOL.md 5.1.2 and 9.11): linked
 * into the module, or a game module (src/games/module.h) loaded once the
 * memory gate lets it.
 *
 * The session tells this file which title it is in (gamemod_begin, as it
 * enters INGAME) and asks it every tick whether the game has arrived
 * (gamemod_tick). A linked game arrives at once, inside gamemod_begin. A
 * module arrives once its file has been found, the VSH has had room for it
 * over the whole of a recent window, and lv2 has loaded and started it; or the
 * session learns there is none to be had (missing, refused, another build),
 * and carries on with no game. The session takes the game away again when the
 * process is gone (gamemod_unload), and every holder of the game's pointer
 * drops it first (the lock audit, PROTOCOL.md 9.11).
 *
 * Tick thread throughout, but for what a network thread uses: gamemod_flags2
 * and gamemod_info read single words the tick writes, and gamemod_touch sets
 * a flag the tick reads.
 */
#ifndef QWARK_GAMEMOD_H
#define QWARK_GAMEMOD_H

#include "../plat/plat.h"

struct game_api;
struct game_title;

/* What the session's game is, in GAME_MODULE's `state` byte. */
#define GM_NONE       0   /* no game: the XMB, or a title no game claims */
#define GM_LINKED     1   /* linked into the module: there from the session's start */
#define GM_CHECKING   2   /* a module is wanted; its file has not been looked at yet */
#define GM_WAITING    3   /* the file is there; the memory gate holds it back */
#define GM_LOADED     4   /* loaded and started */
#define GM_MISSING    5   /* no game-<n>.sprx */
#define GM_FAILED     6   /* lv2 would not load or start it, or it is not a module of ours */
#define GM_OTHER      7   /* module_start refused it: another build's */

/* What gamemod_tick (and gamemod_begin) says. */
#define GM_ARRIVED_NOTHING 0
#define GM_ARRIVED_GAME    1   /* gamemod_game() is the game now */
#define GM_ARRIVED_NONE    2   /* no game will come this session */

/* The gate's defaults, and config.txt's keys for them (PROTOCOL.md 9.11). */
#define GM_FLOOR_KB_DEFAULT   128u     /* game.floor_kb */
#define GM_WINDOW_MS_DEFAULT  3000u    /* game.window_ms */
#define GM_TRANSIENT_KB       64u      /* what a load takes for a moment besides its pages */
#define GM_NOTIFY_US          30000000ull   /* the one notification, this far into a wait */

/*
 * The session has a process of `t` (NULL: a title no game claims). Under the
 * core lock, from enter_ingame. Answers GM_ARRIVED_GAME when the game is here
 * already (linked, or a module kept over the XMB), and NOTHING otherwise.
 */
int gamemod_begin(const struct game_title *t);

/* Every tick while INGAME, outside the core lock: the file, the gate, the load. */
int gamemod_tick(void);

/* The game that arrived, or NULL. */
const struct game_api *gamemod_game(void);

/*
 * The process went. Answers 1 when the session's game was a module's, so the
 * session drops every pointer into it under the core lock; then
 * gamemod_unload, outside the lock, stops and unloads the module, unless
 * config.txt's `game.keep` keeps it for the same game's next boot.
 */
int  gamemod_leaving(void);
void gamemod_unload(void);

/* The module's stop: whatever is loaded goes, kept or not. Tick thread, on its way out. */
void gamemod_shutdown(void);

/* SessionInfo flags2's GAME_MODULE_BAD and GAME_WAITING bits. */
u8   gamemod_flags2(void);

/* GAME_MODULE's reply (PROTOCOL.md 5.1.2), GAME_MODULE_SIZE bytes. */
u32  gamemod_info(u8 *out, u32 cap);

/*
 * The game whose module `path` is (game-<n>.sprx or its .new under
 * /dev_hdd0/qwark/), 0 for none; and a client's write, rename or delete of
 * one: the session's game's is looked at again, if it was found wanting.
 */
u8   gamemod_names(const char *path);
void gamemod_touch_game(u8 game_id);

/* Whether the games are modules here (a test module; the simulator when told). */
int  gamemod_modules(void);

#ifdef QWARK_HOST
/* The simulator's `modules on|off`, and the tests': stand modules in for the linked games. */
void gamemod_set_modules(int on);
#endif

#endif /* QWARK_GAMEMOD_H */
