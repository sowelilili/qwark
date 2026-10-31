/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the Lua
 * helper's address table.
 *
 * Executable: EBOOT.ELF md5 df9d68ada99b3f44ffe798ce03a1549c. Every address and
 * word below is from REPORT-tod-lua.md and was checked again against that file
 * by check.py (beside this header) and in the Ghidra project (NPEA00452.ELF)
 * when the helper was written, 2026-10-27. None of it has been run on a
 * console or on RPCS3 yet: docs/tod-poc.md section 5 lists the live checks.
 *
 * build.sh reads the #define lines, so each is a plain name and a hexadecimal
 * literal with nothing else on it. Everything is also included by the C
 * helper and by the assembler entry (through cpp), so nothing here but
 * #defines and comments.
 */
#ifndef QWARK_LH_TOD_H
#define QWARK_LH_TOD_H

/* ------------------------------------------------------------------ caves */

/*
 * The code cave: the padding of the code segment's last 64 KB page. The
 * segment (PT_LOAD 0, R-X) is 0x10000..0x867360; 0x867318..0x867360 are the
 * process parameters the segment ends with, and from 0x867360 to the end of
 * the page, 0x870000, the file has nothing (the next segment starts at
 * 0x870000). Nothing in the executable refers to the range (check.py).
 * The page is mapped on a console if lv2 maps a segment in whole 64 KB pages,
 * which the RW cave below relies on too; neither is verified live.
 */
#define LH_CAVE     0x00867360
#define LH_CAVE_END 0x00870000

/*
 * The mailbox (src/core/luamb.h, LUAMB_SIZE bytes): the tail of the OPD/TOC
 * segment's last 64 KB page. That segment (PT_LOAD 1, RW) is 0x870000..
 * 0x8C9A08, and its TLS image ends at 0x8C9BF8; 0x8CA000..0x8D0000 is beyond
 * both, inside the page, and unreferenced (check.py). The Lua mailbox has
 * its start, to 0x8CC600; revision 1.29's console menu (lh_menu.h) has its
 * own two blocks further on, at 0x8CD000 and 0x8CE000.
 */
#define LH_MAILBOX     0x008CA000
#define LH_MAILBOX_END 0x008CC600

/* ------------------------------------------------------------------ hooks */

/*
 * The VM hook: `bl CGameplayLuaVM_update` in Game_UpdateSystems (0x4C4574),
 * r3 = &g_gameplayLuaVM loaded the instruction before (`lwz r3,0x6de4(r2)`),
 * a `nop` after it (same TOC range, so no r2 reload), and only reached on a
 * gameplay frame: Game_UpdateFrame skips it when paused, in a menu or when
 * the state is not gameplay. The helper's entry calls the original target
 * first and then looks at the mailbox, so the game's scripts run exactly as
 * before. Relative branch.
 */
#define LH_HOOK_ADDR  0x004C461C
#define LH_HOOK_ORIG  0x4BDCC909
#define LH_HOOK_NEXT  0x60000000

/*
 * The error tap: CLuaVM_reportError, a lone `blr` in retail, which
 * CLuaVM_handleResult calls through the vtable (slot 0x18) with the formatted
 * "<kind> Error - <message>" text in r4. The gameplay VM's own slot,
 * CGameplayLuaVM_reportError (0x290B70), is a call through the cross-TOC
 * trampoline 0x268610 to this same word, so one branch here taps both VMs.
 * The word becomes `b <tap>`, no link: the tap returns to whoever called
 * reportError.
 */
#define LH_ERR_HOOK_ADDR 0x0068F8F8
#define LH_ERR_HOOK_ORIG 0x4E800020

/*
 * Revision 1.29 (pending), the console menu (lh_menu.h, docs/tod-menu.md):
 * two more hooks, both cross-TOC calls whose caller reloads r2 from its own
 * stack afterwards (`ld r2,40(r1)` follows), so each entry stores the caller's
 * r2 there first, as the trampoline it replaces would.
 *
 * The pause menu's frame: PauseMenu_update's call of AnarkMenuSystem_update
 * through the trampoline 0x119D0, r3 = the pause's menu system (lwz r3,0(r31),
 * r31 = &g_pauseMenuSystem), reached only while the pause menu is up (its
 * state 2 or 3), on the main thread. The entry calls the trampoline, then the
 * menu half of the helper on the menu system's lua_State.
 */
#define LH_MENU_HOOK_ADDR  0x000242B0
#define LH_MENU_HOOK_ORIG  0x4BFED721
#define LH_TR_MENU_UPDATE  0x000119D0
#define LH_MENU_UPDATE     0x00653D30

/*
 * Game_UpdateFrame's test of the pause action (InputMapper_isPressed(
 * g_gameInputMapper, 1) through the trampoline 0x2672E0), the last of the
 * checks before it opens the pause menu with PauseMenu_request. The entry
 * calls the test itself (the trampoline's target, in the Lua range) and then
 * lm_pause_check, which answers "pressed" for qwark's open request.
 */
#define LH_PAUSE_HOOK_ADDR 0x004C5ABC
#define LH_PAUSE_HOOK_ORIG 0x4BDA1825
#define LH_TR_BTN_PRESSED  0x002672E0

/* The word after both: ld r2,40(r1), the caller's own r2 back. */
#define LH_TOC_RELOAD      0xE8410028

/* ------------------------------------------------------------------ TOCs */

/*
 * The executable has three TOC ranges (REPORT-tod-lua section 1). The hook
 * site and CGameplayLuaVM_update are in the middle one; every Lua core
 * function the helper calls is in the top one. The entry keeps the caller's
 * r2 on its stack, calls the update with it, sets LH_LUA_TOC for the C part
 * and puts the caller's back before returning.
 */
#define LH_GAME_TOC 0x008BB068
#define LH_LUA_TOC  0x008CB054

/* ------------------------------------------------------- the gameplay VM */

/*
 * g_gameplayLuaVM, a static CGameplayLuaVM. +4 is its lua_State*, 0 between
 * levels; +8 is its LUA::PoolAllocator, which every C entry into Lua makes
 * current first (CLuaVM_executeBuffer does LuaAlloc_setCurrent(vm + 8)).
 */
#define LH_VM       0x102043CC
#define LH_VM_STATE 0x00000004
#define LH_VM_ALLOC 0x00000008

/* The allocator LuaAlloc_* use, which the helper puts back afterwards. */
#define LH_CUR_ALLOC 0x10740DA8

/* g_luaCurrentScriptName, char[64], set before each level script resumes. */
#define LH_SCRIPT_NAME 0x10740DB0

/* ----------------------------------------------------- the pause menu */

/*
 * The pause's menu system (AnarkMenuSystem, 0x14B0 bytes, built per pause by
 * PauseMenuLoadingThread): g_pauseMenuSystem holds it, 0 with no pause, and
 * its lua_State* is at +0x106C, the Anark presentation's own VM (Lua 5.0.2,
 * the menu package's bindings, the presentation's behaviours).
 */
#define LH_PAUSE_MS        0x101CB220
#define LH_MS_STATE        0x0000106C

/* ------------------------------------------------------- game functions */

/* The middle range's; called with the caller's r2. */
#define LH_FN_VM_UPDATE 0x00290F24

/* Lua 5.0.2 core and the game's allocator switch, all the top range's. */
#define LH_FN_ALLOC_SET_CURRENT 0x0068FB9C
#define LH_FN_GETTOP            0x00690E10
#define LH_FN_SETTOP            0x00690E28
#define LH_FN_PCALL             0x00692990
#define LH_FN_TYPE              0x00692CF8
#define LH_FN_TOBOOLEAN         0x00693654
#define LH_FN_TOSTRING          0x0069373C
#define LH_FN_LOADBUFFER        0x00694A64

/*
 * Revision 1.29: the menu half's. AnarkMenu_luaAllocator returns the
 * allocator every Anark VM runs on (AnarkMenuSystem_update makes it current
 * first); the rest are Lua 5.0.2's API, for the glue's C function qw().
 * InputMapper_isPressed is the pause hook's original target.
 */
#define LH_FN_ANARK_ALLOC       0x00653048
#define LH_FN_BTN_PRESSED       0x0068B654
#define LH_FN_PUSHNIL           0x00690EB8
#define LH_FN_PUSHNUMBER        0x00690ED4
#define LH_FN_PUSHLSTRING       0x00690EF4
#define LH_FN_PUSHCCLOSURE      0x0069107C
#define LH_FN_GETTABLE          0x00691FC4
#define LH_FN_SETTABLE          0x006923F8
#define LH_FN_TONUMBER          0x0069354C
#define LH_FN_PUSHSTRING        0x00693E50

/* Lua 5.0's pseudo-index of the globals, and its type of a function. */
#define LH_GLOBALSINDEX         (-10001)
#define LH_T_FUNCTION           6

#endif /* QWARK_LH_TOD_H */
