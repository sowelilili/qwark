/*
 * Up Your Arsenal menu renderer: the seven hook stubs (mh_rac3.h says where
 * each hook goes). Preprocessed, so the offsets come from the same headers the
 * C uses, and commented C-style for the same reason. The msg, combo and rows
 * stubs are probe N3b's, which ran on hardware; the inject stub stands in for
 * N3b's L1 + R1 test with qwark's open request; the launch stub calls the C,
 * which knows the GuiStates. The GuiStates' init, update and deinit and the
 * row callbacks need no stub: the game calls them through descriptors that
 * carry this module's TOC.
 */

#include "mh_rac3.h"
#include "mh_shim.h"

#define PRESSED    (MH_SHIM + SH_PRESSED)
#define REQUEST    (MH_BOX + MBOX_OFF_REQUEST)
#define OPEN_REQS  (MH_STATE + ST_OPEN_REQS)
#define LIST_TABLE (MH_SHIM + SH_LIST_TABLE)
#define LIST_COUNT (MH_SHIM + SH_LIST_COUNT)
#define BAND       (MH_SHIM + SH_BAND)
/*
 * The core's row texts in text set 0, less one: text id MH_MSG_ROW0 + i is row
 * i of the set on show, ST_FRONT's (round 3), MH_SET_SIZE further on for set 1.
 */
#define TEXT_BASE  (MH_STATE + ST_SET0 + SET_TEXT - MH_TEXT_LEN)
#define FRONT      (MH_STATE + ST_FRONT)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_MSG, 0x14BCF0: msg_string(id), a leaf, begins with `lis r4,0`, which
 * becomes `b mh_msg_stub`. In: r3 = the id. 0xF000 is MH_ENTRY_LABEL, "TRAINER", the HELP row's
 * label; 0xF001 .. 0xF001 + MH_MAIN_ROWS - 1 are the core's rows of a MAIN
 * page, which the main pause's init asks for as it lays the menu's rows out.
 * Anything else goes on into msg_string with r4 set as the replaced
 * instruction set it. Writes r3, r4, r5 and cr0, which msg_string itself
 * writes, and returns to msg_string's caller.
 */
	.globl	mh_msg_stub
mh_msg_stub:
	rlwinm	%r5,%r3,0,0,27
	cmplwi	%r5,MH_MSG_BASE
	bne	2f
	andi.	%r5,%r3,15
	bne	1f
	lis	%r3,mh_help_text@ha
	addi	%r3,%r3,mh_help_text@l
	blr
1:	cmplwi	%r5,MH_MAIN_ROWS
	bgt	2f
	mulli	%r5,%r5,MH_TEXT_LEN
	lis	%r3,FRONT@ha
	lbz	%r4,FRONT@l(%r3)
	mulli	%r4,%r4,MH_SET_SIZE
	add	%r5,%r5,%r4
	lis	%r3,TEXT_BASE@ha
	addi	%r3,%r3,TEXT_BASE@l
	add	%r3,%r3,%r5
	blr
2:	lis	%r4,0
	b	game_msg_exit

/*
 * MH_SITE_INJECT, 0x287184 in the mode 0 update: `rlwinm r6,r6,0,20,20`, the
 * test of the pad's presses (pad +0xA4) for Start, becomes `bl mh_inject_stub`.
 * The game has just passed its own checks for this frame and branches to its
 * Start path on r6 non-zero; its gate there (the frame count, the player's
 * state, gameMode 0) decides whether the pause opens. In: r6 = the presses.
 * Out: r6 the game's own test, or Start in the frame that takes an open
 * request. r0, r11, r12 and cr0 are dead here (cmpwi r6,0 follows; every path
 * on sets them before it reads them).
 *
 * Every call puts PRESSED down, so the Start path tells the launcher the pause
 * is the menu's only in the frame that took the request. An open request (1)
 * is taken - the byte goes to 0 - and, outside multiplayer and the vid comics
 * (whose pause is a screen of their own, reason 5), Start is the answer to
 * this test and PRESSED goes up; any other request has nothing to do here and
 * is dropped. No pad bit is written, so nothing else of the game sees a Start.
 */
	.globl	mh_inject_stub
mh_inject_stub:
	rlwinm	%r6,%r6,0,20,20
	lis	%r11,PRESSED@ha
	li	%r0,0
	stb	%r0,PRESSED@l(%r11)
	lis	%r12,REQUEST@ha
	lbz	%r0,REQUEST@l(%r12)
	cmpwi	%r0,0
	beqlr
	cmpwi	%r0,MBOX_REQ_OPEN
	li	%r0,0
	stb	%r0,REQUEST@l(%r12)
	bnelr
	lis	%r12,U_GAME_PTR@ha
	lwz	%r12,U_GAME_PTR@l(%r12)
	lbz	%r12,U_GAME_MP(%r12)
	cmpwi	%r12,0
	bnelr
	lis	%r12,U_PLANET@ha
	lwz	%r12,U_PLANET@l(%r12)
	addi	%r12,%r12,-U_COMIC_FIRST
	cmplwi	%r12,U_COMIC_LAST-U_COMIC_FIRST
	blelr
	li	%r6,PADB_START
	li	%r0,1
	stb	%r0,PRESSED@l(%r11)
	lis	%r11,OPEN_REQS@ha
	lwz	%r12,OPEN_REQS@l(%r11)
	addi	%r12,%r12,1
	stw	%r12,OPEN_REQS@l(%r11)
	blr

/*
 * MH_SITE_COMBO, 0x287264 in the Start path, after the game's gate: `li r5,1`
 * (the pause reason) becomes `bl mh_combo_stub`. It sets r5 = 1 and FORCE to
 * PRESSED: up when this pause opens for the menu's open request, down for a
 * Start of the player's. Next come r6, r7 and mode_requestChange(3, 1, 1, 0,
 * 0). Writes r5, r11 and r12, which that code does not read.
 */
	.globl	mh_combo_stub
mh_combo_stub:
	li	%r5,1
	lis	%r11,MH_SHIM@ha
	addi	%r11,%r11,MH_SHIM@l
	lbz	%r12,SH_PRESSED(%r11)
	stb	%r12,SH_FORCE(%r11)
	blr

/*
 * MH_SITE_LAUNCH, 0x2D4444 in the pause launcher's update, reason 1: `bl
 * 0x2F3C18` (the main pause's getter, which returns its GuiState) becomes `bl
 * mh_launch_stub`, which stands in for that call: mh_launch answers the
 * menu's GuiState or calls the getter itself, and the launcher then calls
 * SwapTo(r3) and resets itself. In: r2 the game's TOC, LR = 0x2D4448. Frame
 * 0x80: 0x00..0x6F the ABI header and the C callee parameter save area, 0x78
 * the game r2; LR into the caller slot, 0x90. The shape of Deadlocked's frame
 * stub, which ran on hardware.
 */
	.globl	mh_launch_stub
mh_launch_stub:
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r2,0x78(%r1)
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_launch
	nop
	ld	%r2,0x78(%r1)
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
	blr

/*
 * MH_SITE_ROWS8, ROWS9A, ROWS9B, the main pause init's three tables (0x2F4160):
 * each `li r6,n` (the row count) becomes `bl mh_rows*_stub`. At each, r3 holds
 * the table and r7 the 0xC50000 the band's address is made from: after it the
 * init stores r3 as the table and r6 as the count, and makes the band
 * 0xC529D0, 0xC529D8 or 0xC529E0 from r7. While the shim's main_start runs the
 * init (the table word is set): r3 = the shim's table, r6 its count and r7 its
 * band less that offset. Otherwise r6 = n and r3, r7 as they were. Writes r3,
 * r6, r7 and cr0, which the init sets again before reading (r3 and r7 only as
 * said; cr0 at 0x2F42A8). Probe N3b's, with the band a fixed word.
 */
#define ROWS_STUB(name, n, off) \
	.globl	name; \
name: \
	lis	%r6,LIST_TABLE@ha; \
	lwz	%r6,LIST_TABLE@l(%r6); \
	cmpwi	%r6,0; \
	beq	1f; \
	ori	%r3,%r6,0; \
	lis	%r7,(BAND-(off))@ha; \
	addi	%r7,%r7,(BAND-(off))@l; \
	lis	%r6,LIST_COUNT@ha; \
	lwz	%r6,LIST_COUNT@l(%r6); \
	blr; \
1:	li	%r6,n; \
	blr

	ROWS_STUB(mh_rows8_stub, 8, MH_BAND_OFF8)
	ROWS_STUB(mh_rows9a_stub, 9, MH_BAND_OFF9A)
	ROWS_STUB(mh_rows9b_stub, 9, MH_BAND_OFF9B)

	.section ".rodata","a"
	.globl	mh_help_text
mh_help_text:
	.asciz	MH_ENTRY_LABEL
