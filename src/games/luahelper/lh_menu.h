/*
 * Tools of Destruction's console menu (revision 1.29, pending): the block of
 * game memory the Lua helper's pause-menu half keeps beside the menu's mailbox
 * (docs/tod-menu.md).
 *
 * The menu's mailbox is the console menu's own (src/core/menubox.h, version
 * 3), the same block every game's renderer speaks: qwark posts pages into it
 * and reads events back, and src/core/menu.c cannot tell ToD from the other
 * games there. What is ToD's alone is who reads it: no renderer of ui.pak, but
 * the Lua helper (lh_menu.c), from the pause menu's own frame, on behalf of a
 * few hundred lines of Lua in the pause menu's VM (lh_glue.lua) that show the
 * page with the game's Cheats screen.
 *
 * This header is plain #defines, shared by the helper (ppu-lv2-gcc, no libc),
 * the host tests and qwark (src/games/tod_game.c, through tod.h): the block's
 * place, its fields and what they say. Every field is big-endian and has one
 * writer, named beside it.
 */
#ifndef QWARK_LH_MENU_H
#define QWARK_LH_MENU_H

/*
 * The mailbox: the RW page past the Lua helper's mailbox (0x8CA000..0x8CC600),
 * unreferenced by the executable (check.py). Thirty-two rows, MENU_VIEW_MAX:
 * the Cheats screen's list scrolls on its own, so a page of up to 32 rows is
 * posted whole, and only a longer one (a save category of the console's
 * library) comes a window at a time.
 */
#define LM_BOX          0x008CD000
#define LM_ROWS         32
#define LM_BOX_SIZE     0x00000880   /* MBOX_OFF_ROWS + LM_ROWS * MBOX_ROW_SIZE */

/*
 * What the mailbox's header says of this "renderer" (menu.c writes it at the
 * install, as a renderer's data image does elsewhere): 32 rows a page of
 * either type, Left and Right, its own back control (the game's Triangle), and
 * open and close on request. No PAGE_KEYS (L1 and R1 are the game's list's)
 * and no picker.
 */
#define LM_CAPS         0x0D         /* MBOX_CAP_STEPS | MBOX_CAP_BACK | MBOX_CAP_REQUEST */

/* ------------------------------------------------------------ the state */

#define LM_STATE        0x008CE000

#define LM_HELP         0x00   /* u8  1: "TRAINER" in place of the pause menu's Help row      qwark  */
#define LM_PENDING      0x01   /* u8  gameplay frames left for a pause the combo opened to
                                *     come up and open the menu, 0 for none                   helper */
#define LM_HAVE         0x02   /* u8  1 once a page has been taken                            helper */
#define LM_TRIED        0x03   /* u8  1 once LM_L's lua_State has qw() and asked for the glue    helper */
#define LM_L            0x04   /* u32 the pause menu's lua_State the glue went into            helper */
#define LM_GLUE         0x08   /* u32 LM_GLUE_*, for qwark's log                                helper */
#define LM_LOADS        0x0C   /* u32 +1 at every new pause menu VM, which wants the glue        helper */
#define LM_SEQ          0x10   /* u32 the mailbox's page_seq when the page was taken            helper */
#define LM_KEY          0x14   /* u32 that page's key                                           helper */
#define LM_GEN          0x18   /* u32 +1 at every page taken whose key is new                   helper */
#define LM_VGEN         0x1C   /* u32 +1 at every page taken                                    helper */
#define LM_MS           0x20   /* u32 the menu system LM_L belongs to                           helper */
/*
 * Revision 1.29, the crash fix (docs/tod-menu.md 9): the helper enters the
 * pause menu's VM only on a frame lm_ready() passes, with the presentation
 * pushed. These say how that went, for qwark's log.
 */
#define LM_WHY          0x24   /* u32 LM_WHY_* of the last pause menu frame kept out, 0 after one let in  helper */
#define LM_WAITS        0x28   /* u32 pause menu frames in a row kept out (0 at one let in, and in play)  helper */
#define LM_NOTE         0x2C   /* u32 the glue's LM_NOTE_* for this VM, 0 until it says              glue   */
/*
 * Revision 1.29, the second crash (docs/tod-menu.md 10): the Anark VMs' Lua
 * pool, which faults when it runs out (lh_tod.h, LH_ANARK_POOL), counted
 * before the helper lets the menu's Lua run (lh_menu.c, lm_room).
 */
#define LM_ROOM         0x30   /* u32 the pool at its last count: (blocks the largest class has left,
                                *     to 0xFFFF) << 16 | (blocks a 20-byte block can still come from,
                                *     counted as far as was needed, to 0xFFFF)                   helper */
#define LM_SHORT        0x34   /* u32 +1 at every full collection that left the pool short     helper */
#define LM_HOLD         0x38   /* u32 pause menu frames before the next collection may be made helper */
/*
 * The page as taken: the mailbox's bytes from MBOX_OFF_PAGE_KEY to the last
 * row, at the same offsets less MBOX_OFF_PAGE_KEY. The glue reads rows only
 * from here, never from the mailbox qwark is writing.
 */
#define LM_PAGE         0x40
#define LM_STATE_SIZE   0x00000890   /* LM_PAGE + LM_BOX_SIZE - MBOX_OFF_PAGE_KEY */

/* LM_GLUE */
#define LM_GLUE_NONE    0
#define LM_GLUE_LOADED  1      /* the glue ran, qwark_frame is called every frame              */
#define LM_GLUE_WANTED  2      /* a new pause menu VM: qwark sends the glue (target MENU)      */
#define LM_GLUE_STOPPED 3      /* qwark_frame raised an error and is no longer called; the
                                * message is the Lua mailbox's err, "qwark-menu"              */

/*
 * LM_NOTE: what the glue found in this pause menu (qw(LM_OP_NOTE, n)), the
 * last thing it said. Revision 1.29, the third console test (docs/tod-menu.md
 * 11): the Options list and the Cheats screen are looked for apart, the
 * Cheats screen at the latest when the Trainer goes to its slide; NONE and
 * NOCH are failures, after which qwark sends lh_diag.lua's look (once).
 */
#define LM_NOTE_IN      1      /* the Options list and the Cheats screen: TRAINER is in                    */
#define LM_NOTE_NONE    2      /* no Options list in its first tries: Help stays the game's (it looks on)  */
#define LM_NOTE_NOCH    3      /* the Trainer waited on the Cheats screen's slide and none came: back out   */
#define LM_NOTE_IN_LIST 4      /* the Options list: TRAINER is in; the Cheats screen not met yet           */
#define LM_NOTE_LATE    5      /* the Cheats screen met only once the Trainer went to its slide            */

/*
 * LM_WHY: what kept a pause menu frame out of the menu's VM (lm_ready), any
 * of them. Nothing the helper or the glue does in that VM touches an Anark
 * object outside these all being clear.
 */
#define LM_WHY_SYSTEM   0x001  /* g_pauseMenuSystem is not this menu system, or the pause is not up (2, 3) */
#define LM_WHY_PARTS    0x002  /* no lua_State, runtime, presentation or Lua bridge yet                  */
#define LM_WHY_FIRST    0x004  /* the presentation's first update has not run (firstUpdate)             */
#define LM_WHY_STOPPED  0x008  /* the menu system is stopping                                            */
#define LM_WHY_YOUNG    0x010  /* fewer than LM_READY_FRAMES updates of the menu system                  */
#define LM_WHY_PUSHED   0x020  /* a presentation is pushed already, or the bridge holds one              */
#define LM_WHY_PUSH     0x040  /* pushing the presentation did not make it the bridge's                  */
#define LM_WHY_MEMORY   0x080  /* the Anark Lua pool is short, even after a full collection (LM_ROOM)    */

/* Updates of a menu system before the helper enters its VM. */
#define LM_READY_FRAMES 2

/*
 * Revision 1.29, the second crash: the pool's blocks the helper wants left
 * (lm_room), counted for a block of LM_ROOM_BLOCK bytes (an upvalue, a small
 * closure, a short string: the size class most of the menu's Lua objects
 * fall in, and every larger class it may take one from) and in the largest
 * class alone (a table's nodes, a long string, a parser's arrays).
 *
 *   a frame of the glue      LM_ROOM_SMALL and LM_ROOM_BIG, or a full
 *                            collection first, as the game's own update makes
 *   a request run in the VM  LM_ROOM_LOAD_*, the glue's load above all (the
 *     (the glue)             parser's garbage), and a collection after it
 *   any collection at all    LM_ROOM_FLOOR_* (a collection allocates too)
 *
 * A collection that leaves the pool short holds the next one off for
 * LM_ROOM_HOLD frames, and takes the glue out of the VM (qwark_quit) if it
 * is in: what it keeps there is the game's room, which it needs more.
 */
#define LM_ROOM_BLOCK      20
#define LM_ROOM_SMALL      1024
#define LM_ROOM_BIG        16
#define LM_ROOM_LOAD_SMALL 3072
#define LM_ROOM_LOAD_BIG   48
#define LM_ROOM_FLOOR_SMALL 128
#define LM_ROOM_FLOOR_BIG  4
#define LM_ROOM_HOLD       60

/* How many gameplay frames a pause the combo asked for has to come up in. */
#define LM_PENDING_FRAMES 30

/* qw()'s operations, as the glue calls them (lh_glue.lua names the same). */
#define LM_OP_POLL      0      /* -> on, help, gen, vgen, nrows, cursor, ptype, pflags          */
#define LM_OP_ROW       1      /* (i) -> label, value, kind, flags of row i, 1-based; nil past  */
#define LM_OP_TEXT      2      /* -> title, hint                                                */
#define LM_OP_EVENT     3      /* (kind, i) -> 1 posted, 0 one still waits for qwark             */
#define LM_OP_STATE     4      /* (h) the mailbox's hstate: 1 open (frames from 0), 0 closed    */
#define LM_OP_TAKE      5      /* -> the request taken: 0 none, 1 open, 2 close                 */
#define LM_OP_NOTE      6      /* (n) LM_NOTE = n, for qwark's log                             */

#endif /* QWARK_LH_MENU_H */
