# Ratchet & Clank 3, Up Your Arsenal (NPEA00387): the code switches.
#
# sw_rac1.s says what a switch is and why it exists. RaC3 has two instruction
# patches, Freeze ammo (the Infinite ammo checkbox) and, since fast loads became
# a code patch, Fast loads, and so two switches; Fast loads' has two sites.
#
# The flags are at 0xD9FF10 and 0xD9FF11, in the zero-filled data where the
# savefile helper keeps its request bytes (0xD9FF00..02), its copy counter
# (0xD9FF08) and its pause words (0xD9FF14, 0xD9FF18): zero at every boot, and
# nothing in the executable refers to them.
#
# The caves are in the savefile helper's dead function, 0x980AC0..0x981064. The
# helper may run up to 0x980F00 (SF_CAVE_END in src/games/sfhelper/sf_rac3.h;
# build 53's pause made it grow from 372 bytes), so the switches start there.
# The two functions after it, 0x981064 and 0x981120, are dead too - nothing
# names their descriptors, 0xBF8188 and 0xBF8190 - and the code is live again
# from 0x981224 (called from 0x9028FC); the switches stop at 0x981064 all the
# same. (A console's own fast-load cave is elsewhere, at RAC3_FASTLOAD_CAVE in
# src/games/rac3.h.)
#
# build.sh assembles this, links it with sw_rac3.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FREEZE_AMMO, 0x00D9FF10
	.set    SW_FLAG_FAST_LOADS,  0x00D9FF11
	.globl  SW_FLAG_FREEZE_AMMO
	.globl  SW_FLAG_FAST_LOADS

# The transition's state and the renderer's, from the NPEA00387 executable
# (src/games/rac3.h has the whole story under RAC3_FASTLOAD_SITE). The first five
# share their high half, 0xDA, so one lis serves them.
	.set    RAC3_TRANSITION_ID,    0x00D990DE  # u16, the transitionN folder
	.set    RAC3_TRANSITION_FRAME, 0x00D990D2  # u16, the pass count, 0 on the first
	.set    RAC3_LOADER_STAGE,     0x00D99094  # u32, the level loader's stage
	.set    RAC3_TRANSITION_HOLD,  0x00D99098  # u32, its sound banks are loaded
	.set    RAC3_SCENE_OWED,       0x00F44E90  # u32, the story scene the trip owes
	.set    RAC3_TRANSITION_FADE,  0x0134EBE0  # f32, how black the screen is
	.set    RAC3_RENDERER,         0x00CB09BC  # the renderer object
	.set    RAC3_RENDERER_HELD,    0x1720      # in it: flips that will not show

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
# Two sites in a pass of the planet transition's frame loop (0x27B360), where a
# console writes a branch each into its cave at RAC3_FASTLOAD_CAVE (rac3.c,
# rac3_fastload_words); these are the same logic behind the flag, with ordinary
# calls where the console's cave points the link register at the loop. With the
# flag clear, or in a multiplayer load (transition 7), both run the game's own
# instruction and go back.
#
# 0x27B49C: ori r3, r26, 0, the update's argument, with the pads read and the
# mode set. With the flag up:
#   - the first pass (RAC3_TRANSITION_FRAME 0) lets every flip show again and
#     fades the screen to black over six frames (0x123988), as RaC2's load does
#     before its loop, and goes on without the update;
#   - while a story scene is owed (RAC3_SCENE_OWED), the update runs: the first
#     clip's end plays the scene, which draws itself;
#   - otherwise, below loader stage 2, the pass goes on without the
#     transition's two routines (0x27B538): the transition stands still, starts
#     no clip and no sound, while the pads and the loader run;
#   - at stage 2 the transition is over: the fade at black, its sound banks
#     unloaded as the game does when its closing clip starts (Transition_
#     UnloadSounds, 0x278A00) if it is one with banks of its own (0 to 6) and
#     still holds them, r27 = 1, and the game's finish runs the rest behind the
#     black.
# r3, r5, r6 and r0 are free at the site; r4, the update's descriptor, is only
# needed on the way to the update, which every path that skips it leaves alone.
	.section .cave.fast_loads, "ax"
fast_loads:
	lis     r5, SW_FLAG_FAST_LOADS@ha
	lbz     r6, SW_FLAG_FAST_LOADS@l(r5)
	cmpwi   cr0, r6, 0
	beq     cr0, 9f
	lis     r5, RAC3_TRANSITION_ID@ha
	lhz     r6, RAC3_TRANSITION_ID@l(r5)
	cmplwi  cr0, r6, 7
	beq     cr0, 9f
	lhz     r3, RAC3_TRANSITION_FRAME@l(r5)
	cmpwi   cr0, r3, 0
	beq     cr0, 2f
	lis     r3, RAC3_SCENE_OWED@ha
	lwz     r3, RAC3_SCENE_OWED@l(r3)
	cmpwi   cr0, r3, 0
	bne     cr0, 9f
	lwz     r3, RAC3_LOADER_STAGE@l(r5)
	cmpwi   cr0, r3, 2
	blt     cr0, 3f
	li      r27, 1
	lis     r3, RAC3_TRANSITION_FADE@ha
	lis     r0, 0x3F80
	stw     r0, RAC3_TRANSITION_FADE@l(r3)
	cmplwi  cr0, r6, 6
	bgt     cr0, 3f
	lwz     r3, RAC3_TRANSITION_HOLD@l(r5)
	cmpwi   cr0, r3, 0
	beq     cr0, 3f
	bl      rac3_transition_unload_sounds
	b       3f
2:	lis     r5, RAC3_RENDERER@ha
	lwz     r5, RAC3_RENDERER@l(r5)
	li      r0, 0
	stw     r0, RAC3_RENDERER_HELD(r5)
	li      r3, 6
	bl      rac3_screen_fade
3:	b       rac3_fastload_resume
9:	ori     r3, r26, 0
	b       fast_loads_ret

	.section .site.fast_loads, "ax"
	b       fast_loads
fast_loads_ret:

# 0x27B550: bl 0x11AD44, the head of the pass's frame (0x11AD44 and 0x11B434
# begin it, 0x27A014 draws the transition, 0x11B4EC ends it and flips). With the
# flag up the pass goes past all of it to 0x27B570, RaC2's nop at 0xBEA8A0:
# nothing is drawn, nothing flips, and no pass waits for a vblank.
	.section .cave.fast_loads_draw, "ax"
fast_loads_draw:
	lis     r3, SW_FLAG_FAST_LOADS@ha
	lbz     r3, SW_FLAG_FAST_LOADS@l(r3)
	cmpwi   cr0, r3, 0
	beq     cr0, 1f
	lis     r3, RAC3_TRANSITION_ID@ha
	lhz     r3, RAC3_TRANSITION_ID@l(r3)
	cmplwi  cr0, r3, 7
	bne     cr0, 2f
1:	bl      rac3_frame_begin
	b       fast_loads_draw_ret
2:	b       rac3_fastload_skip

	.section .site.fast_loads_draw, "ax"
	b       fast_loads_draw
fast_loads_draw_ret:
