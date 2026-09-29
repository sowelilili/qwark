/*
 * Ratchet & Clank (NPEA00385): where the code switches go, and the game's own
 * addresses they need. sw_rac1.s says what each piece is.
 *
 * Every .cave.* section is placed at its trampoline's address and every .site.*
 * section at the instruction it replaces, so the branch into each trampoline and
 * the branch back out of it are both computed here, by the link, from the
 * addresses below. build.sh fails a cave that strays outside
 * SW_CAVE_START..SW_CAVE_END, the dead function the trampolines live in.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/* The dead function at 0x4F69B8, to the end of the dead span it sits in. */
SW_CAVE_START = 0x004F69B8;
SW_CAVE_END   = 0x004F7120;

/* The game function the first fast-load site calls: the loading screen. */
rac1_loading_screen = 0x001639A8;

SECTIONS
{
	.cave.fast_loads_1     0x004F6C00 : { *(.cave.fast_loads_1) }
	.cave.fast_loads_2     0x004F6C20 : { *(.cave.fast_loads_2) }
	.cave.fast_loads_3     0x004F6C40 : { *(.cave.fast_loads_3) }
	.cave.infinite_ammo    0x004F6C60 : { *(.cave.infinite_ammo) }
	.cave.infinite_health  0x004F6C80 : { *(.cave.infinite_health) }

	.site.infinite_health  0x0007F55C : { *(.site.infinite_health) }
	.site.infinite_ammo    0x000AA2DC : { *(.site.infinite_ammo) }
	.site.fast_loads_2     0x00160060 : { *(.site.fast_loads_2) }
	.site.fast_loads_3     0x001641F8 : { *(.site.fast_loads_3) }
	.site.fast_loads_1     0x00165490 : { *(.site.fast_loads_1) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
