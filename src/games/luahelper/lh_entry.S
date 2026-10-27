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

/* lua_load's chunk name: "=" means "use the rest as it is" in messages. */
	.globl  lh_chunkname
lh_chunkname:
	.asciz  "=qwark"
	.align  2
