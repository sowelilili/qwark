# Tools of Destruction's console menu (2026-10-31, a prototype)

The trainer's console menu in Tools of Destruction (NPEA00452 v01.00, `EBOOT.ELF` md5 `df9d68ada99b3f44ffe798ce03a1549c`, game 5, `tod.sprx`): the reverse engineering behind it, and what the prototype in `src/games/menuhelper/mh_tod.*` does.

Everything here was read from the executable and the game's data alone, in Ghidra (DL-ghidra:/NPEA00452.ELF) and with the SDK's objdump. **Nothing has run on a console or on RPCS3 yet.** The prototype is off unless config.txt has `menu.tod=1`, so a normal ToD session is exactly what it was.

Confidence marks, as in the other ToD reports: **high** the code path was read end to end; **medium** read, the effect in game inferred; **low** a lead.

## 1. How the four originals' menus differ from what ToD allows

The four renderers draw the menu with each game's own menu widgets, inside the game's own pause, and turn its Help row into the Trainer row. ToD's pause menu is an Anark presentation (`built/anark/pause/built.dat`, 28 compiled Lua chunks, `onHelp`, `helpHandler`, text tags in `anark_loc_text_tags.*.pkg`) run by the menu's own Lua VM: there is no list of rows in the executable that a word could turn. So ToD's renderer draws its own panel over the frame with the game's HUD primitives, as the draft's "option A" did, and opens with the combo.

## 2. The findings

### 2.1 The frame, and a hook that runs in play and in the pause (high)

- `GameMain_Loop` 0x4C2DE4 runs `GameState_Update` 0x630CE4 for ever. It updates the **top tier** of the state's tier stack only (`GameTierStack_updateTop` 0x640CDC, `GameTier_update` 0x640A14 through the tier's vtable +0x20).
- In a level the top tier is `g_levelTierState` (0x1037A620, `vtbl_LevelTierState` 0x10073470), whose update is `Level_Update` 0x3C0540, which calls `LevelTransition_runFrame` 0x3C5C88 every frame. The load loops (`LevelTransition_waitLoad`) call it too, with a second argument of 1.
- `LevelTransition_runFrame` is one whole frame: `0x2681F0` (into 0x6C2058), `GameState_Dispatch` 0x4C5DF0 (the frame's update and its draws, by `g_gameState`), `0x4C290C` (empty), `0x555510` (moby deletions), then, only on a load frame, **`LoadScreen_DrawArrival` 0x25DCA8**, the load screen's "arriving at" text, and then **`bl 0x26AA40`**, `thunk_Frame_limitAndFlip`, the cross-range trampoline to `Frame_limitAndFlip` 0x6B9900 (the frame limiter, then `Gcm_endFrameFlip` 0x539D68, which ends the command buffer and calls `cellGcmSetFlip`).
- `g_gameState` (0x10387F4C), as `GameState_Dispatch`'s jump table at 0x4C5F3C reads it: 4 play (`Game_UpdateFrame` 0x4C542C), **5 the pause menu (`PauseMenu_update` 0x2406C**, through the trampoline 0x268A50), 7 an FMV (0x3170DC), 9 a realtime cinematic (0x84EFC), 3 a level load, 11 the credits. Ghidra's enum `ToDGameStateId` gained `TOD_GAMESTATE_PAUSE_MENU = 5`.
- **The hook: 0x3C5CD8, `bl 0x26AA40` (word 0x4BEA4D69).** It runs every frame of a level, in play, paused, in a cinematic and on the load screens, after the frame has been drawn and before the flip, the very place the game itself draws the arrival text with the same `Text_Draw`. The stub runs the menu and then branches to the trampoline with the game's r2 and LR, exactly as the replaced `bl` would. The front end (the title menus) is another tier and does not reach it, which is right: the menu is for a level.
- `Game_UpdateSystems` is not usable for this: `Game_UpdateFrame` sets flag 0x10 and skips it in the pause (REPORT-tod-lua 2.3).

### 2.2 The 2D primitives (high for the calls; medium for their look, which no one has seen)

**The debug-text lead is a dead end.** The menu package's `screen_out` (0xB334C) ends in `bl 0x885BC` at 0xB33E8 (the DB-only patch there, `bl puts`, is someone's experiment, left as it is). 0x885BC is the third word of a tolua field setter (0x885B4, a float to int at +0x28), not a function: the debug print it once called was stripped and the linker left the relocation pointing into the next function. It would crash if called. There is no debug text routine in retail.

**Text: `Text_Draw` 0x688C64** (descriptor 0x898140, r2 0x8CB054), the call the load screen's arrival text and the loading pages (`LoadScreen_DrawImageAndText` 0x3BE474) make, and under the HUD's text box widget (`HudTextBox_draw` 0x68A92C) the same layout code:
- `r3` the string (UTF-8 through `Text_NextChar` 0x6BFEC4; ASCII is itself; 0xE0xx are the button glyphs, 0xE2xx and 0xFxxx colour codes);
- `r4` a 16-byte position, x and y first, in the HUD's screen units (below);
- `r6` the colour, RGBA (alpha in the low byte, 0xFF opaque), multiplied by the HUD's alpha stack;
- `r8` the font (0 or 1; the arrival text uses 1);
- the 9th integer argument (the caller's stack at 0x74) the shadow's colour, RGBA, 0 for none;
- `f1` the size, `f2` a wrap width (0 none), `f3`/`f4` the alignment (0 left/top, 0.5 centre, 1 right/bottom), `f5`/`f6` the shadow's offset, `f7` the line spacing (-1 the font's).

It fills a `ToDTextDrawVisitor` (0x90 bytes, `vtbl_TextDrawVisitor` 0x100FC520) and runs `TextVisitor_run` 0x6C0510, which lays the text out; the visitor's `begin` (`TextDrawVisitor_begin` 0x687F28) takes vertex memory from the high push buffer and sets the HUD's vertex and fragment programs and alpha blending, `glyph` (0x687A58) writes four corners a glyph, `flush` (0x6874A0) writes the texture and the draw straight into the GCM command buffer (`g_gcmContext` 0x1066B708). So it draws at once: it has to run where 2D drawing is valid, which the arrival text proves for the hook above. A glyph's screen place is `(glyph + cursor) * size + position`, then `g_hudScaleX/Y` and `g_hudOffsetX/Y` (0x10740BC4/BC0, 0x10740BCC/BC8) take it to the GPU's space.

The fonts: `Font_Load` 0x6BE7F0 reads `built/fonts/<region>/font.dat` (IGHW sections 0x29000..0x29500: two fonts, 378 code points, the glyph table, the texture pages in `font.tp`); `Font_Select` 0x6BCF00 makes one current (`g_fontCurrent`, `g_fontCurrentData`, `g_fontCurrentIndex`); `Font_Height` 0x6BE4DC is its line height in unscaled units (a text's on-screen height is that times the size); `Font_TextWidth` 0x6BF328 a string's width; `Font_ScaleSize` 0x6BD168 the point size the arrival text uses (24 points, font 1); `Font_SetButtonLayout` 0x6BD514 remaps the button glyphs for the cross/circle swap.

**Filled rectangles: `Sprite_Draw` 0x683A44** (descriptor 0x897F38), `(ToDHudRect *rect, ToDHudSprite *sprite)`: a textured quad in the HUD's units, four corner colours, alpha, blend, rotation, an optional shadow. `Sprite_Init` 0x682D30 (0x897EE8) fills a `ToDHudSprite` (0x40 bytes) with the game's defaults: `g_defaultTexture` (0x1066B970, the texture every failed lookup falls back to), UVs 0..1, white corners, alpha 1, blend 0x400 (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). With that texture and four corner colours of one RGBA, a sprite is a filled rectangle. *Inferred*: that the default texture is plain white (it is the fallback of 73 texture slots across the game's code); if it is a checker, the panel shows the checker tinted.

**The HUD's alpha stack:** `Hud_ResetAlpha` 0x682DBC (depth 0, alpha 1.0), `Hud_PushAlpha` 0x682DD8, `Hud_MulAlpha` 0x682E3C, `Hud_PopAlpha` 0x682E0C, `Hud_GetAlpha` 0x682E20, on `g_hudAlphaDepth` 0x10740B30 and `g_hudAlphaStack` 0x10740B34. Every text and sprite flush multiplies its alpha by the top. The game's own full-screen pages start with `Hud_ResetAlpha`, and so does the menu.

**The screen:** `g_hudScreenRect` (0x10740BD0, `ToDHudRect`) is `(0, 0, height * aspect, height)`, which `StageCore_Main` sets through `Hud_SetScreenSize` 0x6894D4 from the display's height and aspect: display pixels, 720 high at 720p. The menu lays itself out in fractions of it, so it is the same on every display mode.

**The Anark UI** draws through the same calls (its text boxes are `HudTextBox_draw`, its images `HudSprite_draw` 0x68A574 on `Sprite_Draw`), so the menu's text and the pause menu's are the same font. Driving an Anark presentation from native code would need its scene graph; not looked at further.

### 2.3 The pad, and keeping it from the game (high)

- `Pad_Update` 0x5D0F50 reads the four pads (`cellPadGetData`) into four `ToDPadState` blocks of 100 bytes at `g_padAnalogs` 0x10691918: the sticks at 0, four sensor floats at 0x10, twelve pressures at 0x20, then the u32 masks pressed 0x50, released 0x54, **held 0x58 (`g_padMask` 0x10691970)** and the previous held 0x5C, a flags byte and the connected byte. The edges are `(previous ^ held) & held` and `& ~held`, the previous being the held mask as the read found it.
- It is called from play (`InputDemo_update` 0x66978C, from `Game_UpdateSystems`) and from the Anark menus (`Input_UpdatePads` 0x6E5380 from `AnarkMenuSystem_update` 0x653D30), so a pad hook inside it covers play and the pause alike, where `InputDemo_update` alone would not.
- **The hook: 0x5D144C, `Pad_Update`'s last call, `bl 0x63182C` (word 0x480603E1, a nop after it, the same TOC range).** The stub notes pad 0's held mask for the menu and, while the menu is up, zeroes every block's floats and edges and its held mask, then calls 0x63182C (an unnamed step over the pads' blocks) and returns. The game sees no stick and no button. At the end of the menu's frame the shim writes pad 0's real held mask back, so the game's next read takes no press for a button that was down all along (no Start leaking into the game when Start closes the menu) and qwark's hot block, which reads `g_padMask`, sees the combos.
- The Anark menu system has a pad block of its own (its input object, filled by `Input_UpdatePads` from these blocks) and a byte at +0x10AA that clears it: not needed, since the menu blanks its source.

### 2.4 Opening the menu

- **The combo (built).** qwark's menu combo puts an open request in the mailbox as for every game. The frame hook takes it on the next frame with `g_gameState` 4 (play) or 5 (the game's pause) and nothing queued on the state stack (`g_gameStatePendingOp` 0x10387DBC is 0); in any other state (a load, a cinematic, an FMV, the credits) it leaves the request, and qwark withdraws it half a second on, as for the other games. Start, Select, Triangle or Circle on the root, or the combo again, close it.
- **A "Trainer" row in the pause menu (not built): feasible, about a week.** The pause is `built/anark/pause/built.dat`, an Anark presentation with its own Lua: `onOptions`, `onGameOptions`, `onControls`, **`onHelp`** and `helpHandler` are compiled Lua functions of the menu VM, and the row's label is a text tag ("HELP") in `anark_loc_text_tags.<lang>.pkg`. Three ways, cheapest first:
  1. **The menu VM's Lua.** After `PauseMenuLoadingThread` brings the pause up ("Pause Menu brought up in %.2f milliseconds"), redefine the global `onHelp` in the pause's `CLuaContext` to a chunk that calls `exit_pause_menu()` and sets a byte the renderer reads, and rewrite the "HELP" tag's string in the loaded localisation table. It needs a native entry into the *menu* VM on its own thread (the Lua helper only knows the gameplay VM, `g_gameplayLuaVM`), the context's address once the pause is up, and a byte the menu VM can write (a binding such as `set_cheat_state` aimed at a spare cheat byte, or a native closure registered by the helper). Medium to hard; every piece is the Lua helper's kind of work.
  2. **The presentation's data**: patch the compiled chunk in memory after the load. The chunk is bytecode in an IGHW container; changing it means a new constant and a new call, and a different layout per language. Hard.
  3. **The text alone**: relabel HELP to TRAINER (the tag table) and open the menu when the help screen opens (`g_gameState` stays 5; the help log's own Lua runs). Cheap, but the help screen would open under the menu. A fallback.
- So the prototype opens by combo only, and has no Help row (`MH_NO_HELP_ROW`, `nhelp` 0).

### 2.5 Pausing (medium)

- The four originals' menus live inside the game's pause. ToD's renderer, opened from play, freezes the game instead: it writes 0.0 to the game clock's time scale (`g_gameClock` 0x1074DDA8 + 0, the word `Lua_time_set_scale` 0x2956F4 writes, which the game's own slow-motion effects write too, 0x4020BC) every frame it is up, and puts back the scale it found as it closes. With the scale at 0 the frame's delta is 0, and `Game_UpdateFrame` then sets its flags 0x1B, which skip the gameplay systems (`Game_UpdateSystems`, the camera's update): the world stops but is still drawn. *Inferred, not seen:* that nothing in play moves with a delta of 0.
- Opened over the game's own pause (state 5) it touches no clock: the game is paused already.
- A level load, a cinematic or the credits taking the game out of states 4 and 5 closes the menu, the clock put back.

### 2.6 Caves (high, from the file; the mapping is the Lua helper's open question)

| What | Where | Size | Used |
|---|---|---|---|
| Lua helper's code | 0x867360..0x867884 | 1,316 bytes | the Lua helper (unchanged) |
| **menu code** | **0x868000..0x86E000** | 24 KB | 8,780 bytes (the core 4,952) |
| Lua helper's mailbox | 0x8CA000..0x8CC600 | 9.5 KB | the Lua helper (unchanged) |
| **menu mailbox** | **0x8CD000..0x8CD800** | 2 KB | 1,024 bytes (14 rows) |
| **menu data** | **0x8CE000..0x8D0000** | 8 KB | 2,876 bytes (the core's state 0x8B0, the shim's 0xC0, read-only data, TOC, descriptors) |
| fast loads' switch and pieces | a dead function at 0x3EB578 | 400 bytes | 308 (unchanged; 52 left, not touched) |

The code cave is the padding of the R-X segment's last 64 KB page (the segment ends at 0x867360, the next starts at 0x870000), the data caves the padding of the OPD/TOC segment's last page (it ends at 0x8C9A08, its TLS image at 0x8C9BF8). Nothing in the executable refers to any of them (check.py). They exist only if lv2 maps whole 64 KB pages, which the Lua helper relies on too and which is its live check 2: on a console where they are not, ps3mapi's write fails and the install is refused, with nothing hooked. More room if ever needed: the code page has 7.8 KB after the menu's area, the data page 2 KB between the two mailboxes and 5.3 KB at the end of the menu's data; the RW BSS page past 0x108A8570 (the fast loads' flag byte at 0x108A8560) is another 31 KB if lv2 maps it whole.

### 2.7 RPCS3

None of the four originals' renderers runs under RPCS3: `menu_enter_id` installs nothing where code cannot be patched, and PROTOCOL.md 9.10 says so ("RPCS3 has no console menu"); the client greys the menu combo there. ToD's prototype is the same. Bringing it to RPCS3 would take, for any game:
- the renderer's code caves and hook words as rows of the RPCS3 patch the client writes (ToD's SWITCH_PATCH, or a new MENU_PATCH op, as LUA_PATCH is for the Lua helper), and an install path in `menu.c` that, under `QWARK_EMULATOR`, writes only the data caves over PINE and checks the hook words are the patch's;
- for ToD in particular, that RPCS3's PPU recompiler runs code in the segment padding at 0x868000 (the Lua helper's open live check 3; the fast loads' switch avoided the question by using a dead function, which has 52 bytes left, far too few for 8.7 KB).

A protocol and client change, so not done here.

## 3. The prototype

Files: `src/games/menuhelper/mh_tod.h` (addresses, words, layout), `mh_tod.c` (the shim), `mh_tod_stubs.S` (the two stubs), `mh_tod_state.S` (the images); `build.sh` (`tod_entry`, `tod_ldflags`, `tod_hooks`, `tod_query`, `MH_NO_HELP_ROW`), `pak.py` (`--tod`, `--tod-id`, kind 3), `check.py` (`check_tod_menu`, the pak with ToD); `src/core/uipak.c` (kind 3 under `QWARK_HAS_TOD`, `MH_TOD_PAK_ID`), `menu.c` (`menu.tod`, `menu_none`), `session.c` (NO_MENU through `menu_none`), `src/games/tod_game.c` (ToD's menu table); the makefile packs ToD's entry in a test build only. `mh_core.c`, `mh_shim.h` and `menubox.h` are unchanged, and the four originals' renderers are byte for byte what they were (`MH_PAK_ID` 0xD06745A8).

**What it does.** With `menu.tod=1` in config.txt, a few seconds into a ToD session qwark writes the mailbox, the state, the read-only data and the code out of ui.pak's ToD entry and then the two words. The combo in play or in the game's pause opens a panel over the frame:
- a darkened box from 20% to 80% of the screen's width, from 15% of its height down;
- the page's title centred in gold, then up to fourteen rows of qwark's page (a MAIN page's one string a row, with ">" for a link; a CONFIG page's label at the left and its value right-aligned, a toggle that is on in green), grey rows the cursor skips;
- the cursor's bar behind its row, "^" and "v" where the page goes on past the window, and a line of the controls;
- text in the arrival text's font 1 at 2.8% of the screen's height with a soft shadow, rows every 3.6%.

Up and Down move (held, they repeat), the cross selects, Left and Right step, L1 and R1 jump a window, Triangle or Circle go back, Start or Select close, as in the other games. The pages are ToD's table: Game (the position slot, Save and Load position, Die, Challenge mode, Set aside file, Load set-aside file), Cheats (Fast loads, Remove cutscenes, Infinite ammo, Infinite health, God Ratchet, Max bolts, Max raritanium), Planets, Unlocks (weapons Locked, V1, V10; devices, gadgets and items Locked or Unlocked; Collectables, the four skill point and gold bolt buttons), Mods (a test build's preview) and Save files: every row does what the client's row does, through the same functions.

**What it does not do (yet).**
- No pause-menu row (2.4). No sounds: the game's UI sounds are the Anark presentation's and were not traced.
- The look is the menu's own, not the game's: no native frame, no button glyphs in the controls line (the font has them as 0xE0xx code points; the right UTF-8 for them was not worked out), no slider for a VALUE row (none of ToD's rows has a range anyway).
- No RPCS3 (2.7).
- Long labels are not cut: a label over about 40 characters can run under its value.

## 4. Tests

Host (`test/test_menu_tod.c`, 62 checks): the pages; the install only with `menu.tod=1` and NO_MENU following it, the combo's notification without it, the two words, menu.enabled = 0 and back; the renderer against mocks of Text_Draw, Sprite_Init, Sprite_Draw and the HUD calls: the request left in a cinematic and withdrawn, the opening in play (the clock frozen and put back) and in the pause (the clock untouched), what is drawn and where, the pad blanked and the held mask put back, navigation and repeat, a toggle switched, back, close, a load closing the menu, the combo closing it. `test_uipak.c` holds the pak's ninth entry to kind 3 and `MH_TOD_PAK_ID`. Smoke (`smoke_tod_menu`): over the wire, nothing in the game without the key, then with it the install, the two words, the mailbox, the root and Cheats pages, an event and the combo, and NO_MENU clear. `check.py` (`check_tod_menu`) holds the renderer to NPEA00452.ELF and the pak with ToD to the table.

## 5. On a console: the test list

Set `menu.tod=1` in `/dev_hdd0/qwark/config.txt` (and `combo.enabled` not 0), install the test build (module, `tod.sprx` and the test build's `ui.pak`, which has the ToD entry), boot ToD and load a save into a level.

1. **qwark.log**: "menu: renderer installed for game 5, mailbox 0x008CD000" a few seconds in. If instead the install failed (a write error at 0x868000 or 0x8CD000), the caves' pages are not mapped: say so, and nothing else was touched.
2. **Play on as normal for a minute**, with the menu never opened: walking, shooting, the pause menu, a death, a cutscene. Nothing should differ (both hooks run every frame; this is the crash test).
3. **The combo (L1+R1+L2+R2+L3+R3) in play**: the panel appears (photo); the world stops (enemies, particles, the clock); Ratchet does not move with the sticks; the HUD still draws under it.
   - Is the panel dark, or a texture, or invisible (the default texture's question)? Is the text readable, the right size, in place? (photo)
4. Up/Down through the root, with and without holding; Cheats; Infinite ammo on (the client's checkbox follows); back with Triangle; Start: the panel goes, the world moves again, and **no pause menu opens** from that Start.
5. Open again, Game > Save position, close, move away, open, Load position.
6. **In the game's pause menu**: Start, then the combo: the panel over the pause; Triangle on the root closes it back to the pause; the pause menu should not have reacted to any press meanwhile.
7. Planets > a planet: the menu closes, the planet loads (from play). From the pause, the load is refused as busy (the game takes a planet load only in play), with a console notification.
8. The combo during a cutscene or a load: nothing happens (by design).
9. Unlocks > Weapons: a weapon Locked → V1 → V10; Save files lists the library.
10. Optional: 1080p and 4:3 output: the panel scales with the screen.

On RPCS3 there is nothing to test: like every game's menu it is console-only, and the combo there says so.
