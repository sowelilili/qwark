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
#define QWARK_BUILD             54

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
 * L2 + R2 + L3 + R3), and the one the COMBO_ENABLE switch does not hold off: a
 * console without a PC may have no other way into the menu. COMBO_SUSPEND
 * still holds it, so a client can capture it.
 */
#define COMBO_OPEN_MENU     5
#define COMBO_COUNT         6
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
