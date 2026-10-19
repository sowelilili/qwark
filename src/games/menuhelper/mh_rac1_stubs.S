/*
 * Ratchet & Clank menu renderer: the two hook stubs (mh_rac1.h says where each
 * hook goes). Preprocessed, so the offsets come from the same headers the C
 * uses, and commented C-style for the same reason. Both are probe N1c's, which
 * ran on hardware, on the renderer's own table and state. The pages' items
 * need no stub: the game calls them through descriptors that carry this
 * module's TOC.
 */

#include "mh_rac1.h"
#include "mh_shim.h"

#define STRTAB     (MH_SHIM + SH_STRTAB)
#define HITS       (MH_SHIM + SH_HITS)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_TEXT, 0x7B7C8 in get_ui_string (0x7B740, a leaf): the not-found
 * path's `lis r3,0x72` becomes `b mh_text_stub`, so this returns straight to
 * get_ui_string's caller. Every miss of the table scan lands here with the id
 * in r4. Ids 0x7E00..0x7E0F with an entry in the shim's table get that string
 * (and a hit counted); everything else gets 0x720B94, "MESSAGE DOES NOT
 * EXIST", as the two replaced instructions gave (they set XER's carry too,
 * which no caller of a function can be reading). It writes r3, r5..r7 and
 * cr0, all of which get_ui_string itself writes (r3..r9, cr0, cr1).
 */
	.globl	mh_text_stub
mh_text_stub:
	addi	%r5,%r4,-R1_ID_BASE
	cmplwi	%r5,R1_ID_COUNT
	bge	1f
	lis	%r6,STRTAB@ha
	addi	%r6,%r6,STRTAB@l
	rlwinm	%r7,%r5,2,0,29
	add	%r6,%r6,%r7
	lwz	%r3,0(%r6)
	cmpwi	%r3,0
	beq	1f
	lis	%r6,HITS@ha
	lwz	%r7,HITS@l(%r6)
	addi	%r7,%r7,1
	stw	%r7,HITS@l(%r6)
	blr
1:	lis	%r3,R1_MSG_MISSING@ha
	addi	%r3,%r3,R1_MSG_MISSING@l
	blr

/*
 * MH_SITE_PLAY, 0xE14C8 in the main loop's state 0 case: the nop after `bl
 * 0x166FB0`, the play update, becomes `bl mh_play_stub`. Probe A's frame:
 * every volatile register, CR, XER and CTR go back as they were, so nothing
 * after the site needs studying (the next instruction is li r3,1 and a call).
 * r2 is the renderer's TOC while the C runs; the game's comes back.
 *
 * Frame 0x160: 0x00..0x6F the ABI header and the callee's parameter save area,
 * 0x70 r0, 0x78 r2, 0x80..0xC8 r3..r12, 0xD0 CR, 0xD8 XER, 0xE0 CTR,
 * 0xE8..0x150 f0..f13; LR into the caller's slot, 0x170.
 */
	.globl	mh_play_stub
mh_play_stub:
	stdu	%r1,-0x160(%r1)
	std	%r0,0x70(%r1)
	mflr	%r0
	std	%r0,0x170(%r1)
	std	%r2,0x78(%r1)
	std	%r3,0x80(%r1)
	std	%r4,0x88(%r1)
	std	%r5,0x90(%r1)
	std	%r6,0x98(%r1)
	std	%r7,0xA0(%r1)
	std	%r8,0xA8(%r1)
	std	%r9,0xB0(%r1)
	std	%r10,0xB8(%r1)
	std	%r11,0xC0(%r1)
	std	%r12,0xC8(%r1)
	mfcr	%r0
	std	%r0,0xD0(%r1)
	mfxer	%r0
	std	%r0,0xD8(%r1)
	mfctr	%r0
	std	%r0,0xE0(%r1)
	stfd	%f0,0xE8(%r1)
	stfd	%f1,0xF0(%r1)
	stfd	%f2,0xF8(%r1)
	stfd	%f3,0x100(%r1)
	stfd	%f4,0x108(%r1)
	stfd	%f5,0x110(%r1)
	stfd	%f6,0x118(%r1)
	stfd	%f7,0x120(%r1)
	stfd	%f8,0x128(%r1)
	stfd	%f9,0x130(%r1)
	stfd	%f10,0x138(%r1)
	stfd	%f11,0x140(%r1)
	stfd	%f12,0x148(%r1)
	stfd	%f13,0x150(%r1)

	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_play
	nop

	lfd	%f0,0xE8(%r1)
	lfd	%f1,0xF0(%r1)
	lfd	%f2,0xF8(%r1)
	lfd	%f3,0x100(%r1)
	lfd	%f4,0x108(%r1)
	lfd	%f5,0x110(%r1)
	lfd	%f6,0x118(%r1)
	lfd	%f7,0x120(%r1)
	lfd	%f8,0x128(%r1)
	lfd	%f9,0x130(%r1)
	lfd	%f10,0x138(%r1)
	lfd	%f11,0x140(%r1)
	lfd	%f12,0x148(%r1)
	lfd	%f13,0x150(%r1)
	ld	%r0,0xE0(%r1)
	mtctr	%r0
	ld	%r0,0xD8(%r1)
	mtxer	%r0
	ld	%r0,0xD0(%r1)
	mtcrf	0xFF,%r0
	ld	%r2,0x78(%r1)
	ld	%r3,0x80(%r1)
	ld	%r4,0x88(%r1)
	ld	%r5,0x90(%r1)
	ld	%r6,0x98(%r1)
	ld	%r7,0xA0(%r1)
	ld	%r8,0xA8(%r1)
	ld	%r9,0xB0(%r1)
	ld	%r10,0xB8(%r1)
	ld	%r11,0xC0(%r1)
	ld	%r12,0xC8(%r1)
	ld	%r0,0x170(%r1)
	mtlr	%r0
	ld	%r0,0x70(%r1)
	addi	%r1,%r1,0x160
	blr
