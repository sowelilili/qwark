/*
 * Ratchet & Clank 3: Up Your Arsenal (NPEA00387, and BCES01503 when the disc
 * trilogy boots it): the address table and the seam between rac3.c (numbers, hot
 * block, descriptors, vtable) and rac3_panel.c (the handlers). Nothing outside
 * those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC3: rac3.cs for the address
 * table and the setups, RAC3Form.cs for the button handlers, UYAUnlocks.cs for
 * the item table and RAC3Form.Designer.cs for the armour and ship-colour names.
 * Where the old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC3_H
#define QWARK_RAC3_H

#include "game.h"

/* -------------------------------------------------------- rac3.cs addresses */

#define RAC3_INPUTS            0x00D99370u  /* u32 pad mask, OG layout */
#define RAC3_ANALOGS           0x00D9954Cu  /* f32 rx, ry, lx, ly */
#define RAC3_LOADING_SCREEN_ID 0x00D99114u

#define RAC3_QE_OFFSET         0x00C1E2C0u  /* s16 */
#define RAC3_CURRENT_PLANET    0x00C1E438u
#define RAC3_ITEM_ARRAY        0x00C1E43Cu  /* one byte per item id: its version */
#define RAC3_BOLTS             0x00C1E4DCu
#define RAC3_CHALLENGE_MODE    0x00C1E50Eu  /* one byte */
#define RAC3_HEALTH_XP         0x00C1E510u
#define RAC3_CURRENT_ARMOR     0x00C1E51Cu  /* u16 */
#define RAC3_QUICK_SELECT      0x00C1E652u  /* one byte, quick-select pause */
#define RAC3_CC_FAKE_ITEM_A    0x00C1E660u  /* CC early: 0x41 */
#define RAC3_CC_FAKE_ITEM_B    0x00C1E664u  /* CC early: 0x41414141 */

#define RAC3_CB_PRIMARY_FRONT  0x00C33AB0u
#define RAC3_CB_PRIMARY_BACK   0x00C33AB4u
#define RAC3_CB_TINT_FRONT     0x00C33AC0u
#define RAC3_CB_TINT_BACK      0x00C33AC4u

#define RAC3_KLUNK_TUNING_2    0x00C36BCCu
#define RAC3_VID_COMIC_MENU    0x00C4F918u
#define RAC3_KLUNK_TUNING_1    0x00C9165Cu
#define RAC3_DROPSHIP_HEALTH   0x00C956A0u

#define RAC3_TROPHY_REFRESH    0x00D9E020u  /* write 1 after deleting the folder */

#define RAC3_COORDS            0x00DA2870u  /* f32 x, y, z (Vec4) */
#define RAC3_GHOST_TIMER       0x00DA29DEu
#define RAC3_PLAYER_STATE      0x00DA4DB4u
#define RAC3_PLAYER_HEALTH     0x00DA5040u
#define RAC3_SKILL_POINTS      0x00DA521Du  /* 30 bytes */
#define RAC3_AMMO_ARRAY        0x00DA5240u  /* + (ammo offset - 0x243) */
#define RAC3_SHIP_COLOUR       0x00DA55E8u  /* one byte */
#define RAC3_UNLOCK_ARRAY      0x00DA56ECu  /* + (unlock offset - 0x4A8) */
#define RAC3_EXP_ARRAY         0x00DA5824u  /* + (exp offset - 0x5F0) */
#define RAC3_VID_COMICS        0x00DA650Bu  /* five bytes, also in the unlock array */
#define RAC3_FILE_TIME         0x00DA64E0u  /* the IGT the old form edited */

#define RAC3_TITANIUM_BOLTS    0x00ECE53Du  /* 128 bytes */
#define RAC3_LEVEL_FLAGS       0x00ECE675u  /* 0x10 per planet, planet ids are 1-based */
#define RAC3_LOAD_PLANET       0x00EE9310u  /* request word, then the planet id */
#define RAC3_DEST_PLANET       0x00EE9314u
#define RAC3_GAME_STATE        0x00EE9334u
#define RAC3_NEFFY_TUNING      0x00EF6098u

#define RAC3_MOBY_TABLE        0x00F22260u
#define RAC3_MOBY_TABLE_END    0x00F22268u
#define RAC3_MOBY_STRIDE       0x100u

#define RAC3_FAST_LOAD_1       0x0134EBD4u  /* set to 3 to force the third screen */
#define RAC3_FAST_LOAD_2       0x0134EE70u  /* then 0x0101 a fifth of a second later */
#define RAC3_CC_HELP_DESK      0x0148A100u

#define RAC3_AMMO_INSTR        0x00182A88u  /* the ammo decrement; the fingerprint */

/*
 * The savefile helper is a mod. RAC3Form's own Load File button and the save
 * manager's pair are separate request bytes; all of them are gated on the helper
 * byte reading 1. 0xD9FF02 was the set-aside byte in a commented-out line of the
 * old form and is not exposed, because nothing ever drove it.
 */
#define RAC3_SF_HELPER         0x00D9FF00u
#define RAC3_SF_LOAD_ASIDE     0x00D9FF01u  /* loadFileButton */
#define RAC3_SF_MGR_SAVE       0x00D9FF03u  /* SavefileLoader api_savefile */
#define RAC3_SF_MGR_LOAD       0x00D9FF04u  /* SavefileLoader api_loadfile */

#define RAC3_POS_BLOB_LEN  30          /* what racman stores for a position slot */
#define RAC3_LF_LEN        0x10        /* level flag bytes per planet */

/* Aquatos: LoadPlanetSafe skips the fast-load arm for this planet id alone. */
#define RAC3_PLANET_AQUATOS 8

/* rac3.cs FastLoadTimer waits 200 ms; the tick loop runs at 120 Hz. */
#define RAC3_FASTLOAD_DELAY_TICKS 24

/*
 * Fingerprint: the original instruction at the infinite-ammo patch site,
 * 0x7C85312E, which racman restores when the toggle is turned off. qwark's own
 * patched form, the nop, is accepted too.
 */
#define RAC3_FP_ADDR RAC3_AMMO_INSTR
extern const u8 rac3_fp[4];
extern const u8 rac3_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

/* inputs: pad at +0, analogs at +0x1DC */
#define RAC3_HOT_INPUTS_ADDR  RAC3_INPUTS
#define RAC3_HOT_INPUTS_LEN   0x1EC
/* state: QE offset at +0 through the quick-select pause byte at +0x392 */
#define RAC3_HOT_STATE_ADDR   RAC3_QE_OFFSET
#define RAC3_HOT_STATE_LEN    0x394
/* player: the coordinate Vec4 plus the rotation behind it */
#define RAC3_HOT_PLAYER_ADDR  RAC3_COORDS
#define RAC3_HOT_PLAYER_LEN   0x20

/* ----------------------------------------------------------- readout slots */

#define RAC3_RO_BOLTS      0
#define RAC3_RO_SAVEFILE   1
#define RAC3_RO_CHALLENGE  2
#define RAC3_RO_HEALTH_XP  3
#define RAC3_RO_HEALTH     4
#define RAC3_RO_ARMOUR     5
#define RAC3_RO_QE_OFFSET  6
#define RAC3_RO_FILE_TIME  7
#define RAC3_RO_SHIP       8
#define RAC3_RO_CB_FRONT   9
#define RAC3_RO_CB_BACK    10
/* Tint front and back mirror each other; one readout and one control cover both. */
#define RAC3_RO_CB_TINT    11

/* ------------------------------------------------------------- feature ids */

/*
 * Stable: new features go on the end, a retired one leaves its number behind.
 * Retired, never to be reused: 4 (freeze Klunk tuning), 17 (make NG+ no-QE
 * file), 28 (give 1337 ammo), 29 (setup NG+ weapons) and 30 (equip the bomb
 * glove).
 */
#define R3_FREEZE_AMMO       0
#define R3_FREEZE_HEALTH     1
#define R3_OHKO              2
#define R3_GHOST             3
/* 4 retired */
#define R3_QS_PAUSE          5

#define R3_DIE               6
#define R3_BOLTS             7
#define R3_CHALLENGE         8
#define R3_HEALTH_XP         9
#define R3_HEALTH            10
#define R3_ARMOUR            11
#define R3_SHIP_COLOUR       12
#define R3_FILE_TIME         13
#define R3_QE_OFFSET         14
#define R3_VENDOR_QE         15

#define R3_SETUP_NGPLUS      16
/* 17 retired */
#define R3_CC_EARLY          18
#define R3_UNTUNE_BOSSES     19
#define R3_RESET_DROPSHIP    20
#define R3_RESET_TROPHIES    21
#define R3_UNLOCK_SKILL      22
#define R3_RESET_SKILL       23
#define R3_UNLOCK_TITANIUM   24
#define R3_RESET_TITANIUM    25
#define R3_UPGRADE_ALL       26
#define R3_DOWNGRADE_ALL     27
/* 28, 29 and 30 retired */

#define R3_SET_ASIDE         31
#define R3_LOAD_ASIDE        32
#define R3_MGR_LOAD          33

#define R3_CB_PRIMARY_FRONT  34
#define R3_CB_PRIMARY_BACK   35
#define R3_CB_TINT_FRONT     36
#define R3_CB_TINT_BACK      37

/* ENUM option counts, from RAC3Form.Designer.cs. */
#define RAC3_ARMOUR_COUNT 8
#define RAC3_SHIP_COUNT   32

/* ------------------------------------------------------ rac3_panel.c exports */

int rac3_trigger(u8 id);
int rac3_set_value(u8 id, u32 value);
int rac3_get_options(u8 id, const char * const **options, u8 *count);
int rac3_load_setaside(void);

int rac3_planet_load(u8 planet, u8 flags);

int rac3_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac3_levelflags_reset(u8 planet);
int rac3_levelflags_set(u8 planet, u16 offset, u8 value);

int rac3_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories);
int rac3_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac3_unlock_set(u8 id, u8 field, u32 value);

/* ---------------------------------------------------------- rac3.c exports */

u8  rac3_planet_count(void);

/* rac3.cs SetFastLoads: writes fastLoad1 now and arms fastLoad2 for on_tick. */
int rac3_arm_fast_loads(void);

#endif /* QWARK_RAC3_H */
