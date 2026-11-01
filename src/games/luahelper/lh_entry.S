/*
 * The Lua helper's entry, which the game's own call to its gameplay VM's
 * update now reaches instead (lh_<game>.h, LH_HOOK_ADDR). Through cpp, so the
 * game's numbers come from the same header the C half and build.sh read.
 *
 * At the hook site r3 holds the VM, r2 the TOC of the code around the call,
 * and the instruction after the call is a `nop`: the game expects r2 back as
 * it was. So the entry:
 *
 *   - builds a frame (LR into the caller's save slot, 0x80 bytes below), and
 *     keeps r31 and the caller's r2 in it, above the 0x70-byte area a callee
 *     may use for its own register saves and parameters;
 *   - calls the update with the caller's r2 and r3, which is the original call
 *     word's own effect;
 *   - sets r2 to the Lua range's TOC (LH_LUA_TOC) and calls lh_tick(vm);
 *   - puts r2, r31 and LR back and returns.
 *
 * The update returns nothing the game reads (the next instruction after the
 * nop is another call), so r3 is left as lh_tick leaves it.
 */
#include "luahelper.h"

	.section .text.entry, "ax"
	.globl  .lh_hook
.lh_hook:
	mflr    r0
	std     r0, 0x10(r1)
	stdu    r1, -0x80(r1)
	std     r31, 0x78(r1)
	std     r2, 0x70(r1)
	mr      r31, r3

	bl      .lh_vm_update
	nop

	mr      r3, r31
	lis     r2, LH_LUA_TOC@ha
	addi    r2, r2, LH_LUA_TOC@l
	bl      .lh_tick
	nop

	ld      r2, 0x70(r1)
	ld      r31, 0x78(r1)
	addi    r1, r1, 0x80
	ld      r0, 0x10(r1)
	mtlr    r0
	blr

/*
 * The error tap's entry is lh_err_tap itself, a C function reached by a plain
 * `b` (no link) from the game's reportError, which returns to reportError's
 * caller; it needs nothing here. Its symbol is .lh_err_tap.
 */

/*
 * Revision 1.29, the console menu: the pause menu's frame (LH_MENU_HOOK_ADDR),
 * which replaces PauseMenu_update's `bl` to the trampoline of
 * AnarkMenuSystem_update. r3 is the pause's menu system and r2 the low
 * range's TOC; the caller reloads its r2 from 40(r1) afterwards, which the
 * trampoline would have stored there, so the entry stores it first. Then:
 * the trampoline, as the game called it (it stores r2 again, in this frame's
 * slot, and runs the update with the Lua range's r2), and lh_menu_tick with
 * the Lua range's TOC set here.
 */
	.globl  .lh_menu_hook
.lh_menu_hook:
	std     r2, 40(r1)
	mflr    r0
	std     r0, 0x10(r1)
	stdu    r1, -0x80(r1)
	std     r31, 0x78(r1)
	mr      r31, r3

	bl      .lh_tr_menu_update

	mr      r3, r31
	lis     r2, LH_LUA_TOC@ha
	addi    r2, r2, LH_LUA_TOC@l
	bl      .lh_menu_tick
	nop

	ld      r31, 0x78(r1)
	addi    r1, r1, 0x80
	ld      r0, 0x10(r1)
	mtlr    r0
	blr

/*
 * The pause test (LH_PAUSE_HOOK_ADDR), which replaces Game_UpdateFrame's `bl`
 * to the trampoline of InputMapper_isPressed(mapper, 1). The same r2 rule:
 * the caller's goes to 40(r1) first. The test itself is in the Lua range, so
 * with that r2 the entry calls it directly, r3 and r4 as the game set them,
 * then lm_pause_check(answer), whose answer the game gets.
 */
	.globl  .lh_pause_hook
.lh_pause_hook:
	std     r2, 40(r1)
	mflr    r0
	std     r0, 0x10(r1)
	stdu    r1, -0x70(r1)
	lis     r2, LH_LUA_TOC@ha
	addi    r2, r2, LH_LUA_TOC@l

	bl      .lh_btn_pressed
	nop
	bl      .lm_pause_check
	nop

	addi    r1, r1, 0x70
	ld      r0, 0x10(r1)
	mtlr    r0
	blr

/*
 * qw()'s descriptor, which the pause menu's Lua calls it through, as it calls
 * every C function: its entry and its TOC, the Lua range's.
 */
	.align  3
	.globl  lh_qw_desc
lh_qw_desc:
	.long   .lh_qw
	.long   LH_LUA_TOC

/* lua_load's chunk name: "=" means "use the rest as it is" in messages. */
	.globl  lh_chunkname
lh_chunkname:
	.asciz  "=qwark"

/* Revision 1.29: the menu's script name in the err area (past the "="), and two globals. */
	.globl  lh_menuname
lh_menuname:
	.asciz  "=qwark-menu"
	.globl  lh_qwname
lh_qwname:
	.asciz  "qw"
	.globl  lh_framename
lh_framename:
	.asciz  "qwark_frame"
/* Revision 1.29, the second crash: the glue's way out, and what the err area says of it. */
	.globl  lh_quitname
lh_quitname:
	.asciz  "qwark_quit"
	.globl  lh_shortmsg
lh_shortmsg:
	.asciz  "the pause menu's Lua pool ran short: the Trainer took itself out"
	.align  2
