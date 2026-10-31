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

/* How many gameplay frames a pause the combo asked for has to come up in. */
#define LM_PENDING_FRAMES 30

/* qw()'s operations, as the glue calls them (lh_glue.lua names the same). */
#define LM_OP_POLL      0      /* -> on, help, gen, vgen, nrows, cursor, ptype, pflags          */
#define LM_OP_ROW       1      /* (i) -> label, value, kind, flags of row i, 1-based; nil past  */
#define LM_OP_TEXT      2      /* -> title, hint                                                */
#define LM_OP_EVENT     3      /* (kind, i) -> 1 posted, 0 one still waits for qwark             */
#define LM_OP_STATE     4      /* (h) the mailbox's hstate: 1 open (frames from 0), 0 closed    */
#define LM_OP_TAKE      5      /* -> the request taken: 0 none, 1 open, 2 close                 */

#endif /* QWARK_LH_MENU_H */
