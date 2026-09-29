/*
 * Ratchet & Clank 2 (NPEA00386): where the code switches go, and the game's own
 * addresses they need. sw_rac2.s says what each piece is; sw_rac1.ld says how
 * the link turns these addresses into branch words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * The tail of the dead function at 0x22A368, past the savefile helper and
 * bl-mod's cave in its head.
 */
SW_CAVE_START = 0x00233200;
SW_CAVE_END   = 0x00233308;

/* The game function the fast-load site calls. */
rac2_load_screen = 0x00BE9308;

SECTIONS
{
	.cave.fast_loads     0x00233200 : { *(.cave.fast_loads) }
	.cave.infinite_ammo  0x00233220 : { *(.cave.infinite_ammo) }

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
