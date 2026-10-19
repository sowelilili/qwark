/*
 * Deadlocked's console menu renderer (NPEA00423): the game header the core and
 * the shim are both built with (mh_shim.h says what a game header defines),
 * where the renderer lives, the four places it hooks and the game it calls.
 * mh_rac4.c is the shim; mh_core.c the core.
 *
 * The shim shows qwark's pages as screen slot 36 of GuiMenu_Screens, which the
 * game leaves empty, with two of the game's own widgets:
 *
 *   MAIN    GuiSupport's button list, the list Options and Extras use (probe
 *           N0, round 1): up to eight rows of "label: value";
 *   CONFIG  the look of Options > GAME SETTINGS (screen 12, MenuAudioVisual):
 *           its screen panel (template 37: its plates, separators, frame,
 *           title and legend), its capitals (round 4: without its colons, the
 *           user's call), its colours and sounds and its sliders, in a GuiState
 *           thread of the shim's own as that screen runs one. Its eight fixed
 *           rows hold five value texts and three sliders in fixed places, so
 *           the rows are a CtlList of the game's two-column row (template 28,
 *           the same font and size as GAME SETTINGS' rows) over the screen's own
 *           row area, eight rows on screen and scrolling to sixteen, a window of
 *           a longer page that slides under the cursor (round 4); a slider is
 *           drawn in a row with the row's own outline and swatch rectangles, at
 *           GAME SETTINGS' place and size (round 3).
 *
 * Every new page is the game's own way between screens: GoToScreen to slot
 * 36 again, whose stop ends the widget shown (the list's MenuMain_Stop, or the
 * thread told to end) and whose start begins the page's (round 3: a new page
 * of the same type too, so a link, a back and a window turn each close and
 * open the screen as the game's own screens do). There are two ways in: the
 * pause main menu's Help row, which becomes TRAINER and opens screen 36
 * instead of Help, and qwark's menu combo, through GuiHud's own Start path. No
 * row is added to any of the game's lists.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines, and
 * the assembler includes this file through the C preprocessor. Each line that
 * build.sh or check.py reads is a name and one hexadecimal literal (or a
 * string), a comment allowed after.
 */
#ifndef QWARK_MH_RAC4_H
#define QWARK_MH_RAC4_H

#define MH_GAME_ID          0x00000004   /* GAME_RAC4 */
#define MH_TITLE            "NPEA00423"

/*
 * Code: 16 KB of the text segment's last 64 KB page, which the executable
 * leaves empty from 0x970D68. The rest of that tail is spoken for: Stereo
 * Music's cave at 0x971000..0x971878, probe A's at 0x972000..0x97315C (whose
 * code also names 0x97A0A0) and probe N0's at 0x97F000..0x97F754 (both probes
 * are mods the user still has). N0 shares this renderer's hook sites, so the
 * two could never be in a game together anyway; keeping clear of its bytes
 * means loading it by mistake cannot overwrite code the game is running.
 *
 * Data: two spans of the read-write segment's last page, which ends at
 * 0x17DCF10 in the executable. The display mods keep their words at
 * 0x17DD000..0x17DD600, probe A at 0x17DE000..0x17DE8CC, N0 at
 * 0x17DEC00..0x17DEDA0, Stereo Music at 0x17DF000..0x17DF0A8 and
 * 0x17DFFD0..0x17E0000. The mailbox takes the first span, the core's and the
 * shim's state, the read-only data, the TOC and the descriptors the second.
 */
#define MH_CAVE             0x0097B000
#define MH_CAVE_END         0x0097F000
#define MH_BOX              0x017DD600
#define MH_BOX_END          0x017DDF00
#define MH_DATA             0x017DF0C0
#define MH_DATA_END         0x017DFFC0

/*
 * The mailbox (menubox.h): sixteen rows of 0x40 bytes (round 4: no
 * descriptions, 1 KB less of the span). Sixteen rows hold every list of
 * Deadlocked's whole (the bot upgrades, the planets, the weapons); a longer
 * one, a save category of the console's library, slides sixteen at a time.
 * The data span has no room for more (sizes in SPEC 9.19).
 */
#define MH_ROWS             0x00000010
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x00000008   /* GuiSupport's list has eight */
#define MH_CFG_ROWS         0x00000010
#define MH_MBOX_SIZE        0x00000480   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/*
 * The game's own casing (read from the PS3 build's english.ps3): its lists of
 * links are in capitals ("OPTIONS"); GAME SETTINGS has its title in capitals
 * ("GAME SETTINGS"), its labels in capitals ending in a colon ("SOUND:",
 * "MUSIC VOLUME:", "HELP TEXT:") and its values in capitals ("STEREO", "ON").
 * Round 4, the user's call: the labels without the colon, and an action
 * with nothing to say blank (round 3 had "SOUND:" and "..."). The current
 * option of a list is marked with a star.
 */
#define MH_CASE_MAIN        0x00000001   /* MH_CASE_UPPER */
#define MH_CASE_HEAD        0x00000001   /* MH_CASE_UPPER */
#define MH_CASE_LABEL       0x00000001   /* MH_CASE_UPPER */
#define MH_CASE_VALUE       0x00000001   /* MH_CASE_UPPER */
#define MH_STYLE            0x00000008   /* MH_STYLE_STAR */
#define MH_MAIN_SEP         ": "

/*
 * Frames an event may go unanswered before the title says so: half a second.
 * qwark answers in a tick or two, so this only shows when it is not running.
 */
#define MH_WAIT_FRAMES      0x0000001E

/*
 * Round 3: a new page starts the screen again on either widget (MH_SHOW_NEW),
 * as the game moves between its own screens: a list closes with MenuMain_Stop
 * and the next opens with ShowMenu, so the frame and the cursor glide to the
 * new page; a GAME SETTINGS panel ends its thread with its sound and the next
 * builds itself with its own. Round 2 refilled both in place, and only a page
 * of the other type moved between screens. A page refreshed in place, its
 * values changed or (MH_SHOW_ROWS alone) its rows grown or shrunk, still is.
 */
#define MH_RESTART_MAIN     0x00000001   /* MH_SHOW_NEW */
#define MH_RESTART_CFG      0x00000001   /* MH_SHOW_NEW */

/*
 * Round 3: how long a screen going still draws once the next has started. The
 * list's MenuMain_Stop is GuiSupport_CloseMenu, which clears bShown; the next
 * GuiSupport_UpdateMenuWidgets (0x367DE0, the frame after) sets the list's
 * container's visibility from bShown, so the old buttons draw at most a frame
 * more (2 to be sure). The CONFIG page's thread clears its canvas layer in its
 * deinit, before GuiMenu_StartNextScreen starts the next. A list followed by a
 * list is no screen going at all: ShowMenu shows the same widget again with
 * the new page's pointers inside the frame of the stop, so the shim holds
 * nothing then (mh_core_hold(0) in main_start).
 */
#define MH_HOLD_MAIN        0x00000002
#define MH_HOLD_CFG         0x00000000

/* The Help row's label, in the capitals of the pause main menu (round 3: TRAINER for RACMAN). */
#define MH_ENTRY_LABEL      "TRAINER"

/* The core's state at the start of the data, then the shim's (SH_* below). */
#define MH_STATE            0x017DF0C0
#define MH_SHIM             0x017DFA90   /* MH_STATE + MH_CORE_SIZE, which mh_rac4.c checks */
#define MH_MAGIC            0x514D4834   /* 'QMH4' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each hook word with the word it replaces (check.py reads both back out of
 * the executable). qwark installs them in this order, after the caves, and
 * takes them out in the reverse order: screen 36's slot, the frame, the Start
 * path's two, then the Help row's two, so the row can only push a screen that
 * is already the menu's.
 *
 * MH_SITE_FRAME    UpdateMenus' `bl GuiSupport_HandleMenuInput` (r3 = player):
 *                  the shim's frame, while a list menu of the game's is fully
 *                  shown, which for screen 36 is a MAIN page.
 * MH_SITE_SCREEN   GuiHud's `li r4,1` before GuiMenu_GoToScreen(0, r4) in the
 *                  single-player and co-op Start path: 36 instead of 1 in the
 *                  frame qwark asked the menu to open.
 * MH_SITE_INJECT   the nop after `bl GB_GetLocalHeroFromSlot` in GuiHud's update,
 *                  just before it tests Start: the frame that takes an open
 *                  request presses Start there for the game.
 * MH_SCREEN_SLOT   GuiMenu_Screens[36], {fcInit, fcReset, fcStart, fcStop}: no
 *                  init and no reset, the shim's start and stop.
 *
 * Round 1 also hooked MenuMain_StartOptions' `bl GuiSupport_SetMenuButtons`
 * (0x3A6F2C) for a RaCMAN row in Options. That row is gone: a row added to a
 * list the player knows moves the rows under it. check.py holds the install to
 * no word in that function and no address in MenuMain_PlayerData.
 */
#define MH_SITE_FRAME       0x00368620   /* bl 0x368304, word 0x4BFFFCE5 */
#define MH_SITE_SCREEN      0x0035E074   /* li r4,1, word 0x38800001 */
#define MH_SITE_INJECT      0x0035DFBC   /* nop, word 0x60000000 */
#define MH_SCREEN_SLOT      0x009E8B58   /* 0, 0, 0x991D40, 0 */
#define MH_SCREEN_ID        0x00000024   /* 36 */

#define MH_ORIG_FRAME       0x4BFFFCE5
#define MH_ORIG_SCREEN      0x38800001
#define MH_ORIG_INJECT      0x60000000
#define MH_ORIG_SLOT_START  0x00991D40   /* GuiMain_StartNextState's descriptor, which nothing calls there */

/*
 * The entry point (round 2): the pause main menu's Help row becomes the menu's.
 * MenuMain_StartMain (0x3A5AE4) builds the single-player and co-op main menu,
 * eight rows, Help the fifth; its label is `bl msg_string` with the row's text
 * id at MH_SITE_HELP_LABEL, its callback MenuMain_MainCMDHelpCB (descriptor
 * 0x993FD0, code 0x3A8540, which nothing else names), which saves the main
 * menu's cursor and pushes screen 30, Help, with `li r4,30` at
 * MH_SITE_HELP_SCREEN. The install makes the label the shim's and the screen
 * 36, so the row keeps its place and the list its eight rows, and back from the
 * menu's root pops back to the main menu with the cursor on the row. The
 * multiplayer branch builds its own rows and has no Help: nothing changes
 * there.
 */
#define MH_SITE_HELP_LABEL  0x003A649C   /* bl 0x198A5C (msg_string), word 0x4BDF25C1 */
#define MH_SITE_HELP_SCREEN 0x003A8584   /* li r4,30, word 0x3880001E */
#define MH_ORIG_HELP_LABEL  0x4BDF25C1
#define MH_ORIG_HELP_SCREEN 0x3880001E
#define MH_HELP_SCREEN_WORD 0x38800024   /* li r4,36 */
#define DL_HELP_CB          0x003A8540   /* MenuMain_MainCMDHelpCB */
#define DL_HELP_CB_OPD      0x00993FD0
#define DL_START_MAIN       0x003A5AE4   /* MenuMain_StartMain */

/* Where the one stub that is not a call goes on to. */
#define GAME_HANDLE_MENU_INPUT 0x00368304   /* GuiSupport_HandleMenuInput */

/* --------------------------------------------------------------- game data */

#define DL_MENU_VSCREENS    0x009EAE3C   /* MenuMain_vScreens: {x, y, w, h} per screen mode */
#define DL_GAME_TYPE        0x00B36DF4   /* g_gameType: 1 multiplayer */
#define DL_PAD0_PTR         0x009C28CC   /* the word holding port 0's PAD */
#define DL_PAD_HUDBITS      0x000000C4   /* PAD.hudBitsOn, this frame's presses, the menus' input */
#define DL_HERO_PAD         0x00002F00   /* LocalHero.pPad */

/*
 * What qwark reads to know the menu is not up before it takes the hooks out:
 * gameMode (3 is the pause, which the menu always runs in), pendingGameMode
 * (-2, nothing queued) and player 0's GuiMenu screen.
 */
#define DL_GAME_MODE        0x00B3C5A0
#define DL_MODE_PAUSE       0x00000003
#define DL_PENDING_MODE     0x009C37F4
#define DL_MODE_NONE        0xFFFFFFFE
#define DL_MENU_SCREEN      0x0110D784   /* GuiMenu_PlayerData[0].iScreen */

/*
 * The CONFIG page (round 3), as MenuAudioVisual_CreateWidgets (0x38F664) and
 * its GuiState Init (0x39063C) build GAME SETTINGS: the screen panel from
 * template 37 on canvas layer 15, its title and its button legend, three
 * plates, the frame and the cursor on its frame element. Read from the PS3
 * build's own template (bonus.ps3, the IGE layouts; check.py reads it back when
 * it is there), template 37's elements are:
 *   1, 4, 7       the footer, bottom and top plates (gui_plates[4], [7], [6]);
 *   2, 3          the separators under and over the rows;
 *   5             the dark backdrop of the rows, 0.2621..0.7284 high: the
 *                 list's target here, as element 5 is on the Cheats screen;
 *   0xA..0x11     each row's invisible focus rectangle, row 7 first;
 *   0x12          the frame's target;
 *   0x13, 0x14    the legend and the title, in the footer;
 *   0x15..0x2B    the eight fixed rows' widgets: five value texts (0x15,
 *                 0x16, 0x17, 0x21, 0x22), three sliders of an outline, a
 *                 track and a fill (0x18..0x20), eight labels (0x23..0x28,
 *                 and 0x2A and 0x2B in the container 0x29).
 * The shim hides 0x15..0x29 and puts its CtlList over element 5, narrowed to
 * the Cheats screen's list width (DL_LIST_*). The rows are
 * template 28, the Cheats screen's two-column row, whose texts are GAME
 * SETTINGS' font (15) and size (1.1103): 3 the label, 2 the value, and two
 * containers of an outline and a swatch (4 holding 5 and 6, 7 holding 8 and
 * 9), hidden unless shown, which a slider row reshapes into GAME SETTINGS'
 * slider: 5 the outline, 6 the track, 9 the fill, 8 hidden.
 */
#define DL_TPL_SCREEN       0x00000025   /* 37 */
#define DL_TPL_ROW          0x0000001C   /* 28 */
#define DL_LAYER            0x0000000F
#define DL_EL_TITLE         0x00000014
#define DL_EL_LEGEND        0x00000013
#define DL_EL_LIST          0x00000005
#define DL_EL_CURSOR        0x00000012
#define DL_EL_ROWS_FIRST    0x00000015   /* the fixed rows' widgets the shim hides */
#define DL_EL_ROWS_LAST     0x00000029
#define DL_EL_PLATE_TOP     0x00000007
#define DL_EL_PLATE_BOTTOM  0x00000004
#define DL_EL_PLATE_FOOT    0x00000001
#define DL_ROW_LABEL        0x00000003
#define DL_ROW_VALUE        0x00000002
#define DL_ROW_BOX_A        0x00000004   /* container: the outline and the track */
#define DL_ROW_OUTLINE      0x00000005
#define DL_ROW_TRACK        0x00000006
#define DL_ROW_BOX_B        0x00000007   /* container: the spare outline and the fill */
#define DL_ROW_SPARE        0x00000008
#define DL_ROW_FILL         0x00000009
#define DL_PLATE_TOP        0x00ACA378   /* &g_wad.hud.gui_plates[6] */
#define DL_PLATE_BOTTOM     0x00ACA380   /* [7] */
#define DL_PLATE_FOOT       0x00ACA368   /* [4] */
#define DL_MSG_SELECT       0x0000234F   /* "\x10 SELECT", the legend's first half */
#define DL_MSG_BACK         0x00002350   /* "\x12 BACK" */

/*
 * GAME SETTINGS' slider (template 37's 0x1E..0x20), in a row's own space,
 * where template 28 centres the row on y 0.5 and right-aligns the value at
 * 0.8887: its right end 0.0117 past the value column's, as GAME SETTINGS'
 * slider ends 0.0117 past its own value column (0.9355 against 0.9238). The
 * fill is the track's rectangle cut at the value, as GuiMain_FillTarget cuts it
 * with no border, in the team colours GAME SETTINGS gives its fills.
 */
#define DL_SLIDER_X         0.72265f     /* the centre */
#define DL_SLIDER_Y         0.5f
#define DL_OUTLINE_W        0.3555f
#define DL_OUTLINE_H        0.0288f
#define DL_TRACK_W          0.3477f
#define DL_TRACK_H          0.0192f
#define DL_RGBA_TRACK       0x80202020

/*
 * The list's box: element 5 made as wide as the Cheats screen's list box
 * (template 27's element 5, 0.04885..0.91795) and left as high as GAME
 * SETTINGS' rows (0.26205..0.72835, eight rows). CtlList draws its scroll bar
 * 0.0039 to 0.0234 past the box's right end; GAME SETTINGS' own backdrop ends
 * at 0.9473, which would put the bar on and past the frame's edge (0.9472),
 * while the Cheats screen's width keeps it inside as it is there, and the rows
 * where the Cheats screen has them.
 */
#define DL_LIST_L           0.04885f
#define DL_LIST_T           0.26205f
#define DL_LIST_R           0.91795f
#define DL_LIST_B           0.72835f

/*
 * The CtlList object (0x6C bytes) and its items (0x18 each). CtlList::CtlList
 * (0x633618) writes the vtable at +0 and nothing else, and nothing calls
 * through it: every screen calls CtlList::Update (0x633E00) directly, and its
 * one virtual slot is that Update. Stats' own Init wipes its list's vtable to 0
 * with the rest of its data and the screen works (the live read in
 * console-ui-probes/live), so the word is written here only to match.
 */
#define DL_CTLLIST_VTABLE   0x008BA020
#define DL_LIST_HANDLE      0x00000030
#define DL_LIST_FOCUS       0x00000054
#define DL_LIST_SIZE        0x0000006C
#define DL_ITEM_SIZE        0x00000018
#define DL_ITEM_PANEL       0x00000004

/*
 * The row colours GAME SETTINGS' select (0x38F49C) and deselect (0x38F578) give
 * a row's label and value: the cursor's row, a row that is not enabled, and the
 * rest. It has no colour for an active row (round 2 had the Cheats screen's gold).
 */
#define DL_RGBA_FOCUS       0x80D0D0D0
#define DL_RGBA_LOCKED      0x80404040
#define DL_RGBA_OFF         0x80808080

/* GuiMain_GetMenuCommand's commands (GuiMain_Bindings). */
#define DL_CMD_CROSS        0x00000001
#define DL_CMD_TRIANGLE     0x00000002
#define DL_CMD_UP           0x00000005
#define DL_CMD_DOWN         0x00000006
#define DL_CMD_LEFT         0x00000007
#define DL_CMD_RIGHT        0x00000008
#define DL_CMD_L1           0x00000009
#define DL_CMD_R1           0x0000000B
#define DL_CMD_CLOSE        0x0000000D

/* The pad bits GuiMain_Bindings maps to commands, in its order of priority. */
#define PADB_L2             0x00000001
#define PADB_R2             0x00000002
#define PADB_L1             0x00000004
#define PADB_R1             0x00000008
#define PADB_TRIANGLE       0x00000010
#define PADB_CIRCLE         0x00000020
#define PADB_CROSS          0x00000040
#define PADB_SQUARE         0x00000080
#define PADB_SELECT         0x00000100
#define PADB_START          0x00000800
#define PADB_UP             0x00001000
#define PADB_RIGHT          0x00002000
#define PADB_DOWN           0x00004000
#define PADB_LEFT           0x00008000

/*
 * Sounds GuiMain_PlaySound takes, and who plays them in the game:
 *   0     the cursor's tick: every list's and screen's Up and Down;
 *   1     the pause opening: MenuMain_StartMain, when the main menu starts
 *         from play rather than back from one of its own screens;
 *   4     a slider or a stepper at its end (GAME SETTINGS' volumes, 3D);
 *   5     a list closed with Start (Options' back callback, CloseMenu(p, 5));
 *   7, 8  a list's row opening a screen, and a list's back;
 *   0xB, 0xC  a panel screen starting and stopping (Cheats, GAME SETTINGS);
 *   0x10  a toggle or a choice changed (GAME SETTINGS' SOUND, HELP TEXT;
 *         the Cheats screen's rows);
 *   0x11  a slider or a stepper moved (GAME SETTINGS' volumes, 3D).
 */
#define SND_MOVE            0x00000000
#define SND_OPEN            0x00000001
#define SND_LIMIT           0x00000004
#define SND_FORWARD         0x00000007
#define SND_BACK            0x00000008
#define SND_SCREEN          0x0000000B
#define SND_SCREEN_CLOSE    0x0000000C
#define SND_CHANGE          0x00000010
#define SND_SLIDE           0x00000011

/*
 * MenuMain_StartMain's open sound (0x3A67D8..0x3A681C): 1 when the main menu
 * sound flag is set (CloseMenu sets it, so a menu closed earlier makes the next
 * one sound), the flag then cleared; else 1 when g_bMenuExit is set, which every
 * callback that opens one of the main menu's screens clears (the Help row's,
 * 0x3A8588) and the main menu sets again once it has started. The shim's start
 * on an open (not a page change) does the same, so the menu sounds as the pause
 * does from the combo and says nothing from the Help row, whose callback has
 * already played 7.
 */
#define DL_MENU_SOUND_FLAG  0x009E88A4   /* u8, GuiMain_GetMainMenuSoundFlag (0x361428) reads it */
#define DL_MENU_EXIT        0x009EAE34   /* u8 g_bMenuExit */

/*
 * The game's function descriptors {code, TOC 0x9BCD58}, each read back out of
 * the executable by check.py.
 */
#define OPD_GET_CANVAS      0x00986328   /* GetCurrentCanvas()                  0x0B2244 */
#define OPD_SET_CANVAS      0x00986320   /* SetCurrentCanvas(c)                 0x0B21F4 */
#define OPD_GET_CURSOR      0x00992278   /* GuiSupport_GetMenuCursor(p)         0x368490 */
#define OPD_GET_SCREEN      0x00992008   /* GuiMenu_GetScreen(p)                0x3622C0 */
#define OPD_SET_CURSOR      0x009923B8   /* GuiSupport_SetMenuCursor(p, i)      0x36B6A4 */
#define OPD_SET_TITLE       0x009923A8   /* GuiSupport_SetMenuTitle(p, s)       0x36B638 */
#define OPD_SCREEN_MODE     0x00991DE8   /* GuiMain_GetScreenMode(p)            0x35F8A0 */
#define OPD_SET_TARGET      0x00992390   /* GuiSupport_SetMenuTarget(p, x, y, w, h) 0x36B2DC */
#define OPD_SET_HEADER      0x009923B0   /* GuiSupport_SetMenuHeader(p, on)     0x36B688 */
#define OPD_SET_BUTTONS     0x009923A0   /* GuiSupport_SetMenuButtons(p, n, rows) 0x36B498 */
#define OPD_SHOW_MENU       0x00992380   /* GuiSupport_ShowMenu(p, close cb)    0x36AEC4 */
#define OPD_SELECT          0x00992378   /* GuiSupport_Select(): the highlight  0x36AE04 */
#define OPD_TAKES_INPUT     0x00991DC0   /* GuiMain_TakesInput(p, kind)         0x35F2D4 */
#define OPD_PLAY_SOUND      0x00991E40   /* GuiMain_PlaySound(id)               0x35FAC0 */
#define OPD_GO_BACK_CB      0x00993F80   /* MenuMain_OptionsCMDGoBackCB(arg)    0x3A809C */
#define OPD_MENU_STOP       0x00993F50   /* MenuMain_Stop(p)                    0x3A7E48 */
#define OPD_GO_TO_SCREEN    0x00991FE0   /* GuiMenu_GoToScreen(p, screen)       0x361F1C */
#define OPD_START_NEXT      0x00992000   /* GuiMenu_StartNextScreen(p)          0x362210 */
#define OPD_GET_COMMAND     0x00991EC0   /* GuiMain_GetMenuCommand(kind)        0x360958 */
#define OPD_RESET_STATE     0x0099E320   /* GuiState::ResetState(gs)            0x611150 */
#define OPD_EXIT_STATE      0x0099E380   /* GuiState::ExitState(gs)             0x611338 */
#define OPD_START_THREAD    0x00986360   /* StartThread(gs)                     0x0B2618 */
#define OPD_PANEL_CREATE    0x0099E438   /* Panel::Create(&panel, tpl, canvas)  0x6114F0 */
#define OPD_CANVAS_LAYER    0x00986338   /* SetCanvasLayer(layer, h)            0x0B2324 */
#define OPD_CLEAR_LAYER     0x00986398   /* ClearCanvasLayer(layer)             0x0B299C */
#define OPD_ADD_FRAME       0x009863C8   /* AddFrameToContainer(h, frame)       0x0B2C4C */
#define OPD_SET_TEXT        0x00986288   /* SetText(h, s)                       0x0B0CDC */
#define OPD_SET_COLOR       0x009862A0   /* SetColor(h, rgba)                   0x0B0FF8 */
#define OPD_SET_TEXTURE     0x009862B0   /* SetTexture(h, on)                   0x0B12E0 */
#define OPD_RECT_SOURCE     0x009863F8   /* SetRectangleDataSource(h, img)      0x0B3258 */
#define OPD_MSG_STRING      0x00989778   /* msg_string(id)                      0x198A5C */
#define OPD_IMAGE_GET       0x009920D8   /* PIB_GetImage(&plate, -1)            0x363950 */
#define OPD_IMAGE_DROP      0x009920E0   /* PIB_DropImage(img)                  0x363998 */
#define OPD_LIST_CREATE     0x0099EF98   /* CtlList::Create(l, items, n, cb, tpl, bounds) 0x633690 */
#define OPD_LIST_CLEAR      0x0099EFA0   /* CtlList::Clear(l)                   0x6336C4 */
#define OPD_LIST_FOCUS      0x0099EFA8   /* CtlList::SetFocus(l, i)             0x633768 */
#define OPD_LIST_NEXT       0x0099EFB0   /* CtlList::FocusNext(l)               0x63388C */
#define OPD_LIST_PREV       0x0099EFC8   /* CtlList::FocusPrev(l)               0x6339D0 */
#define OPD_LIST_RECT       0x0099EFD0   /* CtlList::GetFocusRect(l, &x, &y, &w, &h) 0x633A40 */
#define OPD_LIST_INSERT     0x0099EFD8   /* CtlList::Insert(l, data, at)        0x633B40 */
#define OPD_LIST_UPDATE     0x0099EFE8   /* CtlList::Update(l)                  0x633E00 */
#define OPD_LIST_VALID      0x0099EFB8   /* CtlList::SetValidator(l, fc)        0x6338F0 */
#define OPD_BOX_TARGET      0x0099EF38   /* CtlBox::SetTarget(l, h, jump)       0x632FC4 */
#define OPD_BOX_SHOWN       0x0099EF28   /* CtlBox::SetShown(l, on)             0x632E90 */
#define OPD_FRAME_TARGET    0x009922D0   /* GuiSupport_SetFrameTarget(p, i, h)  0x368FC0 */
#define OPD_SHOW_FRAMES     0x009922E0   /* GuiSupport_ShowFrames(p, on)        0x3690A0 */
#define OPD_ATTACH_CURSOR   0x009922C0   /* GuiSupport_AttachCursor(p, i, h, tgt, 0) 0x368D24 */
#define OPD_SHOW_CURSORS    0x009922A0   /* GuiSupport_ShowCursors(p, on)       0x368818 */
#define OPD_DETACH_CURSOR   0x009921E8   /* GuiSupport_DetachCursor(p, i, h, 0) 0x365CBC */
#define OPD_CURSOR_TARGET   0x00992200   /* GuiSupport_SetCursorTarget(p, i, x, y, w, h, jump) 0x366100 */
#define OPD_CLOSE_MENU      0x00992028   /* GuiMenu_CloseMenu(p, sound)         0x36249C */
#define OPD_SET_VISIBLE     0x00986250   /* SetVisibility(h, on)                0x0B0834 */
#define OPD_SET_RECT        0x009861A0   /* SetRectLTRB(h, l, t, r, b)          0x0AF4D8 */
#define OPD_VERTEX_COLORS   0x009862D0   /* SetVertexColors(h, c0, c1, c2, c3)  0x0B1600 */
#define OPD_TEAM_COLOR      0x00991EE0   /* GuiMain_GetTeamColor(team, i)       0x360EB0 */

/*
 * OPD_GO_BACK_CB is Options' back callback, and the menu has no Options under
 * it: arg 0 (triangle) plays sound 8 and calls GuiMenu_PopScreen, arg 1 (Start,
 * Select) calls GuiMenu_CloseMenu(p, 5). PopScreen with an empty stack calls
 * CloseMenu(p, -1), and the stack is always empty under screen 36: GuiHud's
 * Start path does GoToScreen(0, 36) and PushState(0, 2), the menu thread's
 * init (0x36270C) zeroes the stack depth every time the menu state starts, and
 * the shim moves between its widgets with GoToScreen, which pushes nothing.
 * CloseMenu pops GuiMain's state; leaving the menu state runs screen 36's stop
 * and, from the pause, PIB_RescuePause and mode_pop (0x35E3FC). So both close
 * the menu and unpause, as the main menu's own back does.
 *
 * A CONFIG page closes the way GAME SETTINGS does (0x38FA88): CloseMenu(p, -1),
 * which sets the main menu sound flag and plays nothing; the panel's own stop
 * plays its 0xC as the screen goes.
 */

/* ------------------------------------------------ the shim's own state */

/* Offsets from MH_SHIM. */
#define SH_FORCE36          0x00   /* u8 the Start path answers 36 this frame (the stubs) */
#define SH_CFG_STATE        0x01   /* u8 the CONFIG thread: 0 running, 1 its first frame, 2 leaving */
#define SH_CFG_CURSOR       0x02   /* u8 the cursor frame wants moving to the focus */
#define SH_EMPTY            0x03   /* u8 0: an empty string, a slider row's value */
#define SH_PANEL            0x04   /* u32 the CONFIG screen panel's handle */
#define SH_IMAGES           0x08   /* u32[3] the plates' images */
#define SH_CMD              0x14   /* u8 the CONFIG page's command in hand, for its sound */
#define SH_GS_NEXT          0x15   /* u8 which of the two GuiStates the next CONFIG start runs */
#define SH_WAS              0x16   /* u8 the type of the screen that stopped last (MBOX_PTYPE_*, MH_NONE) */
#define SH_TEAM             0x18   /* u32[2] the slider fill's team colours (GetTeamColor(1, 2), (1, 0)) */
#define SH_GS               0x20   /* GuiState[2] {state, init, deinit, update, pre, post} */
#define SH_LIST             0x50   /* CtlList, DL_LIST_SIZE */
#define SH_ITEMS            0xBC   /* CtlList_Item_s[MH_CFG_ROWS] */
#define SH_BUTTONS          0x23C  /* MenuButton_s[MH_MAIN_ROWS], 12 bytes: u8 enabled, pad, char *label, cb */
#define SH_LEGEND           0x29C  /* char[40] */
#define SH_SIZE             0x2C8

#define DL_GS_SIZE          0x18

#define MH_BUTTON_SIZE      0x0C

#endif /* QWARK_MH_RAC4_H */
