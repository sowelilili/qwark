# Tools of Destruction's console menu: the game's own pause menu (revision 1.29, pending)

The trainer's console menu in Tools of Destruction (NPEA00452 v01.00, `EBOOT.ELF` md5 `df9d68ada99b3f44ffe798ce03a1549c`, game 5, `tod.sprx`), drawn by the game's own pause menu as the four original games' menus are: a **TRAINER** row in place of the pause menu's Help row, and qwark's pages on the game's own Cheats screen. This file has the reverse engineering, the design, and the test lists for a console and RPCS3.

Read from the executable, the pause menu's data (`built/anark/pause/built.dat` in `global_cached.psarc`, its 28 compiled Lua 5.0.2 chunks decompiled) and Ghidra (DL-ghidra:/NPEA00452.ELF). **Nothing here has run on a console or on RPCS3 yet** (section 7). Confidence marks as in the other ToD reports: **high** read end to end; **medium** read, the effect in game inferred; **low** a lead.

It replaces the prototype of 2026-10-31 (commit 55ef029: a panel of its own drawn with `Text_Draw`/`Sprite_Draw` from a frame hook at 0x3C5CD8 and a pad hook at 0x5D144C, ui.pak kind 3, `menu.tod`). The prototype is gone: no renderer of ToD's is in ui.pak any more, `menu.tod` means nothing, and the two hooks are never written. Its findings about the HUD's drawing calls are in that commit's version of this file.

## 1. The pause menu (high)

- **The state.** Start in play reaches `Game_UpdateFrame`'s last test, `thunk_InputMapper_isPressed(g_gameInputMapper, 1)` at **0x4C5ABC**, once every earlier check has passed (no menu up, no load request, `thunk_Game_isPauseBlocked(0)` clear, `g_planetLoadRequest` -1, the level more than 120 frames old). It then calls `thunk_PauseMenu_request` (0x23BB4): `g_pauseMenuRequest` (0x101CB228, `/ToD/Anark/PauseMenuRequest`) filled, and an overlay queued on the state stack (`GameState_queueOverlay`, op 2, state 5).
- `PauseMenu_enter` (0x248B4) sets `g_pauseMenuState` (0x101CB218) to 1 and starts `PauseMenuLoadingThread` (0x24A60): it builds an `AnarkMenuSystem` (0x14B0 bytes, `/ToD/Anark/AnarkMenuSystem`, `AnarkMenuSystem_ctor` 0x654768) out of the pause's arena into **`g_pauseMenuSystem` (0x101CB220)**, loads the presentation, opens the menu package's bindings (`tolua_menu_open`) on its VM, sets +0x10AB (the next update starts the presentation) and leaves. `g_pauseMenuState` 2 and 3 are "up", 4 closing (`PauseMenu_close` 0x23C9C, which `exit_pause_menu` reaches), 0 none (`PauseMenu_exit` 0x24610).
- **The frame.** `PauseMenu_update` (0x2406C, `g_gameState` 5's update, main thread) calls `thunk_AnarkMenuSystem_update` at **0x242B0** (`bl 0x119D0`, then `ld r2,40(r1)`) with r3 = `g_pauseMenuSystem`, only in states 2 and 3. `AnarkMenuSystem_update` (0x653D30) makes the Anark VMs' allocator current (`AnarkMenu_luaAllocator` 0x653048 returns `g_anarkLuaAllocator`, 0x1073DC40), reads the pads (`Input_UpdatePads`), runs the presentation and its Lua, and forces a full GC every second.
- **The menu VM.** The system's **lua_State is at +0x106C** (`AnarkMenuSystem_getLuaState` 0x653BBC; set by `AnarkMenuSystem_startPresentation` 0x654DD4 from the Lua object at +0x1058). It is a new state for every pause (the arena is reset, so the address can repeat). Lua 5.0.2, the base, table, string and math libraries, the menu package (179 functions: `exit_pause_menu`, `get_num_cheats`, `set_cheat_state` ...), the Anark framework (`scene`, `Presentation`, `Node`, `Event`, `Color`, `LOC`), and `SetLocalizedText` (Lua source embedded at 0x100FA7A0: a number is a text tag, a string is shown as it is).
- **The config VM** (`g_configLuaVM` 0x101E7E50) is the tuning sheets' and is not the menu's; the galactic map, vendors and front end are other `AnarkMenuSystem`s with other callers of the update (0x360BC, 0x39A38, the loading thread), which the menu does not touch.

## 2. The pause menu's structure (high, from the decompiled chunks)

The presentation is a scene of slides; each screen's behaviour is a Lua chunk run with `self` the behaviour node, its methods assigned onto it (`self.handleSelect = function ...`), and the nodes it uses found by path properties (`this.parent.ScrollableList`).

- **`ListUpdater`** (the pause's lists, one behaviour): `SetupMenus` builds `self.menuData[menu][i] = { text tag, callback }`. MAIN_MENU_INDEX 1 is Mission, Quick select, Weapons, Inventory, Gadgets, Items, Options, Extras (space combat and arena levels have their own first rows). **OPTIONS_MENU_INDEX 2: Game options (45 is Help's tag...) — 1 Game options, 2 Controls, 3 Moves, 4 Help (`{45, self.onHelp}`), 5 Save, 6 Load.** EXTRAS 3: Skill points, Cheats, Concept art, Cinematics, Skins. `handleSelect` calls the row's callback; `ActivateItem` shows a row's label with `SetLocalizedText(item, menuData[i][1])`. `onHelp` is `scene:goToSlide(12)`, `onCheats` `scene:goToSlide(16)`; every screen's back is `scene:goToSlide(21)`, the list (`blankHandler`), whose activation puts the list back on its own slide (`currentSlide`: 3 main, 7 options, 9 extras).
- **`cheatHandler`** (slide 16, the Cheats screen): a `ScrollableList` (`listBehavior`, six visible rows `cheat1`..`cheat6`, each a `label` and a `value` text; it scrolls any count) driven by the handler's `getNumListItems`, `setListItemValue(i, item)`, `isSelectable(i)`, `onSelectedItemChanged`, `handleSelect`/`handleRight` (`changeCheatState(1)`), `handleLeft` (`changeCheatState(-1)`) and `handleBack` (slide 21); the list calls them through `self.listUpdater:...`, by name at every call. The screen's title is the text node `Page_name2` ("CHEATS"), its description `Description` (`itemDesc`), and `skillpoint_label`/`skillpoint_value` the counter. Its own values come from globals (`get_num_cheats`, `is_cheat_unlocked`, `get_cheat_name`, `get_cheat_state_name`, `get_cheat_desc`, `set_cheat_state` ...).
- The options screens (`optHandler`, `controlHandler`) are label/value lists too; the Cheats screen is the one whose rows are plain label and value text set from Lua, which is what qwark's rows are.

## 3. The design

```
qwark (C, src/core/menu.c)          menubox (0x8CD000)          the Lua helper (C, in the game)       the glue (Lua, pause VM)
 the pages, rows, values,  --post--> page, seqlock    --take-->  lh_menu.c: copy, rows,      <--qw()-- TRAINER row, Cheats screen,
 what a row does           <--ack--- events           <--post--  events, hstate, requests    --qw()--> presses, back, open/close
```

- **qwark's side is unchanged.** The menu's model, pages and rows (`menu.c`, ToD's `game_api.menu` in `tod_game.c`) and the mailbox (`menubox.h`, version 3) are every game's. ToD's mailbox is at 0x8CD000 (0x880 bytes: 32 rows of 0x40, either page type, caps STEPS | BACK | REQUEST, renderer 5). What is new for menu.c is only that the "renderer" is the game's own (`game_api.menu_native`, `struct menu_native`): no ui.pak entry, no cave and no hook of the menu's; its install is the game's Lua helper (`lua_core_helper_ready`) and a mailbox header menu.c writes itself; `menu.help` is a byte (0x8CE000); `menu.enabled` 0 clears the mailbox's identity.
- **The Lua helper's pause-menu half** (`src/games/luahelper/`, revision 1.29), C in the same cave as the gameplay half, two more hooks:
  - **0x242B0** (`bl 0x119D0` -> `bl .lh_menu_hook`): calls the update as the game did, then `lh_menu_tick(ms)` on the pause menu's lua_State with the Anark allocator current: for a new VM it registers the C function `qw` and asks qwark for the glue (`LM_GLUE` WANTED, `LM_LOADS` + 1); it serves requests for the menu's VM (the Lua mailbox's target MENU, below); and it calls the glue's `qwark_frame` every frame under `lua_pcall` (an error goes to the Lua mailbox's err area as "qwark-menu" and stops the glue for that pause; LUA_STATUS shows it).
  - **0x4C5ABC** (`bl 0x2672E0` -> `bl .lh_pause_hook`): the pause test itself, then `lm_pause_check`: qwark's open request in play answers "pressed", so the game opens its pause exactly as for Start, past every check of its own, and the glue opens the menu in it.
  - `lh_menu.c` is the bridge's C, the same jobs as the four renderers' `mh_core.c`: the page copied out of the mailbox under its seqlock into a block of its own (0x8CE040), so the glue reads rows only from there; a press posted as an event with the shown page's key and the row's id, one in flight at a time; hstate and the frame count; the requests. A gameplay frame (`lm_gameplay_frame`) closes the menu (any way out of the pause) and makes the next pause a new VM. Host-tested on the PC (`test/test_tod_menu.c`).
- **The glue** (`src/games/luahelper/lh_glue.lua`, about 7.6 KB once its comments are out): **qwark sends it** to each new pause menu VM as a request of target MENU (`lua_core_menu_glue`, from menu.c's serve, once per `LM_LOADS`), because in the cave it would make the helper's RPCS3 patch (LUA_PATCH) bigger than a reply. It finds the nodes by name (`cheatHandler`, the `ListUpdater` with `menuData`, `Page_name2`, `skillpoint_*`), and wraps, never replaces:
  - Options' row 4 becomes `{ "TRAINER", open }` while the Help byte is 1, the game's `{45, onHelp}` otherwise (checked to be Help before anything is touched);
  - `open()`: hstate open, then the Cheats screen (slide 16), as `onCheats` goes there;
  - while the menu is up, the handler's methods and the cheat globals answer from qwark's page (`qw(1, i)`): the label at the left, the value at the right (">" for a link), INFO rows not selectable, the title in `Page_name2`, qwark's last answer in the description; the cross posts ACTIVATE, Left/Right post LEFT/RIGHT on rows that step, Triangle posts BACK, or at qwark's root leaves to the list (slide 21); `changeCheatState`, `set_cheat_state` and `Node.goToSlide(21)` are caught too, for presses the game might route around the wrapped methods;
  - every frame `qwark_frame` polls (`qw(0)`): a new key restarts the list with the cursor where qwark keeps it, new values refresh the visible rows, a request is taken (open; close leaves the menu and `exit_pause_menu()`), and with qwark gone (identity cleared) the menu leaves and Help is the game's again;
  - the cursor at the window's edge of a page longer than 32 rows posts SCROLL_UP/DOWN, as the renderers do.
- **The combo** (edge-triggered by qwark's combo arming, as for every game: one request per press, a held combo fires nothing more): menu closed -> REQ_OPEN; in play the pause test takes it (the pause opens, the glue opens the menu on its first frames once the pause has settled, 15 frames), in the pause the glue takes it; menu up -> REQ_CLOSE, which leaves the pause. A request nobody takes in 0.5 s is withdrawn by qwark as for every game; one the pause test took waits up to 30 gameplay frames for the pause to come up.
- **The Lua mailbox, version 2** (`luamb.h`): `target` (byte 0x26, qwark's: 0 gameplay VM, 1 the pause menu's) and `menu_beat` (0x3C, +1 every pause menu frame). The gameplay frame serves only target 0, the pause menu's frame only target 1; a MENU request's timeout reads `menu_beat`. LUA_EXEC's `flags` bit0 picks the menu's VM (NOT_FOUND with no pause up). The gameplay half is otherwise as it was.
- **C first.** Every value, label and decision is qwark's, in C; nothing polled goes through Lua (the glue's qw() calls read the helper's copy in the game's memory); the only Lua is what the game's menu system needs to show rows and hand back presses.

## 4. What is native and what is not

- Native: the pause menu, its Options list and its row, the Cheats screen with its list widget, scrolling, highlight, description box, title, its sounds where the game plays them (the glue adds the list's own success and back cues), the game's input, the game's own pause (no frozen clock, no pad blanking), Start/back behaving as the pause does.
- Not native: the rows' text is qwark's English labels, not localised; MAIN and CONFIG pages are both shown on the Cheats screen's two-column list (a link's value column says ">"); there is no slider (no ToD row has a range).

## 5. Memory

| What | Where | Size |
|---|---|---|
| the Lua helper's code (both halves) | 0x867360.. | 4,680 bytes (cave to 0x870000) |
| the Lua mailbox | 0x8CA000..0x8CC600 | 0x2600 |
| the menu's mailbox | 0x8CD000..0x8CD880 | 0x880 |
| the helper's menu block (help byte, glue word, the page's copy) | 0x8CE000..0x8CE890 | 0x890 |

All in the code and OPD segments' last 64 KB pages, unreferenced by the executable (`check.py`). The four hooks go in reverse order: the pause test, the pause menu's update, the error tap, the VM hook last. LUA_PATCH is now about 1,180 words.

## 6. RPCS3

Supported, which none of the four originals' menus is: the menu needs no code of its own beyond the Lua helper, which RPCS3 takes as the LUA_PATCH patch at boot; the menu's mailbox, the help byte and the glue are data writes and a request over PINE. `qwark-rpcs3` installs the menu once it has found the patch (LUA_STATUS installed 1); without the patch the combo says "needs qwark's RPCS3 patch (Lua helper)". MENU_HELP answers OK in `qwark-rpcs3` now. The open question is the Lua helper's own live check 3 (docs/tod-poc.md 5): whether RPCS3 runs code in the segment padding at 0x867360.

## 7. Test lists

**Console** (a test build: `tod.sprx`, no ui.pak needed for ToD; `menu.enabled` and `menu.help` at their defaults):

1. qwark.log a few seconds into a level: "lua: helper installed", then "menu: the game's own menu served for game 5, mailbox 0x008CD000". Play a minute (walking, a death, a cutscene, the pause): nothing differs.
2. Start -> Options: the fourth row reads **TRAINER**. qwark.log has no "glue failed"/"glue stopped" line; LUA_STATUS shows no "qwark-menu" error. (If the row still says HELP, check LUA_STATUS's err.)
3. TRAINER: the Cheats screen shows "Trainer" in its header and qwark's root (Game, Cheats, Manips, Planets, Unlocks, Save files ...), ">" beside each; the skill points counter empty. Up/Down move and scroll; the highlight follows; Triangle goes back to the Options list.
4. Cheats > Infinite ammo: the cross (or Right) turns it On (the client's checkbox follows); Left back Off. Max bolts: the cross. Back with Triangle to the root, Triangle again to Options.
5. Game > Save position, leave the pause, move, pause, Trainer > Game > Load position.
6. Planets > a planet: the pause closes and the planet loads.
7. The combo in play: the pause opens and the Trainer opens on its own about half a second later. The combo again: the pause closes. Hold the combo for a second: it opens once and stays open.
8. The combo in the pause (on its first list, then from inside Weapons): the Trainer opens.
9. Start inside the Trainer: the pause closes as from any screen; the next combo opens it again (qwark saw the close).
10. The game's own Cheats screen (Extras > Cheats) after using the Trainer: the game's cheats, title and counter as before.
11. The client's In-game menus switch off (MENU_HELP 0): Options says HELP again at the next pause, and Help works; the combo still opens the Trainer.
12. `menu.enabled = 0`: no TRAINER, the combo says the menu is off.
13. A space combat level and an arena challenge: Options > TRAINER there too.

**RPCS3** (with the client's new Lua helper patch written and the game restarted): the same list. Before it, live check 3 of docs/tod-poc.md (the heartbeat at 0x8CA028 counting in play), and the menu heartbeat at 0x8CA03C counting while paused.

## 8. Ghidra (DL-ghidra:/NPEA00452.ELF)

Functions: PauseMenu_enter 0x248B4, PauseMenu_exit 0x24610, PauseMenuLoadingThread 0x24A60, PauseMenu_request 0x23BB4 (thunk 0x26AAB0), PauseMenu_close 0x23C9C, PauseMenu_exitFromLua 0x24F0C, PauseMenu_callScreen 0x24F64, MenuLua_exit_pause_menu 0x287A8, MenuLua_call_pause_menu 0x28834, thunk_AnarkMenuSystem_update 0x119D0, AnarkMenu_luaAllocator 0x653048, AnarkMenuSystem_getLuaState 0x653BBC (thunk 0x140C0), AnarkMenuSystem_ctor 0x654768 (thunk 0x11A30), AnarkMenuSystem_startPresentation 0x654DD4, InputMapper_isPressed 0x68B654 (thunk 0x2672E0), InputSystem_registerMapper 0x68B744, InputSystem_unregisterMapper 0x68B5A4, GameplayState_enter 0x4C4224, GameplayState_exit 0x4C4270, Game_isPauseBlocked 0x1F7598 (thunk 0x26AAF0), tolua_Node_getChildByIndex 0x7775F4, tolua_Node_getNumChildren 0x7774C4, CLuaObject_getChildByIndex 0x76B688 (1-based), CLuaObject_getNumChildren 0x76B604. Globals: g_pauseMenuSystem 0x101CB220, g_pauseMenuState 0x101CB218, g_pauseMenuRequest 0x101CB228, g_gameInputMapper 0x10387D70, g_anarkLuaAllocator 0x1073DC40. Structs: /ToD/Anark/AnarkMenuSystem, /ToD/Anark/PauseMenuRequest. Prototypes: AnarkMenuSystem_update, InputMapper_isPressed.
