/*
 * Ratchet & Clank 3 (NPEA00387): where the code switches go, and the game's own
 * address they need. sw_rac3.s says what each piece is; sw_rac1.ld says how the
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

/* The game function the fast-load trampoline calls: Transition_UnloadSounds. */
rac3_transition_unload_sounds = 0x00278A00;

SECTIONS
{
	.cave.freeze_ammo  0x00980F00 : { *(.cave.freeze_ammo) }
	.cave.fast_loads   0x00980F20 : { *(.cave.fast_loads) }

	.site.freeze_ammo  0x00182A88 : { *(.site.freeze_ammo) }
	.site.fast_loads   0x0027B428 : { *(.site.fast_loads) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
