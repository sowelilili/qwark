/*
 * The wire contract from docs/PROTOCOL.md, version 1, revision 1.2. Nothing here
 * may change without changing that file and QWARK_PROTOCOL_VERSION with it.
 *
 * Revision 1.1: SessionInfo carries sixteen readouts and is 164 bytes, Feature
 * gained a `readout` byte at offset 5, and LEVELFLAGS_SET was added.
 * Revision 1.2: Feature flags bit2 SAVE_ASIDE and bit3 LOAD_ASIDE.
 * Revision 1.3: Feature flags bit4 LIVE, and UNLOCK_LIST carries four
 * UnlockFieldDesc rows that name and type the four per-entry value slots.
 * Revision 1.4: the autosplit event stream. AUTOSPLIT_EVENTS and
 * AUTOSPLIT_DESCRIBE in the 0x00A0 block, plus a 20-byte 'QE' datagram pushed
 * to every telemetry subscriber the moment an event happens.
 * Revision 1.5: the Event's second word is `time_ms` rather than a tick count,
 * the kinds gained LOAD_START and LOAD_END, and EventDesc grew to 32 bytes with
 * a `param_us` that carries the game-time adjustment the old ASL scripts made.
 * Revision 1.6: SessionInfo flags bit1 EMULATOR and bit2 NO_CODE_PATCHES.
 * Revision 1.7: the first of Feature's two pad bytes is now `bits`, the width of
 * the field behind a VALUE, and Feature flags bit5 SIGNED says that field is
 * two's complement in that width.
 * Revision 1.8: COMBO_SUSPEND.
 * Revision 1.9: the savefile block, SAVEFILE_INFO / READ / WRITE at 0x00B0. The
 * save no longer travels as a file: qwark installs one helper per game, the
 * helper parks the save in a RAM buffer, and these three ops stream that buffer.
 * Revision 1.10: the savefile library moves onto the console. FILE_RENAME, the
 * five library ops at 0x00B3, and a SAVEFILE_INFO that reports the transfer
 * qwark is running between a file and the aside buffer on its own tick thread.
 * Revision 1.11: QWARK_MAX_PAYLOAD is 16384. Every request and every reply
 * carries at most that, so the bulk ops that named 65536 - MEM_READ, FILE_READ,
 * FILE_WRITE, SAVEFILE_READ, SAVEFILE_WRITE - are capped at 16384 too. No
 * opcode, structure or status changed with it: it is the size of one frame.
 * Revision 1.12: COMBO_ENABLE, the persistent on/off for every combo, and
 * SessionInfo flags bit3 COMBOS_OFF, which reports it.
 * Revision 1.13: POS_EDIT at 0x004A and POS_STORE at 0x004B, the two ways a
 * client writes a position slot without standing in it. POS_EDIT replaces the
 * three coordinates of a slot that already exists and leaves the rest of the
 * blob alone; POS_STORE replaces the whole blob, and takes only a blob of the
 * running game's own length.
 * Revision 1.14 is reserved for opcodes 0x0056 and 0x0057 on a parked branch
 * and is not in this build.
 * Revision 1.15: SAVEFILE_PATCH at 0x00B8, the savefile helper as the words an
 * RPCS3 patch writes, and a savefile block that works on a platform refusing
 * code patches once such a patch has put the helper in the game.
 * Revision 1.16: MOD_PATCH at 0x0066, one mod as the words and bytes an RPCS3
 * patch writes, in SAVEFILE_PATCH's layout. On a platform refusing code patches
 * MOD_LIST's LOADED bit says the mod is in game memory, put there by such a
 * patch at load, and the new CHECKING bit says qwark has not looked yet.
 * Revision 1.17: SWITCH_PATCH at 0x0025, the running game's code switches as
 * the words and bytes an RPCS3 patch writes, in SAVEFILE_PATCH's layout, and
 * SessionInfo flags bit4 CODE_SWITCHES, set on a platform refusing code patches
 * while such a patch has put them in the game. Every WRITES_CODE toggle then
 * works there, as a byte write to its flag.
 * Revision 1.18: the three patch ops and the looks behind them are built only
 * with QWARK_EMULATOR (src/plat/plat.h). The PS3 module answers SAVEFILE_PATCH,
 * MOD_PATCH and SWITCH_PATCH UNSUPPORTED and never sets flags bit4.
 * Revision 1.19: MOD_PATCH is paged, so a mod bigger than one frame can still
 * be written as an RPCS3 patch. The request is u8 index, u8 pad, u16 page (an
 * old request's zeros are page 0), and the reply's pad after n is u16 pages.
 * Revision 1.21: manip categories, MANIP_LIST / SELECT / OPTIONS / APPLY in the
 * 0x00D0 block (src/core/manips.h). The old manip buttons and toggles are
 * retired, ids and all.
 * Revision 1.22: the console menu (src/core/menu.h). Combo action 5 OPEN_MENU,
 * the first combo with a default mask, which COMBO_LIST therefore reports as a
 * sixth entry; config keys combo.menu and menu.enabled. No opcode or structure
 * changed: the menu's mailbox lives in game memory, between qwark and the
 * renderer it writes into the game, and is described in PROTOCOL.md section 9
 * for information only.
 * Revision 1.22, build 54: VSH_MEMORY at 0x0007 and SessionInfo flags bit5,
 * the VSH's free memory as the module reads it (PROTOCOL.md 5.1.1).
 * Revision 1.23, build 55: MENU_HELP at 0x0092 and SessionInfo flags bit6
 * MENU_HELP_OFF, config key menu.help: whether the console menu takes over the
 * game's Help row, swapped in a running game at its next safe moment
 * (PROTOCOL.md 5.10 and 9.2). Combo action 6 TOGGLE_FAST_LOADS, config key
 * combo.fastloads with no mask out of the box, so COMBO_LIST has seven entries.
 * Also build 55: the COMBO_ENABLE switch holds the OPEN_MENU combo off like the
 * rest, which it spared in revision 1.22; the pause menu's Trainer row is the
 * way in then, unless MENU_HELP has given that row back to the game's Help.
 * Revision 1.24, build 56: settings profiles, one per console user, behind the
 * console's switch `profiles.enabled` (src/core/config.h). PROFILE_INFO,
 * PROFILE_ENABLE and COMBO_OVERRIDE at 0x0093..0x0095; SessionInfo byte 29,
 * a pad byte until now, is `flags2`, whose bits 1 to 3 are the profile's,
 * and byte 30 `profile_seq`; the byte after each COMBO_LIST action, a pad
 * byte until now, is its flags, bit0 OVERRIDDEN. PROTOCOL.md 5.10.1.
 * Revision 1.24, test builds (same build 56): SessionInfo flags2 bit4 DEV,
 * set by a test build (QWARK_DEV, the makefile's default), and a u32 build ID
 * after HELLO's SessionInfo (QWARK_BUILD_ID, the CRC of the sources), so a
 * client can tell one test build of a number from another. PROTOCOL.md 3.1.1.
 * Revision 1.30: the run-start warnings (src/core/runwarn.h). RUN_WARN
 * at 0x0096, config key run.warnings; SessionInfo byte 31, a pad byte until
 * now, is `flags3`, bit0 RUN_WARN (the module has them) and bit1 RUN_WARN_OFF;
 * MOD_LIST's flags bit6 LEGAL and bit7 ILLEGAL, a patch.txt's `#- legal:`.
 * PROTOCOL.md 5.7, 5.10 and 5.10.2.
 * Revision 1.32, build 58: the telemetry rate. TELEMETRY_RATE at 0x0097,
 * config key telemetry.hz (30, 60 or 120; 30 when the file does not name it),
 * console-wide; SessionInfo flags3 bit2 TELEMETRY_RATE (the module has it)
 * and bits 3-4 the rate in force (TELEMETRY_RATE_CODE_*). PROTOCOL.md 4 and
 * 5.10.3.
 * Revision 1.33: run.warnings has a third value, 2, the cheats line alone
 * (RUN_WARN_CHEATS_ONLY); RUN_WARN takes 0, 1 or 2. Apply toggles on game
 * reboot: config key reboot.apply (REBOOT_APPLY_ALWAYS, _ASK, _NEVER), which
 * decides what becomes of the previous-session record at a same-game reboot,
 * and REBOOT_APPLY at 0x0098. SessionInfo flags3 bit5 SETTINGS_133 (the
 * module has both) and bit6 RUN_WARN_CHEATS (cheats only is in force); bit7
 * is 0. telemetry.hz reads as 60 when the file does not name it. PROTOCOL.md
 * 5.10.2, 5.10.3 and 5.10.4. The same revision: an autosplit Event's u16 at
 * +10 is its flags (AUTOSPLIT_EV_REVISIT) and EventDesc flags bit5 REVISIT
 * (AUTOSPLIT_FLAG_REVISIT), Tools of Destruction's revisits. PROTOCOL.md 8.
 * Revision 1.34, build 59: combo actions 7 to 13, one per page of the
 * Trainer's root (COMBO_PAGE_*), config keys combo.page.<page>, written only
 * while bound; COMBO_LIST has fourteen entries and after them the pages'
 * names (COMBO_LIST's trailer, PROTOCOL.md 5.9).
 */
#ifndef QWARK_PROTO_H
#define QWARK_PROTO_H

#include "../plat/plat.h"

#define QWARK_PROTOCOL_VERSION  1

/*
 * The module build number, reported as SessionInfo.qwark_version. It is not the
 * protocol version: the wire contract can stay put while the feature tables or
 * some user-visible behaviour move under it. Bump it whenever they do, so a
 * client that ships its own copy of the tables can tell that the SPRX on the
 * console is older than the one it was built against and say so.
 */
#define QWARK_BUILD             59

/*
 * Revision 1.24: which build of that number this is (PROTOCOL.md 3.1.1).
 * QWARK_DEV is 1 in a test build, which SessionInfo flags2 bit4 reports, and
 * QWARK_BUILD_ID is the CRC of the module's sources (tools/buildid.py), which
 * HELLO carries after SessionInfo; 0 is no ID. The makefile sets both, a test
 * build unless it is told QWARK_DEV=0 (`make release`), which is all that
 * tells a release from a test build since build 57; build-host.sh sets
 * them for qwark-host.exe, and qwark-rpcs3.exe, which the client starts beside
 * itself, keeps these: never DEV, no ID.
 */
#ifndef QWARK_DEV
#define QWARK_DEV               0
#endif
#ifndef QWARK_BUILD_ID
#define QWARK_BUILD_ID          0
#endif

/*
 * Revision 1.24: build 56's preview features (PROTOCOL.md 3.1.2): the settings
 * profiles (PROFILE_INFO, PROFILE_ENABLE, COMBO_OVERRIDE, flags2 bits 1 to 3,
 * byte 30), the USB mods (MOD_USB_LIST, flags2 bit0) and the console menu's
 * Mods page. QWARK_PREVIEW 1 builds them in; 0 compiles them out, and the
 * module answers their ops UNSUPPORTED, never sets their bits, never looks at
 * /dev_usb00N and never reads or writes /dev_hdd0/qwark/profiles: settings,
 * combos and mods are 1.4.1's. Build 56's release left them out; build 57
 * ships them in its release, so this is 1 unless told otherwise, whatever
 * QWARK_DEV says, and `make release` pins it to 1. A module built with 0 is
 * one no release ships. A feature that is still being tried out later takes a
 * switch of its own that follows QWARK_DEV.
 */
#ifndef QWARK_PREVIEW
#define QWARK_PREVIEW           1
#endif

/* Revision 1.26: QWARK_GAME_MODULES and the others, game modules (src/games/module.h). */
#include "buildmodes.h"

#define QWARK_PORT              9673
/*
 * Revision 1.11. One frame's payload, request or reply. It was 65600 - 64 KB
 * and a header's worth of slack - and every connection allocated three 64 KB
 * pages out of the VSH's small pool to hold one. Two static 16 KB buffers
 * shared by every connection hold a frame of this size instead, so the module
 * asks the console for no memory at all after it has loaded. A frame that
 * announces more than this closes the connection, exactly as 65601 did.
 */
#define QWARK_MAX_PAYLOAD       16384u
#define QWARK_FRAME_HEADER      8u

/* Reply header is u32 length, u16 seq, u16 status: the same 8 bytes. */

/* ------------------------------------------------------------ status codes */

#define ST_OK            0
#define ST_NOT_INGAME    1
#define ST_UNSUPPORTED   2
#define ST_BAD_ARG       3
#define ST_IO_ERROR      4
#define ST_FULL          5
#define ST_UNKNOWN_OP    6
#define ST_NOT_FOUND     7
#define ST_BUSY          8

/* ---------------------------------------------------------- session states */

#define SESSION_XMB      0
#define SESSION_BOOTING  1
#define SESSION_INGAME   2
#define SESSION_QUITTING 3

/* ------------------------------------------------------------------- games */

#define GAME_NONE 0
#define GAME_RAC1 1
#define GAME_RAC2 2
#define GAME_RAC3 3
#define GAME_RAC4 4
/*
 * Revision 1.27: Tools of Destruction (NPEA00452), a game module of its own,
 * tod.sprx, in a test build, a release (since build 57) and the PC builds;
 * only a QWARK_GAME_MODULES 0 module has no ToD at all.
 */
#define GAME_TOD  5

/* --------------------------------------------------------------- opcodes */

#define OP_HELLO             0x0001
#define OP_HEARTBEAT         0x0002
#define OP_NOTIFY            0x0003
#define OP_PREVIOUS_LIST     0x0004
#define OP_PREVIOUS_REAPPLY  0x0005
#define OP_PREVIOUS_DISMISS  0x0006
/*
 * Revision 1.22, build 54. The VSH's free memory as the module last read it:
 * the latest reading, the lowest since the module started with when, where and
 * in which game, and the reading at module start. Request none, reply
 * VSH_MEMORY_SIZE bytes: u32 free, total, lowest, start (bytes), u32 age_s,
 * lowest_age_s, u8 lowest_at (below), u8 lowest_game, u8 pad[2]. Answers
 * UNSUPPORTED where the platform cannot read it (SessionInfo flags bit5 says
 * whether it can). A control op, so a launch does not hold it up. PROTOCOL.md 5.1.1.
 */
#define OP_VSH_MEMORY        0x0007
/*
 * Revision 1.26: GAME_MODULE, the running game's module and the memory gate in
 * front of it (PROTOCOL.md 5.1.2). Answered inline, in any state.
 */
#define OP_GAME_MODULE       0x0008
#define GAME_MODULE_SIZE     40
#define VSH_MEMORY_SIZE      28

/* VSH_MEMORY's lowest_at: the moment a reading was taken at. */
#define VSHMEM_AT_START       1   /* module start, just before the tick thread */
#define VSHMEM_AT_TICK_THREAD 2   /* just after the tick thread was created */
#define VSHMEM_AT_GAME        3   /* a game reached INGAME */
#define VSHMEM_AT_MENU        4   /* the console menu's renderer went into the game */
#define VSHMEM_AT_PERIODIC    5   /* the ten-second reading */

#define OP_SUBSCRIBE         0x0010
#define OP_UNSUBSCRIBE       0x0011
#define OP_GET_STATE         0x0012

#define OP_DESCRIBE          0x0020
#define OP_FEATURE_SET       0x0021
#define OP_FEATURE_TRIGGER   0x0022
#define OP_FEATURE_SET_AUTO  0x0023
#define OP_FEATURE_OPTIONS   0x0024
/*
 * Revision 1.17. The running game's code switches as what an RPCS3 patch writes
 * to put them there, laid out exactly as SAVEFILE_PATCH's reply: u16 n, u16 pad,
 * u32 stamp, n (u32 addr, u32 value) pairs - every cave as consecutive words,
 * caves in address order, then the site branch words in address order - then
 * u16 nb, u16 pad and nb (u32 addr, u8 value, u8 pad[3]) single bytes, every
 * flag byte of the game at 0. The stamp is the CRC-32 of every byte after it.
 * Request none. See PROTOCOL.md 5.12.2.
 */
#define OP_SWITCH_PATCH      0x0025

#define OP_MEM_READ          0x0030
#define OP_MEM_WRITE         0x0031
#define OP_WATCH_ADD         0x0032
#define OP_WATCH_REMOVE      0x0033
#define OP_WATCH_LIST        0x0034
#define OP_FREEZE_ADD        0x0035
#define OP_FREEZE_REMOVE     0x0036
#define OP_FREEZE_LIST       0x0037
#define OP_PATCH_APPLY       0x0038
#define OP_PATCH_REVERT      0x0039
#define OP_PATCH_LIST        0x003A
#define OP_CLEAR_CLIENT      0x003B

#define OP_POS_SELECT        0x0040
#define OP_POS_SAVE          0x0041
#define OP_POS_LOAD          0x0042
#define OP_POS_LIST          0x0043
#define OP_POS_CLEAR         0x0044
#define OP_PLANET_LIST       0x0045
#define OP_PLANET_SELECT     0x0046
#define OP_PLANET_LOAD       0x0047
#define OP_DIE               0x0048
#define OP_MOBY_TABLE        0x0049
#define OP_POS_EDIT          0x004A
#define OP_POS_STORE         0x004B

/*
 * Revision 1.34: MOBY_TABLE's last halfword, a pad until then and 0 in the
 * four original games, is flags. LAST_INDEX: the end word is the index of
 * the last slot the table can have live (s32, negative for none), not a
 * pointer past its end. LIVE_TEST: two more halfwords follow, an offset in
 * a slot and the big-endian u16 a free slot holds there (PROTOCOL.md 5.5).
 */
#define MOBY_FLAG_LAST_INDEX 0x0001
#define MOBY_FLAG_LIVE_TEST  0x0002

#define OP_UNLOCK_LIST       0x0050
#define OP_UNLOCK_SET        0x0051
#define OP_LEVELFLAGS_GET    0x0053
#define OP_LEVELFLAGS_RESET  0x0054
#define OP_LEVELFLAGS_SET    0x0055
/* Revision 1.28: one bit of the region (PROTOCOL.md 5.6). */
#define OP_LEVELFLAGS_BIT    0x0056
/* 0x0057, LEVELFLAGS_NAMES (revision 1.28), is retired (revision 1.29) and never reused. */

#define OP_MOD_LIST          0x0060
#define OP_MOD_LOAD          0x0061
#define OP_MOD_UNLOAD        0x0062
#define OP_MOD_SET_AUTO      0x0063
#define OP_MOD_RESCAN        0x0064
#define OP_MOD_INFO          0x0065
/*
 * Revision 1.16. One mod as what an RPCS3 patch writes to put it in the game:
 * request u8 index (MOD_LIST's), u8 pad[3]; the reply is SAVEFILE_PATCH's
 * layout, below, with every cave as consecutive words, then the mod's own
 * words, then the tail bytes of any cave that is not whole words, in address
 * order. See PROTOCOL.md 5.7.1.
 *
 * Revision 1.19: a page at a time. The request is u8 index, u8 pad, u16 page,
 * and the reply's pad after n is u16 pages (MOD_PATCH_PAGE_ROWS, below).
 */
#define OP_MOD_PATCH         0x0066
/*
 * Revision 1.24, build 56. The mods on the console's USB drives for the title,
 * as available rather than installed: no request, reply u8 n, UsbMod[n] (120
 * bytes a row, MODUSB_WIRE_SIZE in modusb.h). UNSUPPORTED where code cannot be
 * patched. See PROTOCOL.md 5.7.2.
 */
#define OP_MOD_USB_LIST      0x0067

#define OP_FILE_OPEN         0x0070
#define OP_FILE_WRITE        0x0071
#define OP_FILE_READ         0x0072
#define OP_FILE_CLOSE        0x0073
#define OP_FILE_DELETE       0x0074
#define OP_DIR_LIST          0x0075
#define OP_DIR_CREATE        0x0076
#define OP_DIR_DELETE        0x0077
#define OP_USER_ID           0x0078
/*
 * Revision 1.10. The file block's rename, at the end of the block rather than at
 * 0x0075 where the savefile work first drew it: that number has been DIR_LIST
 * since revision 1, and an opcode is never renumbered.
 */
#define OP_FILE_RENAME       0x0079

#define OP_COMBO_SET         0x0080
#define OP_COMBO_LIST        0x0081
#define OP_COMBO_SUSPEND     0x0082
/*
 * Revision 1.12. The user's own switch under all five combos: 0 holds every one
 * of them off until a later 1. It lives in config beside the masks, so it
 * survives a client restart and a console reboot, and it is nothing like the
 * COMBO_SUSPEND hold above - that one belongs to a capture and expires by
 * itself, this one never expires and never changes on its own.
 */
#define OP_COMBO_ENABLE      0x0083

#define OP_CONFIG_RELOAD     0x0090
#define OP_CONFIG_SAVE       0x0091
/*
 * Revision 1.23, build 55. `menu.help`, a config write like COMBO_ENABLE's:
 * u8 on, 1 the console menu's Trainer row takes the place of the game's Help
 * row in its pause menu (the default), 0 the Help row is the game's own while
 * the rest of the menu stays in, so its combo still opens it. Anything but 0
 * or 1 is BAD_ARG; UNSUPPORTED where code cannot be patched, which has no menu.
 * Reply none. SessionInfo flags bit6 reports it. PROTOCOL.md 5.10 and 9.2.
 */
#define OP_MENU_HELP         0x0092
/*
 * Revision 1.24, build 56. Settings profiles (PROTOCOL.md 5.10.1).
 *   PROFILE_INFO    none -> PROFILE_INFO_SIZE bytes: the active profile and
 *                   the signed-in user
 *   PROFILE_ENABLE  u8 on, u8 setup (PROFILE_SETUP_*): the console's switch,
 *                   and the copy, or the empty start, of the signed-in user's
 *                   profile as it goes on
 *   COMBO_OVERRIDE  u8 action, u8 set, u8 pad[2], u32 mask: the active
 *                   profile's own mask for one combo, or (set 0) none
 * A client sends them only while SessionInfo flags2 (byte 29) bit1 is set.
 */
#define OP_PROFILE_INFO      0x0093
#define OP_PROFILE_ENABLE    0x0094
#define OP_COMBO_OVERRIDE    0x0095
/*
 * Revision 1.30. `run.warnings`, a config write like MENU_HELP's:
 * u8 on, 1 the console warns as a run starts with cheats or mods that are not
 * run-legal in the game (the default), 0 it says nothing; since revision 1.33
 * 2, it warns about the cheats alone (RUN_WARN_*). Anything else is BAD_ARG.
 * Answered in any session state and on every platform. Reply none.
 * SessionInfo flags3 reports it. PROTOCOL.md 5.10.2.
 */
#define OP_RUN_WARN          0x0096
#define RUN_WARN_NONE        0
#define RUN_WARN_ALL         1
#define RUN_WARN_CHEATS_ONLY 2
/*
 * Revision 1.32. `telemetry.hz`, a config write like RUN_WARN's: u8 hz,
 * 30, 60 (the default since revision 1.33) or 120, how often the tick
 * thread sends the telemetry packet (every 4, 2 or 1 ticks). Anything else is BAD_ARG. Console-wide, not
 * per user. Answered in any session state and on every platform. Reply none.
 * SessionInfo flags3 bits 3-4 report it. PROTOCOL.md 5.10.3.
 */
#define OP_TELEMETRY_RATE    0x0097
/*
 * Revision 1.33. `reboot.apply`, a per-user config write like RUN_WARN's: u8
 * mode, what a same-game reboot does with the previous-session record (the
 * toggles, mods, freezes and client patches that were on, section 4.1):
 * 0 Always re-applies all of it as the game arrives; 1 Ask (the default)
 * keeps it for a client to offer (flags PREVIOUS_PENDING), as every earlier
 * build did, but drops it when no client is connected at the reboot; 2 Never
 * drops it. The toggles and mods flagged on boot are applied at every
 * boot whatever it says. Anything else is BAD_ARG. With no byte it is a read:
 * the reply is u8 mode. Answered in any session state and on every platform.
 * PROTOCOL.md 5.10.4. 0x0099 is not assigned (BOOT_ASK in a test build of the
 * revision): UNKNOWN_OP.
 */
#define OP_REBOOT_APPLY      0x0098
#define REBOOT_APPLY_ALWAYS  0
#define REBOOT_APPLY_ASK     1
#define REBOOT_APPLY_NEVER   2

#define OP_AUTOSPLIT_EVENTS   0x00A0
#define OP_AUTOSPLIT_DESCRIBE 0x00A1

/*
 * Revision 1.9, the savefile block. See src/core/savefile.h and PROTOCOL.md 5.12.
 * All three install the game's helper on demand, answer UNSUPPORTED where code
 * cannot be patched and NOT_INGAME outside INGAME. Revision 1.15: where code
 * cannot be patched they work once an RPCS3 patch has put the helper in the game
 * at load, and INFO answers there either way.
 */
#define OP_SAVEFILE_INFO      0x00B0
#define OP_SAVEFILE_READ      0x00B1
#define OP_SAVEFILE_WRITE     0x00B2

/*
 * Revision 1.10, the console-side library. The savefile of record lives at
 * /dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav and qwark itself
 * copies between one of those files and the helper's aside buffer, so a save no
 * longer travels over the wire every time it is loaded. Deletes and renames go
 * through FILE_DELETE and FILE_RENAME with the whole path: the client knows the
 * layout, and PROTOCOL.md 5.13 writes it down.
 */
#define OP_SAVEFILE_CATEGORIES 0x00B3
#define OP_SAVEFILE_LIST       0x00B4
#define OP_SAVEFILE_STORE      0x00B5
#define OP_SAVEFILE_RESTORE    0x00B6
#define OP_SAVEFILE_CATEGORY   0x00B7

/*
 * Revision 1.15. The running game's savefile helper as what a patch writes to
 * put it there: u16 n, u16 pad, u32 stamp, then n (u32 addr, u32 value) pairs,
 * every cave as consecutive words and then the hook words; then u16 nb, u16 pad
 * and nb (u32 addr, u8 value, u8 pad[3]) single bytes, the three request bytes
 * a console install zeroes, at 0. The stamp is the CRC-32 of every byte after
 * it, as they went out. It exists for RPCS3, which cannot take a code patch from
 * qwark but applies its own patch files as it loads a game; see PROTOCOL.md
 * 5.12.1 and 3.2.
 */
#define OP_SAVEFILE_PATCH          0x00B8
#define SAVEFILE_PATCH_HEAD        8
#define SAVEFILE_PATCH_ROW         8
#define SAVEFILE_PATCH_BYTES_HEAD  4
#define SAVEFILE_PATCH_BYTE_ROW    8
#define SAVEFILE_PATCH_NBYTES      3

/*
 * Revision 1.16. MOD_PATCH's reply is laid out exactly as SAVEFILE_PATCH's.
 * Revision 1.19: paged, the pad after `n` being `u16 pages` (PROTOCOL.md
 * 5.7.1). A page is one frame, so it carries at most
 * (16384 - 8 - 4) / 8 = 2046 rows, word rows and byte rows together.
 */
#define MOD_PATCH_HEAD             SAVEFILE_PATCH_HEAD
#define MOD_PATCH_ROW              SAVEFILE_PATCH_ROW
#define MOD_PATCH_BYTES_HEAD       SAVEFILE_PATCH_BYTES_HEAD
#define MOD_PATCH_BYTE_ROW         SAVEFILE_PATCH_BYTE_ROW
#define MOD_PATCH_PAGE_ROWS \
	((QWARK_MAX_PAYLOAD - MOD_PATCH_HEAD - MOD_PATCH_BYTES_HEAD) / MOD_PATCH_ROW)

/* Revision 1.17. So is SWITCH_PATCH's. */
#define SWITCH_PATCH_HEAD          SAVEFILE_PATCH_HEAD
#define SWITCH_PATCH_ROW           SAVEFILE_PATCH_ROW
#define SWITCH_PATCH_BYTES_HEAD    SAVEFILE_PATCH_BYTES_HEAD
#define SWITCH_PATCH_BYTE_ROW      SAVEFILE_PATCH_BYTE_ROW

/*
 * SAVEFILE_INFO's reply was eight bytes until revision 1.10 added the transfer:
 * u32 done, u32 total and u8 error with three pad bytes. A client written
 * against 1.9 reads the first eight and is right about all of them.
 */
#define SAVEFILE_INFO_SIZE    20
#define SAVEFILE_INFO_SIZE_19 8
/*
 * Revision 1.11: one SAVEFILE_READ or SAVEFILE_WRITE is a frame, and a frame is
 * QWARK_MAX_PAYLOAD. It was 65536.
 */
#define SAVEFILE_CHUNK_MAX    QWARK_MAX_PAYLOAD

/* A category or file name on the wire is a fixed 32-byte field, NUL-padded. */
#define SAVEFILE_NAME_LEN     32

/* SAVEFILE_LIST row: char[32] name, u32 size, u32 crc32. */
#define SAVEFILE_ROW_SIZE     40

/* SAVEFILE_CATEGORY's `op`. */
#define SAVEFILE_CATEGORY_CREATE 0
#define SAVEFILE_CATEGORY_DELETE 1

/*
 * Revision 1.21, build 53: manip categories (src/core/manips.h, PROTOCOL.md
 * 5.14). A game describes groups of RAM writes speedrunners call manips; the
 * client picks one, and qwark applies it on its own when a save is loaded or a
 * new game starts. All four answer UNSUPPORTED for a game without groups or
 * outside a game.
 *
 *   MANIP_LIST     none -> the groups, the selection, the two options and the
 *                  last event (layout in PROTOCOL.md)
 *   MANIP_SELECT   u8 group, 0 for None; BAD_ARG for an unknown id
 *   MANIP_OPTIONS  u8 auto_reset, u8 remember
 *   MANIP_APPLY    none; the selected group's setup and per-run writes now. A
 *                  refusal is BUSY, with the reason in the event
 */
#define OP_MANIP_LIST         0x00D0
#define OP_MANIP_SELECT       0x00D1
#define OP_MANIP_OPTIONS      0x00D2
#define OP_MANIP_APPLY        0x00D3

/*
 * Revision 1.27: the game's own Lua (Tools of Destruction; src/core/luacore.c,
 * src/core/luamb.h, PROTOCOL.md 5.15). Where the games are linked into a
 * release, which has no Lua game, all three answer UNSUPPORTED.
 *
 *   LUA_EXEC    u16 len, u16 flags (0), char src[len] (1..8192) -> s32 lua_status,
 *               u32 seq, u8 out_type, u8 out_flags, u16 out_len, char out[out_len];
 *               answered once the game has (at most LUAMB_T_ANSWER_MS and a tick)
 *   LUA_STATUS  none -> u8 installed, u8 phase, u16 pad, u32 heartbeat, u32 state,
 *               u32 err_seq, u32 err_vm, char err_script[64], u16 err_len, u16 pad,
 *               char err[err_len]
 *   LUA_PATCH   none -> the helper as RPCS3 patch words, SAVEFILE_PATCH's layout;
 *               the emulator builds only
 */
#define OP_LUA_EXEC           0x00E0
#define OP_LUA_STATUS         0x00E1
#define OP_LUA_PATCH          0x00E2
#define LUA_EXEC_HEAD         4u
#define LUA_EXEC_REPLY_HEAD   12u
/* Revision 1.29, LUA_EXEC's flags: bit0, the pause menu's VM (ToD's console menu's), not the gameplay VM. */
#define LUA_EXEC_MENU         0x0001u
#define LUA_STATUS_HEAD       88u
#define LUA_INSTALLED_NO      0
#define LUA_INSTALLED_YES     1
#define LUA_INSTALLED_CHECKING 2
#define LUA_PHASE_IDLE        0
#define LUA_PHASE_SENT        1
#define LUA_PHASE_CANCELLING  2
#define LUA_PHASE_HUNG        3

/* 0x00E8, DEBUG_TEXT (revision 1.29, a game's debug pages), is retired and never reused. */

/* A group's flags on the wire. */
#define MANIP_FLAG_NGPLUS       0x01  /* 1 NG+ (challenge mode), 0 new-game */
#define MANIP_FLAG_EXTENSION    0x02  /* a category extension, shown with less emphasis */
#define MANIP_FLAG_PER_RUN      0x04  /* the group has per-run writes */
#define MANIP_FLAG_NO_CHALLENGE 0x08  /* the group skips the challenge-mode check */

/* MANIP_LIST's event_kind. */
#define MANIP_EVENT_NONE          0
#define MANIP_EVENT_SETUP         1   /* activated: the group's first application this boot */
#define MANIP_EVENT_APPLIED       2   /* applied again (per-run, or a repeat Apply now) */
#define MANIP_EVENT_NOT_CHALLENGE 3   /* refused: an NG+ group on a non-challenge file */
#define MANIP_EVENT_CHALLENGE     4   /* refused: a new-game group on a challenge file */
#define MANIP_EVENT_NO_FILE       5   /* refused: Apply now outside a file */

/* ------------------------------------------------------------- table sizes */

#define QWARK_MAX_WATCHES    64
#define QWARK_MAX_FREEZES    64
#define QWARK_MAX_PATCHES    64
#define QWARK_MAX_FEATURES   64
#define QWARK_MAX_GROUPS     16
#define QWARK_MAX_READOUTS   16
#define QWARK_MAX_MODS       32
/*
 * Concurrent TCP connections. Nothing on the wire names this number: a client
 * that finds the console full is closed as it connects, which is what happened
 * at nine before and happens at five now. A PC runs one client and the second
 * slot is what a restarted one lands in while the old connection is still being
 * reaped, so four is two spare rather than none.
 */
#define QWARK_MAX_CLIENTS    4
#define QWARK_MAX_SUBS       8
/*
 * Commands in flight on the tick thread's ring. A connection holds the request
 * arena while it waits for its answer, so no more than QWARK_MAX_CLIENTS
 * commands can ever be queued at once; 16 is four times that, and a full ring
 * still answers BUSY the way it always has.
 */
#define QWARK_RING_SLOTS     16
#define QWARK_POS_SLOTS      8
#define QWARK_MAX_BLOB       64
#define QWARK_MAX_PLANETS    64

/* ------------------------------------------------------- autosplit, rev 1.5 */

/*
 * qwark detects, the client decides. The tick thread watches game memory and
 * emits run events unconditionally: it keeps no timer, applies no user setting
 * and knows nothing about LiveSplit. Every candidate goes out and the PC picks.
 *
 * The one thing qwark does not leave to taste is *timing*: the old ASL scripts
 * adjusted game time at fixed places, and a run that does not reproduce those
 * adjustments to the microsecond is a different time. Those adjustments travel
 * as the FLAT and NORMALISE flags on an EventDesc row, and a client applies them
 * whenever the autosplitter is on, setting or no setting.
 */

/* Event.kind */
#define AUTOSPLIT_START      1
#define AUTOSPLIT_SPLIT      2
#define AUTOSPLIT_RESET      3
#define AUTOSPLIT_PAUSE      4
#define AUTOSPLIT_RESUME     5
#define AUTOSPLIT_LOAD_START 6
#define AUTOSPLIT_LOAD_END   7

/*
 * Event.code is per game and carried by every kind but START and RESET, which
 * are always code 0. A LOAD_END carries the same code as the LOAD_START it
 * closes, and a RESUME the same code as its PAUSE. Code 1 is reserved in every
 * game for "planet entered", whose arg is the planet index in that game's
 * PLANET_LIST order.
 */
#define AUTOSPLIT_CODE_PLANET 1

/* EventDesc.flags */
#define AUTOSPLIT_FLAG_DEFAULT   0x01 /* a client enables this one out of the box */
#define AUTOSPLIT_FLAG_ROUTE     0x02 /* a planet route applies; only code 1 sets it */
/*
 * The two timing flags, revision 1.5. At most one of them is set on a row.
 *
 *   FLAT       subtract param_us of game time when the described event fires;
 *              on a SPLIT row, before the split is taken.
 *   NORMALISE  time the gap from this event to the one that closes it (the
 *              matching LOAD_END, or the RESUME after a PAUSE) and subtract
 *              max(0, duration - param_us), so the interval always costs
 *              exactly param_us however long it really took.
 */
#define AUTOSPLIT_FLAG_FLAT      0x04
#define AUTOSPLIT_FLAG_NORMALISE 0x08
/*
 * Revision 1.29, on a LOAD_START row only, with neither timing flag
 * and param_us 0:
 *
 *   HOLD       game time stops at the LOAD_START and starts again at the
 *              matching LOAD_END, an ASL's isLoading returning true for as
 *              long as the pair is open. Nothing is subtracted afterwards.
 */
#define AUTOSPLIT_FLAG_HOLD      0x10
/*
 * Revision 1.33 (pending), on the planet row (code 1) only:
 *
 *   REVISIT    this game marks a planet event that is a revisit with
 *              AUTOSPLIT_EV_REVISIT in the event's flags, so a client may offer
 *              not to split on one. A row without it never sets the bit.
 */
#define AUTOSPLIT_FLAG_REVISIT   0x20

/*
 * Event.flags, the u16 at +10 (revision 1.33, pending; "reserved" before
 * it, always 0, and still 0 on every event but those below). A client that
 * does not know a bit ignores it.
 *
 *   REVISIT    on a planet event (SPLIT, code 1) of a game whose planet row
 *              has AUTOSPLIT_FLAG_REVISIT: the load is a revisit by the
 *              game's own state (PROTOCOL.md 8.5 says which, per game).
 */
#define AUTOSPLIT_EV_REVISIT     0x0001

#define AUTOSPLIT_EVENT_SIZE   16
#define AUTOSPLIT_DESC_SIZE    32
#define AUTOSPLIT_LABEL_LEN    24
#define AUTOSPLIT_RING_SLOTS   64

/* The UDP push: 'Q','E', version, reserved, then the 16-byte Event. */
#define AUTOSPLIT_MAGIC        "QE"
#define AUTOSPLIT_DGRAM_VERSION 1
#define AUTOSPLIT_DGRAM_SIZE   (4 + AUTOSPLIT_EVENT_SIZE)

/*
 * How many ticks in a row one event's datagram is repeated, counting the tick it
 * was emitted on. Three covers a lost datagram without anyone keeping a timer.
 */
#define AUTOSPLIT_REPEATS      3

/* -------------------------------------------------------------- structures */

#define SESSION_INFO_SIZE    164
#define TELEMETRY_MAGIC      "QWRK"
#define TELEMETRY_MAX        (4 + SESSION_INFO_SIZE + 1 + QWARK_MAX_WATCHES * 12)

/* SessionInfo.flags */
#define SESSION_FLAG_PREVIOUS_PENDING 0x01
/*
 * Revision 1.6. bit1 says qwark is driving an emulator rather than a console;
 * bit2 says this platform refuses code patches, so a client greys every
 * WRITES_CODE row instead of offering a toggle that would answer UNSUPPORTED.
 * Both are set by the PINE (RPCS3) backend and by nothing else.
 */
#define SESSION_FLAG_EMULATOR         0x02
#define SESSION_FLAG_NO_CODE_PATCHES  0x04
/*
 * Revision 1.12. bit3 is set while the COMBO_ENABLE switch is off, so a client
 * draws its combo checkbox out of the block it already reads rather than
 * needing an op to ask. A COMBO_SUSPEND hold is deliberately not in here: that
 * is the asking client's own capture and it lifts itself.
 */
#define SESSION_FLAG_COMBOS_OFF       0x08
/*
 * Revision 1.17. bit4 is set on a platform that refuses code patches (bit2)
 * while the running game's code switches are in it, put there at load by an
 * RPCS3 patch written from SWITCH_PATCH: every WRITES_CODE toggle of the game
 * works then, as a byte write, so a client stops greying those rows. It is
 * never set on a console, where those toggles patch code as they always have,
 * and never before qwark has looked this session.
 */
#define SESSION_FLAG_CODE_SWITCHES    0x10
/*
 * Revision 1.22, build 54. bit5 is set once the module has read the VSH's free
 * memory, so VSH_MEMORY answers with it: from module start on a console, never
 * under RPCS3. A client asks VSH_MEMORY only while it is set, which keeps an
 * older module (UNKNOWN_OP) and RPCS3 (UNSUPPORTED) from ever being asked.
 */
#define SESSION_FLAG_VSH_MEMORY       0x20
/*
 * Revision 1.23, build 55. bit6 is set while config.txt's `menu.help` is 0, so
 * the console menu leaves the game's Help row alone (MENU_HELP). Clear in every
 * earlier build, whose menu always took the row: the bit reads right against
 * those too.
 */
#define SESSION_FLAG_MENU_HELP_OFF    0x40
/*
 * Build 55, revision 1.23. bit7 is set while the console's ui.pak, the file
 * the console menu's renderers are read from (src/core/uipak.h), is missing,
 * damaged or not the one built with this module, so no menu goes into a game.
 * A client offers to write the one it shipped. Set only on a console, from
 * the module's first tick on; never by qwark-rpcs3, which has no menu, and
 * never by an earlier build, whose renderers were in the module.
 */
#define SESSION_FLAG_UI_PAK_BAD       0x80

/*
 * Revision 1.24, build 56. SessionInfo byte 29, a pad byte until then, is
 * `flags2`: bit0 is the USB mods' (their paragraph), bits 1 to 3 the settings
 * profiles'. Byte 30 is `profile_seq` (config_profile_seq). bit1 is the
 * client's capability bit: the module has settings profiles and the console has
 * users to keep them for. Set by the PS3 module from build 56 and the
 * simulator; never by qwark-rpcs3, as RPCS3 has one user, and never by an
 * earlier build, which sent a pad byte.
 */
#define SESSION_PROFILE_SUPPORTED     0x02
#define SESSION_PROFILE_ON            0x04   /* `profiles.enabled` is 1 */
#define SESSION_PROFILE_ACTIVE        0x08   /* a user's profile is in use now */
/* PROFILE_INFO's flags: the three above, in the same bits, and this one. */
#define PROFILE_INFO_HAS_PROFILE      0x20   /* the signed-in user's profile has been made */
#define PROFILE_INFO_SIZE             64
/* COMBO_LIST's byte after each action. */
#define COMBO_LIST_OVERRIDDEN         0x01
/*
 * Revision 1.34: after COMBO_LIST's entries, `u8 m` and m of { u8 action,
 * u8 len, char name[len] }, the page combos in the Trainer's order with each
 * page's name (no NUL, at most COMBO_PAGE_NAME_MAX characters). A reply's
 * largest size is COMBO_LIST_MAX.
 */
#define COMBO_PAGE_NAME_MAX           15
#define COMBO_LIST_MAX                (1u + COMBO_COUNT * 8u + 1u + COMBO_PAGES * (2u + COMBO_PAGE_NAME_MAX))

/*
 * Revision 1.24, build 56: flags2 bit0 USB_MODS, this module reads mods off
 * the console's USB drives and answers MOD_USB_LIST (PROTOCOL.md 5.7.2). Set
 * on a console and in the simulator; never by qwark-rpcs3, where a mod is an
 * RPCS3 patch.
 */
#define SESSION_FLAG2_USB_MODS        0x01
/*
 * Revision 1.24, test builds: flags2 bit4 DEV, this module is a test build
 * (QWARK_DEV), so a client compares the build ID HELLO carries after
 * SessionInfo with its own module's rather than the build number alone.
 */
#define SESSION_FLAG2_DEV             0x10
/*
 * Revision 1.26, game modules (PROTOCOL.md 9.11). bit5 GAME_MODULE_BAD: the
 * running game's module is missing, would not load or start, or is from
 * another build, so the session has no game features until a client writes the
 * file or the game boots again. bit6 GAME_WAITING: the module is there but is
 * held back until the VSH has the memory for it, and loads by itself once it
 * has. Neither is ever set by a module whose games are linked in.
 */
#define SESSION_FLAG2_GAME_MODULE_BAD 0x20
#define SESSION_FLAG2_GAME_WAITING    0x40

/*
 * Revision 1.28. bit7 NO_MENU: the running game has no console
 * menu (its game_api.menu is NULL: Tools of Destruction today), so the menu's
 * combo opens nothing and MENU_HELP's switch has no Help row to give. Set
 * while INGAME with a game whose code is in; clear otherwise, and in every
 * earlier build, so a client goes on drawing both against those.
 */
#define SESSION_FLAG2_NO_MENU         0x80
/*
 * Revision 1.30. SessionInfo byte 31, a pad byte until then, is
 * `flags3`. bit0 RUN_WARN, the client's capability bit: the module has the
 * run-start warnings and answers RUN_WARN (PROTOCOL.md 5.10.2); bit1
 * RUN_WARN_OFF, config.txt's `run.warnings` is 0. An earlier build sent a pad
 * byte, so a client shows no switch against it.
 */
#define SESSION_FLAG3_RUN_WARN        0x01
#define SESSION_FLAG3_RUN_WARN_OFF    0x02
/*
 * Revision 1.32. bit2 TELEMETRY_RATE, the client's capability bit: the module
 * answers TELEMETRY_RATE. Bits 3-4, the rate in force: 0 30 Hz, 1 60 Hz,
 * 2 120 Hz (3 is never sent). An earlier build sent 0 in all three, which a
 * client reads as no switch and 30 Hz.
 */
#define SESSION_FLAG3_TELEMETRY_RATE  0x04
#define SESSION_FLAG3_RATE_SHIFT      3
#define SESSION_FLAG3_RATE_MASK       0x18
#define TELEMETRY_RATE_CODE_30        0
#define TELEMETRY_RATE_CODE_60        1
#define TELEMETRY_RATE_CODE_120       2
/*
 * Revision 1.33. bit5 SETTINGS_133, the client's capability bit for the
 * revision's two settings: the module takes RUN_WARN 2, the cheats line
 * alone, and answers REBOOT_APPLY. bit6 RUN_WARN_CHEATS, config.txt's
 * `run.warnings` is 2. Bit1 keeps meaning exactly "off", so a client before
 * this revision reads off as off and a cheats-only console as on. bit7 is
 * reserved and 0: Ask's question is the previous-session prompt, flags bit0
 * PREVIOUS_PENDING. An earlier module sends 0 in bits 5 and 6, answers
 * RUN_WARN 2 BAD_ARG and REBOOT_APPLY UNKNOWN_OP.
 */
#define SESSION_FLAG3_SETTINGS_133    0x20
#define SESSION_FLAG3_RUN_WARN_CHEATS 0x40
#define HELLO_BUILD_ID_SIZE           4

/* Feature kinds */
#define FEATURE_TOGGLE 0
#define FEATURE_ACTION 1
#define FEATURE_VALUE  2
#define FEATURE_ENUM   3
#define FEATURE_COLOR  4

/* Feature flags */
#define FEATURE_FLAG_AUTO        0x01
#define FEATURE_FLAG_WRITES_CODE 0x02
/*
 * Revision 1.2, retargeted by 1.9. The two savefile-helper ACTIONs a client's
 * save-file manager drives. SAVE_ASIDE asks the game to copy its live save into
 * the helper's aside buffer and LOAD_ASIDE asks it to load what is in that
 * buffer; the bytes travel over SAVEFILE_READ and SAVEFILE_WRITE. Until 1.9 the
 * pair moved a tempsave file under /dev_hdd0/game/<TITLEID>/USRDIR instead.
 * Every game names exactly one of each.
 */
#define FEATURE_FLAG_SAVE_ASIDE  0x04
#define FEATURE_FLAG_LOAD_ASIDE  0x08
/*
 * Revision 1.3. A TOGGLE whose truth is a plain byte the game owns, not a patch
 * and not qwark-side state: its state is read back out of game memory by qwark
 * (10 Hz while INGAME) and toggle_state reports what memory says, so a client's
 * checkbox follows the save file and the running game rather than qwark's idea
 * of what it last wrote. There is nothing for qwark to re-apply, so a client
 * shows no auto-apply control for a LIVE toggle: FEATURE_SET_AUTO on one is
 * answered UNSUPPORTED and it never joins the previous-session record.
 * FEATURE_SET still writes the byte.
 */
#define FEATURE_FLAG_LIVE        0x10
/*
 * Revision 1.7. The field behind this VALUE is two's complement in `bits` bits,
 * so the readout that mirrors it carries the raw field and a client sign-extends
 * it before showing it. FEATURE_SET still carries a u32: the client sends the
 * low `bits` bits of the value it wants (0xFFFF for -1 on a 16-bit field) and
 * qwark writes the field exactly as it always did. A signed feature leaves min
 * and max at 0, because the width already says what the range is.
 */
#define FEATURE_FLAG_SIGNED      0x20
/*
 * Revision 1.24. A COLOR the console works out from another COLOR, whose id is
 * this row's `aux`, until the user overrides it (PROTOCOL.md 5.3.3). Its
 * readout carries FEATURE_COLOR_AUTO while it is worked out; a FEATURE_SET
 * with that bit puts it back to automatic, and one without it overrides it.
 */
#define FEATURE_FLAG_SHADE       0x40
/*
 * Revision 1.24. A COLOR with the game's own colour to go back to (PROTOCOL.md
 * 5.3.4): its readout carries FEATURE_COLOR_AUTO while it is the game's own,
 * and a FEATURE_SET with that bit puts it back.
 */
#define FEATURE_FLAG_STOCK       0x80
#define FEATURE_COLOR_AUTO       0x01000000u

/*
 * Feature, 48 bytes: id, kind, group, aux, flags, readout, bits, pad, u32 min,
 * u32 max, char label[32]. `readout` is the SessionInfo.readout[] index that
 * mirrors a VALUE, ENUM or COLOR feature's current value, 0xFF when there is
 * none and always 0xFF for TOGGLE and ACTION.
 *
 * Revision 1.7: `bits` is the width in bits of the field behind a VALUE, 8, 16
 * or 32. Zero means 32, so every row written before this revision reads the same
 * way it always did; every other kind sends 0.
 */
#define FEATURE_WIRE_SIZE 48
#define FEATURE_NO_READOUT 0xFF
#define FEATURE_BITS_DEFAULT 32
#define UNLOCK_WIRE_SIZE  44
#define MOD_WIRE_SIZE     120

/*
 * Revision 1.3. UNLOCK_LIST carries four of these between the categories and
 * the rows, one per Unlock.value[] slot, always four. `name` is empty for a
 * slot the game never uses; `kind` says whether the client draws a checkbox or
 * a number box; `max` is the largest meaningful value of a number, 0 for none.
 */
#define UNLOCK_FIELD_WIRE_SIZE  16
#define UNLOCK_FIELD_NAME_LEN   12

#define UNLOCK_KIND_FLAG    0
#define UNLOCK_KIND_NUMBER  1

/*
 * Unlock.fields bits, one per Unlock.value[] slot: bit f set = slot f is
 * meaningful for that entry. What a slot means is per game and is spelled out
 * by the UnlockFieldDesc rows, not by these names.
 */
#define UNLOCK_FIELD_0 0x01
#define UNLOCK_FIELD_1 0x02
#define UNLOCK_FIELD_2 0x04
#define UNLOCK_FIELD_3 0x08

/* RaC1's reading of the same four bits, kept because RaC1 still reads that way. */
#define UNLOCK_FIELD_OWNED UNLOCK_FIELD_0
#define UNLOCK_FIELD_GOLD  UNLOCK_FIELD_1
#define UNLOCK_FIELD_LEVEL UNLOCK_FIELD_2
#define UNLOCK_FIELD_AMMO  UNLOCK_FIELD_3

/*
 * Mod flags.
 *
 * LOADED is qwark's own load on a console. On a platform that refuses code
 * patches (revision 1.16) it is whether the mod is in game memory, every cave
 * byte and every word of it, which is how an RPCS3 patch put in at load shows
 * up; CHECKING is set there until qwark has looked this session, and never on
 * a console. PREVIOUS is the mod's bit in the previous-session record while it
 * is not loaded again; it was documented and never set before build 49.
 */
#define MOD_FLAG_LOADED      0x01
#define MOD_FLAG_AUTO        0x02
#define MOD_FLAG_NEEDS_LUA   0x04
#define MOD_FLAG_PREVIOUS    0x08
#define MOD_FLAG_PARSE_ERROR 0x10
#define MOD_FLAG_CHECKING    0x20
/*
 * Revision 1.30: patch.txt's `#- legal:` line, `true` LEGAL and
 * `false` ILLEGAL, either case; a mod with neither (no line, or any other
 * value) is of unknown legality. The run-start warnings count the loaded
 * ILLEGAL ones and the loaded unknown ones (src/core/runwarn.h).
 */
#define MOD_FLAG_LEGAL       0x40
#define MOD_FLAG_ILLEGAL     0x80

/* Patch kinds, PATCH_LIST */
#define PATCH_KIND_CLIENT  0
#define PATCH_KIND_FEATURE 1
#define PATCH_KIND_MOD     2

/* Combo actions */
#define COMBO_SAVE_POSITION 0
#define COMBO_LOAD_POSITION 1
#define COMBO_DIE           2
#define COMBO_LOAD_PLANET   3
#define COMBO_LOAD_SETASIDE 4
/*
 * Revision 1.22. Opens the console menu, or closes it when it is up. The one
 * combo with a mask of its own out of the box (COMBO_MENU_DEFAULT, L1 + R1 +
 * L2 + R2 + L3 + R3). Revision 1.22 had it the one combo the COMBO_ENABLE switch
 * does not hold off; since revision 1.23 the switch and COMBO_SUSPEND hold it
 * as they hold every combo, and a client can capture it.
 */
#define COMBO_OPEN_MENU     5
/*
 * Revision 1.23, build 55. Flips the running game's Fast loads toggle, exactly
 * as FEATURE_SET from a client's checkbox does, and says on the console which
 * way it went, or why not (under RPCS3 without the code switches, the same
 * UNSUPPORTED the checkbox gets). No mask out of the box; COMBO_ENABLE holds it
 * off like the first five.
 */
#define COMBO_TOGGLE_FAST_LOADS 6
/*
 * Revision 1.34, build 59: one combo per page of the Trainer's root, one list
 * for every game, each opening the Trainer straight on its page (menu.c,
 * menu_page_combo), with OPEN_MENU's rules: the arming rule, the switch and
 * the hold, its refusals, nothing while a load waits. A page the running game
 * has not opens nothing. No mask out of the box, and an unbound one is no line
 * in config.txt (config.c). COMBO_LIST names them after its entries
 * (net.c, PROTOCOL.md 5.9). Never renumbered: a new page goes on the end.
 */
#define COMBO_PAGE_FIRST    7
#define COMBO_PAGE_GAME     7
#define COMBO_PAGE_CHEATS   8
#define COMBO_PAGE_PLANETS  9
#define COMBO_PAGE_MANIPS   10
#define COMBO_PAGE_UNLOCKS  11
#define COMBO_PAGE_MODS     12
#define COMBO_PAGE_SAVES    13
#define COMBO_PAGES         7
#define COMBO_COUNT         (COMBO_PAGE_FIRST + COMBO_PAGES)
#define COMBO_MENU_DEFAULT  0x060Fu

/* Planet flags, PLANET_SELECT / PLANET_LOAD */
#define PLANET_FLAG_RESET_LEVELFLAGS 0x01
#define PLANET_FLAG_RESET_BOLTS      0x02

/* PREVIOUS_REAPPLY categories */
#define PREV_TOGGLES 0x01
#define PREV_MODS    0x02
#define PREV_FREEZES 0x04
#define PREV_PATCHES 0x08

/* -------------------------------------------------------- pad mask, OG layout */

#define PAD_L2       0x0001
#define PAD_R2       0x0002
#define PAD_L1       0x0004
#define PAD_R1       0x0008
#define PAD_TRIANGLE 0x0010
#define PAD_CIRCLE   0x0020
#define PAD_CROSS    0x0040
#define PAD_SQUARE   0x0080
#define PAD_SELECT   0x0100
#define PAD_L3       0x0200
#define PAD_R3       0x0400
#define PAD_START    0x0800
#define PAD_UP       0x1000
#define PAD_RIGHT    0x2000
#define PAD_DOWN     0x4000
#define PAD_LEFT     0x8000

#endif /* QWARK_PROTO_H */
