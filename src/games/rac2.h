/*
 * Ratchet & Clank 2: Going Commando (NPEA00386, and BCES01503 when the disc
 * trilogy boots it): the address table and the seam between rac2.c (numbers, hot
 * block, descriptors, vtable) and rac2_panel.c (the handlers). Nothing outside
 * those two files includes this.
 *
 * Every constant is from racman's RaCTrainer/Games/RAC2: rac2.cs for the address
 * table, RAC2Form.cs for the button handlers and the loading-screen watcher,
 * RC2Unlocks.cs for the owned-item table, FormCollectables.cs for the
 * collectable arrays and ChargebootColorPicker.cs for the four colour words.
 * Where the old code was ambiguous the comment says what was assumed.
 */
#ifndef QWARK_RAC2_H
#define QWARK_RAC2_H

#include "game.h"

/* -------------------------------------------------------- rac2.cs addresses */

#define RAC2_LOADSCREEN_TYPE   0x147A257u  /* which of the five load screens */
#define RAC2_LOADSCREEN_COUNT  0x147A25Bu  /* the watcher's trigger, 2 = final */
#define RAC2_INPUTS            0x147A430u  /* u32 pad mask, OG layout */
#define RAC2_ANALOGS           0x147A60Cu  /* f32 rx, ry, lx, ly */

#define RAC2_SAVE_SLOT         0x13298CCu  /* s16, -1 after QE; the QE feature */
#define RAC2_CURRENT_PLANET    0x1329A3Cu
#define RAC2_BOLTS             0x1329A90u
#define RAC2_RARITANIUM        0x1329A94u
#define RAC2_PREV_HELD_WEAPON  0x1329A9Fu  /* gadget storage; Swingshot is 0x0D */
#define RAC2_CHALLENGE_MODE    0x1329AA2u  /* one byte */
#define RAC2_HEALTH_XP         0x1329AA4u
#define RAC2_EXP_ECONOMY       0x1329AA8u  /* one byte, 100 = insta-upgrades */
#define RAC2_BOLT_DEFICIT      0x1329AACu
#define RAC2_NANOTECH_BOOSTS   0x1329AC0u  /* 10 bytes */

#define RAC2_COORDS            0x147F260u  /* f32 x, y, z (Vec4) */
#define RAC2_GHOST_TIMER       0x147F3CEu
#define RAC2_PLAYER_STATE      0x1481474u
#define RAC2_HERO_TYPE         0x1481494u
#define RAC2_FREEZE_HEALTH     0x14816ACu  /* frozen to 42069 by the old form */
#define RAC2_BOSS_SIBERIUS     0x1481792u  /* one byte, cleared on death */
#define RAC2_BOSS_SNIVELAK     0x14817A3u  /* one byte, cleared on death */
#define RAC2_SLOTS_HIT         0x14817AFu
#define RAC2_SKILL_POINTS      0x1481809u  /* 30 bytes */

/* ------------------------------------------------------------ the item arrays */

/*
 * RaC2 uses the same item system as RaC3: every item, gadget and weapon version
 * has an item id, and the game keeps parallel arrays indexed by that id. Each of
 * these is base + id * stride, which is what the old per-row address lists in
 * RC2Unlocks.cs and rac2.cs were writing out by hand:
 *
 *   owned  u8  [id]   0x1481A80   1 = the player has this item
 *   ammo   u32 [id]   0x148182C   rounds in the magazine
 *   exp    u32 [id]   0x1481AF0   experience towards the next version, shifted
 *                                 left five: the HUD bar divides it by 32. Build
 *                                 33 offered it as a value slot and build 34
 *                                 stopped: the column did not fit the table a
 *                                 client draws, so nothing reads this any more
 *                                 and it has no constant of its own
 *   item   u8  [id]   0x1329A40   the item id of the VERSION in use, RaC3's item
 *                                 array under another name; see rac2_panel.c
 *   mods   u8  [id]   0x148190C   not exposed
 *
 * Every one was checked against every row of the community address list that
 * carries a PS3 PAL address (the Lancer's item id is 30 and its words land at
 * 0x1481A9E, 0x14818A4, 0x1481B68 and 0x148192A) and then against the NPEA00386
 * ELF in Ghidra, which indexes all five the same way.
 *
 * 56 is the length the game itself uses: its save and restore routine (0xBBB798)
 * copies exactly 56 entries of each of these arrays, and every loop that walks
 * an item stops at 56. Item ids above 55 are weapon VERSIONS, which have stats
 * records and no inventory of their own, so nothing here indexes an array with
 * one.
 */
#define RAC2_AMMO_ARRAY        0x148182Cu  /* u32 ammo[id] */
#define RAC2_MODS_ARRAY        0x148190Cu  /* u8 mods[id], the ammo array's end */
#define RAC2_OWNED_ARRAY       0x1481A80u  /* u8 owned[id] */
#define RAC2_ITEM_ARRAY        0x1329A40u  /* u8 item[id]: the version in use */
#define RAC2_ITEM_COUNT        56

/*
 * freezeAmmoCheckbox filled 136 bytes from 0x148185C, which in the array above
 * is ammo[12] through ammo[45]: the Bomb Glove to the Shield Charger. Kept
 * exactly as it was rather than widened to the whole array.
 */
#define RAC2_AMMO_FILL_FIRST   12
#define RAC2_AMMO_FILL_LEN     136

/*
 * The per-item stats table: 208-byte (0xD0) records indexed by item id, one per
 * weapon VERSION, and the table the item array above points into. From the ELF:
 *
 *   +0x00  u8    non-zero where the record is a real item
 *   +0x42  u16   the item's name string
 *   +0x46  s16   the item id of the NEXT version, 0 at the top of the chain
 *   +0x48  s16   the item id of the previous version, 0 at the bottom
 *   +0x68  s32   the experience the next version costs, again shifted left five
 *   +0x84  u16   what a magazine costs at a vendor
 *   +0x8A  u16   the magazine size, which is what the game refills ammo[id] to
 *
 * The community address list's own damage, price and capacity columns are the
 * same 208-byte records with the same field spacing, but every address in them
 * is 0x3F0 further along than this build's: they are not PS3 PAL addresses, and
 * the numbers here are the ones the NPEA00386 code actually reads.
 */
#define RAC2_STATS_TABLE       0x1322A90u
#define RAC2_STATS_STRIDE      208
#define RAC2_STATS_CAPACITY    0x8A

/*
 * The highest number of versions any RaC2 weapon has, and so the maximum the
 * Level field advertises in UNLOCK_LIST. Sixteen weapons go to V4; the Clank
 * Zapper and the five RaC1 carry-overs, whose second version is a Slim Cognito
 * purchase, stop at V2; the Zodiac and the RYNO II have no second version at
 * all. UNLOCK_SET clamps to the entry's own count the way RaC3 does.
 */
#define RAC2_MAX_LEVELS        4

#define RAC2_PLATINUM_BOLTS    0x1562540u  /* 0x70 bytes */
#define RAC2_LEVEL_FLAGS       0x15625B0u  /* 0x10 per planet */
#define RAC2_LOAD_PLANET       0x156B050u  /* request word, then the planet index */
#define RAC2_JANKPOT_ACTIVE    0x15718F8u
#define RAC2_CS_STORAGE        0x1578424u
#define RAC2_BOSS_HEALTHBAR    0x15784E8u
#define RAC2_MOBY_TABLE        0x15927B0u
#define RAC2_MOBY_TABLE_END    0x15927B8u
#define RAC2_MOBY_STRIDE       0x100u      /* sizeof(Moby) from rac2.cs */
#define RAC2_DEBUG_FEATURES    0x15B3070u  /* one byte, the built-in debug flag */
#define RAC2_RESPAWN_COORDS    0x15D26E0u  /* 32 bytes copied from the player */

#define RAC2_CHARGE_BUFFER     0x145C180u  /* auto buffer charge, set to 30 */
#define RAC2_AMMO_RESET_INSTR  0x0B30C7Cu  /* the ammo decrement, nopped */
#define RAC2_FASTLOAD_INSTR    0x00BEA8A0u /* also the fingerprint site */

#define RAC2_PBOLTS            0x1390C27u  /* Maktar slots */
#define RAC2_PJACKPOT          0x1390C37u

#define RAC2_CB_PRIMARY_FRONT  0x1318590u
#define RAC2_CB_PRIMARY_BACK   0x1318594u
/*
 * rac2.cs lists chargebootsTintFrontColor and chargebootsTintBackColor at the
 * SAME address, 0x13185a0. The colour picker writes the front colour there and
 * then the mid colour over it, so the mid colour is what survives. Both words
 * are exposed, because that is the pair the picker drove, and they share one
 * readout: whichever is written last is the value the game holds. Worth checking
 * on hardware whether the back tint really lives at 0x13185a4.
 */
#define RAC2_CB_TINT_FRONT     0x13185A0u
#define RAC2_CB_TINT_BACK      0x13185A0u
#define RAC2_PAD_MANIP         0x13185B8u  /* jump-pad speed float */

#define RAC2_SHORTCUTS_INDEX   0x1352684u
#define RAC2_IM_SHORTCUTS      0x135268Cu
#define RAC2_OLDSKOOL_SP       0x133E8A0u
#define RAC2_MOBY3595_PTR      0x13A2D90u
#define RAC2_HRUGIS_MISSION    0x143DB0Fu

#define RAC2_FELTZIN_RARI      0x1A30430u
#define RAC2_SAVED_RACE_INDEX  0x1A4D7E0u
#define RAC2_ENDAKO_BOSS_CS    0x1A58158u
#define RAC2_DORBIT_OPENING    0x1A59764u
#define RAC2_SIB_BOSS          0x1A5A99Fu
#define RAC2_SNIV_BOSS         0x1A6FB73u
#define RAC2_FELTZIN_OPENING   0x1A8495Bu
#define RAC2_FELTZIN_MISSION   0x1A84973u
#define RAC2_LAST_RARITANIUM   0x1A849A8u
#define RAC2_GORN_OPENING      0x1A99A34u
#define RAC2_GORN_MANIP        0x1A99A4Cu
#define RAC2_GORN_MISSION      0x1A99A5Bu
#define RAC2_YEEDIL_BOSS       0x1A9DF90u
#define RAC2_PYRAMID_BOLT      0x1AAC767u

/*
 * The savefile helper's byte. qwark embeds the helper and installs it on the
 * first request of a session (src/core/savefile.c), and it writes 1 here on
 * every call, so this reads 1 as soon as the game has reached the hook once.
 *
 * These are the helper's own addresses, from king_dedede's rc2-save, and not the
 * ones RAC2Form used: that form drove a different mod whose five bytes sat at
 * 0x1BF0000..4 and whose save manager wrote a tempsave file. Nothing writes a
 * file any more, so the two "Save manager" rows are retired and the set-aside
 * and load bytes are driven through src/games/sfhelper_bins.c.
 */
#define RAC2_SF_HELPER         0x10CD71Du

#define RAC2_POS_BLOB_LEN  30          /* what racman stores for a position slot */
#define RAC2_LF_LEN        0x10        /* level flag bytes per planet */

/* ------------------------------------------------- autosplit watcher, rev 1.5 */

/*
 * rac2.cs AutosplitterAddresses, in the order rac2-autosplitter.asl reads them.
 * The game state word (0x156B064), the destination planet (0x156B054), the
 * Protopet health bar (0x133EE7C) and the Endako *entry* flag (0x15625E3) are
 * all read by the old client and used by none of the script's start, reset or
 * split conditions, so they are not read here: the Endako entry split is decided
 * by the hero type instead. The load-screen type (RAC2_LOADSCREEN_TYPE, already
 * in the inputs hot block) is read, because the script's `update` block subtracts
 * a fixed amount of game time every time it changes.
 */
#define RAC2_CHUNK             0x157CE03u  /* rac2.cs currentChunk */
#define RAC2_YEEDIL_SCENE      0x1478991u  /* 6 once the Protopet cutscene starts */

/* Three bytes inside RAC2_LEVEL_FLAGS, named after what the script calls them. */
#define RAC2_LF_ENDAKO_EXIT    0x15625E1u  /* Endako (planet 3) + 0x01 */
#define RAC2_LF_ENDAKO_ENTER   0x15625E3u  /* Endako + 0x03, read but never used */
#define RAC2_LF_BARLOW_RACE    0x15625F7u  /* Barlow (planet 4) + 0x07 */
#define RAC2_LF_ARANOS2_CLANK  0x1562699u  /* Aranos 2 (planet 14) + 0x09 */

/* Planet ids the split conditions name. PLANET_LIST index == game planet id. */
#define RAC2_PLANET_ARANOS   0
#define RAC2_PLANET_MAKTAR   2
#define RAC2_PLANET_ENDAKO   3
#define RAC2_PLANET_BARLOW   4
#define RAC2_PLANET_TABORA   8
#define RAC2_PLANET_ARANOS2  14
#define RAC2_PLANET_YEEDIL   20
#define RAC2_PLANET_MUSEUM   21   /* never split on entering the Insomniac Museum */

/* Reason codes. Code 1 is "planet entered" in every game. */
#define R2_AS_PLANET        1
#define R2_AS_PROTOPET      2
#define R2_AS_A2_CLANK      3
#define R2_AS_MAKTAR_ARENA  4
#define R2_AS_BARLOW_RACE   5
#define R2_AS_ENDAKO_ENTER  6
#define R2_AS_ENDAKO_EXIT   7
#define R2_AS_TABORA_CAVES  8

/*
 * The three load transitions the script's `update` block subtracted for, one
 * code each because each is worth a different number of frames. They are
 * LOAD_START events, not splits.
 */
#define R2_AS_LOAD_SLIDE    9    /* loadScreen becomes 0: right to left, 1 frame */
#define R2_AS_LOAD_CURVED   10   /* becomes 1: the curved wipe, 9 frames */
#define R2_AS_LOAD_WIPE     11   /* becomes 3: top to bottom, 21 frames */

/* What the script subtracted, in microseconds, at 60 frames per second. */
#define RAC2_LOAD_SLIDE_US    16667u    /* 1/60 s */
#define RAC2_LOAD_CURVED_US   150000u   /* 9/60 s */
#define RAC2_LOAD_WIPE_US     350000u   /* 21/60 s */
#define RAC2_PROTOPET_US      116667u   /* 7/60 s, taken at the Protopet split */

/* The three load-screen ids the script named; every other value costs nothing. */
#define RAC2_LOADSCREEN_SLIDE  0
#define RAC2_LOADSCREEN_CURVED 1
#define RAC2_LOADSCREEN_WIPE   3

/*
 * Fingerprint: the original instruction at the fast-load patch site, 0x4BFFEA69
 * ("b -5527"), which racman restores when fast loads are turned off. qwark's own
 * patched form, the nop, is accepted too, so a console where the site is already
 * patched does not sit in BOOTING for ever.
 */
#define RAC2_FP_ADDR RAC2_FASTLOAD_INSTR
extern const u8 rac2_fp[4];
extern const u8 rac2_fp_patched[4];

/* ---------------------------------------------------------------- hot block */

/* inputs: 0x147A250 .. 0x147A61C, one read covering load screen, pad and analogs */
#define RAC2_HOT_INPUTS_ADDR  0x147A250u
#define RAC2_HOT_INPUTS_LEN   0x3CC
/* state: 0x13298CC .. 0x1329AB0, QE offset through the bolt deficit */
#define RAC2_HOT_STATE_ADDR   RAC2_SAVE_SLOT
#define RAC2_HOT_STATE_LEN    0x1E4
/* player: the coordinate Vec4 plus the rotation that follows it */
#define RAC2_HOT_PLAYER_ADDR  RAC2_COORDS
#define RAC2_HOT_PLAYER_LEN   0x20

/* autosplit: player state at +0x00, hero type at +0x20 */
#define RAC2_HOT_PSTATE_ADDR  RAC2_PLAYER_STATE
#define RAC2_HOT_PSTATE_LEN   0x24
/* autosplit: the level-flag window the three split flags live in */
#define RAC2_HOT_FLAGS_ADDR   RAC2_LEVEL_FLAGS
#define RAC2_HOT_FLAGS_LEN    0xEA

/* ----------------------------------------------------------- readout slots */

#define RAC2_RO_BOLTS      0
#define RAC2_RO_SAVEFILE   1
#define RAC2_RO_RARITANIUM 2
#define RAC2_RO_CHALLENGE  3
#define RAC2_RO_HEALTH_XP  4
#define RAC2_RO_QE_OFFSET  5
#define RAC2_RO_CB_FRONT   6
#define RAC2_RO_CB_BACK    7
#define RAC2_RO_CB_TINT    8

/* ------------------------------------------------------------- feature ids */

/* Stable: new features go on the end, a retired one leaves its number behind. */
#define R2_FAST_LOADS        0
#define R2_INFINITE_AMMO     1
#define R2_FREEZE_HEALTH     2
#define R2_GHOST             3
#define R2_INSTA_UPGRADE     4
#define R2_DEBUG_MODE        5

#define R2_DIE               6
#define R2_BOLTS             7
#define R2_RARITANIUM        8
#define R2_CHALLENGE         9
#define R2_HEALTH_XP         10
#define R2_SET_RESPAWN       11
#define R2_STORE_SWINGSHOT   12
#define R2_DEATH_BOSSES      13
#define R2_DEATH_PBOLTS      14

#define R2_RESET_ANYPCT      15
#define R2_RESET_MENUS       16
#define R2_SETUP_NGPLUS      17
#define R2_SETUP_NO_IMG      18
#define R2_SETUP_ALL_MISSION 19
#define R2_MAKTAR_SLOTS      20
#define R2_AUTO_ANYPCT       21
#define R2_AUTO_NGPLUS       22
#define R2_QE_OFFSET         23

#define R2_RESET_PBOLTS      24
#define R2_UNLOCK_PBOLTS     25
#define R2_RESET_NANOTECH    26
#define R2_UNLOCK_NANOTECH   27
#define R2_RESET_SKILL       28
#define R2_UNLOCK_SKILL      29

#define R2_LOAD_ASIDE        30
#define R2_SET_ASIDE         31
/*
 * 32 and 33 retired: "Save manager: save file" and "Save manager: load file"
 * drove the old mod's tempsave file, which protocol 1.9 does away with. The two
 * rows above carry the SAVE_ASIDE and LOAD_ASIDE flags now.
 */

#define R2_CB_PRIMARY_FRONT  34
#define R2_CB_PRIMARY_BACK   35
#define R2_CB_TINT_FRONT     36
#define R2_CB_TINT_BACK      37

/*
 * The two weapon actions the other three games have had all along. RaC2 only
 * grew a Level column when its unlocks moved onto the item arrays, so these go
 * on the end rather than beside the collectable pairs.
 */
#define R2_MAX_LEVELS        38
#define R2_MAX_AMMO          39

/* ------------------------------------------------------ rac2_panel.c exports */

int rac2_trigger(u8 id);
int rac2_set_value(u8 id, u32 value);
int rac2_load_setaside(void);
int rac2_die(void);

int rac2_planet_load(u8 planet, u8 flags);

int rac2_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int rac2_levelflags_reset(u8 planet);
int rac2_levelflags_set(u8 planet, u16 offset, u8 value);

int rac2_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int rac2_unlock_read(const struct game_unlock *entry, u32 values[4]);
int rac2_unlock_set(u8 id, u8 field, u32 value);

/* The loading-screen watcher's two optional resets. */
int rac2_reset_anypct(void);
int rac2_reset_menu_storage(void);

/* ---------------------------------------------------------- rac2.c exports */

u8  rac2_planet_count(void);

/* The fast-load patch, shared with the planet load and the set-aside load. */
int rac2_fastload_force(void);
int rac2_fastload_restore(void);

/* RAC2Form's desiredShortcutIndex; 0xFFFFFFFF means no setup has run. */
void rac2_set_shortcut_index(u32 index);
u32  rac2_shortcut_index(void);

/* The two "on death" toggles SelfDeathExtended took as arguments. */
int rac2_death_bosses(void);
int rac2_death_pbolts(void);

#endif /* QWARK_RAC2_H */
