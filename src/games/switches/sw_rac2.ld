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

/*
 * Build 53, the crash guards: UpdateMobyGrids' epilogue, which restores the
 * registers its prologue saved and returns, and the ship update's step to its
 * next part (r27 and r28 advanced, the loop test).
 */
rac2_grid_done      = 0x00086BD0;
rac2_ship_next_part = 0x00BDF570;

SECTIONS
{
	.cave.fast_loads     0x01141400 : { *(.cave.fast_loads) }
	.cave.infinite_ammo  0x01141420 : { *(.cave.infinite_ammo) }
	.cave.grid_guard     0x01141440 : { *(.cave.grid_guard) }
	.cave.ship_parts     0x01141470 : { *(.cave.ship_parts) }

	.site.grid_guard     0x00086960 : { *(.site.grid_guard) }
	.site.infinite_ammo  0x00B30C7C : { *(.site.infinite_ammo) }
	.site.ship_parts     0x00BDF30C : { *(.site.ship_parts) }
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
