# Ratchet & Clank 3, Up Your Arsenal (NPEA00387): the code switch.
#
# sw_rac1.s says what a switch is and why it exists. RaC3 has one instruction
# patch, Freeze ammo, and so one switch.
#
# The flag is at 0xD9FF10, in the zero-filled data where the savefile helper
# keeps its request bytes (0xD9FF00..02) and its copy counter (0xD9FF08): zero
# at every boot, and nothing in the executable refers to it.
#
# The cave is in the savefile helper's dead function, 0x980AC0..0x981064. The
# helper may run up to 0x980F00 (SF_CAVE_END in src/games/sfhelper/sf_rac3.h;
# build 53's pause made it grow from 372 bytes), so this has 0x980F00 to
# 0x981064. Past that the functions are live code.
#
# build.sh assembles this, links it with sw_rac3.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FREEZE_AMMO, 0x00D9FF10
	.globl  SW_FLAG_FREEZE_AMMO

# ----------------------------------------------------------------- freeze ammo
#
# 0x182A88: stwx r4, r5, r6, the store of the decremented ammo count. Patched:
# nop. It is also qwark's RaC3 fingerprint, which accepts this branch as well
# once the switch is in.
	.section .cave.freeze_ammo, "ax"
freeze_ammo:
	lis     r7, SW_FLAG_FREEZE_AMMO@ha
	lbz     r7, SW_FLAG_FREEZE_AMMO@l(r7)
	cmpwi   cr0, r7, 0
	bne     cr0, 1f
	stwx    r4, r5, r6
1:	b       freeze_ammo_ret

	.section .site.freeze_ammo, "ax"
	b       freeze_ammo
freeze_ammo_ret:
