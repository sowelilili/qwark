/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the address
 * table, the seam between tod_game.c (hot block, descriptors, vtable) and
 * tod_panel.c (the handlers), and the fact sheet (tod.c) the Lua helper and
 * the later per-title data file share (docs/tod-poc.md, REPORT-tod-lua
 * section 7, SPEC-game-modules 4.12).
 *
 * Game 5, built only where the games are modules or linked into a PC build:
 * `make` builds it as tod.sprx, the host builds link it, and `make release`
 * leaves it out altogether (titles.c has no ToD entry there).
 *
 * Every fact carries how it is known (struct tod_mark):
 *   TOD_DEC    read in a decompile of the shipped executable
 *   TOD_DIS    read in its disassembly or raw words
 *   TOD_SCAN   a Python pass over the executable (REPORT-tod-lua section 1)
 *   TOD_LEGACY only from the old C# RaCMAN or the autosplitter
 *   TOD_INFER  inferred from the above; says what would settle it
 * Nothing in here has been read on a running game yet (docs/tod-poc.md 5).
 */
#ifndef QWARK_TOD_H
#define QWARK_TOD_H

#include "game.h"

#define TOD_TITLE "NPEA00452"

/* ------------------------------------------------------- the address table */

/*
 * The game's own names (Ghidra, DL-ghidra:/NPEA00452.ELF) beside each. "dec"
 * is the decompile, "dis" the disassembly, "csv" the game's config sheets.
 */
#define TOD_FP_ADDR          0x002EA5F0u  /* stw r31,0x6e8(r11) in LoadLevel, never patched (dis) */
#define TOD_FP_WORD          0x93EB06E8u

#define TOD_HOT_INPUTS       0x10691918u  /* g_padAnalogs: f32 LX, LY, RX, RY, (v - 128) / 128 (dis) */
#define TOD_HOT_INPUTS_LEN   0x5Cu
#define TOD_OFF_PRESSED      0x50u        /* g_padPressed */
#define TOD_OFF_RELEASED     0x54u        /* g_padReleased */
#define TOD_OFF_MASK         0x58u        /* g_padMask, the game's bit order (tod_pad_to_og) */
#define TOD_PAD_MASK         (TOD_HOT_INPUTS + TOD_OFF_MASK)   /* 0x10691970 */

#define TOD_CURRENT_LEVEL    0x1029C558u  /* g_currentLevel, s32 LEVEL_, g_saveData+0x906E8 (dec) */
#define TOD_CHALLENGE        0x1029C55Cu  /* g_challengeMode, u32: 0 normal, N the Nth challenge mode (dec) */
#define TOD_LOAD_REQUEST     0x1006E764u  /* g_planetLoadRequest, s32, -1 none (dec) */
#define TOD_GAME_STATE       0x10387F4Cu  /* g_gameState, s32, 4 in gameplay (dis) */
#define TOD_STATE_GAMEPLAY   4
#define TOD_REQUESTED_STATE  0x100FA21Cu  /* g_requestedGameState, 3 = a level load (dis) */
#define TOD_QUIT_BYTE        0x1037A8E8u  /* g_quitRequested, SystemQuitCallback sets 1 (dis) */
#define TOD_NPLANETS         19u

/*
 * The hero: [[TOD_LEVEL_OBJ] + TOD_HERO_PTR], rewritten every frame from
 * g_heroList (HeroList_getCurrentHero), 0 between levels (dec).
 */
#define TOD_LEVEL_OBJ        0x10740BB4u  /* the per-level object, 0x5B8F0 bytes (BigObj5B8F0_Init) */
#define TOD_HERO_PTR         0x0000C684u
#define TOD_HERO_MOBY        0x14u        /* ToDHero.moby: ToDMoby*, whose position is +0x30 */
#define TOD_MOBY_POS         0x30u
#define TOD_HERO_PREV_POS    0x1250u      /* ToDHeroMotion prevPos, vec4 */
#define TOD_HERO_POS         0x1260u      /* pos, then the collision centre and its last value */
#define TOD_POS_BLOB_LEN     48u          /* pos, centre, previous centre: never the handle at +0x1290 */
#define TOD_HERO_HEALTH      0x1784u      /* f32 hit points; Hero_checkDeath kills at <= 0 */
#define TOD_HERO_MAX_HEALTH  0x1788u      /* f32 */

/* g_saveData and what is in it (ToDSaveData, dec). */
#define TOD_SAVE_DATA        0x1020BE70u
#define TOD_WEAPONS          0x1020BE70u  /* ToDWeaponSlot[32], indexed by WPN_ */
#define TOD_WEAPON_STRIDE    0x14u
#define TOD_NWEAPONS         32u
#define TOD_WS_XP            0x04u        /* f32 */
#define TOD_WS_AMMO          0x08u        /* f32 */
#define TOD_WS_MODS          0x0Cu        /* u32, bit n = mod n */
#define TOD_WS_OWNED         0x10u        /* u8 */
#define TOD_WS_LEVEL         0x11u        /* u8, 0..9 = V1..V10 */
#define TOD_MAX_LEVEL        10u
#define TOD_OBJECTS          0x1020C174u  /* s32[23] by OBJ_, a count; "has" = non-zero */
#define TOD_NOBJ_OWNED       15u          /* OBJ_ 0..14 are owned flags; 15.. are counters */
#define TOD_NOBJ_LISTED      12u          /* OBJ_ 0..11 on the Unlocks panel; 12..14 have no use there (tod_panel.c) */
#define TOD_BOLTS            0x1020C28Cu  /* u32 */
#define TOD_RARITANIUM       0x1020C290u  /* u32 */
#define TOD_MAX_BOLTS        999999999u   /* Hero_addBolts and Hero_addMinerals cap both there */
#define TOD_LEVEL_SAVE       0x1020C2F8u  /* ToDLevelSave[19], 0x408 each */
#define TOD_LEVEL_SAVE_LEN   0x408u
#define TOD_GOLD_BOLTS       0x1020C6E4u  /* u32 bit mask per level, + L * 0x408 (LevelSave_collectGoldBolt) */
#define TOD_RYNO_PLANS       0x10214564u  /* u32 bit mask; the game's "give all" ORs TOD_RYNO_ALL */
#define TOD_RYNO_ALL         0x0007DEE4u
#define TOD_CHEATS           0x10214568u  /* u8[14] */
#define TOD_SKILL_POINTS     0x10214580u  /* u64, bit i = SKILLPOINT i (SkillPoint_set) */
#define TOD_NSKILL_POINTS    60u

/* Cheats the game left in (dec; nothing in the game writes them). */
#define TOD_NO_DAMAGE        0x101F1F00u  /* g_debugHeroNoDamage, u32: the hero's damage is dropped */
#define TOD_INFINITE_AMMO    0x10385BE8u  /* g_infiniteAmmo, u32: Weapon_hasAmmo always true */

/*
 * The old RaCMAN's god ratchet: g_heroProxyMobyHandle, the remote-controlled
 * proxy moby's handle; while the word is non-zero Hero_Update drops the
 * hero's damage (dec). The old trainer wrote one byte, 154, into its low byte
 * (TOD_PROXY_LOW) and 0 there to turn it off (TOD_LEGACY).
 */
#define TOD_PROXY_HANDLE     0x1020BD48u  /* g_heroProxyMobyHandle, u32 */
#define TOD_PROXY_LOW        0x1020BD4Bu  /* its low byte, the one the old trainer wrote */
#define TOD_PROXY_FAKE       154u

/*
 * g_levelTransitionMinTime, f32 seconds (the memory panel's row). In
 * LevelTransition_waitLoad (0x3C5810) the wait is only for a load that plays
 * a story FMV instead of the flight (g_levelTransitionFmvBuffer 0x1037AB28 set),
 * and there it holds back the start of the data's read while the FMV plays on
 * regardless (dec). LevelTransition_begin picks that FMV list for a load to
 * Metropolis (l01_s01_ntro_fmv) or Cobalia (l02_s01_hypr_fmv, l02_s02_zoni_fmv)
 * while LVL_FAST_SHIP_FIXED (level flag 106) is clear, every time, and adds the
 * FMVs to the seen-movies list (g_levelFlags + 0x30D8) there and then, before
 * they play. Fast loads writes 0 here while it is on and 5.0 back when it goes
 * off (revision 1.29, tod_panel.c); nothing of the game's writes it.
 */
#define TOD_TRANSITION_MIN   0x10073548u
#define TOD_TRANSITION_GAME  0x40A00000u  /* 5.0f, the shipped value */

/*
 * Fast loads (id 17), a code patch since 2026-10-28 (dec). A planet load's
 * screen is the level_transitions level playing cinematics: the departure
 * (g_loadScreenType 0) when there is a level to fly from, then long-load
 * screens (2 and up) for as long as the data is not in, then the arrival (1).
 * Each one plays to its end; LoadScreen_Update (0x25E560, every frame) starts
 * the next only once the player is idle, and the arrival only once
 * LoadScreen_LoadDone has set g_loadScreenLoadDone, which the loader thread's
 * LevelTransition_waitLoad calls every 200 ms once the data is read. The load
 * is over when LoadScreen_LoadDone finds the arrival (1) with the player idle.
 * So a load lasts at least the departure and the arrival, and the rest of
 * whichever long-load screen is playing when the data comes in.
 *
 * The patch is twenty-five words of LoadScreen_Update from 0x25E588, the
 * `bne cr7` that returns while a cinematic plays, on: the arrival up (1),
 * as the game; the load done, the cut; otherwise as the game (a cinematic
 * playing returns, with none the next long-load screen is picked). The cut:
 * the screen black at once (0x25ED38(1.0), what Level_Enter does for the
 * planet), the cinematic stopped as a level's exit stops it (0x84EF4), the
 * player reset as Level_StartGameplay resets it (0x84E28), the transition
 * prop's two handles made invalid (below), and 1 in g_loadScreenType;
 * LoadScreen_LoadDone's next look finds the load over, and no arrival plays.
 * The words they cover past the first seventeen are the arrival's own start,
 * which the patched form never reaches; 0x25E5EC on, with 0x25E5F4 (the
 * long-load screen's play), are untouched. The originals are the game's own
 * (patch_def keeps them).
 *
 * Revision 1.29, the transition prop (the user's crash: Stratus City's opening
 * cinematic with Fast loads on, a NULL read at 0x82624). Cinematic_setupMoby
 * (0x81F60), for each moby a cinematic spawns, spawns one more beside a moby of
 * class 0xBA while a level transition is up (g_levelTransitionActive), level
 * flag 106 is clear and the level is "stratus city" (class 0xBC, half size) or
 * "fastoon" (0xBB), and keeps the two handles in g_transitionPropHandle and
 * g_transitionPropOwnerHandle (u64 each, the moby handle in the low word).
 * Every frame of every cinematic after that, Cinematic_updateTransitionProp
 * (0x82518) resolves both and puts the prop on a joint of the owner's: the
 * record is the owner's class's (moby + 0x54, then + 0x44: 0 unless its byte
 * at + 0xC is 4 or more and its joint word at + 0x930 is not -1), and the
 * joint index at record + 0x244 is read without a test, the crash's
 * lwz r11, 0x244(r31) with r31 0. Nothing clears the
 * handles but the game's static constructor; the game relies on a handle
 * going stale once its moby is gone. But a level's MobyPool_init (0x5578D8)
 * starts the handles' serial at 1 again, and a pool filled in order hands the
 * n-th moby the handle (n + 1) << 16 | n, so the handles of a cinematic early
 * in one level name mobys of the next. The game's own load ends with the
 * arrival, whose handles come late in the transition and name nothing in the
 * planet; with the arrival cut, the departure's or a long-load screen's are
 * the last, and the planet's opening cinematic (Stratus City's
 * l03_s01_rcff_rt, Fastoon's l04_s01a_fint_rt) finds them naming two of its own
 * mobys (in the dump, r30 and r29, slots side by side as the prop is spawned
 * right after its owner), whose class has no such joint. So the cut makes both handles what the
 * constructor makes them (MobyHandle64_invalidate, 0x03FFFFFF_FFFFFFFF), as
 * after the arrival's cleanup they would be stale; no planet cinematic wants
 * the prop, which is only spawned while a transition is up.
 *
 * Under RPCS3 the code switch does the same (src/games/switches/sw_tod.s,
 * SW_TOD_FLAG_FAST_LOADS): its trampoline's second half is these words.
 *
 * Revision 1.29 (the user's hardware test: the departure still played in full,
 * then the cut). LoadScreen_Update runs from Game_UpdateFrame, and a realtime
 * cinematic plays in g_gameState 9, whose GameState_Dispatch case is
 * MovieQueue_update alone, so the cut could only come between two load screen
 * cinematics. The departure does not hold the data back: LevelTransition_begin
 * (the level_transitions level, LoadScreen_Init, the departure) is the transition
 * tier's activation, and the loader thread takes g_levelLoadTier, whose load is
 * LevelTransition_waitLoad, a frame or two later and reads at once on a planet
 * load; LoadScreen_LoadDone sets g_loadScreenLoadDone the moment the read is done
 * and its 200 ms poll waits only for the arrival (1) with the player idle. So
 * when the data is in during the departure, the departure's rest was all wait.
 * Three pieces of qwark's own (sw_tod.s, own_*, which read the same flag byte;
 * a console writes them with the patch and sets the flag) end that wait:
 *
 *   own_departure  LevelTransition_update (g_levelLoadTier's update, a load's
 *                  frames only), the planet path: the data in, the arrival not
 *                  up and the player playing (2), the player to 3, as its own
 *                  end and its skip put it there: the cinematic fades to black
 *                  (the "short fade" the user saw after the departure), the
 *                  queue lets state 9 go, and the cut above follows.
 *   own_fmv_in     LevelTransition_waitLoad, the loader thread, once a story
 *                  FMV load's data is read: the load's FMV start time into
 *                  TOD_FL_FMV_IN, the word after the flag.
 *   own_fmv_cut    LevelTransition_update, the FMV path with an FMV open: its
 *                  own load's stamp there, the FMV closed as its end closes it
 *                  (FmvPlayer_stop), the list run out, the screen black; the
 *                  next frame sets g_levelTransitionFmvDone as the game does,
 *                  and the loader's 200 ms poll ends the load.
 *
 * With g_levelTransitionMinTime at 0 the story FMV load reads at once and ends
 * when the data is in; the FMV is not needed for anything after it.
 */
#define TOD_FASTLOAD_AT      0x0025E588u  /* LoadScreen_Update + 0x28 */
#define TOD_FASTLOAD_NWORDS  25u
#define TOD_FASTLOAD_NSITES  3u           /* qwark's own pieces' sites, after the twenty-five */
#define TOD_PROP_OWNER       0x101D60E0u  /* g_transitionPropOwnerHandle, u64 */
#define TOD_PROP_HANDLE      0x101D60E8u  /* g_transitionPropHandle, u64 */
#define TOD_PROP_INVALID_HI  0x03FFFFFFu  /* MobyHandle64_invalidate's 0x03FFFFFF_FFFFFFFF */
#define TOD_PROP_INVALID_LO  0xFFFFFFFFu
#define TOD_LOAD_SCREEN_DONE 0x102034FCu  /* g_loadScreenLoadDone, u8 */
#define TOD_FL_FMV_IN        0x108A8564u  /* SW_TOD_DATA_FMV_IN: the FMV load's stamp, past the BSS */

/*
 * Revision 1.29: no fade-out before a load qwark starts. A planet load in
 * gameplay is g_planetLoadRequest (PLANET_LOAD writes it, and a savefile load's
 * SaveManager_onLoaded writes it through LoadLevel(level, 1, 0)), which
 * LoadLevel_ProcessPending takes from Game_UpdateFrame: ScreenFade_Start(
 * k_loadLevelFadeTime, 0) every frame and the load once ScreenFade_GetProgress
 * reaches k_loadLevelFadeDone (1.0). The fade time is a word of the TOC
 * (r2 0x8BB068, -0xB4C), in the RW segment, read by ProcessPending alone.
 * ScreenFade_update (0x27DF1C) moves the progress by min(the rest, dt / time),
 * so it lands on 1.0 exactly; a time of 2^-10 s gets there in one frame (0
 * would be dt * inf, NaN on a frame with no time). qwark writes the short time
 * just before its own request and the game's 0.5 back once its load has left
 * the request (tod_fade_tick), or after TOD_FADE_WAIT_TICKS, and as a session
 * enters, so the game's own loads keep their fade.
 */
#define TOD_LOAD_FADE_TIME   0x008BA51Cu  /* k_loadLevelFadeTime, f32 */
#define TOD_LOAD_FADE_DONE   0x008BA524u  /* k_loadLevelFadeDone, f32 1.0, left alone */
#define TOD_LOAD_FADE_GAME   0x3F000000u  /* 0.5f */
#define TOD_LOAD_FADE_QUICK  0x3A800000u  /* 2^-10 s: one frame */
#define TOD_FADE_WAIT_TICKS  720u         /* six seconds of the tick: a load the game never took */

/*
 * Revision 1.29 (2026-10-31): a story FMV load's fade-out. LevelTransition_begin
 * (0x3C8360) plays a list of FMVs instead of the load screen when bit 42 of
 * g_saveData+0x5530 (LVL_FAST_SHIP_FIXED, level flag 106) is clear and the
 * level's name is "metropolis" or "cobalia" (Str_compareNoCase), LEVEL_ 0 and 1.
 * Such a load has no departure: with its FMV gone (Remove cutscenes, or the
 * file deleted from the disc's copy, which qwark cannot see) nothing at all
 * comes between the old level's last frame and the black, and a fade-out of
 * one frame flashes. So qwark's own load of that kind fades for
 * TOD_LOAD_FADE_STORY, 7/64 s: six or seven frames at 60 Hz (three or four
 * at 30), long enough to read as a fade rather than a cut, and still under a
 * quarter of the game's 0.5 s (5/64 s until 2026-11-02, the user's call: a
 * frame or two longer). A load that is not a story FMV load keeps the one-frame fade.
 *
 * tod_story_load answers whether a load of `level` with the level flags at
 * `flags_at` (g_levelFlags for PLANET_LOAD, the save image's for a savefile
 * load, whose flags SaveManager_onLoaded copies in before its LoadLevel) takes
 * that path; a flag that cannot be read counts as clear, the longer fade being
 * the one that never flashes.
 */
#define TOD_LOAD_FADE_STORY  0x3DE00000u  /* 7/64 s = 0.109375, exact: about 6.5 frames at 60 Hz */
#define TOD_LF_FAST_SHIP_FIXED 106u       /* LVL_FAST_SHIP_FIXED */
#define TOD_LEVEL_COBALIA    1            /* LEVEL_COBALIA, the other story FMV load */
int  tod_story_load(u32 level, u32 flags_at);

/*
 * Revision 1.29: Remove cutscenes (id 18). Every FMV is a ToDFmvRecord of the
 * game archive's table (g_archiveGame.toc + 0x2C, the count in the word before
 * it, 0x1F4 bytes each): its name at +0, its file at +4 and the localised file
 * at +0x94 (used while +0x98 is not 0 and the file exists), each a char* into
 * the table. Movie_requestPlay and the level transition's FMV list find the
 * record by name and play its file; when the file will not open
 * (FUN_005FAB70: "Failed to play Bink movie"), FmvState_enter goes on to
 * FmvState_next, as at an FMV's end, and FmvPlayer_play to FmvPlayer_stop, so
 * a missing file is an FMV over at once. That is what the runners' deleted
 * files do, and what this does to the user's eight: the first letter of the
 * file's name (after any '/') at 0, so the path names its folder. Every one of
 * the eight begins with 'l', which is what goes back.
 */
#define TOD_ARCHIVE_GAME     0x102042ACu  /* g_archiveGame, TodArchive */
#define TOD_ARCHIVE_TOC      0x108u       /* TodArchive.toc */
#define TOD_TOC_FMVS         0x2Cu        /* the FMV records */
#define TOD_FMV_STRIDE       0x1F4u
#define TOD_FMV_FILE         0x04u        /* ToDFmvFile: char *path, u32 size */
#define TOD_FMV_ALT          0x94u
#define TOD_FMV_LOOK_LEN     0x9Cu        /* the record's head: both files */
#define TOD_FMV_MAX          1024u        /* a table longer than this is not the table */
#define TOD_FMV_PATH_LEN     64u
#define TOD_CUT_EVERY        240u         /* ticks between two looks while on: the table may come or go */
#define TOD_CUT_KEEP         16u          /* the bytes a look checks before it reads the table again */

/*
 * The save system (docs/tod-savefile.md; Ghidra /ToD/Save/ToDSaveManager).
 * g_saveManager sits right after g_saveData and holds, at +0x1FD08, the image
 * of the last GAME.SAV the game wrote or read: SaveManager_copyForSave copies
 * g_saveData into it before a save, and SaveManager_onLoaded copies it back
 * and calls LoadLevel(level, 1, 0) after a load. SaveSystem_update (end of
 * every GameState_Dispatch, main thread) runs the state machine at +0xE0 (dec).
 */
#define TOD_SAVE_SYSTEM      0x1074D400u  /* g_saveSystem, ToDSaveManager*: g_saveManager (dec) */
#define TOD_SAVE_MANAGER     0x1029C560u  /* g_saveManager, 0xB03F8 bytes (dec) */
#define TOD_SAVE_VTABLE      0x10071C60u  /* vtbl_SaveManager, its first word (dis) */
#define TOD_SM_SAVE_THREAD   0x08u        /* ToDSaveThread: list save, auto save */
#define TOD_SM_LOAD_THREAD   0x20u        /* ToDSaveThread: list load, auto load, the boot check */
#define TOD_SM_DELETE_THREAD 0x38u        /* ToDDeleteThread */
#define TOD_ST_ID            0x08u        /* u64 thread id, TOD_THREAD_NONE while none */
#define TOD_ST_STATE         0x10u        /* s32; a load thread's 5 is "file read", which the poll applies */
#define TOD_SM_STATE         0xE0u        /* s32: 0 not up, 1 idle, 2 saving, 3 loading, 4 deleting */
#define TOD_SM_SLOT          0xECu        /* s32, the slot last saved or loaded, -1 none (autosave needs one) */
#define TOD_NO_SLOT          0xFFFFFFFFu  /* `slot` -1: SaveSystem_request's op 4 then does nothing, autoSavePending left set (dis) */
#define TOD_SM_RESULT        0xF0u        /* s32, what finished: 1 saved, 3 or 4 loaded */
#define TOD_SM_AUTOSAVE      0x104u       /* s32, an autosave asked for while the system was busy */
#define TOD_SM_HEAD_LEN      0x108u       /* everything above, one read */
#define TOD_SAVE_IMAGE       0x102BC268u  /* g_saveManager + 0x1FD08: the aside buffer */
#define TOD_SAVE_SIZE        0x906F0u     /* GAME.SAV, as the stat callback insists */
#define TOD_SAVE_LEVEL_OFF   0x906E8u     /* the save's LEVEL_, what onLoaded loads */
#define TOD_SAVE_CHAL_OFF    0x906ECu     /* challenge mode, at most 1000 */
#define TOD_SS_IDLE          1u
#define TOD_SS_LOADING       3u
#define TOD_SS_HELD          0x5157484Cu  /* qwark's own: SaveSystem_update and _request leave it be (dec) */
#define TOD_LT_READ_OK       5u
#define TOD_THREAD_NONE      0xFFFFFFFFu  /* the low word; the high word is 0 */
#define TOD_THREAD_GONE      0xFFFFFFFEu  /* no thread: sys_ppu_thread_get_priority answers ESRCH (INFER) */
#define TOD_FRONTEND_EXIT    0x1037A458u  /* g_frontEndExitRequested, u8: set by FrontEnd_requestLevel and never cleared (xrefs) */
#define TOD_MAX_CHALLENGE    1000u

/* Lua (REPORT-tod-lua section 2). */
#define TOD_LUA_VM           0x102043CCu  /* g_gameplayLuaVM */
#define TOD_LUA_STATE        0x102043D0u  /* its lua_State*, 0 between levels */
#define TOD_GAME_CLOCK       0x1074DDA8u  /* f32 time scale at +0, game time at +0x14 */

/*
 * The autosplitter (revision 1.28; tod_autosplit.c). Ghidra names,
 * all read in the decompile (dec) unless the mark says otherwise.
 */
#define TOD_FRONT_END        0x1037A459u  /* g_inFrontEnd, u8: 1 from FrontEnd_Enter to FrontEnd_Exit (the title) */
#define TOD_STATE_CREDITS    11           /* g_gameState: GameState_RequestCredits, from Lua_show_credits */
#define TOD_MISSIONS_DONE    0x1021C968u  /* g_saveData+0x10AF8: ToDMissionList[20] by LEVEL_, the completed ones */
#define TOD_MISSION_LIST_LEN 0x7Cu        /* 10 x { u32 label text id, u32 desc text id, u32 flags }, u32 count */
#define TOD_MISSION_STRIDE   0x0Cu
#define TOD_MISSION_COUNT    0x78u
#define TOD_NMISSIONS        10u
#define TOD_LEVEL_METROPOLIS 0            /* LEVEL_METROPOLIS, where a run starts */
#define TOD_LEVEL_FASTOON_II 18           /* LEVEL_FASTOON_II, Tachyon's */

/*
 * Revision 1.29 (pending). The old autosplitter's "long load": game time
 * stopped while the low byte of g_currentLevel (its 0x1029C55B) read 0 and
 * the low byte of g_loadScreenType (its 0x102034FB) read more than 1.
 * g_loadScreenType is the movie the loading screen plays: LoadScreen_Init
 * puts 0 there, or a LoadScreen_PickType pick (2 and up, never 3) when the
 * load has no level to fly from; LoadScreen_Update picks another (2 and up)
 * each time one ends while the load is still going, and 1, the arrival,
 * once LoadScreen_LoadDone has said the load is done. It stays 1 until the
 * next load screen.
 */
#define TOD_LOAD_SCREEN      0x102034F8u  /* g_loadScreenType, s32; its low byte is the legacy's 0x102034FB (dec) */
#define TOD_LONG_LOAD_MIN    2u           /* the legacy's "> 1" */

/*
 * Revision 1.29 (pending): the run's start. MOVIE_METROPOLIS_TACHYON, the
 * level flag the game sets as the first planet is beaten: clear on a new
 * file, set on every save made after Metropolis.
 */
#define TOD_LF_METROPOLIS_TACHYON 185u

/*
 * g_mobyPool's ToDMoby slots (stride 0x180): the object at +0x40, the
 * handle at +0x64. Tachyon is the object whose vtable is vtbl_TachyonBoss
 * (ToDTachyonBoss), its health at +0x24. The fact sheet's; the autosplitter
 * read Tachyon's health for its code 2 until revision 1.29 retired it.
 */
#define TOD_MOBY_POOL        0x1066C300u  /* g_mobyPool, ToDMobyPool: the slots at +0, the highest index at +0x10 (dec) */
#define TOD_VTBL_TACHYON     0x10060CB0u  /* vtbl_TachyonBoss (dec) */

/*
 * Revision 1.29 (pending): a level load, and nothing else. The game's top
 * level is a stack of tiers (ToDGameTier: the vtable at +0, `loaded` at +6,
 * `entered` at +7), and each of its states is a list of them
 * (g_gameStateStacks, ToDGameStateStack, 0x88 bytes each). State 3, a level,
 * is the core tier, 0x1037AA38, the level transition's (LevelTransition_begin
 * its enter), g_levelLoadTier (LevelTransition_waitLoad its load,
 * LevelLoad_onComplete its enter) and g_levelTierState (Level_Enter, Level_Exit).
 * A planet load asks for state 3 with its top three tiers marked to go
 * (RequestLevelLoadState: GameState_Request(3, 3)); a death or a checkpoint
 * asks for it with only the top one marked (Hero_respawnWhenBlack, once the
 * death's fade is black: GameState_Request(3, 1)), so only the level tier
 * goes and comes back: Level_Exit and Level_Enter, a new lua_State and a new
 * fade in, while g_levelLoadTier stays loaded. A quit to the title asks for
 * state 2. GameState_Update unloads the tiers, then GameState_startTierLoads
 * gives the loader thread the state's list, its job, which it puts back to 0
 * once the stack is that list. So a level load is under way exactly while
 * the job is state 3's list and g_levelLoadTier is not loaded: from the
 * frame the old level goes until the new one's data is in, seconds with
 * Fast loads on or off. A death never unloads that tier, and the title is
 * another job (dec, dis).
 */
#define TOD_LOAD_JOB         0x1073B3A0u  /* the tier loader's job: a ToDGameStateStack*, 0 when idle (dis 0x630794) */
#define TOD_LOAD_JOB_LEVEL   0x10073300u  /* g_gameStateStacks[3], state 3's tiers: the level's */
#define TOD_LEVEL_LOAD_TIER  0x1037A908u  /* g_levelLoadTier, a ToDGameTier */
#define TOD_TIER_LOADED      6u           /* ToDGameTier.loaded, u8: GameTier_load sets it, GameTier_unload clears it */

/*
 * Revision 1.29 (pending): the run's two ends as the community times them.
 *
 * The realtime cinematics: g_moviePlayer (ToDMoviePlayer) plays one level
 * movie at a time, by its index in the level's movie list (MovieTable_build,
 * at every level load: g_movieRecords[index] is the record, whose name is
 * the char* just before it). MoviePlayer_update's states: 0 idle, 1 fading
 * out (it waits for ScreenFade's progress to reach 1.0, black, and then asks
 * for g_gameState 9), 2 playing, 3 fading back in, 4 done (MovieQueue_update
 * then plays what is queued next). g_movieQueue holds what waits, its count
 * at +0x404. A named FMV is not the player's: Movie_requestPlay replaces the
 * top of the game-state stack with state 7 and the FMV's record (ToDFmvRecord,
 * its name the char* at +0).
 */
#define TOD_MOVIE_PLAYER     0x101D4F50u  /* g_moviePlayer, ToDMoviePlayer (dec) */
#define TOD_MP_STATE         0x58u        /* s32 0..4, above */
#define TOD_MP_MOVIE         0x64u        /* s32, the movie's index in g_movieRecords */
#define TOD_MP_LOOK_LEN      0x10u        /* +0x58 .. +0x67: the state and the movie in one read */
#define TOD_MOVIE_QUEUE_N    0x101D4F44u  /* g_movieQueue (0x101D4B40) + 0x404, s32: movies waiting */
#define TOD_MOVIE_RECORDS    0x101D50C4u  /* g_movieRecords, record pointers [0x200] by index; the name at [-4] */
#define TOD_NMOVIES          0x200u
#define TOD_FADE_PROGRESS    0x5B62Cu     /* [TOD_LEVEL_OBJ] + this: f32, 0 clear .. 1.0 black (ScreenFade_GetProgress) */
#define TOD_FADE_BLACK       0x3F800000u  /* 1.0f, MoviePlayer_update's own "faded out" test */
/*
 * Revision 1.29: the fade's target (ToDScreenFade, [TOD_LEVEL_OBJ] +
 * 0x5B600: the progress at +0x2C, the rate at +0x30, the target at +0x34).
 * ScreenFade_Set(p) puts p in the progress and the target and 0 in the rate;
 * ScreenFade_StartIn(t, delay) puts 0.0 in the target and 1/t in the rate,
 * ScreenFade_Start the same towards 1.0. Level_Enter does Set(1.0), then
 * StartIn(1.0, 0): black, then one second in.
 */
#define TOD_FADE_TARGET      0x5B634u     /* [TOD_LEVEL_OBJ] + this: f32, 0.0 fading in (or clear), 1.0 out (or black) */
#define TOD_FADE_LOOK_LEN    0x0Cu        /* the progress, the rate and the target in one read */
#define TOD_FADE_START_AT    0x3F0CCCCDu  /* 0.55f (0.550000012): the run's START, 0.45 s into Level_Enter's fade in (2026-11-02; 0.5 from 2026-10-31) */
#define TOD_STATE_FMV        7            /* g_gameState: a named FMV (Movie_requestPlay) */
#define TOD_STATE_CINEMATIC  9            /* g_gameState: a realtime cinematic playing */
#define TOD_GS_DEPTH         0x10387DC8u  /* g_gameStateDepth, s32: the top of g_gameStateStack */
#define TOD_GS_STACK         0x10387DCCu  /* g_gameStateStack, ToDGameStateEntry[], 0x30 each: the arg at +0 */
/* The ending: TachyonEnding_update queues the cinematic, then the two FMVs. */
#define TOD_END_CINEMATIC    "l4b_s04_dimx_rt"
#define TOD_MOVIE_NAME_LEN   16u          /* the name and its NUL */

/* WPN_ classes (csv): 0 the wrench, 1..15 weapons, 16..23 devices, 24..31 gadgets. */
#define TOD_WPN_FIRST        1u
#define TOD_WPN_DEVICES      16u
#define TOD_WPN_GADGETS      24u

/* ------------------------------------------------------------- the marks */

#define TOD_DEC    1
#define TOD_DIS    2
#define TOD_SCAN   3
#define TOD_LEGACY 4
#define TOD_INFER  5

struct tod_mark {
	const char *name;
	u32 addr;
	u8  how;
	const char *note;
};

/* ------------------------------------------------------ the feature rows */

/*
 * One feature, REPORT-tod-lua section 5's shape. A row either writes memory
 * (write_addr, write_size) or runs Lua (lua), never both:
 *
 *   ACTION + lua           send the chunk; the reply's text is the result
 *   ENUM   + write_addr    write the chosen option as a write_size-byte
 *                          big-endian integer (the planet picker)
 *   ENUM   + lua           the chunk with every "%v" replaced by the option,
 *                          in decimal
 *
 * `kind` is proto.h's FEATURE_*. Live state (bolts, unlocks, the planet) is
 * never read through Lua: qwark reads and writes it in C every tick, as for
 * the other games; Lua is for actions and mods.
 */
#define TOD_ROW_NEEDS_LUA   0x01   /* the helper must be in */
#define TOD_ROW_GAMEPLAY    0x02   /* only does anything during gameplay frames */

struct tod_row {
	u8  id;
	u8  kind;
	u8  flags;
	u8  options;           /* ENUM: option count */
	const char *label;
	const char *lua;       /* NULL for a memory row */
	u32 write_addr;        /* 0 for a Lua row */
	u8  write_size;
};

/* ------------------------------------------------------ the fact sheet */

struct tod_facts {
	const char *title;

	/* Identity: one word that is never patched. */
	u32 fp_addr;
	u32 fp_word;

	/* The hot block: one read every tick. */
	u32 hot_addr;
	u16 hot_len;
	u16 hot_analog;        /* 4 x f32 LX, LY, RX, RY, already (v - 128) / 128 */
	u16 hot_pressed;       /* u32, the game's layout */
	u16 hot_released;      /* u32 */
	u16 hot_mask;          /* u32 */

	/* The game's pad bit i is the OG layout's bit pad_remap[i] (PROTOCOL.md 6). */
	u8  pad_remap[16];

	/* Planets. */
	u32 cur_level;         /* u32, the save's LEVEL_ id: what a load writes, never what starts one */
	u32 planet_request;    /* s32, -1 none: the pending load the game polls */
	u32 game_state;        /* s32, 4 during gameplay, when a load request is deferred to planet_request */
	u32 requested_state;   /* s32, 3 once a load is under way */
	u8  nplanets;
	const char * const *planet_names;   /* the LEVEL_ enum, 0 .. nplanets - 1 */

	/* The quit byte, set just before the game exits to the XMB. */
	u32 quit_byte;

	/* Positions: [[pos_root] + pos_hero] + pos_blob, 3 reads. */
	u32 pos_root;
	u32 pos_hero;
	u32 pos_blob;
	u16 pos_len;
	u32 hero_health;       /* off the hero, f32 */

	/* Inventory: g_saveData, which the hero's inventory pointer points at. */
	u32 save_data;
	u32 bolts;             /* u32 */
	u32 raritanium;        /* u32 */
	u32 weapons;           /* 32 slots of weapon_stride bytes, indexed by WPN_ */
	u16 weapon_stride;
	u8  nweapons;
	u8  weapon_owned;      /* u8 within a slot */
	u8  weapon_level;      /* u8, level - 1 */
	u8  weapon_xp;         /* f32 */
	u8  weapon_ammo;       /* f32 */
	u8  weapon_mods;       /* u32, a bit per mod */
	u32 weapon_unlocked;   /* 32 bytes, unlock_weapon's */
	u32 cheats;            /* CHEAT_COUNT bytes, 1 = unlocked */
	u8  ncheats;
	u32 level_flags;       /* g_levelFlags */

	/* Lua. */
	u32 lua_vm;            /* g_gameplayLuaVM */
	u32 lua_state;         /* its lua_State*, 0 between levels */
	u32 game_clock;        /* f32 time scale at +0, f32 game time at +0x14 */

	/* The features of the proof of concept. */
	const struct tod_row *rows;
	u8  nrows;

	const struct tod_mark *marks;
	u8  nmarks;
};

/* ------------------------------------------ the seam, tod_game.c / tod_panel.c */

/* Readout slots. */
#define TOD_RO_BOLTS       0
#define TOD_RO_RARITANIUM  1
#define TOD_RO_CHALLENGE   2
#define TOD_RO_CUTSCENE_CACHE 3   /* revision 1.29: Cache cutscenes' status word (TOD_CC_*) */

/*
 * Feature ids, stable: new ones go on the end. 0 to 3 are the proof of
 * concept's rows (tod_rows below: the Lua test, normal speed, the planet as a
 * write and through Lua), which DESCRIBE does not list: the client's Lua box
 * sends the first two through LUA_EXEC, and planets load through PLANET_LOAD.
 * The four numbers are reserved, never to be another feature's.
 */
#define TF_DIE              4
#define TF_BOLTS            5
#define TF_RARITANIUM       6
#define TF_CHALLENGE        7
#define TF_INFINITE_AMMO    8
#define TF_INFINITE_HEALTH  9      /* "God Ratchet" until 2026-10-27; the label changed, the id did not */
#define TF_UNLOCK_SP        10
#define TF_RESET_SP         11
#define TF_UNLOCK_GB        12
#define TF_RESET_GB         13
/* 14 and 15, the RYNO plans' unlock and reset, are retired, never to be another feature's. */
#define TF_GOD_RATCHET      16     /* the old RaCMAN's, the proxy handle (TOD_PROXY_LOW) */
#define TF_FAST_LOADS       17     /* the load screen's cut, a code patch (TOD_FASTLOAD_AT) */
#define TF_REMOVE_CUTSCENES 18     /* revision 1.29: eight FMVs' files made missing (TOD_ARCHIVE_GAME) */
/* Revision 1.29: the cutscene cache (tod_cache.c, TOD_CC_*); 22 and 23 are this round's, unused. */
#define TF_CACHE_CUTSCENES  19     /* ACTION: Cobalia's, Stratus City's and Fastoon's sound into the FIOS cache */
#define TF_CACHE_ON_RESET   20     /* TOGGLE: the same, by itself, on a new run's Metropolis */
#define TF_NO_CUTSCENE_CACHE 21    /* TOGGLE: each level load drops its planet's cached sound */

/* The savefile manager's round (docs/tod-savefile.md): ids 40..47. */
#define TF_SET_ASIDE        40
#define TF_LOAD_ASIDE       41

/*
 * Unlock row ids: WPN_ 1..31 as themselves, then these bases. Since
 * 2026-10-27 the list has OBJ_ 0..11 alone: 32 + 12..14 (the two sector
 * upgrades and the washer) and the skill point and gold bolt rows, 64.. and
 * 128.., are no longer listed, and their ids are reserved.
 */
#define TOD_UL_OBJ          32u    /* + OBJ_, 0..11 */
#define TOD_UL_SP           64u    /* reserved: + SKILLPOINT, 0..59 */
#define TOD_UL_GB           128u   /* reserved: + the gold bolt's index, 0..31 (tod_gold_bolt) */
#define TOD_NGOLD_BOLTS     32u

/*
 * The autosplitter's reason codes (revision 1.28; PROTOCOL.md 8.5).
 * Code 1 is every game's planet split; 3 is the title index's pause pair.
 */
#define TOD_AS_PLANET       1      /* SPLIT, arg the LEVEL_ just loaded */
/* 2, Tachyon's health reaching 0, was retired in revision 1.29: never reuse it. */
#define TOD_AS_QUIT         3      /* PAUSE / RESUME around a quit to the XMB */
#define TOD_AS_LOAD         4      /* LOAD_START / LOAD_END, the old autosplitter's long load (revision 1.29) */
/* 5, the credits (g_gameState becoming 11), was retired in revision 1.29: never reuse it. */
#define TOD_AS_MISSION      6      /* SPLIT, any mission completed: arg level << 16 | its label id */
#define TOD_AS_END_FADE     7      /* SPLIT, "Tachyon defeated": the screen black before the ending cinematic (revision 1.29) */
/* 8, the ending FMV, was retired in revision 1.29: never reuse it. */
#define TOD_QUIT_PAUSE_US   19950000u  /* the old ToD script's reboot: 19.95 s of game time */

/*
 * What the watcher keeps, a part of tod_game.c's state (the lent area in a
 * module). The `cur_` fields are the hot block's, written by its decode.
 */
struct tod_as_state {
	u32 cur_lua;           /* the gameplay lua_State*, 0 between levels */
	s32 cur_level;         /* g_currentLevel */
	s32 cur_state;         /* g_gameState */
	u8  cur_front;         /* g_inFrontEnd */
	u8  cur_screen;        /* g_loadScreenType's low byte, the legacy's 0x102034FB */
	u8  cur_tier;          /* g_levelLoadTier's loaded byte (revision 1.29) */
	u32 cur_job;           /* the tier loader's job (revision 1.29) */
	u8  primed;
	u8  seen;              /* this process has shown its front end or a level */
	u32 lua;               /* the last tick's */
	u8  front;
	u8  missions_primed;
	u8  nmissions;
	u8  long_load;         /* the legacy's is_loading, as last evaluated */
	u32 origin;            /* the level the player is on, TOD_AS_NOWHERE when none */
	u32 mission_level;     /* whose completed list `missions` is */
	u32 missions[TOD_NMISSIONS];
	u32 ticks;
	/* Revision 1.29: the start at the fade in, the end at the fade to black. */
	u8  start_wait;        /* TOD_SW_*: a new run's RESET is out and its START waits */
	u8  end_primed;        /* the ending's first look at this level up is taken */
	u8  end_fade_done;     /* code 7 is out, or was learnt, this level up */
	u8  movie_is_end;      /* whether movie_seen is TOD_END_CINEMATIC */
	s32 movie_seen;        /* the player's movie index last named, -1 none */
	u8  loading;           /* a level load was under way at the last tick */
	u8  start_armed;       /* the run's level's own fade in seen under way (fade_in_halfway) */
	u8  pad2[2];
	u32 start_lua;         /* the lua_State whose fade in the START waits for */
	u32 start_prog;        /* the fade's progress at the last look, TOD_AS_NO_PROG when none */
};
#define TOD_SW_NONE         0      /* no run waiting */
#define TOD_SW_LOADING      1      /* the load is under way: no level up since the RESET */
#define TOD_SW_LEVEL        2      /* Metropolis is up: START as its fade in starts */
#define TOD_AS_NOWHERE      0xFFFFFFFFu
#define TOD_AS_NO_PROG      0xFFFFFFFFu

/* tod_autosplit.c */
void tod_autosplit_forget(struct tod_as_state *as);
/* The old autosplitter's is_loading: the level's low byte 0, the load screen's above 1. */
int  tod_long_load(s32 level, u8 screen);
void tod_autosplit_tick(struct tod_as_state *as);
/* The game is going: a long load still open is closed before the quit's PAUSE (revision 1.29). */
void tod_autosplit_quit(struct tod_as_state *as);
const struct autosplit_desc *tod_autosplit_describe(u8 *count);

/* tod_panel.c */
int  tod_hero(u32 *hero);
int  tod_trigger(u8 id);
int  tod_set_value(u8 id, u32 value);
int  tod_set_toggle(u8 id, int on);
int  tod_toggle_read(u8 id, int *on);
int  tod_save_blob(u8 *blob, u8 *len);
int  tod_load_blob(const u8 *blob, u8 len);
int  tod_planet_load(u8 planet, u8 flags);
int  tod_die(void);
int  tod_unlock_list(const struct game_unlock **list, u8 *count,
                     const char * const **categories, u8 *ncategories,
                     const struct unlock_field_desc **fields);
int  tod_unlock_read(const struct game_unlock *entry, u32 values[4]);
int  tod_unlock_set(u8 id, u8 field, u32 value);
/* Infinite ammo's hold: once a tick, with the game's flag and the level object as read. */
void tod_ammo_tick(u32 flag, u32 root);
/* A new process: the hold starts over. */
void tod_panel_reset(void);
void tod_panel_init(void);   /* once a module: Fast loads' patch */
/*
 * Revision 1.29. A session's arrival: the fade time and the eight FMVs as the
 * game has them, before the auto-flagged toggles come back. The tick: the
 * fade time back once qwark's load has begun, and the FMVs looked after.
 */
void tod_panel_enter(void);
/* Revision 1.29: the watcher's view (the hot block's job, tier and lua_State) for the forced fast load's end. */
void tod_panel_tick(const struct tod_as_state *as);
/*
 * qwark's own load is about to be asked for: its fade-out a frame long, or
 * TOD_LOAD_FADE_STORY for a story FMV load (`story`, tod_story_load's answer).
 */
void tod_fade_arm(int story);
/* and it was not: the game's fade time back. */
void tod_fade_disarm(void);
/*
 * Revision 1.29 (pending): a load qwark starts (PLANET_LOAD, the Trainer's
 * list, a pick on the galactic map it opened) is fast-loaded, as the four
 * original games' PLANET_LOAD is (rac1_planet_load and the others): Fast
 * loads forced on for that load, the toggle off or not, and put back once
 * the new level is up (the watcher's level load seen, then over, a level's
 * lua_State), or after TOD_FADE_WAIT_TICKS with no load begun. With the
 * toggle on there is nothing to force or put back. tod_galmap_up: the core's
 * menu_picker `up`, the map qwark opened is up: the force and the fade
 * (TOD_LOAD_FADE_STORY while flag 106 is clear, a pick of Metropolis or
 * Cobalia then being a story load; else the frame) armed before any pick, so
 * neither depends on qwark's tick beating the map's exit; the pick's level
 * (TOD_GALMAP_PICK, read once g_gameState was read) then sets the fade it
 * wants, and the map gone without one puts both back at once.
 */
void tod_galmap_up(void);
/* The eight FMVs' files missing (1) or back (0): how many were changed, or -1 for no table. */
int  tod_cutscenes_apply(int remove);

/*
 * tod_save.c, the savefile manager (docs/tod-savefile.md). The two flagged
 * ACTIONs, which keep the set-aside on the HDD; the core's descriptor and
 * driver for the game's own save system; and a session's first look, which
 * gives back a hold or a load a qwark that went away left in the game.
 */
int  tod_set_aside(void);
int  tod_load_aside(void);
u8   tod_savefile_ready(u8 what);
const struct sf_desc *tod_savefile_desc(u8 game_id);
extern const struct sf_driver tod_sf_driver;
void tod_save_recover(void);

/* The gold bolt `index` (0..31) as its level and its bit: 1, or 0 for none. */
int  tod_gold_bolt(u8 index, u8 *level, u8 *bit);
/* A device's (WPN_ 16..23) full count as f32 bits, what Infinite ammo keeps it at; 0 for none. */
u32  tod_device_max_ammo(u8 wpn);
/* The XP weapon `wpn` (1..15) has at level index `level` (0..9), whole points; 0 for none. */
u32  tod_weapon_xp(u8 wpn, u8 level);

/*
 * The level flags (tod_flags.c, revision 1.28): the game's
 * GLOBAL_FLAG_ enum, 292 flags, one bit each in five big-endian u64 words at
 * g_saveData+0x5528. Flag n is bit n & 63 of word n >> 6 (Lua_check_flag,
 * Lua_set_flag, Lua_clear_flag, SetProgressionFlag), so its byte is
 * (n >> 6) * 8 + 7 - ((n & 63) >> 3) and its bit there n & 7. The region is
 * the game's, not a planet's: LEVELFLAGS_GET answers the same 40 bytes for
 * every planet, and LEVELFLAGS_RESET clears the flags of that planet's group
 * (tod_flags.c; their names are the client's since revision 1.29).
 */
#define TOD_LEVEL_FLAGS      0x10211398u  /* g_levelFlags, ulonglong[5] (dec) */
#define TOD_NLEVEL_FLAGS     292u         /* GLOBAL_FLAG_COUNT */
#define TOD_LEVEL_FLAGS_LEN  40u          /* 5 u64 words */
#define TOD_LF_BYTE(n)       ((((n) >> 6) * 8u) + 7u - (((n) & 63u) >> 3))
#define TOD_LF_BIT(n)        ((u16)(TOD_LF_BYTE(n) * 8u + ((n) & 7u)))

int  tod_levelflags_get(u8 planet, u8 *out, u16 cap, u16 *len);
int  tod_levelflags_reset(u8 planet);
int  tod_levelflags_set(u8 planet, u16 offset, u8 value);

/*
 * Revision 1.29 (pending): what the cutscene cache (tod_cache.c, below;
 * REPORT-tod-level-cache.md sections 2, 9 and 10) reads and writes. All read
 * in the decompile (dec).
 *
 * The level warmer, g_levelPrefetch (LevelPrefetch, 0x58 bytes): four files
 * set up at every level load (the level's streaming sound and dialogue, its
 * textures, the next story planet's level_cached), read 256 KB a frame in
 * gameplay through the FIOS block cache, which fills it. `state` is how many
 * files it has started; idle once `state` is 4 and `curFile` 0.
 * GameState_Dispatch writes `enabled` every frame: off in game states 5..9
 * (an FMV, a cinematic), else the two gates ANDed.
 */
#define TOD_PREFETCH         0x1037A704u  /* g_levelPrefetch, LevelPrefetch */
#define TOD_PF_LEN           0x58u
#define TOD_PF_SIZE          0x00u        /* u32[4] the files' sizes */
#define TOD_PF_FILE          0x10u        /* TodFileDesc *[4]: { char *path; u32 size } */
#define TOD_PF_READY         0x24u        /* u32, a read may be issued */
#define TOD_PF_BUSY          0x28u        /* u32, a read in flight */
#define TOD_PF_OFFSET        0x30u        /* u32, where the next read starts */
#define TOD_PF_END           0x34u        /* u32, the current file's end */
#define TOD_PF_CUR           0x48u        /* TodFileDesc *, 0 when idle */
#define TOD_PF_STATE         0x4Cu        /* u32, 0..4 */
#define TOD_PF_ENABLED       0x54u        /* u8 */
#define TOD_PF_NFILES        4u

/*
 * The libFIOS scheduler cache on /dev_hdd1: g_fiosSchedulerCache points at
 * the cache object, whose `blocks` are FiosCacheBlockDesc[numBlocks], 0x28
 * bytes each. A block's `lastUse` (u64, FiosTime_nowNs: ns of the clock) is 2
 * when the slot is free and 3 while it fills; a hit refreshes it, and the
 * smallest is evicted first. Its path is g_fiosPathTable's string for
 * `pathHandle`: [table + 0x5C][handle - 1], `handle` 1 .. [table + 0x64].
 */
#define TOD_FIOS_CACHE       0x106ACBA4u  /* g_fiosSchedulerCache, FiosSchedulerCache * */
#define TOD_FC_BLOCK_SIZE    0xD8u        /* u32, 1 MiB */
#define TOD_FC_NBLOCKS       0xDCu        /* u32, 0x700 */
#define TOD_FC_BLOCKS        0xE0u        /* FiosCacheBlockDesc * */
#define TOD_FC_DESC_LEN      0x28u
#define TOD_FC_LAST_USE      0x00u        /* u64 */
#define TOD_FC_OFFSET        0x18u        /* u64, the block's offset in its file */
#define TOD_FC_PATH          0x24u        /* u32, the path handle */
#define TOD_FC_FREE          2u
#define TOD_FC_FILLING       3u
#define TOD_FC_MAX_BLOCKS    4096u        /* a count past this is not the table */
#define TOD_FIOS_PATHS       0x1086C910u  /* g_fiosPathTable, its table * */
#define TOD_FPATH_STRS       0x5Cu        /* char *[] */
#define TOD_FPATH_COUNT      0x64u        /* u32 */

#define TOD_TOC_LEVELS       0x28u        /* game.toc: TodLevelArchives[count], the count at [-1] */
#define TOD_LA_STRIDE        0x40u        /* { char *name; TodArchiveRef cached, uncached, textures } */
#define TOD_LA_MAX           64u

extern const struct tod_facts tod_facts;

/* The game's pad mask in the OG layout. */
u32 tod_pad_to_og(u32 tod_mask);

/*
 * An ENUM Lua row's chunk with "%v" replaced by `value` in decimal, into dst
 * (cap bytes, always ended). Returns the length, or -1 when it does not fit.
 */
int tod_row_chunk(const struct tod_row *row, u32 value, char *dst, u32 cap);

/*
 * Revision 1.29 (pending): the console menu, the game's own pause menu
 * (docs/tod-menu.md; the helper's side is src/games/luahelper/lh_menu.h).
 * What says no menu can be up: the pause menu's state 0 (none) with nothing
 * queued on the state stack, and the game not in its pause (g_gameState 5).
 */
#define TOD_PAUSE_STATE      0x101CB218u  /* g_pauseMenuState: 0 none, 1 loading, 2..3 up, 4 closing (dec) */
#define TOD_PAUSE_UP         3u
#define TOD_STATE_PENDING_OP 0x10387DBCu  /* g_gameStatePendingOp, 0 with nothing queued (dec) */
#define TOD_STATE_PAUSE      5u           /* g_gameState: the pause menu (PauseMenu_update) */

/*
 * Revision 1.29 (pending): the galactic map, the Trainer's planet picker
 * (docs/tod-menu.md 16). The game opens it in one way only, GalMap_request
 * (0x35084, GameState_queueOverlay op 2, state 6, GalMapState_enter and
 * GalMapState_exit), from the gameplay package's gmap() and from the end of a
 * space combat level; the map's handler (gmapHndler) lists every level of the
 * sectors it shows that GalMap_isLevelVisitable passes (its level's unlocked
 * byte set, the byte after visited clear, not Metropolis, and Fastoon only
 * while Fastoon II is not), and a pick is GalMap_LoadLevel: the level into
 * g_galMap.selectedLevel (Mukow, Rykan V and Kerchu City go to the space
 * combat level before them while its visited byte is clear), and on the way
 * out LoadLevel(level, 0, 0) into g_planetLoadRequest, the game's own load.
 * Back (exit_galactic_map) leaves selectedLevel -1 and goes back to play.
 */
#define TOD_STATE_GALMAP     6u           /* g_gameState: the galactic map (GalMapState_update 0x360BC) */
#define TOD_GALMAP_PICK      0x101CBFC0u  /* g_galMap.selectedLevel, s32: -1 from GalMapState_enter, a pick's level (dec) */
#define TOD_LS_UNLOCKED      0x400u       /* ToDLevelSave.unlocked, u8 (Level_unlock) */
#define TOD_LS_VISITED       0x401u       /* ToDLevelSave.visited, u8 (the space combat levels' redirect, GalMap_selectLevel) */
#define TOD_LS_HIDDEN        0x402u       /* ToDLevelSave.unk_402, u8: 0 for GalMap_isLevelVisitable */
#define TOD_LEVEL_SC1        4u           /* LEVEL_SPACE_COMBAT_I */
#define TOD_LEVEL_SC2        8u
#define TOD_LEVEL_SC3        13u

/*
 * Revision 1.29 (pending): a run's start, shared by the autosplitter's RESET
 * and Cache cutscenes on reset. A level load is under way while the tier
 * loader's job is state 3's list and g_levelLoadTier is not loaded (above);
 * a load into Metropolis is a new run while the file has
 * MOVIE_METROPOLIS_TACHYON clear (a flag that cannot be read: no run).
 */
int  tod_level_loading(u32 job, u8 tier);
int  tod_fresh_run_load(s32 level);

/*
 * Revision 1.29 (pending): the cutscene cache (tod_cache.c;
 * REPORT-tod-level-cache.md sections 2.3, 5.A and 10). A planet's opening
 * cutscene streams its audio from its streaming_sound.dat, inside its
 * level_uncached.psarc, through the FIOS block cache, which those reads never
 * fill; only the level warmer does, and it caches the current planet's sound
 * once its gameplay (and so its opening cutscene) has begun. All data writes.
 *
 * Cache cutscenes (19) has the warmer read Cobalia's sound, then Stratus
 * City's, then Fastoon's (the order a run needs them), ahead of the level's
 * own files: while the warmer reads one of
 * its own slots k (state k + 1), the planet's game.toc .uncached descriptor
 * goes into file[k] with fileSize[k] its sound's end, state back to k, and
 * the slot's own read cut short (fileEnd a little past where it is), so the
 * warmer's step takes ours next and reads it from 0; the next planet, then
 * the game's own file back, go into the same slot while ours is read, and
 * state k again each time, so the game's file is read again after ours (its
 * offset put on where it was cut) and the warmer goes on as the game set it
 * up. With the warmer idle, the single-range form instead: chunkSize, offset
 * and fileEnd, curFile, state 4, readReady last. A level load does not end a
 * request: what is left is picked up on the next level, and the FIOS block
 * table is read first (at the request and at each load) so cached blocks are
 * skipped; a load into a planet still to read drops that planet alone (too
 * late for it), and the request ends failed with its reason once the rest is
 * in. Cache cutscenes on reset (20) asks for the same on a new run's
 * load into Metropolis. Disable cutscene caching (21) frees, in the in-RAM
 * block table, every block of the planet's level_uncached.psarc (and
 * Metropolis's sound in global_uncached.psarc) during each level load, so the
 * planet's opening cutscene always starts cold (dec).
 *
 * The warmer's writes, from LevelPrefetch_update and its read's callback, and
 * why each of ours is safe: `state` and file[]/fileSize[] are read only when
 * chunkSize is 0, at the end of a file, so with TOD_CC_MARGIN left of the
 * file in hand they are ours to write; the callback does offset += chunkSize,
 * chunkSize = min(fileEnd - offset, 256 KB), so a new fileEnd at or past every
 * offset it can reach, and a new offset at least 256 KB before fileEnd, keep
 * every chunk inside the file. A cut whose offset has already reached it is
 * undone at once (the old fileEnd back).
 */
#define TOD_PF_CHUNK         0x2Cu        /* u32, the read the warmer issues next; 0 steps the state */
#define TOD_PF_CHUNK_MAX     0x40000u     /* 256 KB, the warmer's read */
#define TOD_LA_UNCACHED      0x18u        /* TodLevelArchives.uncached: { char *path; u32 size; ... } */
#define TOD_SOUND_PARAM      0x100EB570u  /* g_systemParam122 (Sys_readSystemParams): 2 picks streaming_sound.jp.dat (dec) */
#define TOD_SOUND_PARAM_JP   2u
#define TOD_GLOBAL_UNCACHED  "/packed/game/global_uncached.psarc"
#define TOD_METRO_SOUND_END  59748728u    /* Metropolis's streaming_sound.dat in global_uncached.psarc ends here: blocks 0..56 */
#define TOD_CC_EVERY         4u           /* ticks between two looks at the warmer */
#define TOD_CC_STALL_TICKS   3600u        /* 30 s in gameplay with the warmer on and no read done: stalled */
#define TOD_CC_READ_FAILS    3u
#define TOD_CC_PIECE         32u          /* block descriptors a tick reads, 1280 bytes */
#define TOD_CC_HANDLES       24u          /* path handles a walk remembers */
#define TOD_CC_PATH_LEN      64u
#define TOD_CC_MARGIN        0x200000u    /* 2 MiB of the file in hand left: the slot and state are ours to write */
#define TOD_CC_GRACE         0x100000u    /* a slot's own read cut 1 MiB past where it is */
#define TOD_CC_SLACK         0x200000u    /* ours left the warmer within 2 MiB of its end: it is in */
#define TOD_CC_MAP_WORDS     4u           /* 128 blocks a planet's range can span */
#define TOD_CC_BLOCK         0x100000u    /* the cache's block, 1 MiB: what is left of a range starts on one */
#define TOD_CC_TICK_HZ       120u

/*
 * Cache cutscenes' status word, TOD_RO_CUTSCENE_CACHE (PROTOCOL.md 5.14):
 *   bits 28..31  state   TOD_CC_NONE .. TOD_CC_FAILED
 *   bits 24..27  detail  WAITING: TOD_CC_WAIT_*; CACHING: the planet, 1
 *                        Fastoon, 2 Stratus City, 3 Cobalia; FAILED:
 *                        TOD_CC_WHY_* (a late one once the rest is in)
 *   bits 16..23  CACHING: the percentage of that planet's sound read; DONE:
 *                the read rate in tenths of a MiB/s (at most 255), 0 when
 *                everything was cached already
 *   bits 8..15   seconds since the request, or how long it ran, at most 255
 *   bits 0..7    runs ended since the game started, modulo 256
 * A boot of the game comes back to NONE: the cache is cleared at every boot.
 */
#define TOD_CC_NONE          0u
#define TOD_CC_WAITING       1u
#define TOD_CC_CACHING       2u
#define TOD_CC_DONE          3u
#define TOD_CC_FAILED        4u
#define TOD_CC_WAIT_PLAY     1u           /* not in gameplay */
#define TOD_CC_WAIT_WARMER   2u           /* the warmer has no file in hand to go ahead of, or is near its end */
#define TOD_CC_WHY_LEVEL     1u           /* the title before it finished */
#define TOD_CC_WHY_TABLE     2u           /* game.toc's level table cannot be read, or its planet is not in it */
#define TOD_CC_WHY_STALLED   3u           /* the warmer stopped reading */
#define TOD_CC_WHY_DISABLED  4u           /* Disable cutscene caching is on */
#define TOD_CC_WHY_READ      5u           /* the game's memory could not be read or written */
#define TOD_CC_WHY_QUIT      6u           /* the game is going */
#define TOD_CC_WHY_LATE_SC   7u           /* a load into Stratus City before its sound was in */
#define TOD_CC_WHY_LATE_FN   8u           /* a load into Fastoon before its sound was in */
#define TOD_CC_WHY_LATE_CB   9u           /* a load into Cobalia before its sound was in */
#define TOD_CC_NPLANETS      3u           /* 0 Fastoon, 1 Stratus City, 2 Cobalia; the last is read first */
#define TOD_CC_ALL           7u           /* every planet's bit */
#define TOD_CC_NO_PLANET     0xFFu

struct tod_cc_state {
	u8  state;             /* TOD_CC_* */
	u8  detail;
	u8  planet;            /* the one queued or read, 0 Fastoon, 1 Stratus City, 2 Cobalia */
	u8  pct;
	u8  seq;
	u8  seen_play;         /* the request has seen gameplay: the title after that ends it */
	u8  is_auto;           /* asked by Cache cutscenes on reset */
	u8  read_fails;
	u8  auto_on;           /* Cache cutscenes on reset */
	u8  evict_on;          /* Disable cutscene caching */
	u8  loading;           /* a level load at the last tick */
	u8  primed;
	u8  wait;              /* ticks to the next look at the warmer */
	u8  ev_active;         /* a walk of the block table is under way, Disable's */
	u8  ev_metro;          /* and its planet is Metropolis */
	u8  ev_nh;
	u8  pending;           /* bit p: planet p's sound still to read */
	u8  late;              /* bit p: a load into planet p came first: dropped, too late for it */
	u8  planned;           /* pdesc/pfs/pfe looked up since the request or the last load */
	u8  in_hand;           /* `planet`'s range has been seen in the warmer's hands */
	u8  borrow;            /* slot `slot` holds ours: the game's own is in slot_desc/slot_size */
	u8  slot;
	u8  queued;            /* while `planet` is read: what comes next is in the slot */
	u8  next;              /* that next planet, or TOD_CC_NO_PLANET for the game's own file */
	u8  sc_active;         /* a walk of the block table for the request (shares the ev_ fields) */
	u32 ticks;             /* since the request */
	u32 secs;              /* how long the last run ran */
	u32 desc;              /* `planet`'s .uncached descriptor */
	u32 start;             /* its range still to read */
	u32 end;
	u32 last_off;
	u32 stall;
	u32 slot_desc;         /* the game's own file in the borrowed slot */
	u32 slot_size;
	u32 slot_cut;          /* where its read was cut, the offset to put it back on */
	u32 bytes;             /* read for this request */
	u32 rd_ticks;          /* ticks with ours in the warmer's hands and it on */
	u32 pdesc[TOD_CC_NPLANETS];   /* each planet's .uncached descriptor */
	u32 pfs[TOD_CC_NPLANETS];     /* its whole sound range */
	u32 pfe[TOD_CC_NPLANETS];
	u32 need_lo[TOD_CC_NPLANETS]; /* what is left of it (0, 0: all of it) */
	u32 need_hi[TOD_CC_NPLANETS];
	/* The walks: the table, where it is, the path table, and what it learnt. */
	u32 ev_blocks;
	u32 ev_n;
	u32 ev_i;
	u32 ev_strs;
	u32 ev_npaths;
	u16 ev_freed;
	u16 ev_missed;
	u32 ev_handle[TOD_CC_HANDLES];
	u8  ev_verdict[TOD_CC_HANDLES];   /* Disable's: 0 another file, 1 the planet's level_uncached, 2 global_uncached;
	                                     the request's: 0 another file, 1 + the planet */
	char ev_path[TOD_CC_PATH_LEN];
	u32 sc_bsize;
	u32 sc_first[TOD_CC_NPLANETS];    /* the first block of each planet's range, and how many */
	u32 sc_count[TOD_CC_NPLANETS];
	u32 sc_map[TOD_CC_NPLANETS][TOD_CC_MAP_WORDS];   /* the blocks of it in the cache */
};

void tod_cc_forget(struct tod_cc_state *cc);
void tod_cc_tick(struct tod_cc_state *cc, const struct tod_as_state *as);
void tod_cc_quit(struct tod_cc_state *cc);
int  tod_cc_trigger(struct tod_cc_state *cc, const struct tod_as_state *as);
int  tod_cc_set(struct tod_cc_state *cc, u8 id, int on);
u32  tod_cc_status(const struct tod_cc_state *cc);
/* The status word as the console menu's row under Cache cutscenes says it. */
void tod_cc_menu_status(u8 id, u32 word, char *out, u32 cap);

#endif /* QWARK_TOD_H */
