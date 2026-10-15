# Ratchet & Clank 2, Going Commando (NPEA00386): the code switches.
#
# sw_rac1.s says what a switch is and why it exists. In short: an RPCS3 patch
# replaces each instruction qwark patches on a console with a branch to a
# trampoline here, and the trampoline runs the patched form or the original
# depending on a flag byte, so under RPCS3 the checkbox is a byte write.
#
# Both of RaC2's patched forms are a nop, so each trampoline runs the original
# only when its flag is clear and otherwise falls straight through to the branch
# back:
#
#     lis   rX, FLAG@ha
#     lbz   rX, FLAG@l(rX)
#     cmpwi cr0, rX, 0
#     bne   cr0, 1f          flag 1: the patched form, a nop
#     O                      flag 0: the original
# 1:  b     site + 4
#
# The flags are in the gap between the end of the TLS template and the next
# data at the top of the read-write segment, 0x1317714..0x131773F: zero in the
# file and referenced by nothing in the executable. The savefile helper keeps
# its request bytes in the same gap since build 50 (sf_rac2.h says why they
# moved), from 0x1317728 up, so the switch flags take its first bytes.
#
# The caves are at 0x1141400..0x1141438, in the last function (0x11413F8) of
# the span of library code nothing reaches that the savefile helper starts,
# 0x113EC64..0x11414FC (src/games/sfhelper/sf_rac2.h says why it is dead). The
# helper takes the head of the span, 1100 bytes today, and may grow up to here:
# its SF_CAVE_END is this cave's start.
#
# Build 53 moved them here from 0x233200 and 0x233220, the end of the function
# at 0x22A368..0x233308, which was thought dead. It is not: on Joba (planet 11)
# the planet setup 0x5BB78 points the per-planet hook at 0x1462DAC to 0x205018,
# which calls 0x22A368 every frame, at 0x205254 or through 0x1F6E18, so with the
# switches patched in under RPCS3 Joba could run into the trampolines' bytes as
# part of that function. Nothing else moved: the trampolines are the same
# instructions, the flags are where they were, and only the two site words
# branch somewhere new.
#
# build.sh assembles this, links it with sw_rac2.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FAST_LOADS,    0x01317720
	.set    SW_FLAG_INFINITE_AMMO, 0x01317721
	.globl  SW_FLAG_FAST_LOADS
	.globl  SW_FLAG_INFINITE_AMMO

# ------------------------------------------------------------------ fast loads
#
# 0xBEA8A0: bl 0xBE9308, the call that runs the load screen. Patched: nop. It is
# also qwark's RaC2 fingerprint, which accepts this branch as well once the
# switches are in, and, since build 53 moved this trampoline, the branch to
# 0x233200 that the patch of every earlier build put there (src/games/rac2.c):
# a game with such a patch is still RaC2, with switches from another build.
# r3 is free here: the callee sets it before it reads it, and so does the
# function called next, whether this call ran or not.
	.section .cave.fast_loads, "ax"
fast_loads:
	lis     r3, SW_FLAG_FAST_LOADS@ha
	lbz     r3, SW_FLAG_FAST_LOADS@l(r3)
	cmpwi   cr0, r3, 0
	bne     cr0, 1f
	bl      rac2_load_screen
1:	b       fast_loads_ret

	.section .site.fast_loads, "ax"
	b       fast_loads
fast_loads_ret:

# --------------------------------------------------------------- infinite ammo
#
# 0xB30C7C: stwx r3, r4, r5. This is the magazine clamp rather than a decrement:
# when the rounds in ammo[id] come to more than the magazine holds, the store
# puts them back to the magazine size. Nopped, the 0x7FFFFFFF the toggle fills
# the array with stays where it was put.
	.section .cave.infinite_ammo, "ax"
infinite_ammo:
	lis     r6, SW_FLAG_INFINITE_AMMO@ha
	lbz     r6, SW_FLAG_INFINITE_AMMO@l(r6)
	cmpwi   cr0, r6, 0
	bne     cr0, 1f
	stwx    r3, r4, r5
1:	b       infinite_ammo_ret

	.section .site.infinite_ammo, "ax"
	b       infinite_ammo
infinite_ammo_ret:
