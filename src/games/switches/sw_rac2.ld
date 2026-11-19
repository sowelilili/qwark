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
	/* Build 59: the Clank Zapper's trampoline, after the crash guards. */
	.cave.clank_zapper   0x011414A0 : { *(.cave.clank_zapper) }

	.site.grid_guard     0x00086960 : { *(.site.grid_guard) }
	.site.ship_parts     0x00BDF30C : { *(.site.ship_parts) }
	.site.fast_loads     0x00BEA8A0 : { *(.site.fast_loads) }

	/* Build 59: consume_ammo's 37 stores and the Clank Zapper's 74 decrements (sw_rac2.s). */
	.site.ammo_00        0x000A1F74 : { *(.site.ammo_00) }
	.site.ammo_01        0x0010646C : { *(.site.ammo_01) }
	.site.ammo_02        0x00125CC4 : { *(.site.ammo_02) }
	.site.ammo_03        0x001708BC : { *(.site.ammo_03) }
	.site.ammo_04        0x001C395C : { *(.site.ammo_04) }
	.site.ammo_05        0x001FC86C : { *(.site.ammo_05) }
	.site.ammo_06        0x00247024 : { *(.site.ammo_06) }
	.site.ammo_07        0x0028EC84 : { *(.site.ammo_07) }
	.site.ammo_08        0x002D8E5C : { *(.site.ammo_08) }
	.site.ammo_09        0x00323A4C : { *(.site.ammo_09) }
	.site.ammo_10        0x0036E644 : { *(.site.ammo_10) }
	.site.ammo_11        0x003BF904 : { *(.site.ammo_11) }
	.site.ammo_12        0x003F8994 : { *(.site.ammo_12) }
	.site.ammo_13        0x00441144 : { *(.site.ammo_13) }
	.site.ammo_14        0x004A3ECC : { *(.site.ammo_14) }
	.site.ammo_15        0x004C1BEC : { *(.site.ammo_15) }
	.site.ammo_16        0x00522CAC : { *(.site.ammo_16) }
	.site.ammo_17        0x00542BB4 : { *(.site.ammo_17) }
	.site.ammo_18        0x0058EBFC : { *(.site.ammo_18) }
	.site.ammo_19        0x005DA6C4 : { *(.site.ammo_19) }
	.site.ammo_20        0x00628654 : { *(.site.ammo_20) }
	.site.ammo_21        0x0067DF3C : { *(.site.ammo_21) }
	.site.ammo_22        0x006B9D14 : { *(.site.ammo_22) }
	.site.ammo_23        0x00706DB4 : { *(.site.ammo_23) }
	.site.ammo_24        0x00764334 : { *(.site.ammo_24) }
	.site.ammo_25        0x0079219C : { *(.site.ammo_25) }
	.site.ammo_26        0x007EFD94 : { *(.site.ammo_26) }
	.site.ammo_27        0x0081D2B4 : { *(.site.ammo_27) }
	.site.ammo_28        0x00867C54 : { *(.site.ammo_28) }
	.site.ammo_29        0x008B1354 : { *(.site.ammo_29) }
	.site.ammo_30        0x00918FEC : { *(.site.ammo_30) }
	.site.ammo_31        0x009396EC : { *(.site.ammo_31) }
	.site.ammo_32        0x0098D12C : { *(.site.ammo_32) }
	.site.ammo_33        0x009DA1FC : { *(.site.ammo_33) }
	.site.ammo_34        0x00A06FE4 : { *(.site.ammo_34) }
	.site.ammo_35        0x00A51BDC : { *(.site.ammo_35) }
	.site.ammo_36        0x00A9C7D4 : { *(.site.ammo_36) }
	.site.zapper_00      0x000A2114 : { *(.site.zapper_00) }
	.site.zapper_01      0x000A29F8 : { *(.site.zapper_01) }
	.site.zapper_02      0x0010660C : { *(.site.zapper_02) }
	.site.zapper_03      0x00106EF0 : { *(.site.zapper_03) }
	.site.zapper_04      0x00125E64 : { *(.site.zapper_04) }
	.site.zapper_05      0x00126748 : { *(.site.zapper_05) }
	.site.zapper_06      0x00170A5C : { *(.site.zapper_06) }
	.site.zapper_07      0x00171340 : { *(.site.zapper_07) }
	.site.zapper_08      0x001C3AFC : { *(.site.zapper_08) }
	.site.zapper_09      0x001C43E0 : { *(.site.zapper_09) }
	.site.zapper_10      0x001FCA0C : { *(.site.zapper_10) }
	.site.zapper_11      0x001FD2F0 : { *(.site.zapper_11) }
	.site.zapper_12      0x002471C4 : { *(.site.zapper_12) }
	.site.zapper_13      0x00247AA8 : { *(.site.zapper_13) }
	.site.zapper_14      0x0028EE24 : { *(.site.zapper_14) }
	.site.zapper_15      0x0028F708 : { *(.site.zapper_15) }
	.site.zapper_16      0x002D8FFC : { *(.site.zapper_16) }
	.site.zapper_17      0x002D98E0 : { *(.site.zapper_17) }
	.site.zapper_18      0x00323BEC : { *(.site.zapper_18) }
	.site.zapper_19      0x003244D0 : { *(.site.zapper_19) }
	.site.zapper_20      0x0036E7E4 : { *(.site.zapper_20) }
	.site.zapper_21      0x0036F0C8 : { *(.site.zapper_21) }
	.site.zapper_22      0x003BFAA4 : { *(.site.zapper_22) }
	.site.zapper_23      0x003C0388 : { *(.site.zapper_23) }
	.site.zapper_24      0x003F8B34 : { *(.site.zapper_24) }
	.site.zapper_25      0x003F9418 : { *(.site.zapper_25) }
	.site.zapper_26      0x004412E4 : { *(.site.zapper_26) }
	.site.zapper_27      0x00441BC8 : { *(.site.zapper_27) }
	.site.zapper_28      0x004A406C : { *(.site.zapper_28) }
	.site.zapper_29      0x004A4950 : { *(.site.zapper_29) }
	.site.zapper_30      0x004C1D8C : { *(.site.zapper_30) }
	.site.zapper_31      0x004C2670 : { *(.site.zapper_31) }
	.site.zapper_32      0x00522E4C : { *(.site.zapper_32) }
	.site.zapper_33      0x00523730 : { *(.site.zapper_33) }
	.site.zapper_34      0x00542D54 : { *(.site.zapper_34) }
	.site.zapper_35      0x00543638 : { *(.site.zapper_35) }
	.site.zapper_36      0x0058ED9C : { *(.site.zapper_36) }
	.site.zapper_37      0x0058F680 : { *(.site.zapper_37) }
	.site.zapper_38      0x005DA864 : { *(.site.zapper_38) }
	.site.zapper_39      0x005DB148 : { *(.site.zapper_39) }
	.site.zapper_40      0x006287F4 : { *(.site.zapper_40) }
	.site.zapper_41      0x006290D8 : { *(.site.zapper_41) }
	.site.zapper_42      0x0067E0DC : { *(.site.zapper_42) }
	.site.zapper_43      0x0067E9C0 : { *(.site.zapper_43) }
	.site.zapper_44      0x006B9EB4 : { *(.site.zapper_44) }
	.site.zapper_45      0x006BA798 : { *(.site.zapper_45) }
	.site.zapper_46      0x00706F54 : { *(.site.zapper_46) }
	.site.zapper_47      0x00707838 : { *(.site.zapper_47) }
	.site.zapper_48      0x007644D4 : { *(.site.zapper_48) }
	.site.zapper_49      0x00764DB8 : { *(.site.zapper_49) }
	.site.zapper_50      0x0079233C : { *(.site.zapper_50) }
	.site.zapper_51      0x00792C20 : { *(.site.zapper_51) }
	.site.zapper_52      0x007EFF34 : { *(.site.zapper_52) }
	.site.zapper_53      0x007F0818 : { *(.site.zapper_53) }
	.site.zapper_54      0x0081D454 : { *(.site.zapper_54) }
	.site.zapper_55      0x0081DD38 : { *(.site.zapper_55) }
	.site.zapper_56      0x00867DF4 : { *(.site.zapper_56) }
	.site.zapper_57      0x008686D8 : { *(.site.zapper_57) }
	.site.zapper_58      0x008B14F4 : { *(.site.zapper_58) }
	.site.zapper_59      0x008B1DD8 : { *(.site.zapper_59) }
	.site.zapper_60      0x0091918C : { *(.site.zapper_60) }
	.site.zapper_61      0x00919A70 : { *(.site.zapper_61) }
	.site.zapper_62      0x0093988C : { *(.site.zapper_62) }
	.site.zapper_63      0x0093A170 : { *(.site.zapper_63) }
	.site.zapper_64      0x0098D2CC : { *(.site.zapper_64) }
	.site.zapper_65      0x0098DBB0 : { *(.site.zapper_65) }
	.site.zapper_66      0x009DA39C : { *(.site.zapper_66) }
	.site.zapper_67      0x009DAC80 : { *(.site.zapper_67) }
	.site.zapper_68      0x00A07184 : { *(.site.zapper_68) }
	.site.zapper_69      0x00A07A68 : { *(.site.zapper_69) }
	.site.zapper_70      0x00A51D7C : { *(.site.zapper_70) }
	.site.zapper_71      0x00A52660 : { *(.site.zapper_71) }
	.site.zapper_72      0x00A9C974 : { *(.site.zapper_72) }
	.site.zapper_73      0x00A9D258 : { *(.site.zapper_73) }

	/DISCARD/ : {
		*(.opd)
		*(.toc)
		*(.toc1)
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
