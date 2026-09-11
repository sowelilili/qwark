# The Deadlocked hook.
#
# The mod this helper came from branched out of a `nop` at 0x70719C, the
# call-return slot on the pad routine's no-controller path, and Bot Info and IL
# HUD Display branch out of the same word (and fill the same cave), so loading
# either beside the helper left one of the three with a branch into the wrong
# code. The helper now hooks the last instruction of that same routine instead,
# 0x707430, which nothing else touches:
#
#     707424  li   r3, 0
#     707428  stb  r3, 0x88(r29)
#     70742c  stb  r3, 0x89(r29)
#     707430  stb  r30, 0x457(r29)     <- the hook word goes here
#     707434  ld   r0, 0x210(r1)       the epilogue: r26-r31 and LR back, blr
#
# At that point every scratch register is dead: the epilogue only reloads the
# saved ones from the stack, and the routine returns nothing (its two exits
# leave different things in r3). So the compiled helper can be called as an
# ordinary function from here with nothing saved but the link register, and the
# displaced store, which only reads two registers the helper is bound to
# preserve, is run first. It runs once per pad port per frame, controller or no
# controller, and not at all while the system menu holds the pads (that exit
# skips 0x707430), which is also when nothing is being played.
#
# A proper frame, because the helper is C compiled for the 64-bit ABI: LR into
# the caller's save slot, a minimum 0x70-byte frame below it, and r3 put back to
# the zero the routine had there, for form's sake.

	.section .text.stub,"ax"
	.globl .sf_input_hook
.sf_input_hook:
	# The instruction the hook word displaced at 0x707430.
	stb     r30, 0x457(r29)

	mflr    r0
	std     r0, 0x10(r1)
	stdu    r1, -0x70(r1)

	bl      .sf_entry

	addi    r1, r1, 0x70
	ld      r0, 0x10(r1)
	mtlr    r0
	li      r3, 0
	blr
