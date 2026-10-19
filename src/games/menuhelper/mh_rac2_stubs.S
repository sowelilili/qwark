/*
 * Going Commando menu renderer: the four hook stubs (mh_rac2.h says where each
 * hook goes), and the screens the pause manager is given. Preprocessed, so the
 * offsets come from the same headers the C uses, and commented C-style for the
 * same reason. The msg, combo and root stubs are probe N2c's, which ran on
 * hardware; the inject stub stands in for N2c's L1 + R1 test with qwark's
 * open request, as UYA's does, and asks the C whether the game's root is the
 * one with Help. The screens' item calls (update, draw, enter, exit) need no
 * stub: the game calls them through descriptors that carry this module's TOC.
 */

#include "mh_rac2.h"
#include "mh_shim.h"

#define PRESSED    (MH_SHIM + SH_PRESSED)
#define REQUEST    (MH_BOX + MBOX_OFF_REQUEST)
#define OPENS      (MH_STATE + ST_OPENS)
/*
 * The core's row texts in text set 0, less one: text id MH_MSG_ROW0 + i is row
 * i of the set on show, ST_FRONT's (round 3), MH_SET_SIZE further on for set 1.
 */
#define TEXT_BASE  (MH_STATE + ST_SET0 + SET_TEXT - MH_TEXT_LEN)
#define FRONT      (MH_STATE + ST_FRONT)
#define BARS_POS   (R2_BARS_OFF + R2_R_POS)
#define ITEM_ADDR  (MH_SCREENS + MS_ITEM)
#define SCR_COMBO  (MH_SCREENS + MS_COMBO * R2_SCREEN_SIZE)
#define PICK_ITEM  (MH_SCREENS + MS_PICK_ITEM)
#define PICK_ROWS  (MH_SCREENS + MS_PICK_ROWS)
#define PICK_NAMES (MH_SCREENS + MS_PICK_NAMES)

	.section ".text.entry","ax"
	.align	2

/*
 * MH_SITE_MSG, 0xB28F38: msg_string(id), a leaf with some 800 callers, begins
 * with `lis r4,0x134`, which becomes `b mh_msg_stub`. In: r3 = the id. 0xF000
 * is MH_ENTRY_LABEL, "Trainer", the Help bar's label; 0xF001 .. 0xF001 +
 * MH_MAIN_ROWS - 1 are the core's rows of a MAIN page in the text set on show,
 * which the bars ask for as the shim lays them out (0xCAA71C, which keeps the
 * pointer), only ever after mh_core_start: a screen going keeps the pointers it
 * was given into the set it showed (round 3's pointer rule). 0xF010 + p, p
 * below MH_PLANETS, is the picker's name of qwark's planet p (MS_PICK_NAMES),
 * which the planet list draws.
 *
 * The pickers' round 2: the planet list's draw (0xBCBC58) reads a row's text
 * ids as halfwords and sign-extends them (lhzx then extsh at 0xBCC124, lhz then
 * extsh at 0xBCC384) before it calls msg_string, so our ids reach it as
 * 0xFFFFF0xx: the stub takes that form of them too (the low word xored with
 * 0xFFFF0000 is 0xF0xx again), where it used to send them on into msg_string,
 * whose answer for an id it does not know is "(unavailable)". The game's own
 * rows, the ship's select's among them, carry the game's ids, below 0x8000,
 * which neither form matches, so they say what they always said.
 *
 * Anything else goes on into msg_string with r4 set as the replaced
 * instruction set it: the game's ids end at 0xBB81, so an id of the game's
 * costs six instructions more than msg_string's own. Writes r3, r4, r5 and
 * cr0, which msg_string itself writes, and returns to msg_string's caller.
 */
	.globl	mh_msg_stub
mh_msg_stub:
	rlwinm	%r5,%r3,0,0,25
	cmplwi	%r5,MH_MSG_BASE
	beq	1f
	xoris	%r5,%r5,0xFFFF
	cmplwi	%r5,MH_MSG_BASE
	bne	2f
1:	andi.	%r5,%r3,0x3F
	beq	0f
	cmplwi	%r5,MH_MSG_PLANET-MH_MSG_BASE
	bge	3f
	cmplwi	%r5,MH_MAIN_ROWS
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
0:	lis	%r3,(MH_SCREENS+MS_TRAINER)@ha
	addi	%r3,%r3,(MH_SCREENS+MS_TRAINER)@l
	blr
3:	addi	%r5,%r5,-(MH_MSG_PLANET-MH_MSG_BASE)
	cmplwi	%r5,MH_PLANETS
	bge	2f
	rlwinm	%r5,%r5,2,0,29
	lis	%r4,PICK_NAMES@ha
	addi	%r4,%r4,PICK_NAMES@l
	lwzx	%r3,%r4,%r5
	blr
2:	lis	%r4,0x134
	b	game_msg_exit

/*
 * MH_SITE_INJECT, 0xBF6110 in the mode 0 update: `rlwinm r4,r4,0,20,20`, the
 * test of the pad's presses (pad +0xA4, loaded just before) for Start, becomes
 * `bl mh_inject_stub`. The game has made its own checks for this frame and
 * branches to its Start path on r4 non-zero, where its gate (the frame count,
 * the player's state, gameMode 0) decides whether the pause opens. In: r3 =
 * the player's state, which the gate reads next; r4 = the presses. Out: r3 as
 * it was, r4 the game's own test, or Start in the frame that takes an open
 * request.
 *
 * Every call puts PRESSED down, so the Start path tells the root stub the pause
 * is the menu's only in the frame that took the request. With no request that
 * is all, and the stub writes r0, r4, r11, r12 and cr0. With one, mh_inject
 * decides (the request is taken and dropped where the root would not be the
 * one with Help, a hoverbike race, a ship level or an orbit) and answers the
 * test: a call like any other, in a frame of 0x80 that keeps the game's r2 and
 * r3 above the C callee's parameter save area, LR in the caller's slot (0x90),
 * the shape of UYA's launch stub. It writes what a call may: everything
 * volatile but r3. The update has called 0xB8BE68 just before (0xBF60EC), so
 * none of r0, r4..r12, cr0, cr1, cr5..cr7, CTR, XER or f0..f13 holds anything
 * it reads again without writing it first.
 */
	.globl	mh_inject_stub
mh_inject_stub:
	rlwinm	%r4,%r4,0,20,20
	lis	%r11,PRESSED@ha
	li	%r0,0
	stb	%r0,PRESSED@l(%r11)
	lis	%r12,REQUEST@ha
	lbz	%r0,REQUEST@l(%r12)
	cmpwi	%r0,0
	beqlr
	stdu	%r1,-0x80(%r1)
	mflr	%r0
	std	%r0,0x90(%r1)
	std	%r2,0x78(%r1)
	std	%r3,0x70(%r1)
	ori	%r3,%r4,0
	lis	%r2,_toc_base@ha
	addi	%r2,%r2,_toc_base@l
	bl	.mh_inject
	nop
	ori	%r4,%r3,0
	ld	%r3,0x70(%r1)
	ld	%r2,0x78(%r1)
	ld	%r0,0x90(%r1)
	mtlr	%r0
	addi	%r1,%r1,0x80
	blr

/*
 * MH_SITE_COMBO, 0xBF6190 in the Start path, after the game's gate: `li r5,1`
 * (the pause reason) becomes `bl mh_combo_stub`. It sets r5 = 1 and FORCE to
 * PRESSED: up when this pause opens for the menu's open request, down for a
 * Start of the player's. Next come r6, r7 and mode_requestChange(3, 1, 1, 0,
 * 0), the only request for a pause of reason 1 in the executable. Writes r5,
 * r11 and r12, which that code does not read.
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
 * MH_SITE_ROOT, 0xBB8958 in the pause manager's state 1, reason 1: `addic
 * r3,r3,24804` (r3 = 0x01340000 -> the root 0x13460E4) becomes `bl
 * mh_root_stub`. The next two instructions store r3 as the current and the
 * next screen. With FORCE up: FORCE down, and - where the pause's setup
 * (0xBB6970, from PauseInit for this pause) gave the bars the ordinary root's
 * table, the one with Help - counted, r3 = the combo's screen; otherwise, and
 * with FORCE down, the replaced instruction. Writes r3, r11, r12 and cr0 (the
 * function never uses r11 or r12, and sets cr0 again before reading it).
 */
	.globl	mh_root_stub
mh_root_stub:
	lis	%r11,MH_SHIM@ha
	addi	%r11,%r11,MH_SHIM@l
	lbz	%r12,SH_FORCE(%r11)
	cmpwi	%r12,0
	bne	1f
0:	addic	%r3,%r3,24804
	blr
1:	li	%r12,0
	stb	%r12,SH_FORCE(%r11)
	lis	%r12,R2_UI_MANAGER@ha
	lwz	%r12,R2_UI_MANAGER@l(%r12)
	cmpwi	%r12,0
	beq	0b
	addis	%r12,%r12,BARS_POS@ha
	lwz	%r12,BARS_POS@l(%r12)
	rlwinm	%r3,%r12,16,16,31
	cmplwi	%r3,(R2_POS_NORMAL>>16)
	bne	2f
	rlwinm	%r3,%r12,0,16,31
	cmplwi	%r3,(R2_POS_NORMAL&0xFFFF)
2:	lis	%r3,0x134
	bne	0b
	lis	%r12,OPENS@ha
	lwz	%r3,OPENS@l(%r12)
	addi	%r3,%r3,1
	stw	%r3,OPENS@l(%r12)
	lis	%r3,SCR_COMBO@ha
	addi	%r3,%r3,SCR_COMBO@l
	blr

/*
 * The screens, at MH_SCREENS: the first of the read-only data, which the link
 * puts right after the shim's state (stubs.o is the first object it is given),
 * and so in the data image qwark writes before any word goes in, the Help
 * entry's screen among them. Each is a copy of the root's (0x13460E4: its
 * rects, id 2) or of Cheats' (0x134A2D4: its rects, id 9) with the shim's item
 * as its focus and only item; the Help bar's has the root as its parent, as
 * the game's Help screen has. The game writes into them as into its own (the
 * pending focus, the rects it lays into the item every frame).
 */
#define ROOT_RECTS \
	.long	256, 61, 203, 35, 256, 110, 203, 35, 256, 161, 203, 35, 256, 462, 203, 35; \
	.long	256, 259, 203, 35, 256, 309, 203, 35, 256, 359, 203, 35, -136, 371, 104, 17; \
	.long	560, 357, 53, 34, -70, 286, 59, 62, 144, 462, 55, 58, -155, 283, 60, 63; \
	.long	-102, 203, 42, 26, -72, 122, 25, 26
#define CHEATS_RECTS \
	.long	256, 47, 241, 42, 280, 80, 190, 134, 415, 194, 148, 136, 256, 194, 131, 213; \
	.long	256, 353, 131, 70, 498, 483, 209, 35, 625, 381, 119, 17, -136, 371, 104, 17; \
	.long	560, 357, 53, 34, -70, 286, 59, 62, 144, 462, 55, 58, -155, 283, 60, 63; \
	.long	-102, 203, 42, 26, -72, 122, 25, 26
#define SCREEN(rects, parent, id) \
	rects; \
	.long	parent, id, ITEM_ADDR, ITEM_ADDR; \
	.space	13 * 4; \
	.long	0, 0, 0

	.section ".rodata","a"
	.align	4
	.globl	mh_screens
mh_screens:
	SCREEN(ROOT_RECTS, R2_ROOT_SCREEN, 2)   /* MS_HELP: the Help entry's */
	SCREEN(ROOT_RECTS, 0, 2)                /* MS_COMBO: the root stub's */
	SCREEN(ROOT_RECTS, 0, 2)                /* MS_MAIN_A */
	SCREEN(ROOT_RECTS, 0, 2)                /* MS_MAIN_B */
	SCREEN(CHEATS_RECTS, 0, 9)              /* MS_CFG_A */
	SCREEN(CHEATS_RECTS, 0, 9)              /* MS_CFG_B */

/* MS_ITEM: {update, draw, enter, exit} the shim's descriptors, then the root item's 1 at +0x10. */
	.globl	mh_item
mh_item:
	.long	mh_update, mh_draw, mh_enter, mh_exit, 1
	.space	R2_ITEM_SIZE - 20

/* MS_IDS: the MAIN rows' text ids, which the bars ask msg_string for. */
	.globl	mh_ids
mh_ids:
	.long	MH_MSG_ROW0, MH_MSG_ROW0 + 1, MH_MSG_ROW0 + 2, MH_MSG_ROW0 + 3
	.long	MH_MSG_ROW0 + 4, MH_MSG_ROW0 + 5, MH_MSG_ROW0 + 6, 0

/* MS_TRAINER: the Help bar's label, eight bytes. */
	.globl	mh_trainer
mh_trainer:
	.asciz	MH_ENTRY_LABEL

/* MS_MORE: over the first row or under the last of a CONFIG page that scrolls. */
	.globl	mh_more
mh_more:
	.asciz	"..."

/*
 * MS_PICK: the picker, a copy of the planet select's screen 15 (0x134DD90):
 * its rects and id, its parent the menu's screen the picker was opened from
 * (shim_picker writes it, 0 here), the list the focus, and the game's own
 * galaxy (item 0) and picture (item 4) items where the select has them, so the
 * manager lays the same rects into them.
 */
#define PICK_RECTS \
	.long	256, 45, 470, 38, 123, 179, 184, 220, 336, 172, 307, 178; \
	.long	94, 359, 144, 59, 352, 330, 248, 100, -92, 317, 136, 23; \
	.long	-136, 371, 104, 17, 560, 357, 53, 34, -70, 286, 59, 62; \
	.long	144, 462, 55, 58, -155, 283, 60, 63, -102, 203, 42, 26; \
	.long	-72, 122, 25, 26, 0, 0, 0, 0

	.align	4
	.globl	mh_pick
mh_pick:
	PICK_RECTS
	.long	0, R2_PICK_ID, PICK_ITEM
	.long	R2_PICK_GALAXY, PICK_ITEM, 0, 0, R2_PICK_PICTURE
	.space	9 * 4
	.long	0, 0, 0

/*
 * MS_PICK_ITEM: the list, a copy of the select's (0x134E0DC, R2_LIST_SIZE
 * bytes): the shim's update, enter and exit, the game's draw, its flags, the
 * shim's rows. Everything else zero, as the game's.
 */
	.globl	mh_pick_item
mh_pick_item:
	.long	mh_pick_update, R2_LIST_DRAW, mh_pick_enter, mh_pick_exit
	.space	0x20
	.long	R2_LIST_FLAGS, PICK_ROWS
	.space	R2_LIST_SIZE - 0x38

/*
 * MS_PICK_ROWS: a row a planet, in qwark's id order (row i is planet i), as
 * {text, kind, target, sub, anim}: the game's location for the planet (the
 * text ids at R2_PLANET_TEXT) over its name in qwark's words (0xF010 + i, the
 * msg stub's), or the name alone where the location says the same or there is
 * none; a kind the game's row actions ignore, since the shim takes the cross;
 * then a zero row, which ends the list.
 */
#define PICK_ROW(i, loc) \
	.short	loc, R2_ROW_INERT; .long 0; .short MH_MSG_PLANET + i, 0
#define PICK_ROW_NAME(i) \
	.short	MH_MSG_PLANET + i, R2_ROW_INERT; .long 0; .short 0, 0

	.globl	mh_pick_rows
mh_pick_rows:
	PICK_ROW(0, 0xB2B)          /* Flying Lab, Aranos */
	PICK_ROW(1, 0xB2C)          /* Megacorp Outlet, Oozla */
	PICK_ROW(2, 0xB2D)          /* Maktar Resort */
	PICK_ROW(3, 0xB2E)          /* Megapolis, Endako */
	PICK_ROW(4, 0xB2F)          /* Vukovar Canyon, Barlow */
	PICK_ROW(5, 0xB30)          /* Thug Rendezvous, Feltzin */
	PICK_ROW(6, 0xB31)          /* Canal City, Notak */
	PICK_ROW(7, 0xB32)          /* Frozen Base, Siberius */
	PICK_ROW(8, 0xB33)          /* Mining Area, Tabora */
	PICK_ROW(9, 0xB34)          /* Testing Facility, Dobbo */
	PICK_ROW(10, 0xB35)         /* Deep Space Disposal, Hrugis */
	PICK_ROW(11, 0xB36)         /* Megacorp Games, Joba */
	PICK_ROW(12, 0xB37)         /* Megacorp Armory, Todano */
	PICK_ROW(13, 0xB38)         /* Silver City, Boldan */
	PICK_ROW(14, 0xB39)         /* Flying Lab, Aranos 2 */
	PICK_ROW(15, 0xB3A)         /* Thugs-4-Less Fleet, Gorn */
	PICK_ROW(16, 0xB3B)         /* Thug HQ, Snivelak */
	PICK_ROW(17, 0xB3C)         /* Distribution Facility, Smolg */
	PICK_ROW(18, 0xB3D)         /* Allgon City, Damosel */
	PICK_ROW(19, 0xB3E)         /* Tundor Wastes, Grelbin */
	PICK_ROW(20, 0xB3F)         /* Protopet Factory, Yeedil */
	PICK_ROW_NAME(21)           /* Insomniac Museum (its location says the same) */
	PICK_ROW(22, 0xB41)         /* Testing Facility Moon, Dobbo Orbit */
	PICK_ROW(23, 0xB42)         /* Testing Facility Moon, Damosel Orbit */
	PICK_ROW(24, 0xB43)         /* Slim Cognito's, Slim Cognito */
	PICK_ROW_NAME(25)           /* Wupash (no location) */
	PICK_ROW_NAME(26)           /* Jamming Array (its location says the same) */
	.space	R2_ROW_SIZE

/* MS_PICK_NAMES: the names' addresses, MS_PICK_STRS: the names, PLANET_LIST's (src/games/rac2.c). */
	.globl	mh_pick_names
mh_pick_names:
	.long	.Lpn0, .Lpn1, .Lpn2, .Lpn3, .Lpn4, .Lpn5, .Lpn6, .Lpn7, .Lpn8
	.long	.Lpn9, .Lpn10, .Lpn11, .Lpn12, .Lpn13, .Lpn14, .Lpn15, .Lpn16, .Lpn17
	.long	.Lpn18, .Lpn19, .Lpn20, .Lpn21, .Lpn22, .Lpn23, .Lpn24, .Lpn25, .Lpn26
.Lpn0:	.asciz	"Aranos"
.Lpn1:	.asciz	"Oozla"
.Lpn2:	.asciz	"Maktar"
.Lpn3:	.asciz	"Endako"
.Lpn4:	.asciz	"Barlow"
.Lpn5:	.asciz	"Feltzin"
.Lpn6:	.asciz	"Notak"
.Lpn7:	.asciz	"Siberius"
.Lpn8:	.asciz	"Tabora"
.Lpn9:	.asciz	"Dobbo"
.Lpn10:	.asciz	"Hrugis"
.Lpn11:	.asciz	"Joba"
.Lpn12:	.asciz	"Todano"
.Lpn13:	.asciz	"Boldan"
.Lpn14:	.asciz	"Aranos 2"
.Lpn15:	.asciz	"Gorn"
.Lpn16:	.asciz	"Snivelak"
.Lpn17:	.asciz	"Smolg"
.Lpn18:	.asciz	"Damosel"
.Lpn19:	.asciz	"Grelbin"
.Lpn20:	.asciz	"Yeedil"
.Lpn21:	.asciz	"Insomniac Museum"
.Lpn22:	.asciz	"Dobbo Orbit"
.Lpn23:	.asciz	"Damosel Orbit"
.Lpn24:	.asciz	"Slim Cognito"
.Lpn25:	.asciz	"Wupash"
.Lpn26:	.asciz	"Jamming Array"
	.globl	mh_pick_end
mh_pick_end:

