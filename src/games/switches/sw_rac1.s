# Ratchet & Clank (NPEA00385): the code switches.
#
# What a switch is. Three of qwark's RaC1 toggles are instruction patches: on a
# console qwark writes the patched word over the game's own and puts the
# original back to turn it off. RPCS3 cannot take that - it recompiles PPU code
# ahead of running it, and once it has loaded the executable it takes write
# permission away from the code segment altogether - so there the game carries
# both forms of the instruction instead. An RPCS3 patch, applied as the
# executable loads, replaces each patched instruction with a branch to a
# trampoline here; the trampoline reads one flag byte and runs either the
# patched form or the original, then branches back to the instruction after
# the site. Turning the toggle on or off is then a byte write to data memory.
#
# The shape of every trampoline:
#
#     lis   rX, FLAG@ha
#     lbz   rX, FLAG@l(rX)
#     cmpwi cr0, rX, 0
#     beq   cr0, 1f          flag 0: the original
#     P                      flag 1: the patched form (nothing, for a nop)
#     b     site + 4
# 1:  O
#     b     site + 4
#
# rX and cr0 are dead at every site: each one was read out of the executable
# before it was picked. The sites and the callee addresses are in sw_rac1.ld;
# this file holds the code and the flag bytes.
#
# The flags live in the alignment gap at the end of the executable's
# read-write segment, three bytes that are zero in the file and that nothing in
# the game reads or writes, so every switch is off at every boot.
#
# The caves are in dead code: the function at 0x4F69B8, inside a span of
# unreachable code that runs 0x4F6134..0x4F711F. The savefile helper's input
# stub is at 0x4F6800 in the same span, below this function, and its compiled
# helper at 0x4F8000.
#
# build.sh assembles this, links it with sw_rac1.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FAST_LOADS,      0x009058B0
	.set    SW_FLAG_INFINITE_AMMO,   0x009058B1
	.set    SW_FLAG_INFINITE_HEALTH, 0x009058B2
	.globl  SW_FLAG_FAST_LOADS
	.globl  SW_FLAG_INFINITE_AMMO
	.globl  SW_FLAG_INFINITE_HEALTH

# ------------------------------------------------------------------ fast loads
#
# Three sites, all on the one flag. The first is the call that puts the loading
# screen up (patched: skipped); the other two are where the load picks the sound
# it plays (patched: sound 6, which does not exist, so nothing waits for it).

# 0x165490: bl 0x1639A8, the loading screen. Patched: nop.
	.section .cave.fast_loads_1, "ax"
fast_loads_1:
	lis     r11, SW_FLAG_FAST_LOADS@ha
	lbz     r11, SW_FLAG_FAST_LOADS@l(r11)
	cmpwi   cr0, r11, 0
	beq     cr0, 1f
	b       fast_loads_1_ret
1:	bl      rac1_loading_screen
	b       fast_loads_1_ret

	.section .site.fast_loads_1, "ax"
	b       fast_loads_1
fast_loads_1_ret:

# 0x160060: clrlwi r3, r3, 30. Patched: li r3, 6.
	.section .cave.fast_loads_2, "ax"
fast_loads_2:
	lis     r5, SW_FLAG_FAST_LOADS@ha
	lbz     r5, SW_FLAG_FAST_LOADS@l(r5)
	cmpwi   cr0, r5, 0
	beq     cr0, 1f
	li      r3, 6
	b       fast_loads_2_ret
1:	clrlwi  r3, r3, 30
	b       fast_loads_2_ret

	.section .site.fast_loads_2, "ax"
	b       fast_loads_2
fast_loads_2_ret:

# 0x1641F8: the same instruction in the other load path.
	.section .cave.fast_loads_3, "ax"
fast_loads_3:
	lis     r5, SW_FLAG_FAST_LOADS@ha
	lbz     r5, SW_FLAG_FAST_LOADS@l(r5)
	cmpwi   cr0, r5, 0
	beq     cr0, 1f
	li      r3, 6
	b       fast_loads_3_ret
1:	clrlwi  r3, r3, 30
	b       fast_loads_3_ret

	.section .site.fast_loads_3, "ax"
	b       fast_loads_3
fast_loads_3_ret:

# --------------------------------------------------------------- infinite ammo

# 0xAA2DC: stwx r8, r5, r7, the store of the decremented ammo count. Patched: nop.
	.section .cave.infinite_ammo, "ax"
infinite_ammo:
	lis     r11, SW_FLAG_INFINITE_AMMO@ha
	lbz     r11, SW_FLAG_INFINITE_AMMO@l(r11)
	cmpwi   cr0, r11, 0
	beq     cr0, 1f
	b       infinite_ammo_ret
1:	stwx    r8, r5, r7
	b       infinite_ammo_ret

	.section .site.infinite_ammo, "ax"
	b       infinite_ammo
infinite_ammo_ret:

# ------------------------------------------------------------- infinite health
#
# The console patch writes 0x7F558, addic r3, r4, -25376 -> addic r3, r4, 0, and
# that word is also qwark's RaC1 fingerprint, so the switch leaves it alone and
# hooks the next instruction, 0x7F55C, li r4, 0. r4 still holds what the
# fingerprint instruction added to at that point, so running the patched addic
# again from the trampoline, before the displaced li, leaves r3 and r4 exactly
# as the console patch does. RaC1's fingerprint therefore reads the same with or
# without the switches in.
	.section .cave.infinite_health, "ax"
infinite_health:
	lis     r6, SW_FLAG_INFINITE_HEALTH@ha
	lbz     r6, SW_FLAG_INFINITE_HEALTH@l(r6)
	cmpwi   cr0, r6, 0
	beq     cr0, 1f
	addic   r3, r4, 0
	li      r4, 0
	b       infinite_health_ret
1:	li      r4, 0
	b       infinite_health_ret

	.section .site.infinite_health, "ax"
	b       infinite_health
infinite_health_ret:
