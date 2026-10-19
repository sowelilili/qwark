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
 * widgets:
 *
 *   MAIN    the main pause's own list: its init, update and deinit run on the
 *           shim's row table (rows of the main pause's shape, labels by text
 *           id through the msg_string hook), so the frame is sized to the rows
 *           and the cursor, colours, sounds and the Start close are the
 *           game's; up to nine rows, as the game's own nine-row tables (probe
 *           N3b, on hardware);
 *   CONFIG  the look of Options' AUDIO / VISUAL screen, the settings list of
 *           the game's Options: the submenu frame (the title fitted to its
 *           banner, "SELECT / BACK"), the picture panel above the list with one
 *           of that screen's own pictures in it (round 4, the user's choice:
 *           EFFECTS VOLUME's), and the grid list of raw strings in two columns
 *           with that screen's configuration and font, up to 24 rows of which
 *           eight are on screen at once and the grid scrolls the rest (a window
 *           of a longer page, which slides under the cursor), and its
 *           sliders.
 *
 * The main pause's list lays its rows and frame out only as it starts, so a
 * page that is new or has another number of rows starts the menu's screen again
 * on the other GuiState (MH_RESTART_*), as the game's own screens are started.
 *
 * Two ways in, as in Deadlocked: the main pause's HELP row, which says TRAINER
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
 * Data: three spans of the read-write segment's last page, which ends at
 * 0x1ADAB10 in the executable: the grid's cells (round 4, U_CELLS below) and
 * then the mailbox between that end and probe A's data (0x1ADC000..0x1ADC89C),
 * and the state, the read-only data, the TOC and the descriptors between
 * probe A's data and N3's and N3b's (0x1ADE000..0x1ADF000). Nothing the
 * client ships or the user has names any of them. (Round 3 had the mailbox at
 * 0x1ADB000 and the cells in the shim's state; the 24 rows of round 4 need
 * both out of the data span, which they would overrun by 2 KB.)
 */
#define MH_CAVE             0x00BC4000
#define MH_CAVE_END         0x00BC8000
#define MH_BOX              0x01ADB900
#define MH_BOX_END          0x01ADC000
#define MH_DATA             0x01ADC900
#define MH_DATA_END         0x01ADE000

/*
 * The mailbox (menubox.h): 24 rows of 0x40 bytes (round 4: no descriptions,
 * and 24 rows where round 3 had 16). The CONFIG look scrolls, so a page of up
 * to 24 rows is posted whole, the twenty weapons among them, with no More
 * row; a longer one (the 34 planets, a save category) slides 24 at a time.
 */
#define MH_ROWS             0x00000018
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x00000009   /* the main pause's nine-row tables, widgets 0xD000C..0xD0014 */
#define MH_CFG_ROWS         0x00000018
#define MH_MBOX_SIZE        0x00000680   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */

/*
 * The grid's cells (round 4): MH_CFG_ROWS rows of two cells of U_CELL_SIZE and
 * a word, the buffer GridBuild takes (it builds nothing in one that is not
 * larger than the rows' cells, and clears them itself), in a span of their
 * own past the read-write segment's end, which the install does not write.
 */
#define U_CELLS             0x01ADAC00
#define U_CELLS_END         0x01ADB900
#define U_CELLS_SIZE        0x00000CC4   /* MH_CFG_ROWS * U_GRID_COLS * U_CELL_SIZE + 4 */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/*
 * The game's own casing: every menu of its pause is in capitals, the Options
 * screens' settings lists too ("MUSIC VOLUME:", "ON"). Round 4, the user's
 * call: no colon after a label (round 3 had AUDIO / VISUAL's), and an action
 * with nothing to say blank (it was "..."). A choice shows its value alone as
 * AUDIO / VISUAL's STEREO and MONO, and the current option of a list is
 * marked with a star.
 */
#define MH_CASE_MAIN        0x00000001   /* MH_CASE_UPPER */
#define MH_CASE_HEAD        0x00000001
#define MH_CASE_LABEL       0x00000001
#define MH_CASE_VALUE       0x00000001
#define MH_STYLE            0x00000008   /* MH_STYLE_STAR */
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

/*
 * Round 3: nothing of a screen going draws once the next has started: the
 * thread runner ends the one (its deinit) before it starts the other.
 */
#define MH_HOLD_MAIN        0x00000000
#define MH_HOLD_CFG         0x00000000

/* The HELP row's label, in the main pause's capitals (round 3: TRAINER for RACMAN). */
#define MH_ENTRY_LABEL      "TRAINER"

/* The core's state at the start of the data, then the shim's (SH_* below). */
#define MH_STATE            0x01ADC900
#define MH_SHIM             0x01ADD750   /* MH_STATE + MH_CORE_SIZE, which mh_rac3.c checks */
#define MH_MAGIC            0x514D4833   /* 'QMH3' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each word with the word it replaces (check.py reads both back out of the
 * executable). qwark installs them in this order, after the caves, and takes
 * them out in the reverse order: the label hook, the three row counts, the
 * Start path's three, then the HELP rows' six, so a HELP row can only say
 * TRAINER once the label hook answers and only start a screen that is already
 * the menu's.
 *
 * MH_SITE_MSG      msg_string's first word, `lis r4,0`: `b mh_msg_stub`, which
 *                  answers text id 0xF000 with "TRAINER" and 0xF001 + i with
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
 *                  text id and its callback: 0xF000 ("TRAINER") and the shim's
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

/* The text ids the msg hook answers: TRAINER, then the rows of a MAIN page. */
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
 * The CONFIG page, as Options' AUDIO / VISUAL screen builds itself (getter
 * 0x2DB6B8, init 0x2DD218, the grid at 0x2DC6F0, the sliders at 0x2DBE70):
 * the submenu frame on canvas layers 16 and 17 with the page's title fitted
 * to its banner, the picture panel above the list on layer 17 with one of the
 * screen's pictures in it, and the grid on layer 20 with AUDIO / VISUAL's
 * configuration and the shim's cell buffer, its frame and cursor bar, its
 * cells in font 1, labels left and values right.
 * The frame's hints are Options' and Special's, "\x10 SELECT" (8169 in
 * Japanese) and "\x12 BACK", as N3b's.
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
#define U_GRID_VISIBLE      0x00000008   /* AUDIO / VISUAL's list: min(rows, 8) on screen */
#define U_CELL_SIZE         0x00000044   /* a cell: char[64] and its colour index */
#define U_FONT_MENU         0x00000001   /* the font the Options screens' cells use (0x13777C(1)) */

/*
 * AUDIO / VISUAL's numbers, each a float's bits with the word it is read from
 * (check.py compares them with the executable): its list (0x2DC6F0, from the
 * floats at 0xC4FBE0), its frame and cursor bar, its rules, the picture panel
 * of its widget table (0xC4FC60), and its sliders (0x2DD218, 0x2DBE70).
 */
#define AV_ROW_Y0           0x3EE3D70A   /* 0.445   the first row's y          0xC4FBE0 */
#define AV_ROW_PITCH        0x3D59E83E   /* 0.0532  between rows               0xC4FBE8 */
#define AV_COL0_X           0x3DEB851F   /* 0.115   the labels' x              0xC4FBEC */
#define AV_COL1_X           0x3F628F5C   /* 0.885   the values' x              0xC4FBF0 */
#define AV_COL0_W           0x3F07AE14   /* 0.53    the labels' width          0xC4FBF4 */
#define AV_COL1_W           0x3E75C290   /* 0.24    0.885 - 0.115 - 0.53, as 0x2DCA38 */
#define AV_RULE_Y           0x3CDD2F1B   /* 0.027   the rules' offset          0xC4FBE4 */
#define AV_FRAME_X          0x3F000000   /* 0.5     SetFrameRect               0x2DC6C8 */
#define AV_FRAME_W          0x3F513D53   /* 0.8173  the highlight's width: lis/ori 0x2DC740 */
#define AV_FRAME_H          0x3D3851EC   /* 0.045                              0x2DC6D0 */
#define AV_FRAME_F4         0x3D2E147B   /* 0.0425                             0x2DC6D4 */
#define AV_FRAME_F5         0x3EB33333   /* 0.35                               0x2DC6D8 */
#define AV_RULE_H           0x3BDA3C21   /* 0.00666                            0x2DC6DC */
#define AV_RULE_INSET       0x3CF5C28F   /* 0.03                               0x2DC6E0 */
#define AV_BAR_Y            0x3F21D81F   /* 0.6322  SetCursorBar's y: lis/ori       0x2DC73C */
#define AV_BAR_H            0x3EE0D563   /* 0.4391  and its height: lis/ori    0x2DC734 */
#define AV_PANEL_Y          0x3E880022   /* 0.2656  the picture panel          0xC4FCD4 */
#define AV_PANEL_W          0x3F53CCAF   /* 0.8273                             0xC4FCD8 */
#define AV_PANEL_H          0x3E83B14B   /* 0.2572                             0xC4FCDC */
#define AV_PANEL_R          0x3BA3D70A   /* 0.005   its corners                0xC4FCE8 */
#define AV_PANEL_COLOR      0x332299DE   /*                                    0xC4FCCC */
#define AV_KNOB_W           0x3C9FFD61   /* 0.01953 a slider's knob            0x2DD1EC */
#define AV_KNOB_H           0x3D276534   /* 0.040868                           0x2DD1F0 */
#define AV_ROUND_A          0x3B83126F   /* 0.004   the knob's and track's corners  0x2DD1F4 */
#define AV_ROUND_B          0x3BA3D70A   /* 0.005                              0x2DD1F8 */
#define AV_TRACK_X          0x3F43D70A   /* 0.765   its track                  0x2DD1FC */
#define AV_TRACK_W          0x3E75C290   /* 0.24                               0x2DD200 */
#define AV_TRACK_H          0x3CE56042   /* 0.028                              0x2DD204 */
#define AV_GROOVE_X         0x3F2624DD   /* 0.649   the groove inside it, left-anchored  0x2DD208 */
#define AV_GROOVE_W         0x3E6D9169   /* 0.232                              0x2DD20C */
#define AV_GROOVE_H         0x3C9374BD   /* 0.018                              0x2DD210 */
#define AV_KNOB_SPAN        0x3E6D9169   /* 0.232   the knob's travel          0x2DBE68 */
#define AV_KNOB_LEFT        0x3F272B02   /* 0.653   where it sits at the bottom  0x2DBE6C */
#define AV_KNOB_COLOR       0x706EC8FF   /* CreateRoundedRect's colours, as AUDIO / VISUAL's */
#define AV_TRACK_COLOR      0x332299DE   /* created so, then SetColor ... */
#define AV_TRACK_COLOR2     0x331465B7   /* ... this */
#define AV_GROOVE_COLOR2    0x60000000   /* and the groove this */

/*
 * The widgets the shim adds to AUDIO / VISUAL's look, all in its handle range
 * (0x1E00xx, AUDIO / VISUAL's own and no other screen's), all under the root
 * on layer 17, which the deinit clears: the panel's container and box, the
 * picture in the panel (round 4: round 3 had a description line there), the
 * sliders' container, and three widgets a slider slot (the groove, the
 * track, the knob) for each of the list's rows on screen.
 */
#define AV_ROOT             0x001E0000
#define AV_PANEL            0x001E0001
#define AV_PANEL_BOX        0x001E0002
#define AV_PIC              0x001E0003
#define AV_SLIDERS          0x001E0006
#define AV_SLOT0            0x001E0011   /* slot s: groove +3s, track +3s+1, knob +3s+2 */

/*
 * The picture (round 4), as AUDIO / VISUAL's widget table has it (0xC4FCF0:
 * type 2, a rectangle, under the panel's container, no texture of its own)
 * and as the screen shows a row's: its init (0x2DD218) takes two of the pause
 * screens' picture buffers (0x2D3A4C(0), 0x2DD320), hands them to picture
 * source 1 with the hud.wad's word and AUDIO / VISUAL's picture set in that
 * wad's table (0x2CCD48, 0x2DD370), and at its end ties the rectangle to the
 * source with picture 0, hidden, fading over ten frames (0x2DD9B0 ..
 * 0x2DD9F0); its update shows the cursor row's picture once the cursor has
 * rested thirty frames (0x2DC0B0: 0x2AE710 then SetVisibility at 0x2DC664);
 * its deinit stops the source (its vtable's second entry), gives the buffers
 * back and waits for the disc (0x2DE1D8). The shim does all of it for one
 * picture, EFFECTS VOLUME's (row 0x233's, 4), the user's choice for the
 * header. The hud.wad's table is in memory from the game's start (the HUD and
 * every pause screen read it, none loads it), and the picture is read from
 * the wad into the buffers once asked for, as AUDIO / VISUAL's are.
 */
#define AV_PIC_COLOR        0x80808080   /*                                    0xC4FCFC */
#define AV_PIC_X            0x3F004007   /* 0.501                              0xC4FD00 */
#define AV_PIC_Y            0x3E893B3A   /* 0.268                              0xC4FD04 */
#define AV_PIC_W            0x3F5075A3   /* 0.814                              0xC4FD08 */
#define AV_PIC_H            0x3E7B18DB   /* 0.245                              0xC4FD0C */
#define U_PIC_SOURCE        0x00000001   /* AUDIO / VISUAL's picture source (li r3,1 at 0x2DD344) */
#define U_PIC_HUD_WAD       0x00D60F74   /* u32: the hud.wad's word the source reads from (0x2DD36C) */
#define U_PIC_SET           0x00D63550   /* AUDIO / VISUAL's picture set in the hud.wad's table (0x2DD360) */
#define U_PIC_ROWS          0x00C4FC00   /* AUDIO / VISUAL's rows {text id, callback, picture} */
#define U_PIC_ROW           0x00000003   /* its fourth, EFFECTS VOLUME (0x233) */
#define U_PICTURE           0x00000004   /* that row's picture */
#define U_PIC_WAIT          0x0000001E   /* frames before it shows, as the cursor's rest (0x2DC0B0) */

/*
 * The main pause's frame spans x 0.258 .. 0.747 (its end pieces, 0x2F4764 from
 * the floats at 0x2F4144 and 0x2F4150) and its rules are 0.43 wide (0x2F4138):
 * a MAIN row of the menu's wider than 0.44 shrinks to fit, down to 0.6 of its
 * size, as the grid's cells do (the text widget's box and fit flag).
 */
#define U_ROW_FIT_W         0x3EE147AE   /* 0.44 */
#define U_ROW_FIT_MIN       0x3F19999A   /* 0.6 */

/* The grid's fields the sliders follow: the top row on screen, and the scroll in flight. */
#define U_GRID_TOP          0x00000004
#define U_GRID_SCROLL       0x0000001C
#define U_GRID_SLIDE        0x00000058

/*
 * The grid's colour indexes (GridRowColor, a word after each cell's text): 0
 * draws in the palette's +0x8C, the blue of every Options list's text, and 4
 * in +0x90, the grey that 1 draws in too (0x2D6DC4), but a row of colour 4 is
 * one the grid's Up and Down skip (0x2D6A3C), as Deadlocked's validator skips
 * a row that is not ENABLED. (Round 3 drew a row that was on in 3, white, as
 * CHEATS draws a cheat that is on; round 4, the user's call, has every row
 * in 0 but the grey ones, as AUDIO / VISUAL's.)
 */
#define U_COLOR_OFF         0x00000000
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
#define OPD_TITLE_FIT       0x00BDDBE8   /* a title's scale and banner(s, &sc, &f1) 0x2D4DB0 */
#define OPD_GRID_RULES      0x00BDDC50   /* the rules(grid, on, y, h, inset)    0x2D5BE0 */
#define OPD_GRID_FONT       0x00BDDD40   /* the cells' font(grid, col, font)    0x2D7784 */
#define OPD_SET_SCALE       0x00BDC610   /* SetScale(w, fx, fy)                 0x2AEEC0 */
#define OPD_SET_POS         0x00BDC4D0   /* SetPosition(w, fx, fy)              0x2ACC4C */
#define OPD_SET_VISIBLE     0x00BDC568   /* SetVisibility(w, on)                0x2ADDF8 */
#define OPD_SET_COLOR       0x00BDC5C0   /* SetColor(w, c)                      0x2AE588 */
#define OPD_SET_ANCHOR      0x00BDC550   /* SetAnchor(w, x, y)                  0x2ADAC8 */
#define OPD_SET_LAYER       0x00BDC658   /* SetCanvasLayer(layer, w)            0x2AF6C4 */
#define OPD_CONTAINER       0x00BDC810   /* CreateFrameContainer(w)             0x2B1E90 */
#define OPD_ADD_FRAME       0x00BDC6E0   /* AddFrameToContainer(c, w)           0x2AFDE4 */
#define OPD_ROUND_RECT      0x00BDC7F0   /* CreateRoundedRect(w, c, x, y, sx, sy, ra, rb) 0x2B1A88 */
#define OPD_RECT            0x00BDC7E0   /* CreateRect(w, c, x, y, sx, sy, texture) 0x2B1908 */
#define OPD_TEXT_BOX        0x00BDC718   /* a text's box(w, width, least scale) 0x2B0514 */
#define OPD_TEXT_FIT        0x00BDC728   /* its fit to the box(w, on)           0x2B06C4 */
/* Round 4, the picture (AV_PIC above). */
#define OPD_PIC_BUFFER      0x00BDDB38   /* a picture buffer(0) -> it, or 0     0x2D3A4C */
#define OPD_PIC_BUFFER_DROP 0x00BDDB40   /* (buffer): given back                0x2D3B94 */
#define OPD_PIC_SOURCE      0x00BDD790   /* picture source(i) -> it             0x2CCA44 */
#define OPD_PIC_SOURCE_SET  0x00BDD7E0   /* (source, buf, buf, wad, set) -> 1 when taken 0x2CCD48 */
#define OPD_PIC_SOURCE_STOP 0x00BDD7D8   /* (source): its vtable's second entry 0x2CCD38 */
#define OPD_PIC_TIE         0x00BDC6E8   /* (w, source): the rectangle's source 0x2AFF84 */
#define OPD_PIC_SHOW        0x00BDC5C8   /* (w, picture): which one it shows    0x2AE710 */
#define OPD_PIC_FADE        0x00BDC6F0   /* (w, on, frames)                     0x2B0040 */
#define OPD_DISC_WAIT       0x00BDA6D0   /* (1): the disc's reads done          0x234CA8 */

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
#define SH_TITLE_SCALE      0x160  /* f32 the title's scale for its banner (OPD_TITLE_FIT) */
#define SH_TITLE_BANNER     0x164  /* f32 the banner's size */
#define SH_SLOTS            0x168  /* u8 slider slots built (0 on a page without a slider) */
#define SH_SLOT_ROW         0x169  /* u8[U_GRID_VISIBLE] the row each slot's slider shows, MH_NONE */
#define SH_PIC_WAIT         0x171  /* u8 frames until the picture shows, 0 once it has (round 4) */
#define SH_PIC_ON           0x172  /* u8 the picture source is the shim's: stopped at the deinit */
#define SH_SLOT_KNOB        0x174  /* f32[U_GRID_VISIBLE] where each knob is */
#define SH_PIC_BUF          0x194  /* u32[2] the picture buffers, given back at the deinit */
#define SH_SIZE             0x1A0

#define MH_GS_SIZE          0x18

#endif /* QWARK_MH_RAC3_H */
