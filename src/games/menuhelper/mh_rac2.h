/*
 * Going Commando's console menu renderer (NPEA00386): the game header the core
 * and the shim are both built with (mh_shim.h says what a game header
 * defines), where the renderer lives, the six words it writes and the game it
 * calls. mh_rac2.c is the shim; mh_core.c the core.
 *
 * RaC2's pause is the PS2 game's data-driven pause manager (0x15B2F50) with
 * the HD port's C++ pages drawing it (rewrite/REPORT-rac2-uya-ui.md section
 * 3): a screen is a static struct {rect[14], parent, id, focus, items[14]},
 * an item four descriptors {update, draw, enter, exit}, and a screen opens
 * when its address goes into the manager's next screen; the manager then calls
 * the old screen's exits, plays its screen sound and, a frame later, the new
 * screen's enters. The shim brings six screens of its own, which are the
 * menu's screen, all with the shim's one item, and shows qwark's pages with
 * two of the game's own looks, as probe N2c did on hardware:
 *
 *   MAIN    the pause root's vertical list of bars (the game's object at
 *           *0x133E8A8 + 0x4A670), given the page's rows as text ids that the
 *           msg_string hook answers with the core's rows, drawn by the root's
 *           own draw: up to seven rows, as the game's own root;
 *   CONFIG  Special > Cheats as its draw (0xBC1A50) draws it, drawn by the
 *           shim every frame: the frame's three sprites, the title, the
 *           "Toggle" and "Exit" hints, and the rows in two columns, label left
 *           and value centred, up to eleven on screen, sixteen in a page with
 *           the rest scrolled to.
 *
 * A new page, or another number of rows, opens the next of the shim's screens
 * (MH_RESTART_*), the game's own way between its pause screens, which plays
 * the screen sound the game plays between them.
 *
 * Two ways in: the pause root's Help bar, which says "Trainer" and opens the
 * menu, and qwark's combo, through the Start path of ordinary play and the
 * pause manager's choice of the root.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines, and
 * the assembler includes this file through the C preprocessor. Each line that
 * build.sh or check.py reads is a name and one hexadecimal literal (or a
 * string), a comment allowed after.
 */
#ifndef QWARK_MH_RAC2_H
#define QWARK_MH_RAC2_H

#define MH_GAME_ID          0x00000002   /* GAME_RAC2 */
#define MH_TITLE            "NPEA00386"

/*
 * Code: 16 KB of the text segment's last 64 KB page, which the executable
 * leaves empty from 0x12D6668 to 0x12E0000. Probe A has 0x12D7000..0x12D815C
 * and probes N2, N2b and N2c 0x12DD000..0x12DF000 (mods the user still has);
 * this keeps clear of both, so loading one by mistake writes nothing over the
 * renderer. N2, N2b and N2c hook the same words as this renderer, by design
 * (they are its prototypes), so the install refuses while one is loaded.
 *
 * Data: two spans of the read-write segment's last page, which ends at
 * 0x1FD1090 in the executable: the mailbox between probe A's data
 * (0x1FD2000..0x1FD287C) and the N2 probes' (0x1FD4000..0x1FD5000), and the
 * state, the screens, the read-only data, the TOC and the descriptors after
 * the N2 probes'. Nothing the client ships or the user has names either.
 */
#define MH_CAVE             0x012D9000
#define MH_CAVE_END         0x012DD000
#define MH_BOX              0x01FD3000
#define MH_BOX_END          0x01FD3800
#define MH_DATA             0x01FD5000
#define MH_DATA_END         0x01FD8000

/*
 * The mailbox (menubox.h): sixteen rows of 0x40 bytes. Neither look has a line
 * for a description, so the rows carry none.
 */
#define MH_ROWS             0x00000010
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x00000007   /* the root's bars: seven text bars, as its own Weapons .. Special */
#define MH_CFG_ROWS         0x00000010
#define MH_MBOX_SIZE        0x00000480   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/*
 * The game's own casing: its pause lists and Cheats are in title case
 * ("Weapons", "Skill Points", "Mirrored Levels", "Off"). An action with nothing
 * to say shows "..." on a CONFIG page, a choice its value alone (round 3, every
 * game), and the current option of a list ends in a star. The Help bar says
 * "Trainer", in the root's title case.
 */
#define MH_CASE_MAIN        0x00000002   /* MH_CASE_TITLE */
#define MH_CASE_HEAD        0x00000002
#define MH_CASE_LABEL       0x00000002
#define MH_CASE_VALUE       0x00000002
#define MH_STYLE            0x00000009   /* MH_STYLE_DOTS | MH_STYLE_STAR */
#define MH_MAIN_SEP         ": "
#define MH_ENTRY_LABEL      "Trainer"

/* Half a second at the pause's sixty frames. */
#define MH_WAIT_FRAMES      0x0000001E

/*
 * A new page, or another number of rows, opens another of the shim's screens:
 * the bars are laid out as a screen enters, and moving between pages the way
 * the game moves between its screens gives the menu the game's screen sound.
 * A page refreshed in place has its texts, colours and enabled bars set again.
 */
#define MH_RESTART_MAIN     0x00000003   /* MH_SHOW_NEW | MH_SHOW_ROWS */
#define MH_RESTART_CFG      0x00000003

/*
 * Round 3: nothing of a screen going draws once the next has started. The
 * manager calls the exits and makes no screen current in the frame the next
 * is asked for, and the frame after, the next screen's, draws nothing of the
 * menu until its enter has run (mh_draw); its enter is mh_core_start. So the
 * other text set is free at once.
 */
#define MH_HOLD_MAIN        0x00000000
#define MH_HOLD_CFG         0x00000000

/*
 * The core's state at the start of the data, then the shim's (SH_* below),
 * then the screens (MS_* below), the first of the read-only data: the stubs'
 * .rodata, which the link puts first, at MH_SCREENS, which build.sh and
 * mh_rac2.c check.
 */
#define MH_STATE            0x01FD5000
#define MH_SHIM             0x01FD59D0   /* MH_STATE + MH_CORE_SIZE, which mh_rac2.c checks */
#define MH_SCREENS          0x01FD5A20   /* MH_SHIM + SH_SIZE, 16-aligned */
#define MH_MAGIC            0x514D4832   /* 'QMH2' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each word with the word it replaces (check.py reads both back out of the
 * executable). qwark installs them in this order, after the caves, and takes
 * them out in the reverse order: the label hook, the Start path's three, then
 * the root's Help entry's two, so the Help bar can only say "Trainer" once the
 * label hook answers, and only open a screen that is already the menu's.
 *
 * MH_SITE_MSG          msg_string's first word, `lis r4,0x134`: `b mh_msg_stub`,
 *                      which answers text id 0xF000 with "Trainer" and 0xF001
 *                      + i with the core's row i in the text set on show, and
 *                      sends every other id on into msg_string (probes N2 to
 *                      N2c). The bars ask for their labels as the shim lays
 *                      them out, after mh_core_start, and keep the pointers.
 * MH_SITE_INJECT       the mode 0 update's test for Start this frame, `rlwinm
 *                      r4,r4,0,20,20` of the pad's presses: `bl
 *                      mh_inject_stub`, which makes the test true in the frame
 *                      that takes an open request, for this test only, where
 *                      the game's root is the one with Help, so the game's own
 *                      gate decides whether the pause may open (no Start is
 *                      written to the pad).
 * MH_SITE_COMBO        `li r5,1`, the pause reason of the Start path's
 *                      mode_requestChange(3, 1, 1, 0, 0): `bl mh_combo_stub`,
 *                      which keeps r5 and tells the root stub whether this
 *                      pause is the menu's (N2c).
 * MH_SITE_ROOT         the pause manager's root screen for reason 1, `addic
 *                      r3,r3,24804`: `bl mh_root_stub`, the menu's combo
 *                      screen in place of the root when the flag is up and the
 *                      game set its bars up as the root with Help (N2c).
 * MH_SITE_HELP_SCREEN  the root's entry 4, Help: its screen, the menu's Help
 *                      screen (N2b, N2c).
 * MH_SITE_HELP_LABEL   the root's label 4, Help's text id: 0xF000, "Trainer".
 */
#define MH_SITE_MSG         0x00B28F38   /* lis r4,0x134, word 0x3C800134 */
#define MH_SITE_INJECT      0x00BF6110   /* rlwinm r4,r4,0,20,20, word 0x54840528 */
#define MH_SITE_COMBO       0x00BF6190   /* li r5,1, word 0x38A00001 */
#define MH_SITE_ROOT        0x00BB8958   /* addic r3,r3,24804, word 0x306360E4 */
#define MH_SITE_HELP_SCREEN 0x013467F8   /* the root's entry 4 {11605, kind 3, the Help screen} */
#define MH_SITE_HELP_LABEL  0x0135212C   /* the root's labels 0x135211C, the fifth: 11605 */

#define MH_ORIG_MSG         0x3C800134
#define MH_ORIG_INJECT      0x54840528
#define MH_ORIG_COMBO       0x38A00001
#define MH_ORIG_ROOT        0x306360E4
#define MH_ORIG_HELP_SCREEN 0x01348594   /* Help's screen */
#define MH_ORIG_HELP_LABEL  0x00002D55   /* 11605, "Help" */

/* The text ids the msg hook answers: "Trainer", then the rows of a MAIN page. */
#define MH_MSG_BASE         0x0000F000
#define MH_MSG_HELP         0x0000F000
#define MH_MSG_ROW0         0x0000F001

/* Where the msg stub goes on to: msg_string's second instruction. */
#define GAME_MSG_EXIT       0x00B28F3C

/* --------------------------------------------------------------- game data */

#define R2_UI_MANAGER       0x0133E8A8   /* *: the HD port's UI manager */
#define R2_BARS_OFF         0x0004A670   /* + : the root's bars R */
#define R2_TEXSET_OFF       0x00008780   /* + : the pause's texture set */
#define R2_R_POS            0x000003DC   /* R +0x3DC: the bars' {x, y, scale} table */
#define R2_R_CURSOR         0x000003F0   /* R +0x3F0: the cursor */
#define R2_POS_NORMAL       0x0135218C   /* the table of the root with Help (0xBB6970's seven bars) */
#define R2_HELP_BAR         0x00000004   /* Help's bar on that root, now "Trainer" */

/*
 * The pause manager: +0x14 the current screen, +0x18 the next, +0xE0 the one
 * before (0 when the pause has just opened).
 */
#define R2_PAUSE_MGR        0x015B2F50
#define R2_MGR_CURRENT      0x00000014
#define R2_MGR_NEXT         0x00000018
#define R2_MGR_PREVIOUS     0x000000E0

#define R2_PAD_EDGES        0x0147A444   /* pad 0x147A380 +0xC4: this frame's presses, the menus' input */
#define R2_CONFIRM_MASK     0x0141ABA8   /* the confirm button's bit: cross, or circle in Japanese */
#define R2_SCREEN_W         0x0147AF00   /* the screen's width: Cheats centres on half of it */
#define R2_SPRITE_SCALE     0x01A25698   /* f32 Cheats passes its frame's sprites */
#define R2_ROOT_SCREEN      0x013460E4   /* the root: id 2, item 0x13468A4 */
#define R2_CHEATS_SCREEN    0x0134A2D4   /* Special > Cheats: id 9, item 0x134A404 */
#define R2_SCREEN_SIZE      0x00000130
#define R2_SCREEN_RECTS     0x000000E0   /* rect[14] {x, y, w, h}, the screen's first bytes */
#define R2_ITEM_SIZE        0x00000030
#define R2_ID_TOGGLE        0x00002D4E   /* 11598, "\x10 Toggle", Cheats' hint */
#define R2_ID_EXIT          0x00002D4F   /* 11599, "\x12 Exit" */

/* The player state values the root's setup gives bars of their own (0xBB6970). */
#define R2_PS_HOVERBIKE     0x00000031
#define R2_PS_OTHER         0x0000001C

/*
 * What qwark reads to know the menu is not up before it takes the words out:
 * gameMode (3 is the pause, which the menu always runs in), pendingGameMode
 * (-2, nothing queued), and the shim's own word that one of its screens is the
 * pause manager's.
 */
#define R2_GAME_MODE        0x0156B064
#define R2_MODE_PAUSE       0x00000003
#define R2_PENDING_MODE     0x01329F90
#define R2_MODE_NONE        0xFFFFFFFE
#define R2_SCREEN_UP        0x00000001

/*
 * The CONFIG page, as Cheats' draw (0xBC1A50) lays it out on the 512-wide
 * screen: the frame's sprites 139..141 of the pause's texture set at y 208,
 * the title at 65, the hints at 321 and 346, the rows from y 112, 19 apart,
 * the label at x 43 and the value centred on x 405. Cheats has room for
 * eleven rows; a longer page shows ten, with "..." over the first or under the
 * last where more are scrolled away.
 */
#define CFG_SPRITE_FRAME    0x0000008B   /* 139 */
#define CFG_Y_FRAME         0x000000D0   /* 208 */
#define CFG_Y_TITLE         0x00000041   /* 65 */
#define CFG_Y_HINT1         0x00000141   /* 321 */
#define CFG_Y_HINT2         0x0000015A   /* 346 */
#define CFG_Y_ROW0          0x00000070   /* 112 */
#define CFG_DY              0x00000013   /* 19 */
#define CFG_X_LABEL         0x0000002B   /* 43 */
#define CFG_X_VALUE         0x00000195   /* 405 */
#define CFG_GAP             0x0000000A   /* a label ends this far before its value */
#define CFG_VIS_MAX         0x0000000B   /* rows shown when they all fit */
#define CFG_VIS             0x0000000A   /* rows shown when the page scrolls */

/* Cheats' colours (0xAABBGGRR): a row, the cursor's, a row the cursor skips; the frame's. */
#define CFG_COLOR_TEXT      0x80F0F0F0
#define CFG_COLOR_CURSOR    0x7029A1FF
#define CFG_COLOR_GREY      0x80808080
#define CFG_COLOR_BACK      0x60442D00
#define CFG_COLOR_EDGE      0x55F0C070

/* The pad bits. */
#define PADB_L1             0x00000004
#define PADB_R1             0x00000008
#define PADB_TRIANGLE       0x00000010
#define PADB_SELECT         0x00000100
#define PADB_START          0x00000800
#define PADB_UP             0x00001000
#define PADB_RIGHT          0x00002000
#define PADB_DOWN           0x00004000
#define PADB_LEFT           0x00008000

/*
 * Sounds 0xBDE0E4(id, 0, 0) takes: the cursor's tick (the bars play it
 * themselves), Cheats' change and its refusal, and the screen sound the
 * manager plays between screens and the root plays as it closes.
 */
#define SND_MOVE            0x00000003
#define SND_CHANGE          0x00000004
#define SND_NO              0x00000005
#define SND_SCREEN          0x00000012

/*
 * The game's function descriptors {code, TOC 0x131D0F0} the shim calls, each
 * read back out of the executable by check.py.
 */
#define OPD_ROOT_DRAW       0x012FD150   /* the root item's draw(item)          0xBBEE54 */
#define OPD_BARS_SETUP      0x012FCF68   /* the pause's own bars()              0xBB6970 */
#define OPD_BARS_UPDATE     0x013001C0   /* (R, edges) -> the cursor            0xCA9718 */
#define OPD_BARS_ENABLE     0x01300200   /* (R, i, on)                          0xCAA70C */
#define OPD_BARS_LABELS     0x01300208   /* (R, n, ids)                         0xCAA71C */
#define OPD_BARS_EIGHTH     0x01300218   /* (R, on): the eighth bar             0xCAA8A0 */
#define OPD_MSG_STRING      0x012FA7F8   /* msg_string(id)                      0xB28F38 */
#define OPD_DRAW_BEGIN      0x012FA130   /* (0), as Cheats' draw opens          0xB1060C */
#define OPD_DRAW_END        0x012FA138   /* ()                                  0xB10610 */
#define OPD_SPRITE          0x012FFC38   /* (texture set, id) -> sprite         0xC9FE20 */
#define OPD_SPRITE_DRAW     0x012FE6B0   /* (sprite, 0, 0, colour, x, y, f, f, f) 0xC1296C */
#define OPD_TEXT_CENTRE     0x012FA290   /* (x, y, colour, s, -1)               0xB14A44 */
#define OPD_TEXT_LEFT       0x012FA078   /* (x, y, colour, s, -1)               0xB0E758 */
#define OPD_TEXT_MID        0x012FA088   /* (x, y, colour, s, -1), centred on x 0xB0E960 */
#define OPD_TEXT_WIDTH      0x012FA080   /* (s, -1) -> width                    0xB0E880 */
#define OPD_PLAY_SOUND      0x012FD9A8   /* (id, 0, 0)                          0xBDE0E4 */
/*
 * The six questions the root's setup (0xBB6970) asks before it gives the bars
 * the seven of the ordinary root, Help among them, rather than those of a
 * hoverbike race, a ship level or an orbit; the combo opens the menu only
 * where the answers give the ordinary root.
 */
#define OPD_LV_RACE         0x012E46C8   /* a hoverbike race                    0x05BAC4 */
#define OPD_LV_SHIP         0x012E4698   /* a ship level ...                    0x05B95C */
#define OPD_LV_SHIP_ON      0x012E46A0   /* ... and in the ship                 0x05B998 */
#define OPD_LV_ORBIT        0x012E46B8   /* an orbit                            0x05BA4C */
#define OPD_LV_OTHER        0x012E46C0   /* a level of another kind             0x05BA88 */
#define OPD_LV_PSTATE       0x012E46D0   /* the player's state                  0x05BB00 */

/* ------------------------------------------------ the shim's own state */

/* Offsets from MH_SHIM. */
#define SH_PRESSED          0x00   /* u8 the inject took an open request this frame */
#define SH_FORCE            0x01   /* u8 the root stub answers the menu: the Start path saw PRESSED */
#define SH_FROM_HELP        0x02   /* u8 the menu came from the Help bar: back at the root is the root */
#define SH_SCREEN           0x03   /* u8 which of the six screens is the menu's now */
#define SH_UP               0x04   /* u32 1 while one of them is the manager's (qwark's screen word) */
#define SH_TYPE             0x08   /* u8 the look the screen has: MBOX_PTYPE_* */
#define SH_READY            0x09   /* u8 the screen has entered: its draw may draw */
#define SH_RET              0x0A   /* u8 the update's answer: 1 closes the pause */
#define SH_CCUR             0x0B   /* u8 the CONFIG page's cursor */
#define SH_CTOP             0x0C   /* u8 its first row on screen */
#define SH_PRESS_ROW        0x0D   /* u8 the row the last cross was on */
#define SH_CUTS             0x10   /* u32 labels of the CONFIG page shown cut short to end before their values */
#define SH_SIZE             0x50

/*
 * The screens, at MH_SCREENS (the stubs' .rodata, in the data image): the Help
 * bar's (MAIN, the root's rects, its parent the root), the combo's (MAIN), two
 * more of each look to move between, then the item every one of them has, the
 * MAIN rows' text ids, "Trainer" and "...".
 */
#define MS_HELP             0x00000000   /* screen 0 */
#define MS_COMBO            0x00000001   /* screen 1 */
#define MS_MAIN_A           0x00000002
#define MS_MAIN_B           0x00000003
#define MS_CFG_A            0x00000004
#define MS_CFG_B            0x00000005
#define MS_NSCREENS         0x00000006
#define MS_ITEM             0x00000720   /* 6 * R2_SCREEN_SIZE */
#define MS_IDS              0x00000750   /* u32[MH_MAIN_ROWS], 0xF001 .. */
#define MS_TRAINER          0x00000770
#define MS_MORE             0x00000778   /* "...", where more rows are scrolled away */
#define MS_SIZE             0x0000077C

#endif /* QWARK_MH_RAC2_H */
