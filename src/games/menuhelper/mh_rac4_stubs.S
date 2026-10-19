/*
 * Deadlocked menu renderer: the four hook stubs (mh_rac4.h says where each
 * hook goes). Preprocessed, so the offsets come from the same headers the C
 * uses, and commented C-style for the same reason. Screen 36 start and stop
 * and the callbacks need no stub: the game calls them through descriptors
 * that carry this module's TOC.
 */

#include "mh_rac4.h"
#include "mh_shim.h"

#define FORCE36   (MH_SHIM + SH_FORCE36)
#define REQUEST   (MH_BOX + MBOX_OFF_REQUEST)
#define OPEN_REQS (MH_STATE + ST_OPEN_REQS)
#define OPENS     (MH_STATE + ST_OPENS)
#define PICKER    (MH_STATE + ST_PICKER)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_FRAME, 0x368620 in GuiSupport_UpdateMenus: the call to
 * HandleMenuInput becomes a call to mh_frame_stub, every frame a list menu is
 * fully shown. In: r3 = the player, r2 the game TOC, LR = 0x368624. mh_frame
 * answers 1 when it took this frame input for screen 36, and the stub returns;
 * on 0 it puts r3, r2 and LR back and branches on to HandleMenuInput, which
 * returns to 0x368624 as the original call would have. Built on the shape of
 * probe N0's Options stub, which ran on hardware.
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
	lis	%r11,FORCE36@ha
	li	%r0,0
	stb	%r0,FORCE36@l(%r11)
	lis	%r12,REQUEST@ha
	lbz	%r0,REQUEST@l(%r12)
	cmpwi	%r0,0
	beqlr
	cmpwi	%r0,MBOX_REQ_OPEN
	li	%r0,0
	stb	%r0,REQUEST@l(%r12)
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
	stb	%r0,FORCE36@l(%r11)
	lis	%r11,OPEN_REQS@ha
	lwz	%r12,OPEN_REQS@l(%r11)
	addi	%r12,%r12,1
	stw	%r12,OPEN_REQS@l(%r11)
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
	lis	%r11,FORCE36@ha
	lbz	%r12,FORCE36@l(%r11)
	cmpwi	%r12,0
	beqlr
	li	%r12,0
	stb	%r12,FORCE36@l(%r11)
	li	%r4,MH_SCREEN_ID
	lis	%r11,OPENS@ha
	lwz	%r12,OPENS@l(%r11)
	addi	%r12,%r12,1
	stw	%r12,OPENS@l(%r11)
	blr

/*
 * MH_SITE_HELP_LABEL, 0x3A649C in MenuMain_StartMain: the pause main menu's
 * Help row takes its label from `bl msg_string` with the row's text id; the
 * call becomes a call to mh_help_label, a leaf that answers the menu's label
 * instead, MH_ENTRY_LABEL, "TRAINER" in the capitals of that list. msg_string is a pure lookup
 * and the code after it reloads every register but r3 (the label), so nothing
 * else is missed. The row's callback is the game's Help callback, whose
 * `li r4,30` (MH_SITE_HELP_SCREEN) the install makes 36: it saves the main
 * menu's cursor and pushes screen 36 instead of the Help screen.
 */
	.globl	mh_help_label
mh_help_label:
	lis	%r3,mh_help_text@ha
	addi	%r3,%r3,mh_help_text@l
	blr

/*
 * The planet picker (mailbox version 3, mh_rac4.h): Worlds' X and four gates,
 * each the game's own way unless the core's ST_PICKER byte says the menu
 * opened this Worlds.
 *
 * MH_SITE_SELECT, 0x3B2844 in MenuPlanet_HandleInput: X's `bl HandleSelection`
 * becomes `bl mh_select_stub`. HandleSelection takes nothing (it asks
 * GetCurrentCanvas) and HandleInput goes to its epilogue right after it.
 * mh_worlds_select answers 1 when it dealt with the press; on 0 the stub
 * branches on to HandleSelection with the game's r2 and LR = 0x3B2848, as the
 * call would have. Probe PL4's select stub, which ran on hardware, and the
 * frame stub's frame: 0x00..0x6F the ABI header and the callee's parameter
 * save area, 0x70 the game's r2; LR into the caller's slot, 0x90.
 */
	.globl	mh_select_stub
mh_select_stub:
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r2,0x70(%r1)
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_worlds_select
	nop
	cmpwi	%r3,0
	ld	%r2,0x70(%r1)
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
	bnelr
	b	dl_handle_selection

/*
 * MH_SITE_GHOST, 0x3B177C in MenuPlanet_UpdatePlanets, reached only for Ghost
 * Station's entry: `beq 0x3B17A0` on cr0 from `cmpwi r3,0` (a bit of the byte
 * at 0xB2AA24) becomes `b mh_gate_ghost`, a leaf. With the bit set the game
 * goes on to list Ghost Station (0x3B1780); clear, it asks once more
 * (0x3B17A0), which may leave it out. While the picker is up it is listed
 * either way. r12 and cr0 are dead at both: the code there sets cr0 and reads
 * no volatile register before setting it.
 */
	.globl	mh_gate_ghost
mh_gate_ghost:
	bne	9f
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	bne	9f
	b	dl_ghost_tested
9:	b	dl_ghost_listed

/*
 * MH_SITE_COUNT, 0x3B1424 in MenuPlanet_SetupPlanetTourneyList, a leaf that
 * has written sixteen entries, the sixteenth Interior's: its `li r3,15`, the
 * count it answers (and every branch to it), becomes `b mh_gate_count`, a
 * leaf. 15, or 16 while the picker is up so Interior is listed too, then on
 * to its epilogue (0x3B1428) and the caller's LR. r12 and cr0 are dead there:
 * the epilogue reloads two saved registers and returns, and UpdatePlanets
 * compares the count before it reads cr0.
 */
	.globl	mh_gate_count
mh_gate_count:
	li	%r3,15
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	beq	9f
	li	%r3,16
9:	b	dl_count_done

/*
 * MH_SITE_BACK, 0x3B27C0 in MenuPlanet_HandleInput's Triangle (Cancel): `beq
 * 0x3B28D4` on cr0 from `cmpwi r4,-1` (mf_activeMissionIndex) becomes
 * `b mh_gate_back`, a leaf. A mission active: on to PopScreen (0x3B27C4); none
 * (the trap): the press is ignored (0x3B28D4), unless the picker is up, when
 * it pops all the same, back to the menu. r12 and cr0 are dead at both.
 */
	.globl	mh_gate_back
mh_gate_back:
	bne	9f
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	bne	9f
	b	dl_input_done
9:	b	dl_back_taken

/*
 * MH_SITE_CLOSE, 0x3B2898, the same for Start and Select (SystemCancel):
 * `beq 0x3B28D4` on cr0 from `cmpwi r3,-1` becomes `b mh_gate_close`, a leaf;
 * on to the close (0x3B289C), or the press ignored, which the picker passes.
 */
	.globl	mh_gate_close
mh_gate_close:
	bne	9f
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	bne	9f
	b	dl_input_done
9:	b	dl_close_taken

/*
 * The pickers' round 2: two more gates of the same kind.
 *
 * MH_SITE_LOCKED, 0x3B1590 in MenuPlanet_UpdatePlanets, for a planet's entry
 * (type 0): `stbx r5,r7,r24` stores the entry's bLocked, r5 = 1 where the
 * level's status byte has no 0x02 (unlocked), into the list at r7 (entry r24).
 * It becomes `b mh_gate_locked`, a leaf: the game's r5, or 0 while the picker
 * is up, so the menu's Worlds draws a planet as an unlocked one (no lock; the
 * game's NEW mark where the save has not visited it) and its panel the planet's
 * own text (MenuPlanet_UpdateForPlanet reads bLocked), every planet being one
 * the pick can load. Then on to 0x3B1594. r5 is set again at 0x3B15A0 before
 * it is read; r12 the function never uses; cr0 is set again (0x3B1610,
 * 0x3B1760) before it is read on every path from here.
 */
	.globl	mh_gate_locked
mh_gate_locked:
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	beq	9f
	li	%r5,0
9:	stbx	%r5,%r7,%r24
	b	dl_locked_done

/*
 * MH_SITE_SKILLS, 0x3B2794 in MenuPlanet_HandleInput: `beq 0x3B284C` on cr0
 * from `cmpwi r3,4` (command 4, Circle: the Skills screen) becomes
 * `b mh_gate_skills`, a leaf. Another command goes on to the next test
 * (0x3B2798, `cmpwi r3,2`, r3 untouched); command 4 goes to Skills (0x3B284C),
 * unless the picker is up, when it is ignored (0x3B28D4, the epilogue). r12
 * and cr0 are dead at the last two: Skills calls GetCurrentCanvas first, and
 * the epilogue reads neither.
 */
	.globl	mh_gate_skills
mh_gate_skills:
	bne	8f
	lis	%r12,PICKER@ha
	lbz	%r12,PICKER@l(%r12)
	cmpwi	%r12,0
	bne	9f
	b	dl_skills_taken
8:	b	dl_skills_next
9:	b	dl_input_done

	.section ".rodata","a"
	.globl	mh_help_text
mh_help_text:
	.asciz	MH_ENTRY_LABEL
