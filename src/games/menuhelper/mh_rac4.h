/*
 * Deadlocked's console menu renderer (NPEA00423): where it lives, the five
 * places it hooks, and the game it calls. See mh_rac4.c for what it does and
 * src/core/menubox.h for the mailbox it shares with qwark.
 *
 * The renderer is the native screen of rewrite/REPORT-dl-ui-framework.md, the
 * one probe N0 proved on hardware, grown into a renderer: screen slot 36 of
 * GuiMenu_Screens shows qwark's current page with GuiSupport's button list.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines, and
 * the assembler includes this file through the C preprocessor. Each line that
 * build.sh reads is a name and one hexadecimal literal, a comment allowed after.
 */
#ifndef QWARK_MH_RAC4_H
#define QWARK_MH_RAC4_H

#define MH_GAME_ID          0x00000004   /* GAME_RAC4 */
#define MH_TITLE            "NPEA00423"

/*
 * Code: 8 KB of the text segment's last 64 KB page, which the executable leaves
 * empty from 0x970D68. The rest of that tail is spoken for: Stereo Music's cave
 * at 0x971000..0x971878, probe A's at 0x972000..0x97315C and probe N0's at
 * 0x97F000..0x97F754 (both probes are mods the user still has). N0 shares this
 * renderer's hook sites, so the two could never be in a game together anyway;
 * keeping clear of its bytes means loading it by mistake cannot overwrite code
 * the game is running.
 *
 * Data: in the read-write segment's last page, which ends at 0x17DCF10 in the
 * executable. The display mods keep their words at 0x17DD000..0x17DD600, Stereo
 * Music at 0x17DF000..0x17DF0A8 and 0x17DFFD0..0x17E0000, probe A at
 * 0x17DE000..0x17DE8CC and N0 at 0x17DEC00..0x17DEDA0.
 */
#define MH_CAVE             0x0097D000
#define MH_CAVE_END         0x0097F000
#define MH_DATA             0x017DF200
#define MH_DATA_END         0x017DFF00

/* The mailbox is the start of the data: menubox.h, 8 rows. */
#define MH_ROWS             0x00000008
#define MH_MBOX_SIZE        0x00000280   /* MBOX_OFF_ROWS + 8 * MBOX_ROW_SIZE */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/* The renderer's own state follows the mailbox (the ST_* offsets below). */
#define MH_STATE            0x017DF480
#define MH_STATE_OFF        0x00000280
#define MH_MAGIC            0x514D4834   /* 'QMH4' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each hook word with the word it replaces (tools/check.py reads both back out
 * of the executable). qwark installs them in this order, after the caves, and
 * takes them out in the reverse order.
 *
 * MH_SITE_FRAME    UpdateMenus' `bl GuiSupport_HandleMenuInput` (r3 = player):
 *                  the renderer's frame, while any list menu is fully shown.
 * MH_SITE_SCREEN   GuiHud's `li r4,1` before GuiMenu_GoToScreen(0, r4) in the
 *                  single-player and co-op Start path: 36 instead of 1 in the
 *                  frame qwark asked the menu to open.
 * MH_SITE_INJECT   the nop after `bl GB_GetLocalHeroFromSlot` in GuiHud's update,
 *                  just before it tests Start: the frame that takes an open
 *                  request presses Start there for the game.
 * MH_SITE_OPTIONS  MenuMain_StartOptions' `bl GuiSupport_SetMenuButtons`: the
 *                  RaCMAN row in front of "Go back", as N0 had it.
 * MH_SCREEN_SLOT   GuiMenu_Screens[36], {fcInit, fcReset, fcStart, fcStop}: no
 *                  init and no reset, the renderer's start and stop.
 */
#define MH_SITE_FRAME       0x00368620   /* bl 0x368304, word 0x4BFFFCE5 */
#define MH_SITE_SCREEN      0x0035E074   /* li r4,1, word 0x38800001 */
#define MH_SITE_INJECT      0x0035DFBC   /* nop, word 0x60000000 */
#define MH_SITE_OPTIONS     0x003A6F2C   /* bl 0x36B498, word 0x4BFC456D */
#define MH_SCREEN_SLOT      0x009E8B58   /* 0, 0, 0x991D40, 0 */
#define MH_SCREEN_ID        0x00000024   /* 36 */

#define MH_ORIG_FRAME       0x4BFFFCE5
#define MH_ORIG_SCREEN      0x38800001
#define MH_ORIG_INJECT      0x60000000
#define MH_ORIG_OPTIONS     0x4BFC456D
#define MH_ORIG_SLOT_START  0x00991D40   /* GuiMain_StartNextState's descriptor, which nothing calls there */

/* Where the two stubs that are not calls go on to. */
#define GAME_HANDLE_MENU_INPUT 0x00368304   /* GuiSupport_HandleMenuInput */
#define GAME_SET_MENU_BUTTONS  0x0036B498   /* GuiSupport_SetMenuButtons */

/* --------------------------------------------------------------- game data */

#define DL_MENU_PLAYER      0x0119F9A8   /* MenuMain_PlayerData, 0x1F8 a player */
#define DL_MENU_PLAYER_SZ   0x000001F8
#define DL_MENU_OPT_CURSOR  0x00000004   /* its Options cursor */
#define DL_ROWS_COUNT_OFF   0x00000060   /* vButtons + 0x60 = iButtons */
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

/* Sounds GuiMain_PlaySound takes. */
#define SND_MOVE            0x00000000   /* the cursor's tick */
#define SND_FORWARD         0x00000007
#define SND_BACK            0x00000008
#define SND_SCREEN          0x0000000B

/*
 * The game's function descriptors {code, TOC 0x9BCD58}, each read back out of
 * the executable by tools/check.py.
 */
#define OPD_GET_CANVAS      0x00986328   /* GetCurrentCanvas()                  0x0B2244 */
#define OPD_GET_CURSOR      0x00992278   /* GuiSupport_GetMenuCursor(p)         0x368490 */
#define OPD_PUSH_SCREEN     0x00992018   /* GuiMenu_PushScreen(p, id)           0x362308 */
#define OPD_GET_SCREEN      0x00992008   /* GuiMenu_GetScreen(p)                0x3622C0 */
#define OPD_SET_CURSOR      0x009923B8   /* GuiSupport_SetMenuCursor(p, i)      0x36B6A4 */
#define OPD_SET_TITLE       0x009923A8   /* GuiSupport_SetMenuTitle(p, s)       0x36B638 */
#define OPD_SCREEN_MODE     0x00991DE8   /* GuiMain_GetScreenMode(p)            0x35F8A0 */
#define OPD_SET_TARGET      0x00992390   /* GuiSupport_SetMenuTarget(p, x, y, w, h) 0x36B2DC */
#define OPD_SET_HEADER      0x009923B0   /* GuiSupport_SetMenuHeader(p, on)     0x36B688 */
#define OPD_SET_BUTTONS     0x009923A0   /* GuiSupport_SetMenuButtons(p, n, rows) 0x36B498 */
#define OPD_SHOW_MENU       0x00992380   /* GuiSupport_ShowMenu(p, close cb)    0x36AEC4 */
#define OPD_SELECT          0x00992378   /* GuiSupport_Select(): the highlight  0x36AE04 */
#define OPD_UPDATE_WIDGETS  0x00992260   /* GuiSupport_UpdateMenuWidgets(p)     0x367DE0 */
#define OPD_TAKES_INPUT     0x00991DC0   /* GuiMain_TakesInput(p, kind)         0x35F2D4 */
#define OPD_PLAY_SOUND      0x00991E40   /* GuiMain_PlaySound(id)               0x35FAC0 */
#define OPD_GO_BACK_CB      0x00993F80   /* MenuMain_OptionsCMDGoBackCB(arg)    0x3A809C */
#define OPD_MENU_STOP       0x00993F50   /* MenuMain_Stop(p)                    0x3A7E48 */

/* ------------------------------------------------ the renderer's own state */

/*
 * Offsets from MH_STATE. The first words are counters, so a memory view of
 * them reads as a log (the hardware list in SPEC-console-ui.md section 9.3).
 */
#define ST_MAGIC            0x00   /* u32 MH_MAGIC */
#define ST_STARTS           0x04   /* u32 times screen 36 started */
#define ST_OPTIONS_ROWS     0x08   /* u32 times the RaCMAN row went into Options */
#define ST_OPTIONS_FULL     0x0C   /* u32 times Options had no room for it */
#define ST_OPENS            0x10   /* u32 times the RaCMAN row was pressed */
#define ST_INJECTS          0x14   /* u32 open requests taken (Start pressed for the game) */
#define ST_COMBO_OPENS      0x18   /* u32 times the Start path answered 36 */
#define ST_EVENTS           0x1C   /* u32 events posted */
#define ST_PAGES            0x20   /* u32 pages taken */
#define ST_RETRIES          0x24   /* u32 pages read while qwark was writing them */
#define ST_DROPPED          0x28   /* u32 presses dropped, an event already in flight */
#define ST_SHOWN_SEQ        0x2C   /* u32 page_seq of the page shown */
#define ST_SHOWN_KEY        0x30   /* u32 its key */
#define ST_BUSY_SINCE       0x34   /* u32 MBOX frames when the event in flight went out */
#define ST_FORCE36          0x38   /* u8 the Start path answers 36 this frame (the stubs) */
#define ST_HAVE_PAGE        0x39   /* u8 a page has been taken */
#define ST_NROWS            0x3A   /* u8 rows shown */
#define ST_CURSOR_HINT      0x3B   /* u8 the page's cursor */
#define ST_PFLAGS           0x3C   /* u8 the page's flags */
#define ST_WAITING          0x3D   /* u8 the title says qwark is not answering */
#define ST_LAST_CURSOR      0x3E   /* u8 the cursor when the screen last stopped */
#define ST_NEW_KEY          0x3F   /* u8 the page taken has a key not shown before */
#define ST_KIND             0x40   /* u8[8] */
#define ST_FLAGS            0x48   /* u8[8] */
#define ST_ID               0x50   /* u16[8] */
#define ST_BUTTONS          0x60   /* MenuButton_s[8], 12 bytes: u8 enabled, pad, char *label, cb */
#define ST_TITLE            0xC0   /* char[32] */
#define ST_OPTION_LABEL     0xE0   /* char[16] "RaCMAN" */
#define ST_WAIT_TITLE       0xF0   /* char[32] */
#define ST_TEXT             0x110  /* char[8][64]: label and value, as the list shows them */
#define ST_STAGE            0x310  /* a copy of the mailbox's page: key .. rows */
#define ST_SIZE             0x568  /* ST_STAGE + MH_MBOX_SIZE - MBOX_OFF_PAGE_KEY */

#define MH_TEXT_LEN         0x40
#define MH_BUTTON_SIZE      0x0C

/*
 * Frames an event may go unanswered before the title says so: half a second.
 * qwark answers in a tick or two, so this only shows when it is not running.
 */
#define MH_WAIT_FRAMES      0x0000001E

#endif /* QWARK_MH_RAC4_H */
