/*
 * Ratchet & Clank 3 (NPEA00387): where the code switches go, and the game's own
 * addresses they need. sw_rac3.s says what each piece is; sw_rac1.ld says how the
 * link turns these addresses into branch words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * What the savefile helper leaves free of its dead function. Build 53: the
 * helper may run up to 0x980F00 (SF_CAVE_END in src/games/sfhelper/sf_rac3.h),
 * so the switches start there.
 */
SW_CAVE_START = 0x00980F00;
SW_CAVE_END   = 0x00981064;

/*
 * The game's own code the fast-load trampolines call or go back to: the pass of
 * the transition loop after its two routines and after its frame, the frame's
 * first call, the screen fade and Transition_UnloadSounds.
 */
rac3_fastload_resume          = 0x0027B538;
rac3_fastload_skip            = 0x0027B570;
rac3_frame_begin              = 0x0011AD44;
rac3_screen_fade              = 0x00123988;
rac3_transition_unload_sounds = 0x00278A00;

/*
 * Build 53 made fast loads over, and their trampolines start at 0x980F80 rather
 * than at 0x980F20, where the build's first one was: a game that still carries
 * that one's patch beside this one keeps a first trampoline its site
 * (0x27B428) can run, rather than branching into the middle of these.
 */
SECTIONS
{
	.cave.freeze_ammo      0x00980F00 : { *(.cave.freeze_ammo) }
	.cave.fast_loads       0x00980F80 : { *(.cave.fast_loads) }
	.cave.fast_loads_draw  0x00981020 : { *(.cave.fast_loads_draw) }

	.site.freeze_ammo      0x00182A88 : { *(.site.freeze_ammo) }
	.site.fast_loads       0x0027B49C : { *(.site.fast_loads) }
	.site.fast_loads_draw  0x0027B550 : { *(.site.fast_loads_draw) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
