/*
 * The start of the renderer data image, at MH_DATA: the mailbox as an install
 * leaves it (menubox.h: the header filled in, no event, no page yet) and the
 * renderer state, all zero but its magic. qwark writes this before anything
 * else, and posts the first page into it before a hook goes in.
 */

#include "mh_rac4.h"
#include "../../core/menubox.h"

	.section ".mh_state","aw"
	.align	3
	.globl	mh_box
mh_box:
	.long	MBOX_MAGIC                  /* 0x00 magic */
	.short	MBOX_VERSION                /* 0x04 version */
	.short	MH_MBOX_SIZE                /* 0x06 size */
	.byte	MH_ROWS                     /* 0x08 row_cap */
	.byte	MBOX_ROW_SIZE               /* 0x09 row_size */
	.byte	MH_ROWS                     /* 0x0A max_rows */
	.byte	MH_CAPS                     /* 0x0B caps */
	.byte	MBOX_HSTATE_CLOSED          /* 0x0C hstate */
	.byte	MBOX_REQ_NONE               /* 0x0D request */
	.byte	MH_GAME_ID                  /* 0x0E renderer */
	.byte	0                           /* 0x0F */
	.space	MH_STATE_OFF - 0x10         /* frames .. the rows, and padding to the state */

	.globl	mh_state
mh_state:
	.long	MH_MAGIC                    /* ST_MAGIC */
	.space	ST_SIZE - 4
