/*
 * Tools of Destruction's console menu renderer (NPEA00452 v01.00, game 5):
 * the game's addresses, the hook words and the shim's layout. A prototype
 * (2026-10-31), read from the executable alone and not yet run on a console;
 * docs/tod-menu.md has the reverse engineering behind every number here.
 *
 * ToD's pause menu is an Anark presentation driven by Lua, with no list of
 * rows a word could turn, so unlike the four originals this renderer draws
 * the menu itself, over the frame, with the game's own 2D calls: the HUD's
 * text (Text_Draw, the call the load screen's "arriving" text makes) and its
 * sprite (Sprite_Draw with Sprite_init's default texture, for the panel and
 * the cursor bar). It opens with the combo alone, in play or over the game's
 * own pause, freezes play by the game clock's time scale while it is up and
 * keeps the pad from the game.
 *
 * Two hook words, both `bl`s:
 *
 *   MH_SITE_FRAME  LevelTransition_runFrame's call of the frame's end (the
 *                  flip trampoline 0x26AA40), every frame of a level, in play,
 *                  paused and in a cinematic: the stub runs the menu's frame
 *                  and draws it, then goes on to the flip with the game's r2.
 *                  The load screen's arrival text is drawn just before this
 *                  same call, by the same Text_Draw.
 *   MH_SITE_PAD    Pad_Update's last call (0x63182C), every time the pads are
 *                  read, from play and from the Anark menus alike: the stub
 *                  notes pad 0's buttons for the menu and, while it is up,
 *                  blanks the four pads' blocks before the game sees them.
 *
 * Plain #defines and nothing else: build.sh reads the MH_* and OPD_* lines,
 * check.py reads all of them, and the assembler includes this file through
 * cpp. Every hexadecimal define is a bare literal so build.sh's getdef finds it.
 */
#ifndef QWARK_MH_TOD_H
#define QWARK_MH_TOD_H

#define MH_GAME_ID          0x00000005   /* GAME_TOD */
#define MH_TITLE            "NPEA00452"

/*
 * The code: the padding of the code segment's last 64 KB page (the segment
 * ends at 0x867360), past the Lua helper's 1,316 bytes at 0x867360 and clear
 * of them by a page. Nothing in the executable refers to the range
 * (check.py). The mailbox and the data: the RW segment's last page past its
 * file image (0x8C9A08, its TLS 0x8C9BF8), after the Lua helper's mailbox
 * (0x8CA000..0x8CC600). Both live only if lv2 maps the segments in whole
 * 64 KB pages, which the Lua helper relies on as well; a write there that
 * fails refuses the install, nothing else.
 */
#define MH_CAVE             0x00868000
#define MH_CAVE_END         0x0086E000
#define MH_BOX              0x008CD000
#define MH_BOX_END          0x008CD800
#define MH_DATA             0x008CE000
#define MH_DATA_END         0x008D0000

/*
 * Fourteen rows at once on either page type, which the panel draws in one
 * column; a longer page slides under the cursor (mh_shim.h, round 4).
 */
#define MH_ROWS             0x0000000E
#define MH_ROW_BYTES        0x00000040
#define MH_MAIN_ROWS        0x0000000E
#define MH_CFG_ROWS         0x0000000E
#define MH_MBOX_SIZE        0x00000400   /* MBOX_OFF_ROWS + MH_ROWS * MH_ROW_BYTES */

/* MBOX_CAP_STEPS | PAGE_KEYS | BACK | REQUEST: no planet picker. */
#define MH_CAPS             0x0000000F

/* The game's menus are in title case. */
#define MH_CASE_MAIN        0x00000002   /* MH_CASE_TITLE */
#define MH_CASE_HEAD        0x00000002
#define MH_CASE_LABEL       0x00000002
#define MH_CASE_VALUE       0x00000002
#define MH_STYLE            0x00000000
#define MH_MAIN_SEP         ": "
#define MH_ENTRY_LABEL      "Trainer"
#define MH_WAIT_FRAMES      0x0000001E

/* One drawing for both page types: every page is taken in place, nothing fades. */
#define MH_RESTART_MAIN     0x00000000
#define MH_RESTART_CFG      0x00000000
#define MH_HOLD_MAIN        0x00000000
#define MH_HOLD_CFG         0x00000000

/* The core's state at the start of the data, then the shim's. */
#define MH_STATE            0x008CE000
#define MH_SHIM             0x008CE8B0   /* MH_STATE + MH_CORE_SIZE (0x8B0 for 14 rows), which mh_tod.c checks */
#define MH_MAGIC            0x514D4835   /* 'QMH5' */

/* ToD has no Help row the renderer takes over: build.sh and check.py accept a table without one. */
#define MH_NO_HELP_ROW      0x00000001

/* ------------------------------------------------------------ the hooks */

#define MH_SITE_FRAME       0x003C5CD8   /* LevelTransition_runFrame: bl 0x26AA40, the frame's end */
#define MH_SITE_PAD         0x005D144C   /* Pad_Update: bl 0x63182C, its last call */
#define MH_ORIG_FRAME       0x4BEA4D69
#define MH_ORIG_PAD         0x480603E1

/* Where the stubs go on to: the flip trampoline, with the game's r2, and the pads' last step. */
#define GAME_FRAME_END      0x0026AA40   /* std r2,40(r1); r2 += 0xFFEC; b 0x6B9900 (the frame limiter and flip) */
#define GAME_PAD_TAIL       0x0063182C

/* ------------------------------------------------------------ the game */

/* g_gameState, the level tier's state: 4 play, 5 the pause menu (PauseMenu_update 0x2406C). */
#define TM_STATE      0x10387F4C
#define TM_STATE_PLAY      0x00000004
#define TM_STATE_PAUSE     0x00000005
/* g_gameStatePendingOp: 0 while no push, swap or pop of the state stack is queued. */
#define TM_PENDING   0x10387DBC

/* g_gameClock: f32 time scale at +0 (Lua_time_set_scale's word); 0 freezes play. */
#define TM_CLOCK      0x1074DDA8

/*
 * The pads: four blocks of 100 bytes from g_padAnalogs, Pad_Update's. In a
 * block: f32 LX LY RX RY at 0, four more floats at 0x10, twelve pressures at
 * 0x20, u32 pressed 0x50, released 0x54, held 0x58 (g_padMask), previous 0x5C.
 */
#define TM_PAD0            0x10691918
#define TM_PAD_STRIDE      0x00000064
#define TM_PADS            0x00000004
#define TM_PAD_PRESSED     0x00000050
#define TM_PAD_RELEASED    0x00000054
#define TM_PAD_HELD        0x00000058
#define TM_PAD_BLANK       0x00000058   /* the bytes blanked: the floats and the edges, up to the held mask */

/* The HUD's screen, {x0, y0, x1, y1} f32, as StageCore_Main sets it (0, 0, height * aspect, height). */
#define TM_SCREEN_RECT     0x10740BD0

/* The pad's bits (REPORT-tod-lua section 7). */
#define PADB_TRIANGLE       0x00000001
#define PADB_CIRCLE         0x00000002
#define PADB_CROSS          0x00000004
#define PADB_SQUARE         0x00000008
#define PADB_L1             0x00000040
#define PADB_R1             0x00000080
#define PADB_SELECT         0x00000100
#define PADB_START          0x00000200
#define PADB_UP             0x00001000
#define PADB_RIGHT          0x00002000
#define PADB_DOWN           0x00004000
#define PADB_LEFT           0x00008000

/*
 * The game's descriptors the shim calls, each {code, r2 0x8CB054} in the OPD
 * segment; the code address in the comment for check.py.
 *
 *   Text_Draw(str, pos, -, colour, -, font, -, -, shadow; f1 size, f2 wrap
 *       width (0 none), f3/f4 alignment (0 left/top .. 1 right/bottom),
 *       f5/f6 the shadow's offset, f7 line spacing (-1 the font's))
 *   Sprite_Init(sprite): white corners, Sprite_Draw's default texture
 *   Sprite_Draw(rect {x0, y0, x1, y1}, sprite)
 *   Hud_ResetAlpha(): the HUD's alpha stack back to one level at 1.0
 *   Font_Select(font, 0), Font_Height() -> f1, the current font's line height
 */
#define OPD_TEXT_DRAW       0x00898140   /* 0x688C64 */
#define OPD_SPRITE_INIT     0x00897EE8   /* 0x682D30 */
#define OPD_SPRITE_DRAW     0x00897F38   /* 0x683A44 */
#define OPD_HUD_RESET_ALPHA 0x00897F00   /* 0x682DBC */
#define OPD_FONT_SELECT     0x00899910   /* 0x6BCF00 */
#define OPD_FONT_HEIGHT     0x00899998   /* 0x6BE4DC */
#define TM_LUA_TOC         0x008CB054   /* the r2 of every descriptor above */

/* The font the load screen's arrival text uses. */
#define TM_FONT            0x00000001

/* Sprite_Init's sprite: 0x40 bytes; its four corner colours at 0x18, RGBA. */
#define SPR_SIZE            0x00000040
#define SPR_COLOURS         0x00000018

/* ------------------------------------------------------------ the shim's state */

#define SH_UP               0x00   /* u32 1 while the menu is up (qwark's mode and screen word) */
#define SH_ZERO             0x04   /* u32 0, always */
#define SH_CURSOR           0x08   /* u8 the cursor's row */
#define SH_FROZEN           0x09   /* u8 the menu froze play (opened from state 4) */
#define SH_REPEAT           0x0A   /* u8 frames a direction has been held */
#define SH_BLANKED          0x0B   /* u8 the pads were blanked since the menu's last frame */
#define SH_PRESSES          0x0C   /* u32 pad 0's new presses since the menu's last frame */
#define SH_HELD             0x10   /* u32 pad 0's buttons as Pad_Update read them */
#define SH_PREV             0x14   /* u32 the same at the read before */
#define SH_SCALE            0x18   /* f32 the time scale the menu found, put back as it closes */
#define SH_OPENS            0x1C   /* u32 times opened */
#define SH_FRAMES           0x20   /* u32 frames drawn */
#define SH_POS              0x28   /* f32[4] Text_Draw's position */
#define SH_RECT             0x38   /* f32[4] Sprite_Draw's rectangle */
#define SH_SPRITE           0x48   /* Sprite_Init's sprite, SPR_SIZE bytes */
#define SH_HINT             0x88   /* char[48] the controls' line, written as the menu opens */
#define SH_GLYPHS           0xB8   /* "^", "v" and ">", each with its NUL, the same */
#define SH_SIZE             0xC0

/* ------------------------------------------------------------ the look */

/* Fractions of the screen's height and width, in thousandths. */
#define LOOK_LINE           0x00000024   /* 36: a row is 3.6% of the screen's height */
#define LOOK_TEXT           0x0000001C   /* 28: the text's height, 2.8% */
#define LOOK_LEFT           0x000000C8   /* 200: the panel from 20% of the width */
#define LOOK_RIGHT          0x00000320   /* 800: to 80% */
#define LOOK_TOP            0x00000096   /* 150: from 15% of the height */
#define LOOK_PAD            0x00000014   /* 20: the text this far inside the panel */

#define COL_PANEL           0x080810C8   /* RGBA: near black, about 80% */
#define COL_CURSOR          0x2A5C9CE0
#define COL_TITLE           0xF0C850FF
#define COL_TEXT            0xF0F0F0FF
#define COL_GREY            0x808080FF
#define COL_ON              0x8CF08CFF
#define COL_SHADOW          0x000000A0
#define COL_HINT            0xB0B0B0FF

/* Frames a direction is held before it repeats, and then every this many. */
#define REPEAT_FIRST        0x00000012
#define REPEAT_EVERY        0x00000004

#endif /* QWARK_MH_TOD_H */
