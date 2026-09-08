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
#define RAC1_LOADING_SCREEN   0x9645C4u   /* the force-okay-load block, 12 bytes */
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
#define RAC1_DREK_SKIP        0xFACC7Bu   /* one byte */
#define RAC1_DREK_CUTSCENE    0xFACC74u   /* one byte */
#define RAC1_MOBY_TABLE       0x0A390A0u
#define RAC1_MOBY_TABLE_END   0x0A390A8u
#define RAC1_MOBY_STRIDE      0x100u      /* sizeof(Moby) from rac1.cs */

/* Jankpot, from JankpotForm.cs. */
#define RAC1_JANKPOT_STATE    0xA15F2Cu   /* non-zero while bolt mining is on */
#define RAC1_JANKPOT_TIMER    0xA0FD14u   /* frames spent in the state */
#define RAC1_JANKPOT_BOLTS    0xA0FD18u

/* The shooting skill point block RAC1Form resets alongside RAC1_SHOOT_SP. */
#define RAC1_SONIC_SP         0xA15F3Cu   /* 8 bytes, Batalia Sonic Summoner */

/*
 * The savefile helper is a mod, not part of the game: it polls these bytes and
 * does the file work. 0xB00070 reads 1 only when the mod is loaded, so the
 * three requests are refused as UNSUPPORTED without it.
 */
#define RAC1_SAVEFILE_HELPER  0xB00070u
#define RAC1_SAVEFILE_LOAD    0xB00071u   /* write 1 */
#define RAC1_SAVEFILE_ASIDE   0xB00072u   /* write 1 */
#define RAC1_SAVEFILE_AUTO    0xB00073u   /* write 3, not 1 */

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
 */
#define RAC1_FP_ADDR 0x0007F558u
extern const u8 rac1_fp[4];
extern const u8 rac1_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

#define RAC1_HOT_INPUTS_ADDR RAC1_ANALOGS         /* 0x964A40 .. 0x964AF4 */
#define RAC1_HOT_INPUTS_LEN  0xB4
#define RAC1_HOT_PLAYER_ADDR RAC1_CURRENT_PLANET  /* 0x969C70 .. 0x969D80 */
#define RAC1_HOT_PLAYER_LEN  0x110

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
 */
#define F_FAST_LOADS        0
#define F_INFINITE_AMMO     1
#define F_INFINITE_HEALTH   2
#define F_GHOST             3
#define F_DIE               4
#define F_BOLTS             5
#define F_GOODIES           6
#define F_DREK_SKIP         7
#define F_DREK_CUTSCENE     8
#define F_FORCE_OKAY_LOAD   9
#define F_RESET_SHOOT_SP    10
#define F_SETUP_SHOOT_SP    11
#define F_RESET_GOLDBOLTS   12
#define F_UNLOCK_GOLDBOLTS  13
#define F_RESET_STYLE       14
#define F_UNLOCK_STYLE      15
#define F_ANYPCT_RESET      16
#define F_MAX_AMMO          17
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

/* -------------------------------------------------------- level flag regions */

#define RAC1_LF_MAIN_LEN  0x10
#define RAC1_LF_MISC_LEN  0x100
#define RAC1_LF_TOTAL     (RAC1_LF_MAIN_LEN + RAC1_LF_MISC_LEN)

/* ------------------------------------------------------ rac1_panel.c exports */

int rac1_trigger(u8 id);
int rac1_set_value(u8 id, u32 value);
int rac1_get_options(u8 id, const char * const **options, u8 *count);
int rac1_load_setaside(void);

int rac1_planet_load(u8 planet, u8 flags);

int rac1_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac1_levelflags_reset(u8 planet);
int rac1_levelflags_set(u8 planet, u16 offset, u8 value);

int rac1_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories);
int rac1_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac1_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac1.c exports */

u8  rac1_planet_count(void);
int rac1_debug_bit(u32 bit, int on);

#endif /* QWARK_RAC1_H */
