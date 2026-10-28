# Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): the code
# switches.
#
# sw_rac1.s says what a switch is and why it exists: an RPCS3 patch replaces
# each instruction qwark patches on a console with a branch to a trampoline
# here, and the trampoline runs the patched form or the original depending on a
# flag byte, so under RPCS3 the checkbox is a byte write. ToD has one such
# toggle, Fast loads (id 17): one switch in LoadScreen_Update and, since
# revision 1.29, three pieces of qwark's own (own_*, sw_rac4.s says what those
# are) that read the same flag themselves, which a console writes as well.
#
# The flag is a byte of the RW segment's last page past its BSS (the segment
# ends at 0x108A8550, its 4 KB page at 0x108A9000): zero at every boot, and
# nothing in the executable refers to anything past 0x108A8550. The FMV load's
# stamp (own_fmv_in) is the word after it.
#
# The caves are a dead function of the shipped game (sw_tod.ld), 400 bytes:
# the switch takes the first 96 of them and qwark's own pieces the next 212.
#
# build.sh assembles this, links it with sw_tod.ld and cuts each .cave.* and
# .site.* section out as bytes, so every branch below is computed by the link.

	.set    SW_FLAG_FAST_LOADS, 0x108A8560
	.globl  SW_FLAG_FAST_LOADS
	.set    SW_DATA_FMV_IN,     0x108A8564
	.globl  SW_DATA_FMV_IN

# The game's words qwark's own pieces read and write (tod.h).
	.set    TOD_LOAD_DONE,      0x102034FC   # g_loadScreenLoadDone, u8
	.set    TOD_LOAD_TYPE,      0x102034F8   # g_loadScreenType, s32
	.set    TOD_MP_STATE,       0x101D4FA8   # g_moviePlayer + 0x58, s32
	.set    TOD_MP_ENTERED,     0x101D4FAC   # g_moviePlayer + 0x5C, the game time the state began
	.set    TOD_MP_NEW,         0x101D4FB0   # g_moviePlayer + 0x60, u8: the state is new
	.set    TOD_GAME_TIME,      0x1074DDBC   # g_gameClock + 0x14
	.set    TOD_FMV_STARTED,    0x1037AA1C   # g_levelTransitionFmvStartTime, its low word

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

# ------------------------------------------ fast loads, qwark's own (revision 1.29)
#
# LoadScreen_Update runs from Game_UpdateFrame, which g_gameState 9 (a realtime
# cinematic playing, MoviePlayer_update's state 2) does not run: that state's
# dispatch is MovieQueue_update alone. So the cut above can only come between
# two load screen cinematics, and the departure, which plays while the data is
# read, always played in full. own_departure ends a load screen cinematic the
# way its own end does once the data is in; the cut follows as the game comes
# back to state 4.
#
# The other two are for the story FMV loads (Metropolis and Cobalia before
# Fastoon's ship is fixed: LevelTransition_begin's FMV list), which have no load
# screen: the loader thread stamps the load's FMV start time once the data is
# read (own_fmv_in), and the main thread, finding its own load's stamp, closes
# the FMV as its own end closes it, runs the list out and puts the screen at
# black (own_fmv_cut). g_levelTransitionMinTime at 0, a data write of the
# toggle's (tod_panel.c), is the rest: the data is read at once.
	.section .cave.own_fast_loads, "ax"

# 0x3C5744 in LevelTransition_update, a planet load's frame: or r3, r28, r28
# before the frame's GameState_Dispatch (0x3C5C88). LevelTransition_update is
# g_levelLoadTier's update, which runs only while that tier is on top and not
# yet active: a load's frames and no others. r9 to r11 and cr7 are dead here;
# the call after takes them all. The load done, the arrival not up and the
# player playing (2): the player goes to 3 as MoviePlayer_update's own end and
# its skip put it there, the state new and the game time it began, and fades to
# black as at the cinematic's end.
own_departure:
	lis     r9, SW_FLAG_FAST_LOADS@ha
	lbz     r10, SW_FLAG_FAST_LOADS@l(r9)
	cmpwi   cr7, r10, 0
	beq     cr7, 1f
	lis     r9, TOD_LOAD_DONE@ha
	lbz     r10, TOD_LOAD_DONE@l(r9)
	cmpwi   cr7, r10, 0
	beq     cr7, 1f
	lwz     r10, TOD_LOAD_TYPE@l(r9)
	cmpwi   cr7, r10, 1
	beq     cr7, 1f
	lis     r9, TOD_MP_STATE@ha
	lwz     r10, TOD_MP_STATE@l(r9)
	cmpwi   cr7, r10, 2
	bne     cr7, 1f
	li      r10, 3
	stw     r10, TOD_MP_STATE@l(r9)
	li      r10, 1
	stb     r10, TOD_MP_NEW@l(r9)
	lis     r11, TOD_GAME_TIME@ha
	lwz     r10, TOD_GAME_TIME@l(r11)
	stw     r10, TOD_MP_ENTERED@l(r9)
1:	or      r3, r28, r28
	b       own_departure_ret

# 0x3C57DC in LevelTransition_update, an FMV load's frame with an FMV open,
# under the transition's lwmutex: addi r12, r28, 0x14 before
# FmvPlayer_update(&tier->fmv, 1). r9 to r12, f1 and cr7 are dead; the call
# after takes them. This load's stamp (its FMV start time, never 0): the FMV
# closed as FmvPlayer_update closes it at its end (FmvPlayer_stop, r4 1 as
# there), the list's index run on to its terminating 0, so that the next frame
# finds no FMV left and sets g_levelTransitionFmvDone as the game does, and the
# screen at black (ScreenFade_Set(1.0) through the stub the frame uses; the
# 1.0f is k_loadLevelFadeDone of this TOC); then the frame's own end, the
# unlock at 0x3C56FC.
own_fmv_cut:
	lis     r9, SW_FLAG_FAST_LOADS@ha
	lbz     r10, SW_FLAG_FAST_LOADS@l(r9)
	cmpwi   cr7, r10, 0
	beq     cr7, 9f
	lwz     r10, SW_DATA_FMV_IN@l(r9)
	lis     r11, TOD_FMV_STARTED@ha
	lwz     r11, TOD_FMV_STARTED@l(r11)
	cmpw    cr7, r10, r11
	bne     cr7, 9f
	addi    r3, r28, 0x14
	li      r4, 1
	bl      tod_fmv_stop
	nop
	lwz     r9, 0x2BB4(r2)          # &g_levelTransitionFmvIndex
	lwz     r10, 0x2B9C(r2)         # &g_levelTransitionFmvList
	lwz     r10, 0(r10)
	lwz     r11, 0(r9)
1:	slwi    r12, r11, 2
	lwzx    r12, r10, r12
	cmpwi   cr7, r12, 0
	beq     cr7, 2f
	addi    r11, r11, 1
	b       1b
2:	stw     r11, 0(r9)
	lfs     f1, -0xB44(r2)          # 1.0f
	bl      tod_fade_set_stub
	ld      r2, 0x28(r1)
	b       tod_lt_unlock
9:	addi    r12, r28, 0x14
	b       own_fmv_cut_ret

# 0x3C58C8 in LevelTransition_waitLoad, the loader thread, once
# StageLevelHelper_loadLevelData has returned on an FMV load: lwz r9,
# 0x2BBC(r2), &g_levelTransitionFmvDone, for the wait that follows. r9, r10
# and cr7 are dead (the call at 0x3C58DC takes them). The stamp is this load's
# FMV start time, which LevelTransition_begin took from the time base (never
# 0) before this thread was given the tier.
own_fmv_in:
	lis     r9, SW_FLAG_FAST_LOADS@ha
	lbz     r10, SW_FLAG_FAST_LOADS@l(r9)
	cmpwi   cr7, r10, 0
	beq     cr7, 1f
	lis     r10, TOD_FMV_STARTED@ha
	lwz     r10, TOD_FMV_STARTED@l(r10)
	stw     r10, SW_DATA_FMV_IN@l(r9)
1:	lwz     r9, 0x2BBC(r2)
	b       own_fmv_in_ret

	.section .site.own_departure, "ax"
	b       own_departure
own_departure_ret:

	.section .site.own_fmv_cut, "ax"
	b       own_fmv_cut
own_fmv_cut_ret:

	.section .site.own_fmv_in, "ax"
	b       own_fmv_in
own_fmv_in_ret:
