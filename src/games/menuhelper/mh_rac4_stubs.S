/*
 * Deadlocked menu renderer: the three hook stubs (mh_rac4.h says where each
 * hook goes). Preprocessed, so the offsets come from the same headers the C
 * uses, and commented C-style for the same reason. Screen 36 start and stop
 * and the row callbacks need no stub: the game calls them through descriptors
 * that carry this module TOC.
 */

#include "mh_rac4.h"
#include "../../core/menubox.h"

#define FORCE36  (MH_STATE_OFF + ST_FORCE36)
#define INJECTS  (MH_STATE_OFF + ST_INJECTS)
#define OPENS36  (MH_STATE_OFF + ST_COMBO_OPENS)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_FRAME, 0x368620 in GuiSupport_UpdateMenus: the call to
 * HandleMenuInput becomes a call to mh_frame_stub, every frame a list menu is
 * fully shown. In: r3 = the player, r2 the game TOC, LR = 0x368624. mh_frame
 * answers 1 when it took this frame input for screen 36, and the stub returns;
 * on 0 it puts r3, r2 and LR back and branches on to HandleMenuInput, which
 * returns to 0x368624 as the original call would have. Built on the shape of
 * probe N0's Options stub, which ran on hardware and is gone with the row.
 *
 * Frame 0x80: 0x00..0x6F the ABI header and the C callee parameter save area,
 * 0x70 r3, 0x78 the game r2; LR into the caller slot, 0x90.
 */
	.globl	mh_frame_stub
mh_frame_stub:
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r3,0x70(%r1)
	std	%r2,0x78(%r1)
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_frame
	nop
	cmpwi	%r3,0
	ld	%r3,0x70(%r1)
	ld	%r2,0x78(%r1)
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
	beq	1f
	blr
1:	b	game_handle_menu_input

/*
 * MH_SITE_INJECT, 0x35DFBC in the GuiHud update: the nop after the call to
 * GB_GetLocalHeroFromSlot becomes a call to mh_inject_stub, a leaf. The game
 * has just checked gameMode 0 and GuiMain_TakesInput, and is about to test the
 * hero pad presses (hudBitsOn) for Start. In: r3 = the hero, which must come
 * back as it was. r0, r11, r12 and cr0 are free here: the code after reloads
 * every register it uses from r3 and the saved ones.
 *
 * Every call clears FORCE36, so the Start path answers 36 only in the frame
 * that took the request. An open request (1) is taken - the byte goes to 0 -
 * and, outside multiplayer, Start goes into the pad presses for this frame and
 * FORCE36 goes up; any other request has nothing to do here and is dropped.
 */
	.globl	mh_inject_stub
mh_inject_stub:
	lis	%r11,mh_box@ha
	addi	%r11,%r11,mh_box@l
	li	%r0,0
	stb	%r0,FORCE36(%r11)
	lbz	%r12,MBOX_OFF_REQUEST(%r11)
	cmpwi	%r12,0
	beqlr
	stb	%r0,MBOX_OFF_REQUEST(%r11)
	cmpwi	%r12,MBOX_REQ_OPEN
	bnelr
	lis	%r12,DL_GAME_TYPE@ha
	lwz	%r12,DL_GAME_TYPE@l(%r12)
	cmpwi	%r12,1
	beqlr
	lwz	%r12,DL_HERO_PAD(%r3)
	cmpwi	%r12,0
	beqlr
	lwz	%r0,DL_PAD_HUDBITS(%r12)
	ori	%r0,%r0,PADB_START
	stw	%r0,DL_PAD_HUDBITS(%r12)
	li	%r0,1
	stb	%r0,FORCE36(%r11)
	lwz	%r12,INJECTS(%r11)
	addi	%r12,%r12,1
	stw	%r12,INJECTS(%r11)
	blr

/*
 * MH_SITE_SCREEN, 0x35E074 in the GuiHud single-player and co-op Start path:
 * the li r4,1 becomes a call to mh_screen_stub, a leaf. The next instruction
 * calls GuiMenu_GoToScreen(r3 = 0, r4), so r3 must come back as it was. r4 is
 * 1, the main menu, unless mh_inject_stub pressed Start for the menu in this
 * frame (FORCE36), when it is 36 and the flag goes down. r11, r12 and cr0 are
 * dead there.
 */
	.globl	mh_screen_stub
mh_screen_stub:
	li	%r4,1
	lis	%r11,mh_box@ha
	addi	%r11,%r11,mh_box@l
	lbz	%r12,FORCE36(%r11)
	cmpwi	%r12,0
	beqlr
	li	%r12,0
	stb	%r12,FORCE36(%r11)
	li	%r4,MH_SCREEN_ID
	lwz	%r12,OPENS36(%r11)
	addi	%r12,%r12,1
	stw	%r12,OPENS36(%r11)
	blr
