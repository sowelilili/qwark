# qwark wire protocol, version 1

Revision 1.1 (2026-09-07): SessionInfo grew to 164 bytes (16 readouts), Feature is 48 bytes with a `readout` field, LEVELFLAGS_SET added.
Revision 1.2 (2026-09-08): Feature flags bit2 SAVE_ASIDE and bit3 LOAD_ASIDE mark the savefile-helper actions so a client can drive the save-file manager without matching labels.
Revision 1.3 (2026-09-08): Feature flags bit4 LIVE marks a TOGGLE whose state qwark reads back out of game memory, and UNLOCK_LIST now carries four `UnlockFieldDesc` rows that name and type its four per-entry value slots.
Revision 1.4 (2026-09-09): the autosplit event stream. AUTOSPLIT_EVENTS and AUTOSPLIT_DESCRIBE in the 0x00A0 block, and a 20-byte `QE` datagram pushed to every telemetry subscriber the moment a run event happens. See section 8.

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
u8   qwark_version      module build number, currently 4 (see below)
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
u64  toggle_state       bit i set = TOGGLE feature id i is on. A LIVE toggle's bit is whatever game memory says, re-read at 10 Hz (section 5.3.1)
u64  toggle_auto        bit i set = feature id i re-applies itself on game boot. Never set for a LIVE toggle
u64  freeze_active      bit i set = freeze id i is active
u32  mod_loaded         bit i set = console mod index i is loaded
u32  mod_auto           bit i set = console mod index i auto-applies on game boot
u32  mod_previous       bit i set = mod i was loaded before the last same-game reboot and is neither re-applied nor dismissed
```

### 3.1 `qwark_version`, the build number

`qwark_version` is the build number of the module itself (`QWARK_BUILD` in `src/core/proto.h`), and is independent of `protocol_version`. The wire contract can hold still while what sits behind it moves, so the build number increments whenever the feature tables change — a feature added, retired or relabelled — or whenever any other user-visible behaviour changes. It never decrements and is never reused.

Retiring a feature leaves its id behind for good: ids are the wire contract for the toggle bitmaps, nothing is renumbered, and a client that still knows a retired id simply never sees it in DESCRIBE again.

A client ships knowing the build it was developed against. When the console reports a *lower* `qwark_version` than that, the client warns the user that the SPRX on the console is out of date and should be re-uploaded; DESCRIBE still answers, so the client keeps working against whatever tables the older module actually has. A *higher* number is not an error: the console is newer than the client, and DESCRIBE remains the authority on what exists.

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

The same UDP port also receives 20-byte autosplit datagrams, which start `QE` rather than `QWRK` (section 8.3). Tell them apart by the magic and the length.

### 4.1 Same-game reboot and the previous-session record

When the game exits and the same title comes back:

- Watches are kept and resume automatically; their ids do not change.
- Every toggle that was on (LIVE toggles excepted: their state is a byte the game and its save file own, so there is nothing for qwark to re-apply), every active freeze, every client patch and every loaded mod is moved into the **previous-session record** and cleared from the live tables. Items flagged auto (toggle_auto, mod_auto) are re-applied silently when the session reaches INGAME and are removed from the record. If anything remains, `flags.PREVIOUS_PENDING` is set; the client is expected to prompt the user (default No) and answer with PREVIOUS_REAPPLY or PREVIOUS_DISMISS.

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
| 0x0021 | FEATURE_SET | `u8 id, u32 value` (TOGGLE: 0 or 1) | none. Works on a LIVE toggle too: it writes the byte |
| 0x0022 | FEATURE_TRIGGER | `u8 id` (ACTION only) | none |
| 0x0023 | FEATURE_SET_AUTO | `u8 id, u8 auto` | none. Persisted in config. UNSUPPORTED on a LIVE toggle |
| 0x0024 | FEATURE_OPTIONS | `u8 id` (ENUM only) | `u8 count, char[24] option[count]` |

```
Feature (48 bytes):
u8   id
u8   kind
u8   group          index into DESCRIBE groups
u8   aux            ENUM: option count. 0 for every other kind
u8   flags          bit0 AUTO (same as toggle_auto), bit1 WRITES_CODE (instruction patch), bit2 SAVE_ASIDE (this ACTION makes the game write its current save to /dev_hdd0/game/<TITLEID>/USRDIR/tempsave), bit3 LOAD_ASIDE (this ACTION makes the game load that tempsave), bit4 LIVE (TOGGLE only, section 5.3.1)
u8   readout        VALUE, ENUM, COLOR: index into SessionInfo.readout[] that mirrors the current value, 0xFF if none. TOGGLE and ACTION: 0xFF
u8   pad[2]
u32  min
u32  max            VALUE and ENUM range, inclusive; both 0 when unbounded
char label[32]
```

Toggle state travels in `toggle_state`; every other kind that has state travels through its readout, so a second client or a restarted one sees the real value.

#### 5.3.1 LIVE toggles (revision 1.3)

Most toggles are qwark's: an instruction patch, a freeze entry, a piece of module state. A few are not — they are a plain byte the game owns, which lives in the save file and which the player, another tool or the game itself can change at any moment. Those are flagged `LIVE`.

For a LIVE toggle:

- qwark re-reads the byte out of game memory every twelfth tick (10 Hz) while INGAME and sets `toggle_state` bit *id* to match. The client's checkbox therefore shows what the game says, not what qwark last wrote, and a client that connects mid-session sees the truth immediately.
- There is nothing for qwark to re-apply, so **FEATURE_SET_AUTO is answered UNSUPPORTED** and the bit never appears in `toggle_auto`. A client should draw no auto-apply control for a LIVE toggle.
- It never joins the previous-session record (section 4.1): after a reboot the byte comes back from the save file on its own.
- **FEATURE_SET still works** and still writes the byte, on and off. The next poll simply agrees with it.

The LIVE toggles today:

| Game | Feature id | Label | What it is |
|---|---|---|---|
| RaC1 | 6 | Goodies menu | one byte at 0x969CD3, part of the save |
| RaC1 | 24, 25, 26 | Update Ratchet / mobys / particles | three bits of the debug update word at 0x95C5C8 |
| RaC2 | 5 | Enable debug mode | one byte at 0x15B3070 |
| RaC3 | 5 | Quick-select pause | one byte at 0xC1E652 |

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

POS_SELECT and PLANET_SELECT write straight through to `config.txt`, so the selection is remembered across a client restart and across a console reboot; no CONFIG_SAVE is needed. It comes back in `SessionInfo.selected_slot`.

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
| 0x0050 | UNLOCK_LIST | none | `u8 ncat, char[24] category[ncat], UnlockFieldDesc[4], u8 n, Unlock[n]`, values read live |
| 0x0051 | UNLOCK_SET | `u8 id, u8 field, u16 pad, u32 value` | none. A field the entry does not declare is UNSUPPORTED |
| 0x0052 | reserved | | |
| 0x0053 | LEVELFLAGS_GET | `u8 planet` | `u16 len, bytes`, the game's flag regions for that planet concatenated |
| 0x0054 | LEVELFLAGS_RESET | `u8 planet` | none. The same reset PLANET_LOAD's bit0 performs |
| 0x0055 | LEVELFLAGS_SET | `u8 planet, u8 value, u16 offset` | none. Writes one byte at `offset` into the concatenated region returned by LEVELFLAGS_GET; an offset past its end is BAD_ARG |

```
UnlockFieldDesc (16 bytes), revision 1.3, one per value slot 0..3, always four:
char name[12]        NUL-padded, e.g. "Owned", "Gold", "Level", "XP", "Ammo".
                     Empty = the game never uses this slot; the client draws
                     no column for it and no row declares it
u8   kind            0 flag (draw a checkbox, write 0 or 1)
                     1 number (draw a number box)
u8   max             kind 1: the largest meaningful value, 0 = no limit
u8   reserved[2]     zero

Unlock (44 bytes):
u8   id
u8   category
u8   fields          bit f set = value slot f is meaningful for this entry
u8   pad
u32  value[4]        one per slot, as the UnlockFieldDesc rows describe them
char name[24]
```

Before revision 1.3 the four slots were fixed as owned / gold / level / ammo, which was RaC1's reading of them and wrong everywhere else. The `UnlockFieldDesc` rows are now the authority; slot 0 is "Owned" in every game qwark knows, but nothing on the wire requires that.

What the four games ship today:

| Game | Slot 0 | Slot 1 | Slot 2 | Slot 3 |
|---|---|---|---|---|
| RaC1 | Owned, flag | Gold, flag | — | Ammo, number |
| RaC2 | Owned, flag | — | — | — |
| RaC3 | Owned, flag | **Level, number, max 8** | **XP, number** | Ammo, number |
| Deadlocked | Owned, flag | — | — | — |

RaC3's slot 1 is the weapon version UYAUnlocks drew as a v1..v8 combo box, not a gold flag: gold weapons are a RaC1 idea and RaC3 has none. `max` is the game-wide maximum, so a client may offer 8 for every weapon; UNLOCK_SET clamps to the entry's own level count, and the R3YNO — the one weapon that stops at v5 — lands on v5. Writing 1 is a real downgrade, not "unset".

Setting field 0 to 1 may carry its game's side effects: in RaC1 a weapon is handed its maximum ammo, as the old client did.

An entry may withhold a slot its category otherwise offers, and UNLOCK_SET on that slot is then UNSUPPORTED: RaC3's Suck Cannon declares no ammo, because the game does not count any for it.

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

### 5.11 Autosplitting (0x00Ax)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x00A0 | AUTOSPLIT_EVENTS | `u32 since_seq` | `u32 latest_seq, u8 n, Event[n]`: every event with `seq > since_seq` that the 64-entry ring still holds, oldest first (`since_seq` 0 = all of it); `latest_seq` is 0 before the first event |
| 0x00A1 | AUTOSPLIT_DESCRIBE | none | `u8 n, EventDesc[n]` for the running game. UNSUPPORTED when no game is running or the game has no watcher |

AUTOSPLIT_EVENTS answers whatever the session state is: a client that reconnects after a crash still wants the splits that happened while it was away. AUTOSPLIT_DESCRIBE is gated on INGAME the same way DESCRIBE is, because under BCES01503 the running game is not known until the fingerprint answers.

## 6. Pad mask layout

OG layout, shared by all four games: l2 0x1, r2 0x2, l1 0x4, r1 0x8, triangle 0x10, circle 0x20, cross 0x40, square 0x80, select 0x100, l3 0x200, r3 0x400, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000.

## 7. Connection lifecycle from the client side

1. Connect, send HELLO, read SessionInfo.
2. SUBSCRIBE with a bound UDP port, then DESCRIBE, AUTOSPLIT_DESCRIBE, PLANET_LIST, WATCH_LIST, FREEZE_LIST, PATCH_LIST, MOD_LIST, COMBO_LIST.
3. Render from telemetry. When `game` changes, repeat step 2 from DESCRIBE. When `PREVIOUS_PENDING` appears, PREVIOUS_LIST and prompt.
4. On any socket error: drop everything, reconnect with backoff, start again at step 1. There is no client state to restore.

## 8. Autosplitting (revision 1.4)

**qwark detects, the client decides.** qwark watches game memory on the tick thread and emits run events. It keeps **no timer**, applies **no user setting**, and knows nothing about LiveSplit: every split candidate goes out unconditionally with a reason code, and the PC decides which ones to act on. Anything the old ASL scripts gated on a *setting* is still emitted here; only what they gated on *game state* is a condition qwark evaluates.

Only Deadlocked emits PAUSE and RESUME. RaC1, RaC2 and RaC3 never do.

Events are emitted only while the session is INGAME.

### 8.1 Event, 16 bytes

```
u32 seq        1, 2, 3 ... per qwark load; never reset while the module runs
               (a game reboot does not reset it)
u32 tick       session tick count when the event was detected
u8  kind       1 START, 2 SPLIT, 3 RESET, 4 PAUSE, 5 RESUME
u8  code       per-game reason (see AUTOSPLIT_DESCRIBE); 0 for START/RESET/PAUSE/RESUME
u16 reserved   0
u32 arg        code-specific; for the planet-enter code it is the planet index in
               PLANET_LIST order
```

In every game the old script's `start` and `reset` blocks are the same expression, so qwark emits **RESET immediately followed by START** on that condition and the client applies whichever suits its timer, exactly as LiveSplit does with the two blocks.

### 8.2 EventDesc, 28 bytes

```
u8  code      1..255; code 1 is RESERVED for "planet entered" in every game
              (arg = planet index in PLANET_LIST order)
u8  kind      the kind this code is emitted with (2 SPLIT for all of them today)
u8  flags     bit0 = a client should enable it by default
              bit1 = "planet route" applies (only code 1 sets it)
u8  reserved  0
char label[24]  NUL-padded, what the client shows as a checkbox
```

The list is static per game and never changes within a session.

### 8.3 The UDP push

On the tick an event is emitted, and again on each of the next two ticks, qwark sends a 20-byte datagram to every telemetry subscriber — the same UDP socket and subscriber list telemetry uses. Three sends cover a lost datagram without anyone keeping a timer; a client that sees the same `seq` three times ignores the repeats.

```
char magic[2]  'Q','E'
u8   version   1
u8   reserved  0
Event          16 bytes, section 8.1
```

The magic and the length tell it apart from a telemetry packet, which always starts `QWRK` and is never 20 bytes. An event is **never** carried inside the telemetry packet. Datagrams go out every tick rather than every fourth, because a split has to reach the PC in single-digit milliseconds. AUTOSPLIT_EVENTS over TCP is the catch-up read for anything that was dropped.

### 8.4 Reason codes per game

Ported from racman's `rac1-autosplitter.asl`, `rac2-autosplitter.asl`, `rac3-autosplitter.asl` and `rac4-LC-autosplitter.asl`. "Default" is bit0 of `flags`, and matches whether the script's setting defaulted to true.

**RaC1** — START/RESET when the game state goes 6 to 0 while on Veldin (planet 0).

| Code | Label | Default | `arg` |
|---|---|---|---|
| 1 | Planet entered | yes (route) | the destination planet |
| 2 | Veldin | yes | 0. Fires once per run; a latch stops the double split |
| 3 | Drek button | yes | 0. The player state reaching 34 within 1.7 of one of the four buttons |
| 4 | Gold bolt collected | no | the helper mod's counter |
| 5 | Skill point | no | the helper mod's counter |
| 6 | Item collected | no | the helper mod's counter |
| 7 | Infobot | no | the helper mod's counter |

Codes 4 to 7 count changes in four words the `gb_sp_as_helper` mod keeps; without that mod loaded they never change, which is what the old autosplitter did too. Code 4 also fires for the Kalebo3 gold bolt, code 6 also for the codebot and the raritanium, exactly as the script had them.

**RaC2** — START/RESET when the player state goes 0 to 98 while on Aranos (planet 0).

| Code | Label | Default | `arg` |
|---|---|---|---|
| 1 | Planet entered | yes (route) | the planet just entered; never the Insomniac Museum (21) |
| 2 | Protopet defeated | yes | 0 |
| 3 | Aranos 2 Clank swap | yes | 0 |
| 4 | Maktar arena entry | no | 0 |
| 5 | Barlow race entry | no | 0 |
| 6 | Endako Clank entry | no | 0 |
| 7 | Endako Clank exit | no | 0 |
| 8 | Tabora caves | no | 0 |

**RaC3** — START/RESET when the game state goes 6 to 0 while on Veldin (planet 1).

| Code | Label | Default | `arg` |
|---|---|---|---|
| 1 | Planet entered | yes (route) | the destination planet |
| 2 | LDF entered | no | 0 |
| 3 | Tyhrraguise obtained | no | 0 |
| 4 | Koros bolt 2 | no | 0. The second titanium bolt of the run on Koros |
| 5 | Biobliterator defeated | yes | 0. Armed by an odd Neffyrious phase at full health |

The script's split route and its strict-order mode are the client's business: qwark emits code 1 for every planet change and the PC filters.

**Deadlocked** — START/RESET when a load starts for Dread Zone (planet 1) with the tutorial flag clear.

| Code | Label | Default | `arg` |
|---|---|---|---|
| 1 | Planet entered | yes (route) | the destination planet |
| 2 | Vox defeated | yes | the destination planet, which is 0 by then |

Deadlocked is the only game whose detection already ran on the console; the two codes replace the old command stream's single "split" whose packet byte the LC script had to read as "0 means Vox". The script's AEC setting, which disables resetting, is the client's business: qwark still emits every RESET.

PAUSE goes out when the game leaves the running state — the quit hook fires, or the process disappears underneath qwark — and RESUME on the first tick after Deadlocked is INGAME again. Some categories quit to the VSH as a strategy, and the client stops its timer for as long as that lasts.
