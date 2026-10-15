/*
 * Ratchet & Clank 3 (NPEA00387): where the code switch goes. sw_rac3.s says what
 * it is; sw_rac1.ld says how the link turns these addresses into branch words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * What the savefile helper leaves free of its dead function. Build 53: the
 * helper may run up to 0x980F00 (SF_CAVE_END in src/games/sfhelper/sf_rac3.h),
 * so the switches start there.
 */
SW_CAVE_START = 0x00980F00;
SW_CAVE_END   = 0x00981064;

SECTIONS
{
	.cave.freeze_ammo  0x00980F00 : { *(.cave.freeze_ammo) }

	.site.freeze_ammo  0x00182A88 : { *(.site.freeze_ammo) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
