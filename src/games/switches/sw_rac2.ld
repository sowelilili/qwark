/*
 * Ratchet & Clank 2 (NPEA00386): where the code switches go, and the game's own
 * addresses they need. sw_rac2.s says what each piece is; sw_rac1.ld says how
 * the link turns these addresses into branch words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * Build 53: the last function of the dead library span the savefile helper
 * starts (0x113EC64..0x11414FC, src/games/sfhelper/sf_rac2.h), from its third
 * instruction to where the span ends. The helper's own limit, SF_CAVE_END, is
 * where this starts.
 */
SW_CAVE_START = 0x01141400;
SW_CAVE_END   = 0x011414FC;

/* The game function the fast-load site calls. */
rac2_load_screen = 0x00BE9308;

SECTIONS
{
	.cave.fast_loads     0x01141400 : { *(.cave.fast_loads) }
	.cave.infinite_ammo  0x01141420 : { *(.cave.infinite_ammo) }

	.site.infinite_ammo  0x00B30C7C : { *(.site.infinite_ammo) }
	.site.fast_loads     0x00BEA8A0 : { *(.site.fast_loads) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
