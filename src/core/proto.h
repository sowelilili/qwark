/*
 * The wire contract from docs/PROTOCOL.md, version 1, revision 1.2. Nothing here
 * may change without changing that file and QWARK_PROTOCOL_VERSION with it.
 *
 * Revision 1.1: SessionInfo carries sixteen readouts and is 164 bytes, Feature
 * gained a `readout` byte at offset 5, and LEVELFLAGS_SET was added.
 * Revision 1.2: Feature flags bit2 SAVE_ASIDE and bit3 LOAD_ASIDE.
 */
#ifndef QWARK_PROTO_H
#define QWARK_PROTO_H

#include "../plat/plat.h"

#define QWARK_PROTOCOL_VERSION  1
#define QWARK_BUILD_VERSION     1

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

#define OP_COMBO_SET         0x0080
#define OP_COMBO_LIST        0x0081

#define OP_CONFIG_RELOAD     0x0090
#define OP_CONFIG_SAVE       0x0091

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

/* -------------------------------------------------------------- structures */

#define SESSION_INFO_SIZE    164
#define TELEMETRY_MAGIC      "QWRK"
#define TELEMETRY_MAX        (4 + SESSION_INFO_SIZE + 1 + QWARK_MAX_WATCHES * 12)

/* SessionInfo.flags */
#define SESSION_FLAG_PREVIOUS_PENDING 0x01

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
 * Revision 1.2. The two savefile-helper ACTIONs a client's save-file manager
 * drives: SAVE_ASIDE makes the game write its save to
 * /dev_hdd0/game/<TITLEID>/USRDIR/tempsave, LOAD_ASIDE makes it load that file.
 * A game may expose further savefile actions; only the manager's pair is flagged.
 */
#define FEATURE_FLAG_SAVE_ASIDE  0x04
#define FEATURE_FLAG_LOAD_ASIDE  0x08

/*
 * Feature, 48 bytes: id, kind, group, aux, flags, readout, pad[2], u32 min,
 * u32 max, char label[32]. `readout` is the SessionInfo.readout[] index that
 * mirrors a VALUE, ENUM or COLOR feature's current value, 0xFF when there is
 * none and always 0xFF for TOGGLE and ACTION.
 */
#define FEATURE_WIRE_SIZE 48
#define FEATURE_NO_READOUT 0xFF
#define UNLOCK_WIRE_SIZE  44
#define MOD_WIRE_SIZE     120

/* Unlock.fields bits, one per Unlock.value[] slot. */
#define UNLOCK_FIELD_OWNED 0x01
#define UNLOCK_FIELD_GOLD  0x02
#define UNLOCK_FIELD_LEVEL 0x04
#define UNLOCK_FIELD_AMMO  0x08

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
