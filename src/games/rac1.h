/*
 * Ratchet & Clank (NPEA00385): the address table and the seam between rac1.c
 * (numbers, hot block, descriptors, vtable) and rac1_panel.c (the handlers that
 * do the work). Nothing outside those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC1. The ones that were
 * ambiguous in the old code carry a comment saying what was assumed.
 */
#ifndef QWARK_RAC1_H
#define QWARK_RAC1_H

#include "game.h"

/* ------------------------------------------------------- rac1.cs addresses */

#define RAC1_INPUTS           0x964AF0u   /* u32 pad mask, OG layout */
#define RAC1_ANALOGS          0x964A40u   /* f32 rx, ry, lx, ly */
#define RAC1_CURRENT_PLANET   0x969C70u
#define RAC1_BOLTS            0x969CA0u
#define RAC1_GOLD_ITEMS       0x969CA8u   /* one byte per unlock index */
/*
 * These two are the same value at two widths: rac1.cs calls 0x969CD0 read as a
 * word "ngPlusGoodies" and its low byte at 0x969CD3 "goodiesMenu", and the
 * goodies toggle writes that byte. Both are reported, because the old client
 * used them for different things, but turning the toggle on moves both.
 */
#define RAC1_NGPLUS_GOODIES   0x969CD0u   /* u32 */
#define RAC1_GOODIES_MENU     0x969CD3u   /* its low byte */
#define RAC1_COORDS           0x969D60u   /* f32 x, y, z (Vec4) */
#define RAC1_GHOST_TIMER      0x969EACu
#define RAC1_STYLE_POINTS     0x96C08Cu   /* 30 bytes */
#define RAC1_AMMO_BASE        0x96C0ACu   /* per-unlock ammo words, see the +8 below */
#define RAC1_UNLOCK_ARRAY     0x96C140u   /* one byte per unlock index */
#define RAC1_MOVIE_FLAGS      0x96BFF0u   /* also the five index-less items */
#define RAC1_SHOOT_SP         0x96C9DCu   /* 32 bytes, eight words */
#define RAC1_NGPLUS_STATE     0x96C9FCu   /* challenge mode */
#define RAC1_INFOBOT_FLAGS    0x96CA0Cu
#define RAC1_LOAD_PLANET      0xA10700u   /* request word, then the planet index */
#define RAC1_GOLD_BOLTS       0xA0CA34u   /* 4 per planet, 80 bytes */
#define RAC1_LEVEL_FLAGS      0xA0CA84u   /* 0x10 per planet */
#define RAC1_MISC_LEVEL_FLAGS 0xA0CD1Cu   /* 0x100 per planet */
#define RAC1_ANYPCT_FLAG      0xA0CD04u   /* Blarg bridge */
#define RAC1_ANYPCT_RACE      0xE5EFD0u   /* three floats, the Rilgar race */
#define RAC1_DEBUG_UPDATE     0x95C5C8u   /* bit field, see RAC1_DBG_* */
#define RAC1_DEBUG_MODE       0x95C5D4u   /* 0 normal, 1 freecam, 2 freecam character */
/*
 * The two final-cutscene flags, each a word holding 0 or 1, the way the game
 * writes them (stw: the cutscene's 1 at 0x3D3F54 and 0 at 0x277B88, the skip's
 * 1 at 0x276AC8 and 0 at 0x279DAC) and reads them (lwz and a test against 0,
 * at 0x279830 for the cutscene and 0x273A90, 0x276A88, 0x276DF0, 0x27C680 and
 * 0x27C6BC for the skip). Nothing reads or writes them a byte at a time. The
 * old buttons wrote one byte, at 0xFACC74 and 0xFACC7B, which set the
 * cutscene's high byte rather than its value; build 53's manip groups write
 * both as words.
 */
#define RAC1_DREK_SKIP        0xFACC78u   /* u32, 0 or 1 */
#define RAC1_DREK_CUTSCENE    0xFACC74u   /* u32, 0 or 1 */
/*
 * Build 53, Freeze health (classic.h). rac1.cs names the hit points, a word;
 * the maximum is the game's own word beside its other globals, from the
 * NPEA00385 executable: 4 from the new-game reset (0xE7190), 5 and 8 when the
 * Premium and Ultra Nanotech are bought (0x24A940, 0x24AA10) or a save that has
 * them is loaded (0x13E950, 0x13E970), and the respawn puts the hit points back
 * to it (0x7F2E0).
 */
#define RAC1_PLAYER_HEALTH    0x96BF88u   /* s32 */
#define RAC1_MAX_HEALTH       0x71FB28u   /* s32: 4, 5 or 8 */
#define RAC1_MOBY_TABLE       0x0A390A0u
#define RAC1_MOBY_TABLE_END   0x0A390A8u
#define RAC1_MOBY_STRIDE      0x100u      /* sizeof(Moby) from rac1.cs */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * rac1.cs AutosplitterAddresses, in the order rac1-autosplitter.asl reads them
 * back. The x and y floats are the first two words of RAC1_COORDS, which the hot
 * block already decodes.
 */
#define RAC1_DEST_PLANET      (RAC1_LOAD_PLANET + 4)  /* 0xA10704, low byte at +3 */
#define RAC1_GAME_STATE       0xA10708u
#define RAC1_PLANET_FRAMES    0xA10710u
#define RAC1_PLAYER_STATE     0x96BD64u   /* the script reads the halfword at +2 */
#define RAC1_KALEBO_BOLT      0xA0CA75u   /* inside RAC1_GOLD_BOLTS, Kalebo3 +1 */

/*
 * rac1.cs loadingScreenID, whose low byte the script's isLoading block read: the
 * game is on a loading screen whenever this is anything but 4. Protocol 1.5
 * turns that into a LOAD_START / LOAD_END pair the client normalises.
 */
#define RAC1_LOADING_SCREEN   0x9645CBu   /* low byte of the word at 0x9645C8 */
#define RAC1_LOADING_IDLE     4           /* what it reads when nothing is loading */

/*
 * The gold-bolt / skill-point / item / infobot counters are not the game's: the
 * "gb_sp_as_helper" mod keeps them, one word each, and the old client read the
 * low byte of every word. qwark embeds that mod (see rac1.c) and writes it on
 * every entry, so codes 4 to 7 work without anyone loading anything.
 */
#define RAC1_AS_COUNTERS      0xAFF000u
#define RAC1_AS_GOLDBOLTS     0xAFF000u
#define RAC1_AS_SKILLPOINTS   0xAFF010u
#define RAC1_AS_ITEMS         0xAFF020u
#define RAC1_AS_INFOBOTS      0xAFF030u

/* The two index-less items the script watches beside the item counter. */
#define RAC1_ITEM_CODEBOT     (RAC1_MOVIE_FLAGS + 1)   /* 0x96BFF1 */
#define RAC1_ITEM_RARI        (RAC1_MOVIE_FLAGS + 2)   /* 0x96BFF2 */

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R1_AS_PLANET      1
#define R1_AS_VELDIN      2
#define R1_AS_DREK_BUTTON 3
#define R1_AS_GOLD_BOLT   4
#define R1_AS_SKILL_POINT 5
#define R1_AS_ITEM        6
#define R1_AS_INFOBOT     7
#define R1_AS_LOADING     8   /* LOAD_START / LOAD_END, not a split */

/* Jankpot, from JankpotForm.cs. */
#define RAC1_JANKPOT_STATE    0xA15F2Cu   /* non-zero while bolt mining is on */
#define RAC1_JANKPOT_TIMER    0xA0FD14u   /* frames spent in the state */
#define RAC1_JANKPOT_BOLTS    0xA0FD18u

/* The shooting skill point block RAC1Form resets alongside RAC1_SHOOT_SP. */
#define RAC1_SONIC_SP         0xA15F3Cu   /* 8 bytes, Batalia Sonic Summoner */

/*
 * The savefile helper's request bytes. qwark embeds the helper and installs it
 * on the first request of a session (src/core/savefile.c), so the helper byte
 * reads 1 as soon as the game has reached the hook once. The set-aside and load
 * bytes beside it are driven through the shared table in
 * src/games/sfhelper_bins.c; the two named here are the ones this game reads for
 * itself - the helper byte feeds the readout, and the autosave byte is RaC1's
 * alone.
 *
 * Build 53: they were the mod's 0xB00070 and 0xB00073, inside the game's bump
 * arena; src/games/sfhelper/sf_rac1.h has the move.
 */
#define RAC1_SAVEFILE_HELPER  0x1080000u
#define RAC1_SAVEFILE_AUTO    0x1080003u  /* write 3, not 1: the autosave mode */

/*
 * Build 53: what the savefile's readiness test reads (rac1_savefile_ready). The
 * game's save buffer is *RAC1_SAVEDATA_PTR + RAC1_SAVEDATA_BIAS, and it holds a
 * save only once the game has saved: the serialiser (0xE7984) writes the two
 * block sizes below at its head, and the restore (0xE6FE4) takes nothing else.
 * The planet the save was made on is the word at RAC1_SAVE_PLANET_OFFSET, one of
 * RAC1_PLANET_COUNT.
 *
 * A load goes from the start menu, which the helper enters itself from any of
 * the states the main loop runs (RAC1_GAME_STATE -1 to 7) but a movie (1), which
 * plays in a loop of its own until it ends; and not while a planet load is
 * already on its way. src/games/sfhelper/sf_rac1.h has the states.
 */
#define RAC1_SAVEDATA_PTR       0xA10928u
#define RAC1_SAVEDATA_BIAS      0x100000u
#define RAC1_SAVE_HEAD_GLOBAL   0x153Cu
#define RAC1_SAVE_HEAD_PLANET   0x82A4u
#define RAC1_SAVE_PLANET_OFFSET 0x18u
#define RAC1_PLANET_COUNT       20u
#define RAC1_STATE_MOVIE        1u
#define RAC1_STATE_LAST         7u   /* the states run are -1 .. RAC1_STATE_LAST */

/*
 * Revision 1.21, the manips' file-load trigger (rac1.c). Game state 6 is the
 * planet transition and a level's entry, intro and ship cutscenes, not the
 * title: the title loop runs states 0, 1, 3 and 4. RAC1_IN_FRONTEND is the
 * game's own front-end byte, 1 from the front end's start (0x12F00, and the
 * top loop's 0x13DD8 on the way back to it) until it hands over to the first
 * level (0x13728).
 */
#define RAC1_STATE_TRANSITION   6u
#define RAC1_IN_FRONTEND        0xA15F38u

/* Bits of RAC1_DEBUG_UPDATE, from rac1.cs SetDebugOption. */
#define RAC1_DBG_RATCHET   0x1u
#define RAC1_DBG_MOBYS     0x2u
#define RAC1_DBG_PARTICLES 0x4u
#define RAC1_DBG_CAMERA    0x8u

#define RAC1_POS_BLOB_LEN  30          /* what racman stores for a position slot */

/*
 * Fingerprint: the original instruction at the infinite-health patch site,
 * 0x30649CE0 ("addic r3, r4, -25376"), which racman restores when the toggle is
 * turned off. It proves the executable is mapped and initialised before qwark
 * writes anything.
 *
 * qwark's own patched form, 0x30640000, is accepted too: a console where another
 * tool (or a previous qwark session that never got to revert) already patched
 * this instruction would otherwise sit in BOOTING for ever. Both forms are the
 * same four bytes at the same address, so either one still proves the map.
 *
 * Build 53: qwark no longer patches it. Infinite health became Freeze health
 * (F_FREEZE_HEALTH below), a freeze on the hit points, and the patched form
 * stays accepted for the tools and the earlier builds that still write it.
 */
#define RAC1_FP_ADDR 0x0007F558u
extern const u8 rac1_fp[4];
extern const u8 rac1_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

#define RAC1_HOT_INPUTS_ADDR RAC1_ANALOGS         /* 0x964A40 .. 0x964AF4 */
#define RAC1_HOT_INPUTS_LEN  0xB4
#define RAC1_HOT_PLAYER_ADDR RAC1_CURRENT_PLANET  /* 0x969C70 .. 0x969D80 */
#define RAC1_HOT_PLAYER_LEN  0x110

/* state: player state at +0x00 through the two index-less items at +0x28E */
#define RAC1_HOT_STATE_ADDR  RAC1_PLAYER_STATE
#define RAC1_HOT_STATE_LEN   0x290
/* load: the load request at +0x00, game state at +0x08, planet frames at +0x10 */
#define RAC1_HOT_LOAD_ADDR   RAC1_LOAD_PLANET
#define RAC1_HOT_LOAD_LEN    0x14
/* the helper mod's four counters, 0x10 apart */
#define RAC1_HOT_AS_LEN      0x34

/* ----------------------------------------------------------- readout slots */

#define RAC1_RO_BOLTS          0
#define RAC1_RO_SAVEFILE       1
#define RAC1_RO_JANK_STATE     2
#define RAC1_RO_JANK_BOLTS     3
#define RAC1_RO_JANK_TIMER     4
#define RAC1_RO_NGPLUS_GOODIES 5
#define RAC1_RO_NGPLUS_STATE   6
#define RAC1_RO_CAMERA         7
#define RAC1_RO_DBG_HERO       8
#define RAC1_RO_DBG_MOBYS      9
#define RAC1_RO_DBG_PART       10
#define RAC1_RO_GOODIES        11

/* ------------------------------------------------------------- feature ids */

/*
 * Stable: a client may hold on to an id across sessions and versions, so new
 * features go on the end and a retired one leaves its number behind.
 * Retired, never to be reused: 9 (force okay load), and since build 53 the
 * manip buttons the manip groups replaced (protocol 1.21): 7 (Drek skip),
 * 8 (Drek cutscene), 10 (reset shooting skill points), 11 (setup shooting skill
 * points) and 16 (any% all-missions reset); and 17 (max ammo, all weapons),
 * which infinite ammo covers, and 28 (below).
 *
 * Build 53: 2 was Infinite health, the code patch at RAC1_FP_ADDR, and is
 * Freeze health now, held at the game's maximum like the other three games'
 * (classic.h). It keeps the number the previous-session record and a client
 * already know, and rac1.c names the old label (rac1_former), so an auto flag
 * ticked under it still counts.
 */
#define F_FAST_LOADS        0
#define F_INFINITE_AMMO     1
#define F_FREEZE_HEALTH     2
#define F_GHOST             3
#define F_DIE               4
#define F_BOLTS             5
#define F_GOODIES           6
/* 7 and 8 retired: Drek skip, Drek cutscene */
/* 9 retired */
/* 10 and 11 retired: reset and setup shooting skill points */
#define F_RESET_GOLDBOLTS   12
#define F_UNLOCK_GOLDBOLTS  13
#define F_RESET_STYLE       14
#define F_UNLOCK_STYLE      15
/* 16 retired: any% all-missions reset */
/* 17 retired: max ammo, all weapons */
#define F_LOAD_SETASIDE     18
#define F_SET_ASIDE_FILE    19
#define F_FORCE_AUTOSAVE    20
#define F_JANK_BOLTS        21
#define F_JANK_TIMER        22
#define F_JANK_ACTIVATE     23
#define F_DBG_RATCHET       24
#define F_DBG_MOBYS         25
#define F_DBG_PARTICLES     26
#define F_DBG_CAMERA        27

/*
 * 28 retired: Freeze health's own row, while it sat beside Infinite health
 * earlier in build 53; it is id 2 now.
 */

/* -------------------------------------------------------- level flag regions */

#define RAC1_LF_MAIN_LEN  0x10
#define RAC1_LF_MISC_LEN  0x100
#define RAC1_LF_TOTAL     (RAC1_LF_MAIN_LEN + RAC1_LF_MISC_LEN)

/* ------------------------------------------------------ rac1_panel.c exports */

int rac1_trigger(u8 id);
int rac1_set_value(u8 id, u32 value);
int rac1_get_options(u8 id, const char * const **options, u8 *count);
int rac1_load_setaside(void);
u8  rac1_savefile_ready(u8 what);

int rac1_planet_load(u8 planet, u8 flags);

int rac1_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac1_levelflags_reset(u8 planet);
int rac1_levelflags_set(u8 planet, u16 offset, u8 value);

int rac1_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac1_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac1_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac1.c exports */

u8  rac1_planet_count(void);
int rac1_debug_bit(u32 bit, int on);

/*
 * Build 53: the fast-load patch, forced around a planet load and a set-aside
 * load as RaC2's is. rac1_fastload_forced is 1 while a load's force is on.
 */
int rac1_fastload_force(void);
int rac1_fastload_forced(void);
int rac1_fastload_restore(void);

#endif /* QWARK_RAC1_H */
