# Ratchet & Clank 3, Up Your Arsenal (NPEA00387): the code switches.
#
# sw_rac1.s says what a switch is and why it exists. RaC3 has two instruction
# patches, Freeze ammo and (since fast loads became a code patch) Fast loads,
# and so two switches.
#
# The flags are at 0xD9FF10 and 0xD9FF11, in the zero-filled data where the
# savefile helper keeps its request bytes (0xD9FF00..02), its copy counter
# (0xD9FF08) and its pause words (0xD9FF14, 0xD9FF18): zero at every boot, and
# nothing in the executable refers to them.
#
# The caves are in the savefile helper's dead function, 0x980AC0..0x981064. The
# helper may run up to 0x980F00 (SF_CAVE_END in src/games/sfhelper/sf_rac3.h;
# build 53's pause made it grow from 372 bytes), so the switches start there.
# Past 0x981064 the functions are live code. (A console's own fast-load cave is
# elsewhere, at RAC3_FASTLOAD_CAVE in src/games/rac3.h.)
#
# build.sh assembles this, links it with sw_rac3.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FREEZE_AMMO, 0x00D9FF10
	.set    SW_FLAG_FAST_LOADS,  0x00D9FF11
	.globl  SW_FLAG_FREEZE_AMMO
	.globl  SW_FLAG_FAST_LOADS

# The transition's state, from the NPEA00387 executable (src/games/rac3.h has
# the whole story under RAC3_FASTLOAD_SITE).
	.set    RAC3_TRANSITION_ID,   0x00D990DE   # u16, the transitionN folder
	.set    RAC3_LOADER_STAGE,    0x00D99094   # u32, the level loader's stage
	.set    RAC3_TRANSITION_HOLD, 0x00D99098   # u32, its sound banks are loaded
	.set    RAC3_TRANSITION_FADE, 0x0134EBE0   # f32, how black the screen is

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

# ------------------------------------------------------------------ fast loads
#
# 0x27B428: cmpwi r27, 0, at the top of the planet transition's frame loop
# (0x27B360), which asks whether the transition is over. Not a single patched
# word as on RaC1 and RaC2: a console writes the cave at 0x980860 and a branch
# here (rac3.c rac3_fastload_words), and this is the same logic behind the flag.
#
# With the flag set, a transition that plays clips (transitions 0 to 5, every
# planet trip but the vid comics, Holostar Clank's way back and multiplayer)
# ends the moment the level loader has read the level (stage 2) while the
# transition still holds its sound banks: it unloads them the way the game does
# at a clip end (Transition_UnloadSounds, 0x278A00, which lets the loader go
# on), puts the fade at black, as a clip end leaves it, and says the transition
# is over. The game's own finish then runs the rest of the load behind a black
# screen. A story scene the trip owes (Aquatos's two, 0xF44E90) holds the loader
# below stage 2 until the transition has played it, so it still plays.
#
# Either way the trampoline ends by doing what the site did, cmpwi r27, 0, for
# the beq after it. r3 and r4 are free here: the call before the site returned
# nothing anyone reads, and the code after it sets both before reading them.
	.section .cave.fast_loads, "ax"
fast_loads:
	cmpwi   cr0, r27, 0
	bne     cr0, 1f
	lis     r3, SW_FLAG_FAST_LOADS@ha
	lbz     r3, SW_FLAG_FAST_LOADS@l(r3)
	cmpwi   cr0, r3, 0
	beq     cr0, 1f
	lis     r3, RAC3_TRANSITION_ID@ha
	lhz     r4, RAC3_TRANSITION_ID@l(r3)
	cmplwi  cr0, r4, 5
	bgt     cr0, 1f
	lwz     r4, RAC3_LOADER_STAGE@l(r3)
	cmpwi   cr0, r4, 2
	blt     cr0, 1f
	lwz     r4, RAC3_TRANSITION_HOLD@l(r3)
	cmpwi   cr0, r4, 0
	beq     cr0, 1f
	bl      rac3_transition_unload_sounds
	lis     r3, RAC3_TRANSITION_FADE@ha
	lis     r4, 0x3F80
	stw     r4, RAC3_TRANSITION_FADE@l(r3)
	li      r27, 1
1:	cmpwi   cr0, r27, 0
	b       fast_loads_ret

	.section .site.fast_loads, "ax"
	b       fast_loads
fast_loads_ret:
