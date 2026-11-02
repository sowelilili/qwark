/*
 * Ratchet & Clank Future: Tools of Destruction (NPEA00452 v01.00): where the
 * code switches go, and the game's own addresses they need. sw_tod.s says what
 * each piece is; sw_rac1.ld says how the link turns these addresses into branch
 * words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * A dead function of the shipped game, 0x3EB578..0x3EB708 (Ghidra
 * LevelList_fill_unused): it fills a menu list with the level names, nothing
 * branches to it, and the one descriptor that names it (0x88CAF0) is named by
 * no word of the executable (src/games/menuhelper/check.py, check_tod).
 */
SW_CAVE_START = 0x003EB578;
SW_CAVE_END   = 0x003EB708;

/*
 * LoadScreen_Update's own code the trampoline goes back to: the word after its
 * site, its epilogue, and its next long-load screen (the LoadScreen_PickType
 * call). Then what the cut calls, all in LoadScreen_Update's TOC range (r2 is
 * 0x8AB078 there): the screen fade's set, the level exit's cinematic stop
 * (li r3, 0 then the stop of every cinematic) and the cinematic player's reset
 * every Level_StartGameplay makes.
 */
tod_ls_update_next    = 0x0025E58C;
tod_ls_update_return  = 0x0025E5FC;
tod_ls_update_filler  = 0x0025E614;
tod_screen_fade_set   = 0x0025ED38;
tod_movie_stop_all    = 0x00084EF4;
tod_movie_reset       = 0x00084E28;

/*
 * Revision 1.29, qwark's own pieces (sw_tod.s): FmvPlayer_stop, the stub
 * through which LevelTransition_update calls ScreenFade_Set (r2 to
 * LoadScreen's TOC and back), and LevelTransition_update's frame end, the
 * transition lwmutex's unlock, where its own FMV path goes once the FMV is
 * updated.
 */
tod_fmv_stop          = 0x003BE718;
tod_fade_set_stub     = 0x00267780;
tod_lt_unlock         = 0x003C56FC;

SECTIONS
{
	.cave.fast_loads      0x003EB578 : { *(.cave.fast_loads) }
	/* Revision 1.29: qwark's own pieces, straight after the switch. */
	.cave.own_fast_loads  0x003EB5F8 : { *(.cave.own_fast_loads) }

	.site.fast_loads      0x0025E588 : { *(.site.fast_loads) }
	.site.own_departure   0x003C5744 : { *(.site.own_departure) }
	.site.own_fmv_cut     0x003C57DC : { *(.site.own_fmv_cut) }
	.site.own_fmv_in      0x003C58C8 : { *(.site.own_fmv_in) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
