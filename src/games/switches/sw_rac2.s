# Ratchet & Clank 2, Going Commando (NPEA00386): the code switches.
#
# sw_rac1.s says what a switch is and why it exists. In short: an RPCS3 patch
# replaces each instruction qwark patches on a console with a branch to a
# trampoline here, and the trampoline runs the patched form or the original
# depending on a flag byte, so under RPCS3 the checkbox is a byte write.
#
# Both of RaC2's patched forms are a nop, so each trampoline runs the original
# only when its flag is clear and otherwise falls straight through to the branch
# back (the two crash guards at the end have no flag, and say why):
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
# The caves are at 0x1141400..0x11414AC, in the last function (0x11413F8) of
# the span of library code nothing reaches that the savefile helper starts,
# 0x113EC64..0x11414FC (src/games/sfhelper/sf_rac2.h says why it is dead). The
# helper takes the head of the span, about 1 KB today, and may grow up to here:
# its SF_CAVE_END is this cave's start. The crash guards follow the two
# toggles' trampolines, from 0x1141440 to 0x114149C, and leave 96 bytes of the
# span unused; build.sh fails a cave that would run past SW_CAVE_END.
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

# ---------------------------------------------------------------- crash guards
#
# Build 53. Not a toggle's switch: the two guards have no flag byte and run on
# every frame once the patch is in, because on a console qwark writes them into
# the game itself when the session enters it and nothing turns them off
# (src/games/rac2.c, rac2_guards_install, has the crash and that form of them -
# these same words at these same addresses). In short: a transition to or from
# Dobbo Orbit, Damosel Orbit or the Jamming Array does not empty the ship's
# part list (0x15D46B0), and an in-space story cutscene it plays moves the
# parts still in it, which are the last level's, against the space wad's moby
# table. UpdateMobyGrids (0x868B0) then searches a grid cell for an index that
# cannot be there and runs off the end of memory. Only a planet change qwark
# makes gets there, so neither guard changes anything in a game played as it
# was made. The switches' look counts them like any other cave and site.
#
# cr7 is free in both functions (neither reads or writes it), and so is every
# GPR touched.

# 0x86960: addc r25, r4, r25, in UpdateMobyGrids, after the moby's index (r30)
# is worked out and every register is saved. An index that does not fit a grid
# cell's 16-bit entries - a moby that is not in the moby table - leaves the grid
# alone and goes straight to the epilogue. r0 holds nothing here since the link
# register was saved.
	.section .cave.grid_guard, "ax"
grid_guard:
	extsh   r0, r30
	cmpw    cr7, r0, r30
	bne     cr7, 1f
	addc    r25, r4, r25
	b       grid_guard_ret
1:	b       rac2_grid_done

	.section .site.grid_guard, "ax"
	b       grid_guard
grid_guard_ret:

# 0xBDF30C: lhz r3, 52(r31), the head of the ship update's loop over its parts
# (0xBDEF18; r28 points at the list entry). A part that is not inside the moby
# table, [*0x15927B0, *0x15927B8), is passed over to the next. r3, r4 and r5
# are written before they are read on both paths out of the head. The table's
# two pointers share their high half, so one lis serves both loads.
	.set    RAC2_MOBY_TABLE,     0x015927B0
	.set    RAC2_MOBY_TABLE_END, 0x015927B8

	.section .cave.ship_parts, "ax"
ship_parts:
	lwz     r3, 0(r28)
	lis     r4, RAC2_MOBY_TABLE@ha
	lwz     r5, RAC2_MOBY_TABLE@l(r4)
	cmplw   cr7, r3, r5
	blt     cr7, 1f
	lwz     r5, RAC2_MOBY_TABLE_END@l(r4)
	cmplw   cr7, r3, r5
	bge     cr7, 1f
	lhz     r3, 52(r31)
	b       ship_parts_ret
1:	b       rac2_ship_next_part

	.section .site.ship_parts, "ax"
	b       ship_parts
ship_parts_ret:
