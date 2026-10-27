/*
 * The title index (SPEC-game-modules.md 4.5): what the core knows of each game
 * before any of the game's own code is there. Title ids, the game id, the
 * fingerprint that proves the executable is mapped (and, under BCES01503, says
 * which of the trilogy it is), the quit hook, and the few things that must
 * happen in the first moments of a boot, which cannot wait for a game module:
 *
 *   boot          patches the core puts in at a session's start, on a platform
 *                 that patches code, and never takes out: Deadlocked's quit
 *                 and loading hooks
 *   pause pair    a PAUSE emitted as the game quits and its RESUME the moment
 *                 the game is playable again, which game time hangs on and a
 *                 module loaded seconds later would miss (Deadlocked's quit to
 *                 the XMB)
 *
 * Resident in every build: src/games/titles.c. Linked into the core, where
 * the games themselves may not be.
 */
#ifndef QWARK_TITLES_H
#define QWARK_TITLES_H

#include "game.h"
#include "../core/mem.h"

/* A patch the core applies as a session starts, then the byte it zeroes after it (0: none). */
struct game_boot_patch {
	const struct patch_def *def;
	u32 clear_addr;
};

struct game_title {
	/* Up to four title ids, NULL-terminated. */
	const char *title_ids[4];
	u8 game_id;                     /* GAME_RAC1 .. GAME_RAC4: also which module (title_module_name) */

	/*
	 * Proof the executable is mapped: fp_len bytes at fp_addr must equal
	 * fp_bytes, or fp_alt when that is not NULL. The alternative exists so a
	 * console where another tool already patched the fingerprint site does not
	 * stall in BOOTING for ever.
	 *
	 * Build 50: or fp_switch when that is not NULL. Three games' fingerprints
	 * are instructions their code switches replace (src/games/switches/), so
	 * under RPCS3 with the switch patch in, the site holds the branch into the
	 * trampoline instead, and that proves the executable just as well. RaC1's
	 * switch leaves its fingerprint alone and it has none. A console never
	 * has the switch patch, so the index names it through GAME_FP_SWITCH
	 * (game.h) and on the PS3 it is NULL (build 51).
	 *
	 * Build 53: or fp_switch_old when that is not NULL, the branch an earlier
	 * build's switch patch put at the site before this build moved the
	 * trampoline it goes to. A game with that patch is still the game; its
	 * switches are from another build, which the switches' own look says
	 * (src/core/switches.h), and the client offers to write the patch again.
	 * fp_len 0 matches whatever is mapped.
	 */
	u32 fp_addr;
	const u8 *fp_bytes;
	const u8 *fp_alt;
	const u8 *fp_switch;
	const u8 *fp_switch_old;
	u16 fp_len;

	/*
	 * A game-side byte that goes non-zero when the player asked to quit, or 0
	 * when none is known; the session then relies on IS_INGAME and the PID.
	 */
	u32 quit_hook_addr;

	/* Where the pad mask lives, for the host simulator's `pad` command. */
	u32 pad_mask_addr;

	const struct game_boot_patch *boot;
	u8 nboot;

	/*
	 * The pause pair, 0 for none: the autosplit reason code a quit to the XMB
	 * PAUSEs under, and the byte a boot patch sets to 0xFF when the game is
	 * playable again, which RESUMEs it (and is zeroed). Where the boot patch
	 * could not go in (RPCS3) the RESUME goes out on the first tick back in a
	 * game instead.
	 */
	u8  pause_code;
	u32 resume_addr;

	/* The game's vtable where it is linked in; NULL in a test module, whose games are modules. */
	const struct game_api *linked;
};

/* Every entry registered for `title_id`, in order, at most `cap`. */
u32 title_candidates(const char *title_id, const struct game_title **out, u32 cap);

/* Walk the index, for the host simulator and the tests. NULL past the end. */
const struct game_title *title_at(u32 index);

/* The first entry of a game, NULL for none (GAME_NONE among them). */
const struct game_title *title_for_game(u8 game_id);

/*
 * The file a game's module is in, named for its game: "rac1.sprx" (RaC1) to
 * "rac4.sprx" (Deadlocked), NULL for a game id that has none. A table by game
 * id, never a number formatted into a name, so a later game takes a name of
 * its own (Tools of Destruction's will be "tod.sprx").
 */
#define GAME_MODULE_DIR "/dev_hdd0/qwark/"
#define GAME_MODULE_NAME_MAX 16      /* the longest name, its NUL and room to spare */
const char *title_module_name(u8 game_id);

/* "/dev_hdd0/qwark/<name>", or its ".new"; "" for a game with no module. */
void title_module_path(u8 game_id, char *out, u32 cap, int dot_new);

#endif /* QWARK_TITLES_H */
