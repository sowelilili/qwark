/*
 * Ratchet: Deadlocked (NPEA00423): the address table and the seam between rac4.c
 * (numbers, hot block, descriptors, vtable) and rac4_panel.c (the handlers).
 * Nothing outside those two files includes this.
 *
 * The addresses come from racman's RaCTrainer/Games/RAC4 (rac4.cs, RAC4Form.cs,
 * BotsUnlocksFactory.cs); the quit hook and the crash patches come from the
 * Deadlocked autosplitter SPRX, rac4-autosplitter-src/ratchetron.c. Where the
 * old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC4_H
#define QWARK_RAC4_H

#include "game.h"

/* -------------------------------------------------------- rac4.cs addresses */

#define RAC4_BOLTS             0x009C32E8u
#define RAC4_DREAD_POINTS      0x009C32F4u
#define RAC4_SKIN              0x009C32FBu  /* one byte, 22 skins */
#define RAC4_BADGES            0x009C3308u  /* six bytes */
#define RAC4_CHALLENGE_MODE    0x009C330Eu  /* one byte */
#define RAC4_RANGE             0x009C331Fu  /* one byte, 0 Marauder .. 4 Liberator */
#define RAC4_BOTS_UNLOCK_SAVE  0x009C3325u  /* 16 bytes, the saved copy */
#define RAC4_CAMERA_MODE       0x009C287Cu
#define RAC4_LOAD_PLANET_OLD   0x009C3240u  /* the autosplitter's "current planet" */
#define RAC4_BOTS_UNLOCK       0x009D2775u  /* 16 bytes, the live copy */
#define RAC4_CURRENT_CHALLENGE 0x009D195Cu

#define RAC4_SKIN_APPLY        0x0110D975u  /* written 1 after the skin byte */

#define RAC4_INPUTS            0x00B11F30u  /* u32 pad mask, OG layout */
#define RAC4_ANALOGS           0x00B1210Cu  /* f32 rx, ry, lx, ly */

#define RAC4_IN_GAME           0x00B1F460u  /* 0 in the main menu, 1 in game */
#define RAC4_PREV_PLANET       0x00B1F465u
#define RAC4_TUTORIAL_FLAGS    0x00B1F46Cu  /* the softlock fix watches byte +3 */
#define RAC4_IS_LOADING        0x00B0FD84u

#define RAC4_LEVEL_SAVES       0x00B27FF0u  /* MF_LevelSave[15], 0x304 each */
#define RAC4_GAME_STATE        0x00B3C5A0u  /* the fast-load restore watches +3 */
#define RAC4_FRAME_COUNTER     0x00B3C59Cu  /* the IL timer readout */
#define RAC4_LOAD_PLANET2      0x00B36DCCu  /* write 1 to start the load */
#define RAC4_TARGET_PLANET     0x00B36DD0u  /* the planet to load */
#define RAC4_CUTSCENE_PTR      0x00B36DE8u

#define RAC4_MOBY_TABLE        0x00C845A0u
#define RAC4_MOBY_TABLE_END    0x00C845A4u
#define RAC4_MOBY_STRIDE       0x100u

#define RAC4_COORDS            0x010D44D0u  /* Vec4 position then Vec4 rotation */
#define RAC4_COORDS2           0x010D7334u  /* position only, 0x10 bytes */
#define RAC4_GHOST_TIMER       0x010D47CEu
#define RAC4_CAMERA_LR         0x010D5DF0u  /* f32 yaw */
#define RAC4_CAMERA_UD         0x010D5E00u  /* f32 pitch */
#define RAC4_PLAYER_HEALTH     0x010D7250u
#define RAC4_PLAYER_STATE      0x010D69FCu

#define RAC4_CURRENT_PLANET    0x0119353Cu  /* 0 in the main menu */
#define RAC4_SAVE_INFO         0x011B1BD8u
#define RAC4_CHALLENGE_SEL     0x011A0AE0u
#define RAC4_SOFTLOCK          0x011C04C0u  /* written 0 on a fresh file */

/* Act tuning, one byte each. */
#define RAC4_TUNE_SHELLSHOCK   0x00A947D3u
#define RAC4_TUNE_REACTOR      0x00A94944u
#define RAC4_TUNE_EVISCERATOR  0x00A969E3u
#define RAC4_TUNE_ACE          0x00A96E43u
#define RAC4_TUNE_VOX          0x00A07C7Fu

/* The instruction sites RAC4Form patched. */
#define RAC4_AMMO_INSTR        0x001D7D18u  /* also the fingerprint site */
#define RAC4_FASTLOAD_INSTR_1  0x002A8AF4u  /* bl memcard_Save */
#define RAC4_FASTLOAD_INSTR_2  0x002A8C50u  /* bctrl g_fnTransitionUpdate */

/*
 * The autosplitter's quit hook: three words replacing some logging inside the
 * game's quit-requested callback, so the game itself stores 0xFF at
 * RAC4_QUIT_FLAG when the player asks to quit. That is the only game-side quit
 * signal any of the four titles has.
 */
#define RAC4_QUIT_HOOK_ADDR    0x00013780u
#define RAC4_QUIT_FLAG         0x01700000u

/*
 * The autosplitter's other hook, ADDR_LOADING_HOOK_1 / _2 and ADDR_LOADING_VAL:
 * ten words of trampoline at 0x11904 and a branch to them at 0x11884, so the
 * game stores 0xFF at RAC4_LOADING_VAL the moment it has put the SCE logo up.
 * That is the SPRX's STATUS_INGAME_PENDING gate — the point at which it decided
 * the game was really back and sent CMD_UNPAUSE — and it is where qwark's RESUME
 * belongs too, because the process reappears well before the game is playable.
 */
#define RAC4_LOADING_HOOK_1    0x00011884u  /* the branch into the trampoline */
#define RAC4_LOADING_HOOK_2    0x00011904u  /* the trampoline itself, ten words */
#define RAC4_LOADING_VAL       0x01710000u  /* reads 0xFF once the logo is up */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * Deadlocked is the one game whose detection already ran on the console: the old
 * SPRX (rac4-autosplitter-src/ratchetron.c) polled these words itself and sent
 * commands to the PC, and rac4-LC-autosplitter.asl consumed them. The addresses
 * are that SPRX's ADDR_* constants, and where the two disagree the LC script
 * wins, so the watcher below reproduces its `command` stream.
 *
 * The current planet is the SPRX's ADDR_CURRENT_PLANET, which is not the same
 * word as RAC4_CURRENT_PLANET: it reads 0 when the box is beaten, and both it
 * and the destination are taken modulo 0x14 for co-op. It is saved with the
 * file, so it is not the origin of a planet change: rac4.c keeps its own origin
 * planet for that, and this word is only read for the Vox split and the readout.
 */
#define RAC4_AS_PLANET       RAC4_LOAD_PLANET_OLD  /* 0x009C3240 */
#define RAC4_AS_PLANET_MOD   0x14u
#define RAC4_VOX_HP          0x449BEAD0u  /* f32, negative once Vox is beaten */

#define RAC4_PLANET_MAINMENU 0
#define RAC4_PLANET_DREADZONE 1
#define RAC4_PLANET_INTERIOR 15

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R4_AS_PLANET 1
#define R4_AS_VOX    2
#define R4_AS_QUIT   3   /* PAUSE / RESUME around a quit to the XMB, not a split */

/*
 * What the LC script added back on the way in: it stopped game time for the
 * whole quit and reload and then added 14.8 s, so a quit always costs exactly
 * that. NORMALISE with this parameter is the same arithmetic.
 */
#define RAC4_QUIT_PAUSE_US   14800000u

/* The savefile helper mod's three bytes. */
#define RAC4_SF_HELPER         0x015CD71Du
#define RAC4_SF_LOAD_ASIDE     0x015CD71Eu
#define RAC4_SF_SET_ASIDE      0x015CD71Fu

/* MF_LevelSave / MF_MissionSave, from rac4.cs. */
#define RAC4_LEVEL_SIZE        0x304u
#define RAC4_MISSION_SIZE      0x0Cu
#define RAC4_MISSION_STATUS    8u
#define RAC4_PLANET_SAVES      15
#define RAC4_MISSIONS_DREAD    64      /* planet 0 has the whole array */
#define RAC4_MISSIONS_OTHER    16

/* gameMode while the ship is in the space transition; the load ends when it leaves. */
#define RAC4_GAME_MODE_SPACE   6

#define RAC4_BOTS_COUNT        16
#define RAC4_SKIN_COUNT        22

/*
 * Position slots: 0x20 bytes of position and rotation, then the two camera
 * floats. Loading puts the 0x20 back at RAC4_COORDS and the first 0x10 at
 * RAC4_COORDS2; the camera is saved but not restored, exactly as rac4.cs has it.
 */
#define RAC4_POS_MAIN_LEN  0x20
#define RAC4_POS_ALT_LEN   0x10
#define RAC4_POS_BLOB_LEN  (RAC4_POS_MAIN_LEN + 8)

/*
 * Fingerprint: the original instruction at the refill-ammo patch site,
 * 0x4182000C ("beq +12"), which RAC4Form restores when the checkbox is cleared.
 * qwark's own patched form, the nop, is accepted too.
 */
#define RAC4_FP_ADDR RAC4_AMMO_INSTR
extern const u8 rac4_fp[4];
extern const u8 rac4_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

#define RAC4_HOT_INPUTS_ADDR RAC4_INPUTS
#define RAC4_HOT_INPUTS_LEN  0x1EC
#define RAC4_HOT_STATS_LEN   0x40      /* bolts through the saved bot unlocks */
#define RAC4_HOT_FLAGS_LEN   0x10      /* in-game byte through the tutorial flags */
/* autosplit: load request +0x00, target planet +0x04, cutscene pointer +0x1C */
#define RAC4_HOT_LOAD_ADDR   RAC4_LOAD_PLANET2
#define RAC4_HOT_LOAD_LEN    0x20

/* ----------------------------------------------------------- readout slots */

#define RAC4_RO_BOLTS      0
#define RAC4_RO_SAVEFILE   1
#define RAC4_RO_DREAD      2
#define RAC4_RO_CHALLENGE  3
#define RAC4_RO_FRAMES     4
#define RAC4_RO_CUR_CHAL   5
#define RAC4_RO_SKIN       6
#define RAC4_RO_IN_GAME    7

/* ------------------------------------------------------------- feature ids */

/* Stable: new features go on the end, a retired one leaves its number behind. */
#define R4_CRASH_PATCHES   0
#define R4_SOFTLOCK_FIX    1
#define R4_FAST_LOADS      2
#define R4_REFILL_AMMO     3
#define R4_FREEZE_HEALTH   4
#define R4_GHOST           5

#define R4_DIE             6
#define R4_BOLTS           7
#define R4_DREAD_POINTS    8
#define R4_CHALLENGE       9
#define R4_SKIN            10

#define R4_UNLOCK_PLANETS  11
#define R4_ACT_TUNE        12

#define R4_SET_ASIDE       13
#define R4_LOAD_ASIDE      14

/* ------------------------------------------------------ rac4_panel.c exports */

int rac4_trigger(u8 id);
int rac4_set_value(u8 id, u32 value);
int rac4_get_options(u8 id, const char * const **options, u8 *count);
int rac4_load_setaside(void);
int rac4_die(void);

int rac4_planet_load(u8 planet, u8 flags);

int rac4_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac4_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac4_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac4.c exports */

u8  rac4_planet_count(void);

/* The fast-load patch, forced on for a planet load and restored when it ends. */
int rac4_fastload_force(void);

#endif /* QWARK_RAC4_H */
