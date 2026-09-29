/*
 * Ratchet: Deadlocked (NPEA00423): where the code switches go, and the game's
 * own addresses they need. sw_rac4.s says what each piece is; sw_rac1.ld says
 * how the link turns these addresses into branch words.
 */
OUTPUT_FORMAT("elf64-powerpc-celloslv2")

/*
 * The dead lobby code below the savefile helper, up to where its cave starts
 * (SF_CAVE_STUB in src/games/sfhelper/sf_rac4.h).
 */
SW_CAVE_START = 0x00667BCC;
SW_CAVE_END   = 0x00667F70;

/* The game function the first fast-load site calls: memcard_Save. */
rac4_memcard_save = 0x003C8D54;

SECTIONS
{
	/* Back to back, in the order of the sites they serve. */
	.cave.refill_ammo       0x00667BCC : { *(.cave.refill_ammo) }
	.cave.fast_loads_1      0x00667BE8 : { *(.cave.fast_loads_1) }
	.cave.fast_loads_2      0x00667C00 : { *(.cave.fast_loads_2) }
	.cave.crash_dropship_1  0x00667C18 : { *(.cave.crash_dropship_1) }
	.cave.crash_dropship_2  0x00667C38 : { *(.cave.crash_dropship_2) }
	.cave.crash_dropship_3  0x00667C58 : { *(.cave.crash_dropship_3) }
	.cave.crash_dropship_4  0x00667C78 : { *(.cave.crash_dropship_4) }
	.cave.crash_dropship_5  0x00667C98 : { *(.cave.crash_dropship_5) }
	.cave.crash_dropship_6  0x00667CB0 : { *(.cave.crash_dropship_6) }
	.cave.crash_vendor_1    0x00667CC8 : { *(.cave.crash_vendor_1) }
	.cave.crash_vendor_2    0x00667CE8 : { *(.cave.crash_vendor_2) }
	.cave.crash_vendor_3    0x00667D08 : { *(.cave.crash_vendor_3) }
	.cave.crash_vendor_4    0x00667D28 : { *(.cave.crash_vendor_4) }
	.cave.crash_bot         0x00667D48 : { *(.cave.crash_bot) }
	/* lvx ignores the low four bits of its address. */
	.cave.crash_bot_vec     0x00667D70 : { *(.cave.crash_bot_vec) }

	.site.refill_ammo       0x001D7D18 : { *(.site.refill_ammo) }
	.site.fast_loads_1      0x002A8AF4 : { *(.site.fast_loads_1) }
	.site.fast_loads_2      0x002A8C50 : { *(.site.fast_loads_2) }
	.site.crash_dropship_1  0x0051E1E8 : { *(.site.crash_dropship_1) }
	.site.crash_dropship_2  0x0051E200 : { *(.site.crash_dropship_2) }
	.site.crash_dropship_3  0x0051E21C : { *(.site.crash_dropship_3) }
	.site.crash_dropship_4  0x0051E228 : { *(.site.crash_dropship_4) }
	.site.crash_dropship_5  0x0051E240 : { *(.site.crash_dropship_5) }
	.site.crash_dropship_6  0x0051E24C : { *(.site.crash_dropship_6) }
	.site.crash_vendor_1    0x00618008 : { *(.site.crash_vendor_1) }
	.site.crash_vendor_2    0x00618024 : { *(.site.crash_vendor_2) }
	.site.crash_vendor_3    0x00618034 : { *(.site.crash_vendor_3) }
	.site.crash_vendor_4    0x0061806C : { *(.site.crash_vendor_4) }
	.site.crash_bot         0x008088EC : { *(.site.crash_bot) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
