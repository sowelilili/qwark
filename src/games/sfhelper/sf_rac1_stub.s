# The RaC1 input hook.
#
# RaC1 is the one game whose hook site is not a call. racman's mod replaces the
# instruction at 0x11E3A0, in the middle of the routine that has just polled the
# pad, so the registers that routine is holding have to come back exactly as they
# were and the displaced instruction has to be run on the way out. The other three
# games replace a `bl`, where the calling convention already says r3 upwards are
# scratch, and their hook branches straight into the compiled helper.
#
# This is racman's tramp.bin and inputp.bin folded into one: the same registers
# it saved, saved here, and a relative branch to the helper instead of a call
# through r0 to a second cave. What it does not save - r11, r12 and the condition
# register - is what those two did not save either, and the shipped mod has run
# on consoles for years with that set.
#
#   -0x90  frame
#    0x28  r10 ... 0x60 r3
#    0x68  CTR
#    0x70  LR

	.section .text.stub,"ax"
	.globl .sf_input_hook
.sf_input_hook:
	addi    r1, r1, -0x90
	mflr    r0
	std     r0, 0x70(r1)
	mfctr   r0
	std     r0, 0x68(r1)
	std     r3, 0x60(r1)
	std     r4, 0x58(r1)
	std     r5, 0x50(r1)
	std     r6, 0x48(r1)
	std     r7, 0x40(r1)
	std     r8, 0x38(r1)
	std     r9, 0x30(r1)
	std     r10, 0x28(r1)

	bl      .sf_entry

	ld      r10, 0x28(r1)
	ld      r9, 0x30(r1)
	ld      r8, 0x38(r1)
	ld      r7, 0x40(r1)
	ld      r6, 0x48(r1)
	ld      r5, 0x50(r1)
	ld      r4, 0x58(r1)
	ld      r3, 0x60(r1)
	ld      r0, 0x68(r1)
	mtctr   r0
	ld      r0, 0x70(r1)
	mtlr    r0
	addi    r1, r1, 0x90

	# The instruction the hook word displaced at 0x11E3A0, run with the caller's
	# own r1, and then back to the instruction after it.
	ld      r0, 0xA0(r1)
	blr
