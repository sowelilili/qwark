/*
 * Up Your Arsenal's console menu renderer (NPEA00387): the game header the core
 * and the shim are both built with (mh_shim.h says what a game header
 * defines), where the renderer lives, the thirteen words it writes and the game
 * it calls. mh_rac3.c is the shim; mh_core.c the core.
 *
 * UYA's single-player pause runs on the IGE widget engine, as Deadlocked's
 * does, but without GuiMenu's screen table: each pause screen is a GuiState
 * "thread" {state; init, deinit, update, pre, post} that the pause launcher or
 * another screen starts with SwapTo, which queues it and has the running one
 * reset itself (rewrite/REPORT-rac2-uya-ui.md section 2). The shim brings two
 * GuiStates of its own, the menu's screen, and moves between them as the game
 * moves between its screens. It shows qwark's pages with two of the game's own
 * widgets, as probe N3b did on hardware:
 *
 *   MAIN    the main pause's own list: its init, update and deinit run on the
 *           shim's row table (rows of the main pause's shape, labels by text
 *           id through the msg_string hook), so the frame is sized to the rows
 *           and the cursor, colours, sounds and the Start close are the
 *           game's; up to nine rows, as the game's own nine-row tables;
 *   CONFIG  CHEATS' screen: the submenu frame (the title, "SELECT / BACK") and
 *           the grid list of raw strings in two columns, with the column
 *           alignment CHEATS sets, up to sixteen rows of which seven are on
 *           screen at once and the grid scrolls the rest.
 *
 * The main pause's list lays its rows and frame out only as it starts, so a
 * page that is new or has another number of rows starts the menu's screen again
 * on the other GuiState (MH_RESTART_*), as the game's own screens are started.
 *
 * Two ways in, as in Deadlocked: the main pause's HELP row, which says RACMAN
 * and starts the menu, and qwark's combo, through the mode 0 update's Start
 * path and the pause launcher. No row is added to any of the game's lists.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines, and
 * the assembler includes this file through the C preprocessor. Each line that
 * build.sh or check.py reads is a name and one hexadecimal literal (or a
 * string), a comment allowed after.
 */
#ifndef QWARK_MH_RAC3_H
#define QWARK_MH_RAC3_H

#define MH_GAME_ID          0x00000003   /* GAME_RAC3 */
#define MH_TITLE            "NPEA00387"

/*
 * Code: 16 KB of the text segment's last 64 KB page, which the executable
 * leaves empty from 0xBC1B68 to 0xBD0000. Probe A has 0xBC2000..0xBC3150 and
 * probes N3 and N3b 0xBCC000..0xBCF000 (mods the user still has); this keeps
 * clear of both, so loading one by mistake writes nothing over the renderer.
 * N3 and N3b hook the same words as this renderer, by design (they are its
 * prototype), so the install refuses while either is loaded.
 *
 * Data: two spans of the read-write segment's last page, which ends at
 * 0x1ADAB10 in the executable: the mailbox at 0x1ADB000, and the state, the
 * read-only data, the TOC and the descriptors between probe A's data
 * (0x1ADC000..0x1ADC89C) and N3's and N3b's (0x1ADE000..0x1ADF000). Nothing
 * the client ships or the user has names either.
 */
#define MH_CAVE             0x00BC4000
#define MH_CAVE_END         0x00BC8000
#define MH_BOX              0x01ADB000
#define MH_BOX_END          0x01ADB800
#define MH_DATA             0x01ADC900
#define MH_DATA_END         0x01ADE000

/*
 * The mailbox (menubox.h): sixteen rows of 0x40 bytes. The game's screens have
 * no description line, so the rows carry none.
 */
#define MH_ROWS             0x00000010
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x00000009   /* the main pause's nine-row tables, widgets 0xD000C..0xD0014 */
#define MH_CFG_ROWS         0x00000010
#define MH_MBOX_SIZE        0x00000480   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/*
 * The game's own casing: every menu of its pause is in capitals (the user's
 * call, from probe N3b: "all caps throughout"). Actions with nothing to say
 * show "..." on a CONFIG page, a choice "< VALUE >", as N3b's did, and the
 * current option of a list is marked with a star.
 */
#define MH_CASE_MAIN        0x00000001   /* MH_CASE_UPPER */
#define MH_CASE_HEAD        0x00000001
#define MH_CASE_LABEL       0x00000001
#define MH_CASE_VALUE       0x00000001
#define MH_STYLE            0x0000000B   /* MH_STYLE_DOTS | MH_STYLE_ARROWS | MH_STYLE_STAR */
#define MH_MAIN_SEP         ": "

/* Half a second at the pause's sixty frames. */
#define MH_WAIT_FRAMES      0x0000001E

/*
 * A new page, or another number of rows, starts the screen again: the main
 * pause's init lays the rows, the separators and the frame out once, and the
 * grid is built for a number of rows. A page refreshed in place only has its
 * texts and colours set again.
 */
#define MH_RESTART_MAIN     0x00000003   /* MH_SHOW_NEW | MH_SHOW_ROWS */
#define MH_RESTART_CFG      0x00000003

/* The core's state at the start of the data, then the shim's (SH_* below). */
#define MH_STATE            0x01ADC900
#define MH_SHIM             0x01ADD2B8   /* MH_STATE + MH_CORE_SIZE, which mh_rac3.c checks */
#define MH_MAGIC            0x514D4833   /* 'QMH3' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each word with the word it replaces (check.py reads both back out of the
 * executable). qwark installs them in this order, after the caves, and takes
 * them out in the reverse order: the label hook, the three row counts, the
 * Start path's three, then the HELP rows' six, so a HELP row can only say
 * RACMAN once the label hook answers and only start a screen that is already
 * the menu's.
 *
 * MH_SITE_MSG      msg_string's first word, `lis r4,0`: `b mh_msg_stub`, which
 *                  answers text id 0xF000 with "RACMAN" and 0xF001 + i with
 *                  the core's row i, and sends every other id on into
 *                  msg_string (probes N3 and N3b).
 * MH_SITE_ROWS8 .. the main pause init's three `li r6,n`, the row count after
 *                  it picks one of its tables by planet: `bl mh_rows*_stub`,
 *                  which while the shim runs that init hands over the shim's
 *                  table, its count and the band the frame's end pieces sit
 *                  on, and the game's otherwise (N3b).
 * MH_SITE_INJECT   the mode 0 update's test for Start this frame, `rlwinm
 *                  r6,r6,0,20,20` of the pad's presses: `bl mh_inject_stub`,
 *                  which makes the test true in the frame that takes an open
 *                  request, for this test only, so the game's own gate decides
 *                  whether the pause may open (no Start is written to the pad).
 * MH_SITE_COMBO    `li r5,1`, the pause reason of the Start path's
 *                  mode_requestChange(3, 1, 1, 0, 0): `bl mh_combo_stub`,
 *                  which keeps r5 and tells the launcher whether this pause is
 *                  the menu's (N3).
 * MH_SITE_LAUNCH   the pause launcher's call of the main pause's getter for
 *                  reason 1: `bl mh_launch_stub`, the menu's GuiState in place
 *                  of the main pause when the flag is up (N3).
 * MH_SITE_HELP*    in each of the main pause's three tables, the HELP row's
 *                  text id and its callback: 0xF000 ("RACMAN") and the shim's
 *                  mh_help, which starts the menu over the main pause.
 */
#define MH_SITE_MSG         0x0014BCF0   /* lis r4,0, word 0x3C800000 */
#define MH_SITE_ROWS8       0x002F4284   /* li r6,8: table 0xC527C8, band 0xC529D0 */
#define MH_SITE_ROWS9A      0x002F41FC   /* li r6,9: table 0xC52868, band 0xC529D8 */
#define MH_SITE_ROWS9B      0x002F4250   /* li r6,9: table 0xC5291C, band 0xC529E0 */
#define MH_SITE_INJECT      0x00287184   /* rlwinm r6,r6,0,20,20, word 0x54C60528 */
#define MH_SITE_COMBO       0x00287264   /* li r5,1, word 0x38A00001 */
#define MH_SITE_LAUNCH      0x002D4444   /* bl 0x2F3C18, word 0x4801F7D5 */
#define MH_SITE_HELP8_TEXT  0x00C52844   /* 0xC527C8 row 6, HELP: text 0x106 */
#define MH_SITE_HELP8_CB    0x00C5284C   /* its callback 0xBDE430 */
#define MH_SITE_HELP9A_TEXT 0x00C528F8   /* 0xC52868 row 7 */
#define MH_SITE_HELP9A_CB   0x00C52900
#define MH_SITE_HELP9B_TEXT 0x00C529AC   /* 0xC5291C row 7 */
#define MH_SITE_HELP9B_CB   0x00C529B4

#define MH_ORIG_MSG         0x3C800000
#define MH_ORIG_ROWS8       0x38C00008
#define MH_ORIG_ROWS9       0x38C00009
#define MH_ORIG_INJECT      0x54C60528
#define MH_ORIG_COMBO       0x38A00001
#define MH_ORIG_LAUNCH      0x4801F7D5
#define MH_ORIG_HELP_TEXT   0x00000106
#define MH_ORIG_HELP_CB     0x00BDE430   /* the HELP row's callback descriptor, code 0x2F3FEC */

/* The band offsets the main pause init adds to 0xC50000 after each `li r6,n`. */
#define MH_BAND_OFF8        0x000029D0
#define MH_BAND_OFF9A       0x000029D8
#define MH_BAND_OFF9B       0x000029E0

/* The text ids the msg hook answers: RACMAN, then the rows of a MAIN page. */
#define MH_MSG_BASE         0x0000F000
#define MH_MSG_HELP         0x0000F000
#define MH_MSG_ROW0         0x0000F001

/* Where the msg stub goes on to: msg_string's second instruction. */
#define GAME_MSG_EXIT       0x0014BCF4

/* --------------------------------------------------------------- game data */

#define U_PAD_PTR           0x00C1D574   /* the word holding port 0's pad (0xD992C0) */
#define U_PAD_EDGES         0x000000C4   /* pad +0xC4: this frame's presses, the menus' input */
#define U_CONFIRM_MASK      0x00CBA7F0   /* the confirm button's bit: cross, or circle in Japanese */
#define U_JAPANESE          0x00D9A8B4   /* u8: the confirm swap, and the frame's "SELECT" hint */
#define U_NO_EXIT           0x00EEED08   /* u8: a Start close does not leave the pause */
#define U_CURSOR            0x00C529F0   /* the main pause's cursor, shared by the shim's list */
#define U_ROW_HANDLE        0x000D000C   /* the main pause's first row widget */
#define U_ROW_SIZE          0x00000014
#define U_GAME_PTR          0x00CBA7E8   /* *: the game's state; +0xC8 non-zero in multiplayer */
#define U_GAME_MP           0x000000C8
#define U_PLANET            0x00C1E438   /* the current planet */
#define U_COMIC_FIRST       0x0000001E   /* planets 30..36, the vid comics: their own pause (reason 5) */
#define U_COMIC_LAST        0x00000024

/*
 * What qwark reads to know the menu is not up before it takes the words out:
 * gameMode (3 is the pause, which the menu always runs in), pendingGameMode
 * (-2, nothing queued), and the shim's own word that one of its GuiStates runs.
 */
#define U_GAME_MODE         0x00EE9334
#define U_MODE_PAUSE        0x00000003
#define U_PENDING_MODE      0x00C1E840
#define U_MODE_NONE         0xFFFFFFFE
#define U_SCREEN_UP         0x00000001

/*
 * The CONFIG page, as CHEATS builds its screen (init 0x2E2558): the submenu
 * frame on canvas layers 16 and 17 (title widget 0xD0005), the grid on layer
 * 20 from CHEATS' configuration (0xC506EC) with the shim's cell buffer, the
 * frame and cursor bar CHEATS places, and the column alignment probe N3b took
 * from the grid screens. The frame's hints are Options' and Special's,
 * "\x10 SELECT" (8169 in Japanese) and "\x12 BACK", as N3b's.
 */
#define U_LAYER_FRAME       0x00000010
#define U_LAYER_FRAME2      0x00000011
#define U_FRAME_CONTAINER   0x000D000B
#define U_TITLE_HANDLE      0x000D0005
#define U_MSG_SELECT        0x00000124   /* 292 */
#define U_MSG_SELECT_JP     0x00001FE9   /* 8169 */
#define U_MSG_BACK          0x00000CD5   /* 3285 */
#define U_GRID_LAYER        0x00000014
#define U_GRID_COLS         0x00000002
#define U_GRID_VISIBLE      0x00000007   /* the rows CHEATS' frame holds */
#define U_CELL_SIZE         0x00000044   /* a cell: char[64] and its colour index */

/*
 * The grid's colour indexes (GridRowColor): CHEATS writes 0 for a cheat that is
 * off and 3 for one that is on (0x2E2A7C); 4 is the palette's grey, and the
 * grid's Up and Down skip a row of colour 4 (0x2D6A3C), as Deadlocked's
 * validator skips a row that is not ENABLED.
 */
#define U_COLOR_OFF         0x00000000
#define U_COLOR_ON          0x00000003
#define U_COLOR_SKIP        0x00000004

/* The pad bits. */
#define PADB_L1             0x00000004
#define PADB_R1             0x00000008
#define PADB_TRIANGLE       0x00000010
#define PADB_START          0x00000800
#define PADB_UP             0x00001000
#define PADB_RIGHT          0x00002000
#define PADB_DOWN           0x00004000
#define PADB_LEFT           0x00008000

/* Sounds 0x26B804(id, 0, 0) takes: the cursor's tick, and the one the pause screens play for a change. */
#define SND_MOVE            0x00000003
#define SND_CHANGE          0x00000004

/*
 * The game's function descriptors {code, TOC 0xC0A4E0} the shim calls, each
 * read back out of the executable by check.py.
 */
#define OPD_SWAP_TO         0x00BDC668   /* SwapTo(state)                       0x2AF7BC */
#define OPD_RESET_STATE     0x00BD56C8   /* ResetState(state)                   0x0932A4 */
#define OPD_SET_TEXT        0x00BDC5A0   /* SetText(widget, s)                  0x2AE244 */
#define OPD_MAIN_INIT       0x00BDE440   /* the main pause's init(state)        0x2F4160 */
#define OPD_MAIN_UPDATE     0x00BDE448   /* its update(state)                   0x2F4830 */
#define OPD_MAIN_DEINIT     0x00BDE450   /* its deinit(state)                   0x2F4E74 */
#define OPD_MAIN_PAUSE      0x00BDE3D8   /* the main pause's getter()           0x2F3C18 */
#define OPD_PAUSE_EXIT      0x00BDDA98   /* GetPauseExit()                      0x2D291C */
#define OPD_START_CLOSE     0x00BDDA08   /* the Start-close request()           0x2D21C8 */
#define OPD_START_ACK       0x00BDDA10   /* its acknowledgement()               0x2D2204 */
#define OPD_FRAME           0x00BDDBF0   /* the submenu frame builder           0x2D4EE8 */
#define OPD_GET_GRID        0x00BDDC70   /* GetGrid(i)                          0x2D5D14 */
#define OPD_GRID_BUILD      0x00BDDCB8   /* GridBuild(grid, cfg)                0x2D6764 */
#define OPD_GRID_UPDATE     0x00BDDD30   /* GridUpdate(grid, edges)             0x2D7550 */
#define OPD_GRID_CURSOR     0x00BDDCC8   /* GridCursor(grid)                    0x2D69C8 */
#define OPD_GRID_SET_CURSOR 0x00BDDCC0   /* GridSetCursor(grid, row)            0x2D6934 */
#define OPD_GRID_CELL       0x00BDDC78   /* GridCell(grid, col, row)            0x2D5DB0 */
#define OPD_GRID_COLOR      0x00BDDCD0   /* GridRowColor(grid, row, i)          0x2D69DC */
#define OPD_GRID_ALIGN      0x00BDDD38   /* the cells' alignment(grid, col, x, y) 0x2D76E0 */
#define OPD_FRAME_RECT      0x00BDDC58   /* SetFrameRect(grid, f1..f5)          0x2D5BF4 */
#define OPD_CURSOR_BAR      0x00BDDC60   /* SetCursorBar(grid, f1..f4)          0x2D5C14 */
#define OPD_GRID_FREE       0x00BDDCA8   /* GridFree(grid)                      0x2D62D8 */
#define OPD_CLEAR_LAYER     0x00BDC6B8   /* ClearCanvasLayer(layer)             0x2AFC00 */
#define OPD_PLAY_SOUND      0x00BDB448   /* the sound(id, 0, 0)                 0x26B804 */

/*
 * Descriptors the shim hands the game rather than calls: the pause screens'
 * shared pre- and post-render, which the main pause's own (0xBDE458,
 * 0xBDE460) call, for the shim's GuiStates.
 */
#define U_DESC_PRE          0x00BDDAB0   /* 0x2D29B4 */
#define U_DESC_POST         0x00BDDAC8   /* 0x2D2A40 */
#define U_GAME_TOC          0x00C0A4E0

/* ------------------------------------------------ the shim's own state */

/* Offsets from MH_SHIM. */
#define SH_PRESSED          0x00   /* u8 the inject stub took an open request this frame */
#define SH_FORCE            0x01   /* u8 the launcher answers the menu: the Start path saw PRESSED */
#define SH_FROM_HELP        0x02   /* u8 the menu came from the HELP row: back at the root is the main pause */
#define SH_CUR              0x03   /* u8 which of the two GuiStates is the menu's */
#define SH_UP               0x04   /* u32 1 while one of them runs (qwark's screen word) */
#define SH_GAME_CURSOR      0x08   /* u32 the main pause's cursor, kept to put back */
#define SH_LIST_TABLE       0x0C   /* u32 non-zero while the main pause init runs on the shim's table */
#define SH_LIST_COUNT       0x10   /* u32 its rows */
#define SH_BAND             0x14   /* f32[2] the frame's end pieces for that many rows */
#define SH_GS               0x1C   /* GuiState[2] {state, init, deinit, update, pre, post} */
#define SH_ROWS             0x4C   /* main pause rows[MH_MAIN_ROWS], U_ROW_SIZE each */
#define SH_GRIDCFG          0x100  /* the grid's configuration, 24 words */
#define SH_CELLS            0x160  /* MH_CFG_ROWS x 2 cells of U_CELL_SIZE, and a word */
#define SH_CELLS_SIZE       0x00000888
#define SH_SIZE             0x9E8

#define MH_GS_SIZE          0x18

#endif /* QWARK_MH_RAC3_H */
