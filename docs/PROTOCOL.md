# qwark wire protocol, version 1

Revision 1.1 (2026-09-07): SessionInfo grew to 164 bytes (16 readouts), Feature is 48 bytes with a `readout` field, LEVELFLAGS_SET added.
Revision 1.2 (2026-09-08): Feature flags bit2 SAVE_ASIDE and bit3 LOAD_ASIDE mark the savefile-helper actions so a client can drive the save-file manager without matching labels.

This file is the contract between qwark (the PS3 SPRX) and every client. Both sides are written against it; when it changes, `QWARK_PROTOCOL_VERSION` changes with it.

All integers are big-endian. Floats are IEEE 754 single precision, big-endian. Fixed-width string fields are NUL-padded and need not be NUL-terminated when full. Variable strings are given with an explicit length unless stated.

Transport: TCP on port 9673 for commands, UDP telemetry sent to the port the client names in SUBSCRIBE. Up to 8 concurrent TCP clients.

## 1. Frames

```
Request:  u32 length | u16 seq | u16 opcode | payload[length]
Reply:    u32 length | u16 seq | u16 status | payload[length]
```

- `length` is the payload size, at most `QWARK_MAX_PAYLOAD` = 65600. A larger length closes the connection.
- Every request gets exactly one reply carrying the same `seq`. Replies come back in request order, so clients may pipeline.
- An unknown opcode is answered with `UNKNOWN_OP` and an empty payload; the payload is drained and the connection stays open.
- A payload shorter than the opcode requires is answered with `BAD_ARG`.
- A reply listed as **none** below is a frame with status and an empty payload; every request still gets its frame.

## 2. Status codes (u16)

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | |
| 1 | NOT_INGAME | Session state is not INGAME and the command touches game memory |
| 2 | UNSUPPORTED | The current game does not offer this feature, or no game is running |
| 3 | BAD_ARG | Malformed payload or out-of-range argument |
| 4 | IO_ERROR | cellFs or PS3MAPI call failed |
| 5 | FULL | A fixed table is full |
| 6 | UNKNOWN_OP | |
| 7 | NOT_FOUND | Handle, mod, slot or path does not exist |
| 8 | BUSY | Command ring is full; retry |

## 3. Session info block

Shared by HELLO, GET_STATE and telemetry. 164 bytes.

```
u8   protocol_version   = 1
u8   qwark_version      build number, informational
u8   state              0 XMB, 1 BOOTING, 2 INGAME, 3 QUITTING
u8   game               0 NONE, 1 RAC1, 2 RAC2, 3 RAC3, 4 RAC4 (Deadlocked)
                        BCES01503, the disc trilogy, reports 1, 2 or 3 depending
                        on which executable is mapped; the client never picks
u32  generation         incremented every time a game enters BOOTING
u32  tick               tick-thread counter, 120 Hz
char title_id[12]       e.g. "NPEA00385"
u8   flags              bit0 PREVIOUS_PENDING: a previous-session record is waiting (section 4.1)
u8   selected_slot      0..7, used by the save/load combos
u8   selected_planet    used by the load-planet combo
u8   planet_flags       bit0 reset level flags, bit1 reset special bolts, used by the load-planet combo
u8   current_planet     game planet index, 0 when unknown
u8   pad[3]
f32  pos[3]             player x, y, z
u32  pad_mask           controller buttons, OG layout (section 10)
f32  analog[4]          rx, ry, lx, ly, each -1..1
u32  readout[16]        game-defined live values; DESCRIBE names them. readout[0] is the bolt count when the game has one. VALUE, ENUM and COLOR features name the readout that mirrors their current value
u64  toggle_state       bit i set = TOGGLE feature id i is on
u64  toggle_auto        bit i set = feature id i re-applies itself on game boot
u64  freeze_active      bit i set = freeze id i is active
u32  mod_loaded         bit i set = console mod index i is loaded
u32  mod_auto           bit i set = console mod index i auto-applies on game boot
u32  mod_previous       bit i set = mod i was loaded before the last same-game reboot and is neither re-applied nor dismissed
```

## 4. Telemetry packet (UDP)

Sent every fourth tick (30 Hz) to every subscriber. One packet is a complete snapshot; the client keeps only the latest one.

```
char magic[4]        "QWRK"
SessionInfo          164 bytes (section 3)
u8   nwatch
{ u8 id, u8 size, u8 valid, u8 pad, u64 value }[nwatch]    value is right-aligned: a 1-byte watch is in the low byte
```

`valid` is 0 when the watch could not be read this tick (not in game). Maximum 64 watches, so the packet never exceeds 937 bytes.

GET_STATE returns exactly these bytes over TCP.

### 4.1 Same-game reboot and the previous-session record

When the game exits and the same title comes back:

- Watches are kept and resume automatically; their ids do not change.
- Every toggle that was on, every active freeze, every client patch and every loaded mod is moved into the **previous-session record** and cleared from the live tables. Items flagged auto (toggle_auto, mod_auto) are re-applied silently when the session reaches INGAME and are removed from the record. If anything remains, `flags.PREVIOUS_PENDING` is set; the client is expected to prompt the user (default No) and answer with PREVIOUS_REAPPLY or PREVIOUS_DISMISS.

When a different title comes back, everything including watches is dropped and the record is empty. The client notices `game` changing in telemetry and re-issues DESCRIBE.

## 5. Opcodes

### 5.1 Session (0x00xx)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0001 | HELLO | `u8 client_protocol_version` | SessionInfo |
| 0x0002 | HEARTBEAT | none | none. Any frame counts as a heartbeat for the telemetry subscription; this one exists for idle clients |
| 0x0003 | NOTIFY | text, at most 255 bytes, no terminator | none. Shows an XMB notification |
| 0x0004 | PREVIOUS_LIST | none | `u64 toggles, u32 mods, u8 nfreeze, { u8 size, u8 pad[3], u32 addr, u64 value }[nfreeze], u8 npatch, { u32 first_addr, u16 nwords, u16 pad }[npatch]` |
| 0x0005 | PREVIOUS_REAPPLY | `u8 flags` bit0 toggles, bit1 mods, bit2 freezes, bit3 patches | none. Re-applies the chosen categories, then clears the record |
| 0x0006 | PREVIOUS_DISMISS | none | none. Clears the record |

### 5.2 Telemetry (0x001x)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0010 | SUBSCRIBE | `u16 udp_port` | none. Packets go to the connection's remote address and this port. The subscription ends when the connection closes or after 5 s without any frame on it |
| 0x0011 | UNSUBSCRIBE | none | none |
| 0x0012 | GET_STATE | none | the telemetry packet, magic included |

### 5.3 Features (0x002x)

Each game exposes up to 64 features with stable ids. Kinds: 0 TOGGLE, 1 ACTION, 2 VALUE (u32), 3 ENUM, 4 COLOR (`0x00RRGGBB`).

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0020 | DESCRIBE | none | `u8 game, u8 ngroups, char[24] group[ngroups], u8 nreadouts, char[24] readout[nreadouts], u8 nfeatures, Feature[nfeatures]`. At most 16 groups, 16 readouts, 64 features |
| 0x0021 | FEATURE_SET | `u8 id, u32 value` (TOGGLE: 0 or 1) | none |
| 0x0022 | FEATURE_TRIGGER | `u8 id` (ACTION only) | none |
| 0x0023 | FEATURE_SET_AUTO | `u8 id, u8 auto` | none. Persisted in config |
| 0x0024 | FEATURE_OPTIONS | `u8 id` (ENUM only) | `u8 count, char[24] option[count]` |

```
Feature (48 bytes):
u8   id
u8   kind
u8   group          index into DESCRIBE groups
u8   aux            ENUM: option count. 0 for every other kind
u8   flags          bit0 AUTO (same as toggle_auto), bit1 WRITES_CODE (instruction patch), bit2 SAVE_ASIDE (this ACTION makes the game write its current save to /dev_hdd0/game/<TITLEID>/USRDIR/tempsave), bit3 LOAD_ASIDE (this ACTION makes the game load that tempsave)
u8   readout        VALUE, ENUM, COLOR: index into SessionInfo.readout[] that mirrors the current value, 0xFF if none. TOGGLE and ACTION: 0xFF
u8   pad[2]
u32  min
u32  max            VALUE and ENUM range, inclusive; both 0 when unbounded
char label[32]
```

Toggle state travels in `toggle_state`; every other kind that has state travels through its readout, so a second client or a restarted one sees the real value.

### 5.4 Memory (0x003x)

All of these except the LIST ops return NOT_INGAME outside INGAME. Watches and freezes carry values right-aligned in a u64.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0030 | MEM_READ | `u32 addr, u32 len` (len at most 65536) | `len` bytes |
| 0x0031 | MEM_WRITE | `u32 addr, bytes` | none |
| 0x0032 | WATCH_ADD | `u32 addr, u8 size` (1, 2, 4 or 8) | `u8 id`. Adding the same address and size again returns the existing id |
| 0x0033 | WATCH_REMOVE | `u8 id` | none |
| 0x0034 | WATCH_LIST | none | `u8 n, { u8 id, u8 size, u8 pad[2], u32 addr }[n]` |
| 0x0035 | FREEZE_ADD | `u32 addr, u8 size, u8 pad[3], u64 value` | `u8 id` |
| 0x0036 | FREEZE_REMOVE | `u8 id` | none |
| 0x0037 | FREEZE_LIST | none | `u8 n, { u8 id, u8 size, u8 pad[2], u32 addr, u64 value }[n]` |
| 0x0038 | PATCH_APPLY | `u16 nwords, u16 pad, { u32 addr, u32 word }[nwords]` (at most 64 words) | none. A client patch keyed by its first address; applying it twice is a no-op |
| 0x0039 | PATCH_REVERT | `u32 first_addr` | none |
| 0x003A | PATCH_LIST | none | `u8 n, { u32 first_addr, u16 nwords, u8 kind, u8 pad, char[32] name }[n]`. kind: 0 client, 1 feature, 2 mod |
| 0x003B | CLEAR_CLIENT | none | none. Removes every watch, freeze and client patch. Toggles and mods are untouched |

### 5.5 Positions and planets (0x004x)

Eight slots per planet per game. Slot contents are opaque game-defined blobs of at most 64 bytes.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0040 | POS_SELECT | `u8 slot` | none |
| 0x0041 | POS_SAVE | `u8 slot`, 0xFF = selected | none |
| 0x0042 | POS_LOAD | `u8 slot`, 0xFF = selected | none |
| 0x0043 | POS_LIST | none | `u8 planet, u8 nslots, { u8 filled, u8 pad[3], f32 x, f32 y, f32 z }[nslots]` for the current planet |
| 0x0044 | POS_CLEAR | `u8 slot` | none |
| 0x0045 | PLANET_LIST | none | `u8 count, char[24] name[count]`; index is the game's planet index |
| 0x0046 | PLANET_SELECT | `u8 planet, u8 flags` (bit0 reset level flags, bit1 reset special bolts) | none |
| 0x0047 | PLANET_LOAD | `u8 planet` 0xFF = selected, `u8 flags` 0xFF = selected | none. The planet request goes out first, then the flags act on the planet being loaded |
| 0x0048 | DIE | none | none |
| 0x0049 | MOBY_TABLE | none | `u32 table_ptr_addr, u32 table_end_ptr_addr, u16 stride, u16 pad`. The client reads the pointers and the entries with MEM_READ |

### 5.6 Unlocks and level flags (0x005x)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0050 | UNLOCK_LIST | none | `u8 ncat, char[24] category[ncat], u8 n, Unlock[n]`, values read live |
| 0x0051 | UNLOCK_SET | `u8 id, u8 field, u16 pad, u32 value` | none. A field the entry does not declare is UNSUPPORTED |
| 0x0052 | reserved | | |
| 0x0053 | LEVELFLAGS_GET | `u8 planet` | `u16 len, bytes`, the game's flag regions for that planet concatenated |
| 0x0054 | LEVELFLAGS_RESET | `u8 planet` | none. The same reset PLANET_LOAD's bit0 performs |
| 0x0055 | LEVELFLAGS_SET | `u8 planet, u8 value, u16 offset` | none. Writes one byte at `offset` into the concatenated region returned by LEVELFLAGS_GET; an offset past its end is BAD_ARG |

```
Unlock (44 bytes):
u8   id
u8   category
u8   fields          bit f set = field f is meaningful for this entry
u8   pad
u32  value[4]        field 0 owned, 1 gold or upgraded, 2 level or XP, 3 ammo
char name[24]
```

Setting field 0 to 1 may carry its game's side effects: in RaC1 a weapon is handed its maximum ammo, as the old client did.

A game that does not offer level flags answers all three ops UNSUPPORTED, and a client that gets that is expected to hide its level-flag panel for that game. RaC2 and RaC3 return 0x10 bytes per planet. RaC1 and Deadlocked answer UNSUPPORTED: Deadlocked has no such region, and RaC1's is not laid out the way the old client's viewer assumed, so it is withheld until the format has been worked out. RaC1's PLANET_LOAD bit0 still resets the flags, which is the one thing racman actually did with them.

### 5.7 Mods (0x006x)

Mods live under `/dev_hdd0/qwark/mods/<TITLEID>/<dirname>/` on the console. The client owns the library and uploads a mod the first time it is used or whenever its content changes: it computes CRC32 over `patch.txt` followed by every referenced `.bin` in patch order, compares it to the `hash` reported by MOD_LIST, and if they differ writes the files with the file ops, writes `qwark.sum` containing the eight hex digits, then MOD_RESCAN and MOD_LOAD.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0060 | MOD_LIST | none | `u8 n, Mod[n]` |
| 0x0061 | MOD_LOAD | `char[32] dirname` | none |
| 0x0062 | MOD_UNLOAD | `char[32] dirname` | none |
| 0x0063 | MOD_SET_AUTO | `char[32] dirname, u8 auto` | none. Persisted in config |
| 0x0064 | MOD_RESCAN | none | none |
| 0x0065 | MOD_INFO | `char[32] dirname` | description text, at most 1024 bytes |

```
Mod (120 bytes):
u8   index
u8   flags           bit0 loaded, bit1 auto, bit2 needs_lua, bit3 previous, bit4 parse_error
u8   pad[2]
u32  hash            content of qwark.sum, 0 when absent
char dirname[32]
char name[32]
char version[16]
char author[32]
```

### 5.8 Files (0x007x)

Paths are absolute, at most 511 bytes, sent as the remainder of the payload with no terminator. Only paths under `/dev_hdd0/` and `/dev_usb` are accepted. These run on the network thread and never touch game memory.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0070 | FILE_OPEN | `u8 mode` (0 read, 1 write and truncate), `path` | `u32 handle` |
| 0x0071 | FILE_WRITE | `u32 handle, bytes` (at most 65536) | none |
| 0x0072 | FILE_READ | `u32 handle, u32 len` | up to `len` bytes; fewer at end of file |
| 0x0073 | FILE_CLOSE | `u32 handle` | none |
| 0x0074 | FILE_DELETE | `path` | none |
| 0x0075 | DIR_LIST | `path` | `u16 n, { u8 type (0 file, 1 dir), u8 namelen, u32 size, char name[namelen] }[n]` |
| 0x0076 | DIR_CREATE | `path` | none. Creates missing parents |
| 0x0077 | DIR_DELETE | `path` | none. Recursive |
| 0x0078 | USER_ID | none | `u32 user_id` |

### 5.9 Combos (0x008x)

A combo fires once when `pad_mask` equals its mask exactly, and re-arms when `pad_mask` returns to 0. Mask 0 disables the combo. Persisted in config.

Actions: 0 SAVE_POSITION, 1 LOAD_POSITION, 2 DIE, 3 LOAD_PLANET, 4 LOAD_SETASIDE_FILE. LOAD_SETASIDE_FILE does nothing in a game that has no savefile helper.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0080 | COMBO_SET | `u8 action, u8 pad[3], u32 mask` | none |
| 0x0081 | COMBO_LIST | none | `u8 n, { u8 action, u8 pad[3], u32 mask }[n]` |

### 5.10 Config (0x009x)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0090 | CONFIG_RELOAD | none | none. Re-reads `/dev_hdd0/qwark/config.txt` after a user edited it |
| 0x0091 | CONFIG_SAVE | none | none. Config is also written on every change |

Config keys the module reads for itself: `log` (1 by default) writes a line per event to `/dev_hdd0/qwark/qwark.log`; `log = 0` silences it, which costs a file open, write and close per line on the console.

## 6. Pad mask layout

OG layout, shared by all four games: l2 0x1, r2 0x2, l1 0x4, r1 0x8, triangle 0x10, circle 0x20, cross 0x40, square 0x80, select 0x100, l3 0x200, r3 0x400, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000.

## 7. Connection lifecycle from the client side

1. Connect, send HELLO, read SessionInfo.
2. SUBSCRIBE with a bound UDP port, then DESCRIBE, PLANET_LIST, WATCH_LIST, FREEZE_LIST, PATCH_LIST, MOD_LIST, COMBO_LIST.
3. Render from telemetry. When `game` changes, repeat step 2 from DESCRIBE. When `PREVIOUS_PENDING` appears, PREVIOUS_LIST and prompt.
4. On any socket error: drop everything, reconnect with backoff, start again at step 1. There is no client state to restore.
