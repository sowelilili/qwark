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
#define QWARK_BUILD             16

#define QWARK_PORT              9673
#define QWARK_MAX_PAYLOAD       65600u
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

/* --------------------------------------------------------------- opcodes */

#define OP_HELLO             0x0001
#define OP_HEARTBEAT         0x0002
#define OP_NOTIFY            0x0003
#define OP_PREVIOUS_LIST     0x0004
#define OP_PREVIOUS_REAPPLY  0x0005
#define OP_PREVIOUS_DISMISS  0x0006

#define OP_SUBSCRIBE         0x0010
#define OP_UNSUBSCRIBE       0x0011
#define OP_GET_STATE         0x0012

#define OP_DESCRIBE          0x0020
#define OP_FEATURE_SET       0x0021
#define OP_FEATURE_TRIGGER   0x0022
#define OP_FEATURE_SET_AUTO  0x0023
#define OP_FEATURE_OPTIONS   0x0024

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

#define OP_UNLOCK_LIST       0x0050
#define OP_UNLOCK_SET        0x0051
#define OP_LEVELFLAGS_GET    0x0053
#define OP_LEVELFLAGS_RESET  0x0054
#define OP_LEVELFLAGS_SET    0x0055

#define OP_MOD_LIST          0x0060
#define OP_MOD_LOAD          0x0061
#define OP_MOD_UNLOAD        0x0062
#define OP_MOD_SET_AUTO      0x0063
#define OP_MOD_RESCAN        0x0064
#define OP_MOD_INFO          0x0065

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

#define OP_CONFIG_RELOAD     0x0090
#define OP_CONFIG_SAVE       0x0091

#define OP_AUTOSPLIT_EVENTS   0x00A0
#define OP_AUTOSPLIT_DESCRIBE 0x00A1

/*
 * Revision 1.9, the savefile block. See src/core/savefile.h and PROTOCOL.md 5.12.
 * All three install the game's helper on demand, answer UNSUPPORTED where code
 * cannot be patched and NOT_INGAME outside INGAME.
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
 * SAVEFILE_INFO's reply was eight bytes until revision 1.10 added the transfer:
 * u32 done, u32 total and u8 error with three pad bytes. A client written
 * against 1.9 reads the first eight and is right about all of them.
 */
#define SAVEFILE_INFO_SIZE    20
#define SAVEFILE_INFO_SIZE_19 8
#define SAVEFILE_CHUNK_MAX    65536u

/* A category or file name on the wire is a fixed 32-byte field, NUL-padded. */
#define SAVEFILE_NAME_LEN     32

/* SAVEFILE_LIST row: char[32] name, u32 size, u32 crc32. */
#define SAVEFILE_ROW_SIZE     40

/* SAVEFILE_CATEGORY's `op`. */
#define SAVEFILE_CATEGORY_CREATE 0
#define SAVEFILE_CATEGORY_DELETE 1

/* ------------------------------------------------------------- table sizes */

#define QWARK_MAX_WATCHES    64
#define QWARK_MAX_FREEZES    64
#define QWARK_MAX_PATCHES    64
#define QWARK_MAX_FEATURES   64
#define QWARK_MAX_GROUPS     16
#define QWARK_MAX_READOUTS   16
#define QWARK_MAX_MODS       32
#define QWARK_MAX_CLIENTS    8
#define QWARK_MAX_SUBS       8
#define QWARK_RING_SLOTS     32
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

/* Mod flags */
#define MOD_FLAG_LOADED      0x01
#define MOD_FLAG_AUTO        0x02
#define MOD_FLAG_NEEDS_LUA   0x04
#define MOD_FLAG_PREVIOUS    0x08
#define MOD_FLAG_PARSE_ERROR 0x10

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
#define COMBO_COUNT         5

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
