# Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the code
# switches.
#
# sw_rac1.s says what a switch is and why it exists: an RPCS3 patch replaces
# each instruction qwark patches on a console with a branch to a trampoline
# here, and the trampoline runs the patched form or the original depending on a
# flag byte, so under RPCS3 the checkbox is a byte write. ToD has one such
# toggle, Fast loads (id 17), and one site.
#
# The flag is a byte of the RW segment's last page past its BSS (the segment
# ends at 0x108A8550, its 4 KB page at 0x108A9000): zero at every boot, and
# nothing in the executable refers to anything past 0x108A8550.
#
# The cave is a dead function of the shipped game (sw_tod.ld), 400 bytes, and
# the trampoline takes 96 of them.
#
# build.sh assembles this, links it with sw_tod.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FAST_LOADS, 0x108A8560
	.globl  SW_FLAG_FAST_LOADS

# ------------------------------------------------------------------ fast loads
#
# tod.h says what the console's words do and why; the trampoline's second half,
# fast_loads_on, is those seventeen words word for word, but for the five that
# branch out of them, which the link points at the same places.
#
# 0x25E588 in LoadScreen_Update: bne cr7, 0x25E5FC, the return while the
# loading screen's cinematic plays (cr7 is the player's state against 0). r9 and
# cr1 are dead here: the game sets both again before it reads them.
	.section .cave.fast_loads, "ax"
fast_loads:
	lis     r9, SW_FLAG_FAST_LOADS@ha
	lbz     r9, SW_FLAG_FAST_LOADS@l(r9)
	cmpwi   cr1, r9, 0
	bne     cr1, fast_loads_on
	beq     cr7, 1f
	b       tod_ls_update_return    # the game's own bne, taken
1:	b       tod_ls_update_next      # and not taken

# The patched form. The arrival is up (1, the game's own or the one below): as
# the game. The load is done: the cut. Otherwise as the game: a cinematic still
# playing returns, and with none the next long-load screen is picked.
fast_loads_on:
	lwz     r9, 0x7D74(r2)          # &g_loadScreenLoadDone
	lbz     r9, 0(r9)
	lwz     r29, 0x7D78(r2)         # &g_loadScreenType, which the pick stores through
	lwz     r3, 0(r29)
	cmpwi   cr1, r3, 1
	beq     cr1, 2f
	cmpwi   cr6, r9, 0
	bne     cr6, 1f
	bne     cr7, 2f
	b       tod_ls_update_filler
# The cut: the screen black at once, as Level_Enter makes it for the planet;
# the cinematic stopped as a level's exit stops it, and the player reset as
# Level_StartGameplay resets it; and the arrival's 1 in g_loadScreenType, which
# LoadScreen_LoadDone waits for (with the player idle) before the load is over.
1:	lfs     f1, 0x7D84(r2)          # 1.0f
	bl      tod_screen_fade_set
	bl      tod_movie_stop_all
	bl      tod_movie_reset
	li      r0, 1
	stw     r0, 0(r29)
2:	b       tod_ls_update_return

	.section .site.fast_loads, "ax"
	b       fast_loads
