/*
 * Tools of Destruction's menu renderer: its two hook stubs (mh_tod.h says
 * where each goes). Preprocessed, so the offsets come from the same headers
 * the C uses, and commented C-style for the same reason.
 *
 * Both replace a `bl` of the game's, so everything volatile is the stub's to
 * write, as it is any callee's, and both end in the call they replaced, made
 * with the game's own r2 and the game's return address.
 */

#include "mh_tod.h"
#include "mh_shim.h"

#define UP       (MH_SHIM + SH_UP)
#define REQUEST  (MH_BOX + MBOX_OFF_REQUEST)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_FRAME, 0x3C5CD8 in LevelTransition_runFrame: `bl 0x26AA40`, the
 * frame's end (the flip trampoline, which stores the caller's r2 at 40(r1),
 * moves r2 to the top range's and goes to the frame limiter and flip,
 * 0x6B9900), becomes `bl mh_frame_stub`. In: r2 the middle range's TOC
 * (0x8BB068), no argument. The frame has been run and drawn by then.
 *
 * With the menu down and no request (the whole of almost every frame) it goes
 * straight on, three loads and two compares. Otherwise mh_frame runs with the
 * renderer's TOC, in a frame of 0x80 that keeps the game's r2, LR in the
 * caller's slot (0x90), and the stub then pops its frame and branches to the
 * trampoline with the game's r2 and LR, as the replaced `bl` would have: the
 * flip returns to runFrame's next word, which reloads r2 from 40(r1) as
 * before. runFrame keeps cr4 across the call; the C keeps it as every callee
 * must.
 */
	.globl	mh_frame_stub
mh_frame_stub:
	lis	%r11,UP@ha
	lwz	%r0,UP@l(%r11)
	lis	%r12,REQUEST@ha
	lbz	%r12,REQUEST@l(%r12)
	or.	%r0,%r0,%r12
	beq	1f
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r2,0x78(%r1)
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_frame
	nop
	ld	%r2,0x78(%r1)
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
1:	b	game_frame_end

/*
 * MH_SITE_PAD, 0x5D144C in Pad_Update: `bl 0x63182C`, its last call, with a
 * nop after it (the same TOC range), becomes `bl mh_pad_stub`. In: r2 the top
 * range's TOC (0x8CB054); 0x63182C takes no argument (its first word moves LR
 * into r3). The four pads' blocks are filled by then.
 *
 * mh_pad runs first, with the renderer's TOC, in a frame of 0x80 that keeps
 * the game's r2, LR in the caller's slot: it notes pad 0's buttons and, while
 * the menu is up, blanks the blocks. Then 0x63182C runs with the game's r2, a
 * plain `bl` from here, and the stub returns to Pad_Update's epilogue.
 */
	.globl	mh_pad_stub
mh_pad_stub:
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r2,0x78(%r1)
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_pad
	nop
	ld	%r2,0x78(%r1)
	bl	game_pad_tail
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
	blr
