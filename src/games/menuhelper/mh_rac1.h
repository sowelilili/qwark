/*
 * Ratchet & Clank's console menu renderer (NPEA00385): the game header the core
 * and the shim are both built with (mh_shim.h says what a game header defines),
 * where the renderer lives, the four words it writes and the game it calls.
 * mh_rac1.c is the shim; mh_core.c the core.
 *
 * RaC1's pause menu is a data-driven page/item framework (rewrite/
 * REPORT-rac1-ui.md): pages are 0x88-byte structs reached by pointer, with a
 * pose per pane for the fourteen UIWindowPane mobys and an item per pane, and
 * an item is {update, draw, enter, exit} descriptors and a tail per widget. The
 * shim brings pages and items of its own in its state, and the game pauses,
 * poses the panes, draws and plays its sounds for them, as probes N1, N1b and
 * N1c showed on hardware:
 *
 *   MAIN    the main pause's own look (probe N1c): one row per pane on the main
 *           page's seven panes and poses, each a one-row generic list with the
 *           main page's text size, a row too wide taking the small size and,
 *           failing that, cut short. RaC1 has no poses from the main layout to
 *           itself, so the shim moves the panes itself between MAIN pages: the
 *           page's panes fly out (the main page's poses forwards) and the next
 *           page's fly in (backwards), the way the pause opens.
 *   CONFIG  Goodies > Cheats (screen 38, 0x72F5D4), probe N1c's CONFIG look,
 *           which ran on hardware: Cheats' poses for its large list pane, its
 *           hint pane ("Toggle / Exit", Cheats' own hint item) and the two
 *           panes Cheats leaves empty, and for the title pane the pose of
 *           Goodies > In-Level Movies (0x72FA74), the sibling page that shares
 *           every other pose with Cheats and whose title pane is made for the
 *           longest of their titles. In the list pane, the one Cheats,
 *           Cinematics and In-Level Movies share (the game puts Cinematics'
 *           29 rows in it), the shim's own list draw: the geometry of Cheats'
 *           list draw (0x14A86C), which Camera Options' (0x14AD88) shares,
 *           the label at the left and the value right-aligned in the medium
 *           font, the rows sharing the pane's height, the cursor row in the
 *           game's cursor colour, a row that is not ENABLED in the generic
 *           list's grey, and a slider row drawn as Camera Options draws its 3D
 *           depth slider (frame, track, the fill sprite). The game's own list
 *           cannot scroll; the shim's shows a window of the page's rows the
 *           pane holds and scrolls it. (Camera Options' layout, round 3's
 *           first choice, has a list pane of three rows: its size is its
 *           pose's, from the level data, and cannot grow for one page alone.)
 *
 * Moving between pages: MAIN to MAIN is the shim's own fly-out and fly-in; MAIN
 * to CONFIG flies the panes out, then the game's transition to the CONFIG page
 * (its poses forwards, from Goodies' layout); CONFIG to MAIN the game's
 * transition back (those poses backwards) and the MAIN page's panes fly in;
 * CONFIG to CONFIG the game's transition between two CONFIG page structs.
 * Every new page and every change in its number of rows takes one of these
 * (MH_RESTART_*).
 *
 * Two ways in: the main pause's Help row, which says "Trainer" and opens the
 * root, and qwark's combo, through the game's own "reopen the pause on page X"
 * fields, which the play hook writes once Start's own gates would let the pause
 * open. No row is added to any of the game's lists.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines, and
 * the assembler includes this file through the C preprocessor. Each line that
 * build.sh or check.py reads is a name and one hexadecimal literal (or a
 * string), a comment allowed after.
 */
#ifndef QWARK_MH_RAC1_H
#define QWARK_MH_RAC1_H

#define MH_GAME_ID          0x00000001   /* GAME_RAC1 */
#define MH_TITLE            "NPEA00385"

/*
 * Code: RaC1's text segment ends at 0x6FD5E8 and the user's charge-boots mod
 * fills its tail, so the renderer goes into code nothing reaches, in four runs
 * (check.py proves each: from a function's start to the next's, nothing
 * outside branching into it, no lis/addi pair naming it, and the only words
 * that do its own functions' descriptors, which nothing names): the stubs and
 * the core open 0x57DD40..0x57F854 (55 functions, descriptors 0x717290..
 * 0x717448), and build.sh spreads the shim's functions over what is left of it
 * and 0x4FA800..0x4FB500, 0x5F0AA0..0x5F1788 and 0x609CF4..0x60A9B4. All four
 * are clear of probe A's 0x50D240, probes N1, N1b and N1c's 0x53C800 and
 * 0x628110, qwark's savefile helper and code switches (0x4F63C0..0x4F8C68),
 * and every mod in the libraries, charge-boots' 0x5394A0..0x539EDC among them.
 *
 * Data: two spans of the read-write segment's last page, which ends at
 * 0x1606310 in the executable: the mailbox at 0x160A000 and the state, the
 * read-only data, the TOC and the descriptors at 0x160B000, past probe A's
 * data (0x1607000..0x16078CC) and the N1 probes' (0x1608000..0x160A000).
 */
#define MH_CAVE             0x0057DD40
#define MH_CAVE_END         0x0057F854
#define MH_CAVE2            0x004FA800
#define MH_CAVE2_END        0x004FB500
#define MH_CAVE3            0x005F0AA0
#define MH_CAVE3_END        0x005F1788
#define MH_CAVE4            0x00609CF4
#define MH_CAVE4_END        0x0060A9B4
#define MH_BOX              0x0160A000
#define MH_BOX_END          0x0160A800
#define MH_DATA             0x0160B000
#define MH_DATA_END         0x0160F000

/*
 * The mailbox (menubox.h): 24 rows of 0x40 bytes (round 4: sixteen before, the
 * span holding at most 30). The shim's window scrolls a page of up to 24, the
 * nineteen planets among them, with no More row; a longer one, a save category
 * of the console's library, slides 24 at a time.
 */
#define MH_ROWS             0x00000018
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x00000007   /* the main page's seven panes */
#define MH_CFG_ROWS         0x00000018   /* the shim's window scrolls through all 24 */
#define MH_MBOX_SIZE        0x00000680   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */
#define MH_CAPS             0x0000000F   /* steps, page keys, back button, requests */

/*
 * The game's own casing: RaC1's menus are in title case ("Camera Options",
 * "Quick Select", "Quit Game"), and the user's design keeps it. An action with
 * nothing to say is blank (round 4, every game; it was "..."); no brackets
 * round a choice.
 */
#define MH_CASE_MAIN        0x00000002   /* MH_CASE_TITLE */
#define MH_CASE_HEAD        0x00000002
#define MH_CASE_LABEL       0x00000002
#define MH_CASE_VALUE       0x00000002
#define MH_STYLE            0x00000000
#define MH_MAIN_SEP         ": "

/* Half a second at the pause's sixty frames. */
#define MH_WAIT_FRAMES      0x0000001E

/*
 * Every new page, and every page with another number of rows, moves the panes:
 * the one-row panes are laid out for a page of rows, and a CONFIG page is a
 * page of the game's with its transition. A page refreshed in place only has
 * its texts and colours again, which the game asks for every frame.
 */
#define MH_RESTART_MAIN     0x00000003   /* MH_SHOW_NEW | MH_SHOW_ROWS */
#define MH_RESTART_CFG      0x00000003

/*
 * No screen going draws the core's text after the next has started: a MAIN
 * page's panes fly out before mh_core_start, showing the shim's own copies of
 * their rows, and a CONFIG page is not drawn at all during the game's
 * transition (no page is current then).
 */
#define MH_HOLD_MAIN        0x00000000
#define MH_HOLD_CFG         0x00000000
#define MH_ENTRY_LABEL      "Trainer"

/* The core's state at the start of the data, then the shim's (SH_* below). */
#define MH_STATE            0x0160B000
#define MH_SHIM             0x0160BE50   /* MH_STATE + MH_CORE_SIZE, which mh_rac1.c checks */
#define MH_MAGIC            0x514D4831   /* 'QMH1' */

/* ------------------------------------------------------- the writes it makes */

/*
 * Each word with the word it replaces (check.py reads both back out of the
 * executable). qwark installs them in this order, after the caves, and takes
 * them out in the reverse order, so the Help row can only say "Trainer" once
 * the text hook answers and only open a page the play hook keeps.
 *
 * MH_SITE_TEXT       get_ui_string's not-found path, `lis r3,0x72` (0x7B7C8):
 *                    `b mh_text_stub`, which answers the shim's text ids
 *                    0x7E00..0x7E0F from its table and every other miss with
 *                    "MESSAGE DOES NOT EXIST", as the two replaced instructions
 *                    did (probes N1 to N1c). The user's charge-boots mod hooks
 *                    the same function's entry (0x7B740); both work.
 * MH_SITE_PLAY       the nop after the play update's call (0xE14C8, the main
 *                    loop's state 0 case): `bl mh_play_stub`, the shim's
 *                    frame in play: the open request, the menu's close, the
 *                    pane flag put back (N1c).
 * MH_SITE_HELP_TEXT  the main page's Help entry's first half-words, text 0x4EE6
 *                    and kind 3 (go to a page): the shim's text id 0x7E00,
 *                    "Trainer", kind 3 kept.
 * MH_SITE_HELP_PAGE  that entry's page, Help's 0x72AF48: the shim's MAIN page.
 */
#define MH_SITE_TEXT        0x0007B7C8   /* lis r3,0x72, word 0x3C600072 */
#define MH_SITE_PLAY        0x000E14C8   /* nop after bl 0x166FB0 */
#define MH_SITE_HELP_TEXT   0x00729B68   /* main page item 4's entry: text 0x4EE6, kind 3 */
#define MH_SITE_HELP_PAGE   0x00729B6C   /* its page */

#define MH_ORIG_TEXT        0x3C600072
#define MH_ORIG_PLAY        0x60000000
#define MH_ORIG_HELP_TEXT   0x4EE60003
#define MH_ORIG_HELP_PAGE   0x0072AF48   /* Help, screen 7 */

/* The text ids the hook answers (the live read found none of the game's in 0x7E00..0x7EFF). */
#define R1_ID_BASE          0x00007E00
#define R1_ID_COUNT         0x00000010
#define R1_ID_TRAINER       0x00007E00   /* the Help row */
#define R1_ID_ROW0          0x00007E01   /* MAIN rows 0..6 */
#define R1_ID_TITLE         0x00007E08   /* a CONFIG page's title */
#define MH_HELP_WORD        0x7E000003   /* MH_SITE_HELP_TEXT's new word: id 0x7E00, kind 3 */
#define R1_MSG_MISSING      0x00720B94   /* "MESSAGE DOES NOT EXIST" */

/* --------------------------------------------------------------- game data */

#define R1_PAD_HELD         0x00964AF0   /* pad +0xB0, held */
#define R1_PAD_REPEAT       0x00964AF4   /* pad +0xB4, the menus' auto-repeat */
#define R1_PAD_EDGES        0x00964B04   /* pad +0xC4, what the menus read as pressed */
#define R1_CONFIRM_MASK     0x00902ED4   /* 0x40 cross (0x60 in Japanese) */
#define R1_STATE            0x00A10708   /* 0 play, 3 the pause menu */
#define R1_STATE_PAUSE      0x00000003
#define R1_FRAMES           0x00A1070C   /* frames in the state */
#define R1_SHOULD_LOAD      0x00A10700   /* a planet load asked for */
#define R1_PLAYER_STATE     0x0096BD64
#define R1_HAND_LOCK        0x0096AD8C   /* u8, 2 refuses Start */
#define R1_96BD91           0x0096BD91   /* u8, non-zero refuses Start */
#define R1_HP               0x0096BF88
#define R1_DEBUG_CAM        0x0095C5D4
#define R1_JAPANESE         0x0159C351   /* u8: the Japanese fonts */
#define R1_MENU_PAGE        0x00A52BB4   /* the current page, 0 in a transition */
#define R1_MENU_NEXT        0x00A52BB8   /* the page to go to */
#define R1_MENU_CLOSE       0x00A52BBC   /* the close under way, 0 for none */
#define R1_UNCLOSABLE       0x00A52CD0   /* the title's menu: Start does not close it */
#define R1_GOODIES_PANE     0x00A52C88   /* u8: the render draws pane 6 */
#define R1_PANES            0x00A52D60   /* the 14 pane mobys */
#define R1_RESUME_PAGE      0x00A52C9C   /* non-zero in play: the pause reopens on it */
#define R1_RESUME_SCREEN    0x00A52CA0
#define R1_RESUME_CD4       0x00A52CDC   /* copied into 0x969CD4 by the opener */
#define R1_969CD4           0x00969CD4
#define R1_SF_LOAD          0x01080001   /* qwark's savefile helper: a load asked for */
#define R1_SF_WAIT          0x01080008   /* and waiting for the pause */
#define R1_MAIN_PAGE        0x00729A80   /* the pause's main page, screen 2 */
#define R1_CHEATS_PAGE      0x0072F5D4   /* Goodies > Cheats, screen 38: the CONFIG look */
#define R1_CHEATS_TITLE     0x0072F65C   /* its title item (copied) */
#define R1_CHEATS_LIST      0x0072F6B4   /* its list item (the draw's geometry copied) */
#define R1_CHEATS_HINT      0x0072F718   /* its hint, "Toggle", "Exit", used as it is */
#define R1_MOVIES_PAGE      0x0072FA74   /* Goodies > In-Level Movies, screen 40: the title pane's pose */
#define R1_CAMERA_LIST      0x0072D360   /* Options > Camera Options' list item (its 3D slider copied) */
#define R1_SCREEN           0x00000032   /* 50: a screen id nothing special-cases (N1) */

/* The pause's state that says the menu is down: not the pause, no reopen queued. */
#define R1_RESUME_NONE      0x00000000
#define R1_SCREEN_UP        0x00000001

/* Moby fields and the fonts. */
#define R1_MOBY_CLASS       0x00000024   /* moby +0x24: its class; class +0x48 + 4 * pose: the pose */
#define R1_POSE_FRAMES      0x00000010   /* pose +0x10, u8: its frames */
#define R1_MOBY_SPEED       0x00000058   /* moby +0x58: the pose's speed, 1.0 forwards */
#define R1_FONT_MEDIUM      0x007368B0   /* glyphs {.., .., .., s8 width}: Camera Options' font */
#define R1_FONT_LARGE       0x00736FF0   /* list flags & 4: the main page's size */
#define R1_FONT_SMALL       0x00736C50   /* list flags & 8 */

/*
 * The layouts. The main page's poses (0x729A80); the CONFIG page's, Cheats'
 * (0x72F5D4: the list pane 0x9A on pane 1, the hint 0x9B on pane 2, 0x9C and
 * 0x9D on panes 3 and 4, which Cheats gives no item) with In-Level Movies'
 * title pose (0x72FA74's 0xA4) on pane 0; the one-row panes' flags (0x729BB0's:
 * the generic draw, large); and the CONFIG list's geometry from Cheats' list
 * draw (0x14A86C), which Camera Options' (0x14AD88) shares: the label at x 12,
 * the value right-aligned at w - 12, the first row at h / (n + 1) - 8; the
 * slider's, Camera Options': its frame from w / 2 + 87 to w - 10, its track
 * from w / 2 + 89 to w - 12, its fill the sprite 0xE99E frame 8 over 10 units
 * of range. R1_CFG_ROW_H is the least a row takes before the shim scrolls: the
 * medium font is 12 high and the game's compact lists space it 15 (0x144638).
 */
#define R1_MAIN_POSE0       0x0000000E   /* slot k: 0x0E + k */
#define R1_CONF_TITLE_POSE  0x000000A4   /* pane 0: In-Level Movies' title */
#define R1_CHEATS_POSE0     0x00000099   /* panes 1..4: 0x99 + k, Cheats' list, hint and two empty panes */
#define R1_CONF_LIST_PANE   0x00000001   /* Cheats' list pane */
#define R1_CONF_HINT_PANE   0x00000002   /* Cheats' hint pane */
#define R1_ROW_FLAGS        0x00000084
#define R1_ROW_SMALL        0x00000008
#define R1_CFG_ROW_H        0x00000014   /* 20 */
#define R1_CFG_VIS          0x00000008   /* rows on screen before the pane has been measured */
#define R1_COLOR_TEXT       0x80FFA888
#define R1_COLOR_CURSOR     0x8020FFFF
#define R1_COLOR_GREY       0x80303030   /* the generic list's grey row (0x1420E0) */
#define R1_COLOR_FRAME      0x80696969
#define R1_COLOR_TRACK      0x80383838
#define R1_SLIDER_SPRITE    0x0000E99E

/* The pad bits, qwark's layout. */
#define PADB_L1             0x00000004
#define PADB_R1             0x00000008
#define PADB_TRIANGLE       0x00000010
#define PADB_UP             0x00001000
#define PADB_RIGHT          0x00002000
#define PADB_DOWN           0x00004000
#define PADB_LEFT           0x00008000
#define PADB_CLOSE          0x00000D00   /* Start, Select, R3: the menus' close */

/* play_moby_sound(id, 17, pane): the menus' select, cursor and page sounds. */
#define SND_SELECT          0x00000000
#define SND_CURSOR          0x00000001
#define SND_PAGE            0x00000004

/*
 * The game's function descriptors {code, TOC 0x722328} the shim calls, each
 * read back out of the executable by check.py.
 */
#define OPD_LIST_UPDATE     0x007075F0   /* the generic list's update(item)          0x141408 */
#define OPD_LIST_DRAW       0x007075F8   /* its draw, which the one-row items name   0x141C90 */
#define OPD_PLAY_SOUND      0x00707BC0   /* play_moby_sound(id, 17, moby)             0x15D12C */
#define OPD_ANIM_SET        0x00706520   /* the pose a moby plays (moby, pose, frame) 0x0FDCF0 */
#define OPD_DRAW_TEXT       0x00704A68   /* text(x, y, colour, s, len, scale)         0x06FDA4 */
#define OPD_DRAW_RIGHT      0x00704A80   /* right-aligned text(x, y, ..., scale)      0x06FF88 */
#define OPD_BOX             0x00705728   /* box(x0, y0, x1, y1, colour, 0)            0x0CA72C */
#define OPD_SPRITE_INDEX    0x00705718   /* sprite frame(group, i)                    0x0CA3E4 */
#define OPD_SPRITE          0x00705950   /* sprite(t, x0, y0, x1, y1, u0, v0, u1, v1, a) 0x0D8364 */
#define OPD_BLEND           0x00711838   /* blend(...) as Camera Options' draw sets   0x502F7C */
#define OPD_GS_REG          0x00707FF0   /* set a GS register(n, value)               0x174F80 */

/*
 * Descriptors the shim hands the game rather than calls: the title item's
 * update and draw (Cheats' title, 0x72F65C, which has no enter).
 */
#define OPD_LABEL_UPDATE    0x007075A8   /* 0x13FA8C */
#define OPD_LABEL_DRAW      0x007075E8   /* 0x1402C8 */
#define R1_GAME_TOC         0x00722328

/* ------------------------------------------------ the shim's own state */

/*
 * Offsets from MH_SHIM. The first words read as a log in a memory view. The
 * pages and items the game reaches by pointer are here, set up by the play
 * hook before the menu can first open, and the Help row's page word points at
 * SH_PAGE_LIST.
 */
#define SH_READY            0x00   /* u32 MH_MAGIC once the pages and items are set up */
#define SH_UP               0x04   /* u32 1 while one of the menu's pages is up (qwark's screen word) */
#define SH_AT_PAGE          0x08   /* u32 the page the shim's items last ran on */
#define SH_PHASE            0x0C   /* u8 PH_*: the panes moving, or the game's transition asked for */
#define SH_THEN             0x0D   /* u8 T_*: what follows a fly-out */
#define SH_TIMER            0x0E   /* u8 frames left in the phase */
#define SH_PENDING          0x0F   /* u8 frames a written open has left */
#define SH_COMBO            0x10   /* u8 the root came from the combo: back there closes */
#define SH_OPENING          0x11   /* u8 the open brings all seven panes in: the spare ones go */
#define SH_GONE             0x12   /* u8 left for the main page: the Help row may bring it back */
#define SH_CLOSING          0x13   /* u8 the next update closes the pause */
#define SH_GOODIES          0x14   /* u8 the shim set the render's pane 6 flag */
#define SH_GOODIES_WAS      0x15   /* u8 the game's own */
#define SH_CONF             0x16   /* u8 which CONFIG page struct is the current */
#define SH_NROWS            0x17   /* u8 MAIN rows on their panes */
#define SH_CUR              0x18   /* u8 the CONFIG cursor */
#define SH_TOP              0x19   /* u8 the CONFIG window's first row */
#define SH_VIS              0x1A   /* u8 CONFIG rows on screen */
#define SH_STARTED          0x1B   /* u8 the core's screen is started (mh_core_start without its stop yet) */
#define SH_HITS             0x1C   /* u32 text hook answers (mh_rac1_stubs.S) */
#define SH_TIMEOUTS         0x20   /* u32 opens written and taken back unused */
#define SH_CUTS             0x24   /* u32 rows shrunk or cut short to fit */
#define SH_ROW_W            0x28   /* u16 the first one-row pane's width and height, draw units */
#define SH_ROW_H            0x2A
#define SH_CONF_W           0x2C   /* u16 the CONFIG list's */
#define SH_CONF_H           0x2E
#define SH_STRTAB           0x30   /* u32[R1_ID_COUNT]: text id 0x7E00 + i -> string, or 0 */
#define SH_PAGE_LIST        0x70   /* the MAIN page, 0x88 */
#define SH_PAGE_CONF        0xF8   /* two CONFIG pages, 0x88 each */
#define SH_ROWS             0x208  /* seven one-row generic lists, 0x50 each */
#define SH_TITLE            0x438  /* the CONFIG title, a label item, 0x58 */
#define SH_LIST             0x490  /* the CONFIG list, 0x40 */
#define SH_ENTRIES          0x4D0  /* seven pairs of generic entries (a row, the end), 24 each */
#define SH_CUTBUF           0x578  /* a label cut to fit, 0x30 */
#define SH_ENTRY_TEXT       0x5A8  /* "Trainer", 8 */
#define SH_TITLE_TEXT       0x5B0  /* the CONFIG title as shown, MH_TITLE_LEN */
#define SH_TEXT             0x5D4  /* the MAIN rows as shown, 7 x MH_TEXT_LEN */
#define SH_SIZE             0x7B0

#define R1_PAGE_SIZE        0x88
#define R1_LIST_SIZE        0x50
#define R1_ENTRY_SIZE       0x18
#define R1_CUTBUF_LEN       0x30

#endif /* QWARK_MH_RAC1_H */
