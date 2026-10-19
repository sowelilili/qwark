/*
 * The renderer's data images: the mailbox at MH_BOX as an install leaves it
 * (menubox.h: the header filled in, no event, no page yet), and at MH_STATE
 * the core's state and the shim's, all zero but the core's magic. qwark writes
 * both before anything else, and posts the first page before a hook goes in.
 * The shim sets its pages, items and text table up itself, from the play hook,
 * before the menu can first open.
 *
 * Each is a few bytes and then zeros, in sections of their own (.mh_box and
 * .mh_box_zero, .mh_state and .mh_state_zero), so the module carries the bytes
 * and only the length of the zeros (build.sh, struct mh_cave's `zeros`).
 */

#include "mh_rac1.h"
#include "mh_shim.h"

	.section ".mh_box","aw"
	.align	3
	.globl	mh_box
mh_box:
	.long	MBOX_MAGIC                  /* 0x00 magic */
	.short	MBOX_VERSION                /* 0x04 version */
	.short	MH_MBOX_SIZE                /* 0x06 size */
	.byte	MH_ROWS                     /* 0x08 row_cap */
	.byte	MH_ROW_BYTES                /* 0x09 row_size */
	.byte	MH_MAIN_ROWS                /* 0x0A max_rows */
	.byte	MH_CAPS                     /* 0x0B caps */
	.byte	MBOX_HSTATE_CLOSED          /* 0x0C hstate */
	.byte	MBOX_REQ_NONE               /* 0x0D request */
	.byte	MH_GAME_ID                  /* 0x0E renderer */
	.byte	MH_CFG_ROWS                 /* 0x0F cfg_rows */

	.section ".mh_box_zero","aw"
	.space	MH_MBOX_SIZE - 0x10         /* frames .. the rows */

	.section ".mh_state","aw"
	.align	3
	.globl	mh_state
mh_state:
	.long	MH_MAGIC                    /* ST_MAGIC */
	.long	0                           /* ST_STARTS: the zeros start 8-aligned */

	.section ".mh_state_zero","aw"
	.space	MH_CORE_SIZE - 8
	.globl	mh_shim
mh_shim:
	.space	SH_SIZE
