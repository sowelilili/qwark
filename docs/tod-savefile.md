# Tools of Destruction: the savefile manager

Game 5, NPEA00452 v01.00 (EBOOT.ELF md5 df9d68ada99b3f44ffe798ce03a1549c). Written 2026-10-27, from the shipped executable in Ghidra (DL-ghidra:/NPEA00452.ELF), its disassembly, and the GAME.SAV files a user's RPCS3 keeps. **Verdict: viable, and implemented.** It needs no code in the game at all: everything is data writes, so a console and RPCS3 behave the same, with no ui.pak entry and no RPCS3 patch.

Nothing below has run on a console or a real RPCS3 yet. Section 6 lists the live checks.

## 1. What the user gets

The same Save files panel and quick buttons as the four original games:

- **Save to a slot** (SAVEFILE_STORE): the game's last save goes into the console library, `/dev_hdd0/qwark/savefiles/NPEA00452/<category>/<name>.sav`. "Last save" means what the other games mean by it: the image of the last GAME.SAV the game wrote (an autosave or a menu save) or read (a load), not the live state since then.
- **Load a file** (SAVEFILE_RESTORE): any `.sav` of this game, from the library, the PC mirror or the premade savefiles, is loaded as the game loads a save from its own menu. The game fades out and loads the save's planet.
- **Set aside / Load set-aside** (features 40 and 41, flagged SAVE_ASIDE and LOAD_ASIDE): the same pair as the other games, also on the Game page's quick block and the Load-set-aside combo. In ToD the set-aside is a file, `/dev_hdd0/qwark/savefiles/NPEA00452/set-aside.sav`, which no listing shows (it sits beside the categories rather than in one). It survives autosaves and reboots. See section 4.3 for the reason.
- **Premade savefiles**: the client's mechanism needs nothing new. A folder `premade-savefiles/NPEA00452/` with `.sav` files and a manifest entry ships them. None ships yet: we have no ToD saves to make premades from (section 5).

A `.sav` is the GAME.SAV image byte for byte, 0x906F0 = 591,600 bytes. The GAME.SAV files RPCS3 keeps in its savedata folder are unencrypted and the same layout, so a user can copy one into the library as it is. A console's GAME.SAV is a protected file (secureFileId), and the copy on its HDD is encrypted.

## 2. How the game saves and loads (the RE)

### 2.1 The objects

| Name | Address | What |
|---|---|---|
| `g_saveData` | 0x1020BE70 | `ToDSaveData`, 0x906F0 bytes: the live save state the game plays on (weapons, bolts, level saves, flags; the current level at +0x906E8, challenge mode at +0x906EC) |
| `g_saveManager` | 0x1029C560 | `ToDSaveManager` (/ToD/Save), 0xB03F8 bytes, right after `g_saveData` |
| `g_saveSystem` | 0x1074D400 | `ToDSaveManager*`, set to `&g_saveManager` in `StageCore_Main` |
| `vtbl_SaveManager` | 0x10071C60 | the manager's vtable (base `vtbl_SaveSystem` 0x100FC748) |
| the file image | 0x102BC268 | `g_saveManager.fileImage` (+0x1FD08), a `ToDSaveData`: the last GAME.SAV written or read |

The manager's fields (Ghidra struct `/ToD/Save/ToDSaveManager`):

| Offset | Field | Notes |
|---|---|---|
| +0x08 | `saveThread` | `ToDSaveThread` {mgr, pad, u64 threadId, s32 state, s32 slot}: list save, auto save |
| +0x20 | `loadThread` | the same: list load, auto load, the boot check |
| +0x38 | `deleteThread` | `ToDDeleteThread` {mgr, pad, u64 threadId} |
| +0x4C | `memContainer` | the container handed to `cellSaveDataList*` |
| +0x60 | `secureFileId[16]` | GAME.SAV is a protected file |
| +0x70 | `fileName` | "GAME.SAV" |
| +0x7D | `dirPrefix` | "BCES00052_SAVE_" (`%s_SAVE_` of `g_saveTitleId`) |
| +0x9D | `dirName` | prefix + slot number |
| +0xC0..0xDC | buffers | dir list (+0x108, 0x800), icon (+0x908, 0x1F400), file (+0x1FD08, 0x906F0) |
| +0xE0 | `state` | 0 not up, 1 idle, 2 saving, 3 loading, 4 deleting |
| +0xEC | `slot` | the slot last saved or loaded, -1 none |
| +0xF0 | `lastResult` | 1 saved, 3 list-loaded, 4 auto-loaded |
| +0xF8 | `lastOp` | the last `SaveSystem_request` op |
| +0x104 | `autoSavePending` | an autosave asked for while the system was busy |

### 2.2 The save-data utility calls

All in the Lua TOC range (r2 0x8CB054), each on a PPU thread of its own (priority 0x3E9, 16 KB stack):

| Thread | Function | Call |
|---|---|---|
| "Save Thread" | `SaveThread_listSaveMain` 0x6BC148 | `cellSaveDataListSave2` (the system's save list) |
| "Auto Save Thread" | `SaveThread_autoSaveMain` 0x6BC264 | `cellSaveDataListAutoSave` into `dirName`; checks `/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN` first |
| list load | `LoadThread_listLoadMain` 0x6EC95C | `cellSaveDataListLoad2` |
| "Auto Load Thread" | `LoadThread_autoLoadMain` 0x6ECA94 | `cellSaveDataListAutoLoad` of `dirName` |
| "Check Thread" | `LoadThread_checkMain` 0x6EC610 | `cellSaveDataListAutoLoad` with a fixed callback that only counts the saves |
| "Delete Thread" | `DeleteThread_main` 0x6EC2D0 | `cellSaveDataDelete2` |

The load threads' callbacks: `LoadThread_statCallback` 0x6EC6EC finds GAME.SAV and insists its size is 0x906F0, and takes the slot from the directory name (`SaveSystem_slotFromDirName` 0x6BC9EC); `LoadThread_fileCallback` 0x6EC510 reads the file into the file image. A thread that read its file sets its state to 5 before it exits.

### 2.3 The state machine

`SaveSystem_request(mgr, op)` 0x6BB740 starts an operation, from whichever thread asks (the menu's own PPU thread for the save and load screens). It does nothing unless `state` is 1, except that an autosave asked for then sets `autoSavePending`:

| op | What | Thread | state |
|---|---|---|---|
| 1 | save screen | `SaveManager_copyForSave`, then the save thread | 2 |
| 2 | load screen | list load | 3 |
| 3 | delete screen | delete | 4 |
| 4 | autosave (needs `slot` != -1) | `copyForSave`, then auto save | 2 |
| 5 | auto load of `slot` | auto load | 3 |
| 6 | the boot check | check | 3 |

The menu's Lua has `request_save_interface` (op 1, 0x31810), `request_load_interface` (op 2, 0x3189C) and `save_load_is_interface_displayed`; the gameplay Lua has `request_auto_save` (op 4). Lua can reach none of the file handling itself.

`SaveSystem_update` 0x6BB94C runs on the main thread at the end of every `GameState_Dispatch` (0x4C5FA8, every game state, paused or not). In state 3 it calls `LoadThread_poll` 0x6EC328:

```
if (loadThread.threadId != 0xFFFFFFFF)
    if (sys_ppu_thread_get_priority(threadId) == ESRCH)   /* the thread is gone */
        if (loadThread.state == 5) {                      /* and it read its file */
            mgr->vtable->onLoaded(mgr);                   /* SaveManager_onLoaded */
            threadId = 0xFFFFFFFF; loadThread.state = 0;
        } else threadId = 0xFFFFFFFF;
```

and once the id is none again the state goes back to 1. `SaveManager_onLoaded` 0x3889F8 copies the file image into `g_saveData`, sets three volumes from it, and calls `LoadLevel(saved level, 1, 0)`. During gameplay `LoadLevel` only writes `g_planetLoadRequest`, and `LoadLevel_ProcessPending` fades out and loads; in the front end it loads directly (`FrontEnd_requestExit`).

`SaveManager_copyForSave` 0x388730 copies `g_saveData` into the file image (after a stats update) on the requesting thread, before the save thread starts, so the file image is always a consistent copy.

### 2.4 Slots and file names

The PS3 folder is `/dev_hdd0/home/<user>/savedata/BCES00052_SAVE_<n>/` with GAME.SAV (0x906F0), ICON0.PNG, PIC1.PNG, PARAM.SFO and PARAM.PFD. `StageCore_setSaveTitleId` 0x3BF060 writes "BCES00052", the EU disc's id, into `g_saveTitleId` at boot, so the PSN NPEA00452 and the disc share their saves. `<n>` is the slot as a decimal number, which is what `slot` holds. The user's RPCS3 folder has 20 of them (`BCES00052_SAVE_1` to `_19`), all 591,600 bytes; all but one are unencrypted and carry weapon ids 0, 1, 2, 3 at the head, and the odd one is a console's encrypted copy.

## 3. Can a load be triggered programmatically? Yes, with data

The poll above is the whole trigger. With the manager idle, four writes make its next update load the file image:

1. `loadThread.state` (0x1029C590) = 5, the file was read;
2. `loadThread.threadId` (0x1029C588, u64) = 0x00000000_FFFFFFFE, an id no thread has, which `sys_ppu_thread_get_priority` answers with ESRCH;
3. `lastResult` = 0 and `autoSavePending` = 0;
4. `state` (0x1029C640) = 3, last.

The update then runs `SaveManager_onLoaded` on the game's main thread at the end of the frame, which is exactly the code path of a load from the game's pause menu: the image goes into `g_saveData`, the level loads through the fade. Nothing else in the game reads the load thread's words.

The file image itself is free to write while the manager is idle: only `copyForSave` (which overwrites it), `onLoaded` and the save thread's file callback touch it. So a RESTORE copies the file into it and then raises the load, and a set-aside needs no copy at all, because it already holds the last save.

Alternatives considered and dropped:

- **A helper in the game** like sfhelper, calling `onLoaded` from a hook: works, but needs a code cave, a ui.pak entry and an RPCS3 patch, for nothing the data path cannot do.
- **Lua**: `request_load_interface` only opens the system's load screen, and the gameplay VM has nothing that loads a buffer. Not usable.
- **Writing `g_saveData` and `g_planetLoadRequest` directly**: a copy across several ticks of a game that is running on that data, and the game's own autosave could catch it half way. The file image plus the game's own `onLoaded` avoids both.

## 4. The design

### 4.1 qwark

- `src/games/tod_save.c` is the whole game side: the `sf_desc` (aside buffer 0x102BC268, 0x906F0 bytes, no caves, no hooks, no request bytes), `tod_savefile_ready`, and a `struct sf_driver` (`src/core/savefile.h`) of seven entries.
- `struct game_api` gains `savefile_driver` (in `QWARK_HAS_TOD` builds only), and `src/core/savefile.c` drives a game that has one instead of request bytes:
  - **hold**: before a STORE or RESTORE copies, the manager's `state` is set to 0x5157484C, a value of qwark's own. `SaveSystem_request` refuses anything but state 1 (an autosave is noted in `autoSavePending`), and `SaveSystem_update` runs nothing for a state past 4, so no autosave rewrites the image under the copy. The copy checks every tick that the hold is still in place, and stops with error 6 if a request of the game's slipped in. It is given back when the copy ends, whichever way.
  - **set-aside**: nothing to do; the request settles in the usual 30 ticks.
  - **load**: the four writes of section 3 from the hold. The load is pending until the load thread's id is no longer qwark's, then settles. If the game has not taken it after 600 ticks (five seconds), the thread is put back and the request ends with error 6.
  - **aside_ok**: a load is refused with error 7 unless the image is a ToD save: weapon slot i's id is i for i < 4, the level is below 19 and challenge mode at most 1000.
  - SAVEFILE_INFO says installed on every platform and running while the save system is up; SAVEFILE_PATCH answers UNSUPPORTED (nothing to patch). The RPCS3 presence look is skipped.
  - `aside_file`: SAVE_ASIDE and LOAD_ASIDE are a STORE into and a RESTORE of `set-aside.sav` beside the categories. A Load set-aside with no file is BUSY with error 7.
- **Readiness** (`tod_savefile_ready`): the save system idle (state 1, or qwark's hold; no thread on any of the three; no autosave waiting outside the hold). A set-aside or STORE needs a save: `slot` != -1 (the game has saved into a slot or loaded one this boot) or a save qwark itself restored into the image. A load needs gameplay (`g_gameState` 4) with no planet load queued and the front end not on its way out.
- **Recovery**: on the way into a session `tod_save_recover` gives back a hold, or puts back a load thread, that a qwark which went away (a crash, a module update) left in the game; otherwise the game would never save again.
- The two ACTIONs are features 40 and 41 in a fourth group, "Savefile"; `load_setaside` (the combo) is Load set-aside.

### 4.2 Console and RPCS3

The same code on both. On a console the writes go through qwark's process memory access; under RPCS3 through PINE. A RESTORE on a console is about five ticks of copying; under RPCS3 it is as fast as PINE moves 591 KB. No install, no ui.pak entry, no RPCS3 patch, so the client's Connection panel shows the savefile helper as active under RPCS3 (INFO `installed` 1) and offers no patch for it.

### 4.3 Why the set-aside is a file

The other games' set-aside lives in an aside buffer of qwark's in game memory, which the game's own saves never touch. ToD's aside buffer is the game's own file image, which its next autosave (at every checkpoint) rewrites; there is no spare 578 KB of game memory to keep a copy in, and qwark cannot hold one itself. A file costs one 591 KB write per set-aside and one read per load, and keeps the set-aside across autosaves and reboots, which is what Load set-aside is for.

### 4.4 The client

Nothing ToD-specific is needed: the Save files panel keys on the two flagged ACTIONs and SAVEFILE_INFO, and both are there. The client side of this round adds tests against the fake server and the user guide's ToD line.

## 5. Risks

- **The ESRCH id.** The trigger depends on `sys_ppu_thread_get_priority(0xFFFFFFFE)` answering ESRCH (0x80010005) on lv2 and in RPCS3. RPCS3's implementation answers ESRCH for an id it has no thread for; lv2 is expected to. If it does not, the load never happens: qwark puts the thread back after five seconds and reports error 6, and nothing is lost. Live check 1.
- **Loading in gameplay.** The load applies at the end of a frame and the level loads through the fade, so the frames of the fade run on the new save data. That is the game's own pause-menu load, but it has not been watched on hardware. Loads are refused outside gameplay (`g_gameState` 4), in the front end too unless it reports state 4; the front end's own load path is the direct one.
- **The user's slot.** After a load the game's next autosave writes the loaded save into the slot the user was playing in (`slot`), as in the other games. qwark drops an autosave asked for during its own copy, so the loaded save is not written over the slot the moment it is in. A boot with no slot (a new game never saved) never autosaves.
- **A hold left behind.** If qwark goes away in the middle of a copy, the game cannot save until the next session gives the hold back. The window is a few ticks on a console and longer under RPCS3. A quit to the XMB in that window is harmless: the game does not wait for the save system on its way out.
- **A request racing the hold.** The game's request can read state 1 just before qwark's write. The hold's own look and the per-tick check catch every case but one in which the game's thread starts after both; that thread's work and qwark's copy would then meet in the file image. The window is a few instructions of the game's.
- **The volumes.** `onLoaded` sets three audio volumes from the save, as any load does.
- **No premades yet.** A premade needs a real save file; the user's RPCS3 saves are theirs. Any of them dropped into `premade-savefiles/NPEA00452/` works as it is.

## 6. Live checks

1. RPCS3, gameplay: RESTORE a `.sav` made from an RPCS3 GAME.SAV. Expect the fade and the save's planet, `g_saveManager.state` back to 1 and `loadThread.threadId` 0xFFFFFFFF.
2. The same on a console.
3. STORE right after an autosave and compare the file with the slot's GAME.SAV in RPCS3's folder: identical.
4. Set aside, play to the next checkpoint (an autosave), Load set-aside: the set-aside save, not the checkpoint.
5. Open the game's own save screen in the pause menu while a STORE is copying (under RPCS3, where the copy is slower): the game's screen does not open or qwark stops with error 6; either way, both saves still work afterwards.
6. Kill qwark-rpcs3 during a RESTORE's copy, start it again: "tod: the save system was still held; given back" in its log, and the game autosaves again.
7. A load from the title screen: refused with error 6, or (if the front end is state 4) the front end's direct load.

## 7. Ghidra

Program DL-ghidra:/NPEA00452.ELF, saved.

- Structs `/ToD/Save/ToDSaveManager` (0xB03F8), `ToDSaveThread` (0x18), `ToDDeleteThread` (0x10); `ToDSaveManager` applied at 0x1029C560, `ToDSaveManager *` at 0x1074D400.
- Globals: `g_saveManager`, `g_saveSystem`, `vtbl_SaveManager`, `vtbl_SaveSystem`, `g_saveTitleId` (0x10387050), `g_inFrontEnd` (0x1037A459), `g_frontEndExitRequested` (0x1037A458), `s_SaveDirFormat` (0x1002A150).
- Functions: `SaveManager_setupBuffers/copyForSave/onLoaded/onOperationDone/getFileLevelInfo/dtor/deletingDtor`, `SaveData_init`, `SaveData_initHeroBlock`, `SaveData_staticInitDestroy`, `SaveSystem_ctor/request/update/updateGlobal/waitIdle/fillListSettings/slotFromDirName/requestSaveUI/requestLoadUI/requestDelete`, the cross-TOC stubs `SaveSystem_request_xtoc`, `SaveSystem_request_xtoc0`, `SaveSystem_updateGlobal_xtoc`, `SaveThread_startListSave/startAutoSave/poll/isRunning/listSaveMain/autoSaveMain`, `LoadThread_startListLoad/startAutoLoad/startCheck/poll/isRunning/listLoadMain/autoLoadMain/checkMain/listCallback/fileCallback/checkFixedCallback/statCallback/autoFixedCallback`, `DeleteThread_start/main/poll/isRunning`, `MenuLua_request_save_interface/request_load_interface/save_load_is_interface_displayed`, `FrontEnd_isActive/requestExit/init/shutdown`, `StageCore_setSaveTitleId/hddGameThread/sysCacheMountThread`. Prototypes set on the main ones. 0x388730 and 0x3889F8 were first named `SaveManager_prepareSave` and `SaveManager_applyLoad` here; another agent renamed them `SaveManager_copyForSave` and `SaveManager_onLoaded` the same day, and those names are kept.
