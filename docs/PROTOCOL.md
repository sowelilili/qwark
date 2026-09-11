# qwark wire protocol, version 1

Revision 1.1 (2026-09-07): SessionInfo grew to 164 bytes (16 readouts), Feature is 48 bytes with a `readout` field, LEVELFLAGS_SET added.
Revision 1.2 (2026-09-08): Feature flags bit2 SAVE_ASIDE and bit3 LOAD_ASIDE mark the savefile-helper actions so a client can drive the save-file manager without matching labels.
Revision 1.3 (2026-09-08): Feature flags bit4 LIVE marks a TOGGLE whose state qwark reads back out of game memory, and UNLOCK_LIST now carries four `UnlockFieldDesc` rows that name and type its four per-entry value slots.
Revision 1.4 (2026-09-09): the autosplit event stream. AUTOSPLIT_EVENTS and AUTOSPLIT_DESCRIBE in the 0x00A0 block, and a 20-byte `QE` datagram pushed to every telemetry subscriber the moment a run event happens. See section 8.
Revision 1.5 (2026-09-09): autosplit timing. The Event's second word is `time_ms` rather than a tick count, the kinds gained 6 LOAD_START and 7 LOAD_END, and EventDesc is 32 bytes with a `param_us` and two new flags, FLAT and NORMALISE, that carry the game-time adjustments the old ASL scripts made. See section 8.
Revision 1.6 (2026-09-09): SessionInfo `flags` gained bit1 EMULATOR and bit2 NO_CODE_PATCHES, so a client can tell that qwark is driving RPCS3 through PINE rather than a console and that every WRITES_CODE feature is refused there. See section 3.2.
Revision 1.7 (2026-09-09): signed VALUEs. The first of Feature's two pad bytes is now `bits`, the width in bits of the field behind a VALUE, and Feature flags bit5 SIGNED says that field is two's complement in that width. See section 5.3.2.
Revision 1.8 (2026-09-09): COMBO_SUSPEND, which holds every stored combo off while a client captures a new one, so the buttons being recorded do not also fire the combos already there. See section 5.9.
Revision 1.9 (2026-09-10): the savefile block. SAVEFILE_INFO, SAVEFILE_READ and SAVEFILE_WRITE in the 0x00B0 block. A save no longer travels as a file: qwark embeds one helper per game, installs it invisibly on first use, and the helper parks the save in a RAM buffer these three ops stream. See section 5.12.
Revision 1.10 (2026-09-10): the savefile library moves onto the console. SAVEFILE_CATEGORIES, SAVEFILE_LIST, SAVEFILE_STORE, SAVEFILE_RESTORE and SAVEFILE_CATEGORY at 0x00B3, FILE_RENAME at 0x0079, and a SAVEFILE_INFO grown to 20 bytes that reports the copy qwark now runs between a file and the aside buffer. See section 5.13.

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
u8   qwark_version      module build number, currently 12 (see below)
u8   state              0 XMB, 1 BOOTING, 2 INGAME, 3 QUITTING
u8   game               0 NONE, 1 RAC1, 2 RAC2, 3 RAC3, 4 RAC4 (Deadlocked)
                        BCES01503, the disc trilogy, reports 1, 2 or 3 depending
                        on which executable is mapped; the client never picks
u32  generation         incremented every time a game enters BOOTING
u32  tick               tick-thread counter, 120 Hz
char title_id[12]       e.g. "NPEA00385"
u8   flags              bit0 PREVIOUS_PENDING: a previous-session record is waiting (section 4.1)
                        bit1 EMULATOR: qwark is driving an emulator, not a console (section 3.2)
                        bit2 NO_CODE_PATCHES: this platform refuses code patches (section 3.2)
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

### 3.2 `flags` bit1 EMULATOR and bit2 NO_CODE_PATCHES (revision 1.6)

qwark also runs on a PC as `qwark-rpcs3.exe`, driving RPCS3 through its PINE IPC server instead of a console through PS3MAPI. The core, the game tables and every opcode in this document are the same; two things about the platform are not, and both travel in `flags`.

**bit1 EMULATOR** is set when the thing on the other side is an emulator. It is cosmetic: say "RPCS3" rather than "console" and do not offer to upload an SPRX.

**bit2 NO_CODE_PATCHES** is not cosmetic. RPCS3 recompiles PPU code ahead of execution, so writing an instruction word changes memory and the game carries on running the old instruction. Rather than let a checkbox lie, qwark refuses everything that depends on a code patch:

| Request | Answer when bit2 is set |
|---|---|
| FEATURE_SET or FEATURE_TRIGGER on a feature flagged `WRITES_CODE` | `UNSUPPORTED` |
| PATCH_APPLY | `UNSUPPORTED` |
| MOD_LOAD of a mod with patch words or code caves | `UNSUPPORTED` |
| SAVEFILE_INFO, SAVEFILE_READ, SAVEFILE_WRITE, and the SAVE_ASIDE and LOAD_ASIDE actions | `UNSUPPORTED` (section 5.12) |
| Every op of the savefile library: SAVEFILE_CATEGORIES, SAVEFILE_LIST, SAVEFILE_STORE, SAVEFILE_RESTORE, SAVEFILE_CATEGORY | `UNSUPPORTED` (section 5.13). The library is only reachable through the helper, so without one there is nothing to copy out of and nothing to hand a copy to |

Everything else works unchanged: memory reads and writes, freezes, watches, positions, planet loads, unlocks, level flags, colours, values, and every toggle whose truth is a data byte rather than an instruction.

DESCRIBE is **unchanged**: the WRITES_CODE rows are still listed, with the same ids, labels, groups and flags. A client greys them itself from bit2 rather than discovering one refusal at a time, so the same DESCRIBE parsing works against both platforms and a client built before this revision still functions, it simply sees the refusals instead of predicting them.

Two side effects a client should know about, both inside qwark rather than on the wire:

- Auto-flagged WRITES_CODE toggles (Deadlocked's crash patches, for instance) are skipped when a game reaches INGAME. `toggle_state` reports them as off, which is the truth.
- The games' own embedded helpers are code patches too, so they are not installed. RaC1's autosplit helper is one, and without it the four collectable reason codes (gold bolt, skill point, item, infobot) never fire; every other RaC1 split is a plain memory read and is unaffected. Deadlocked's quit and loading hooks are the others: the PAUSE still fires when the game goes away, because the session also watches the process itself, and the RESUME then fires on the way back INGAME rather than on the SCE logo. The savefile helper is a third, which is why the whole of section 5.12 is refused here.

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

Each game exposes up to 64 features with stable ids. Kinds: 0 TOGGLE, 1 ACTION, 2 VALUE (u32, or two's complement when the row is flagged SIGNED, section 5.3.2), 3 ENUM, 4 COLOR (`0x00RRGGBB`).

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0020 | DESCRIBE | none | `u8 game, u8 ngroups, char[24] group[ngroups], u8 nreadouts, char[24] readout[nreadouts], u8 nfeatures, Feature[nfeatures]`. At most 16 groups, 16 readouts, 64 features |
| 0x0021 | FEATURE_SET | `u8 id, u32 value` (TOGGLE: 0 or 1; SIGNED VALUE: the low `bits` bits, section 5.3.2) | none. Works on a LIVE toggle too: it writes the byte |
| 0x0022 | FEATURE_TRIGGER | `u8 id` (ACTION only) | none |
| 0x0023 | FEATURE_SET_AUTO | `u8 id, u8 auto` | none. Persisted in config. UNSUPPORTED on a LIVE toggle |
| 0x0024 | FEATURE_OPTIONS | `u8 id` (ENUM only) | `u8 count, char[24] option[count]` |

```
Feature (48 bytes):
u8   id
u8   kind
u8   group          index into DESCRIBE groups
u8   aux            ENUM: option count. 0 for every other kind
u8   flags          bit0 AUTO (same as toggle_auto), bit1 WRITES_CODE (instruction patch), bit2 SAVE_ASIDE (this ACTION asks the game to copy its current save into the savefile helper's aside buffer), bit3 LOAD_ASIDE (this ACTION asks the game to load what is in that buffer), bit4 LIVE (TOGGLE only, section 5.3.1), bit5 SIGNED (VALUE only, section 5.3.2)
u8   readout        VALUE, ENUM, COLOR: index into SessionInfo.readout[] that mirrors the current value, 0xFF if none. TOGGLE and ACTION: 0xFF
u8   bits           VALUE: the width in bits of the field behind it, 8, 16 or 32. 0 means 32, so a row that names no width reads as it always did. 0 for every other kind (revision 1.7)
u8   pad
u32  min
u32  max            VALUE and ENUM range, inclusive; both 0 when unbounded. A SIGNED VALUE leaves both 0: `bits` already says what the range is
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

#### 5.3.2 Signed VALUEs (revision 1.7)

A VALUE is a `u32` on the wire, which is right for a bolt count and wrong for a field the game itself reads as signed: the QE offsets are halfwords whose useful value is -1, and a client that shows 65535 there is showing the bits rather than the number.

Two fields carry the width and the signedness:

- **`bits`**, the first of what used to be Feature's two pad bytes, is the width in bits of the field behind a VALUE: 8, 16 or 32. **0 means 32**, so every row written before this revision, and every row that has no reason to say, is read exactly as it was. Every other kind sends 0.
- **flags bit5 SIGNED** says that field is two's complement in `bits` bits. It only ever appears on a VALUE.

What each side does:

- The **readout** that mirrors the feature keeps carrying the **raw field**, zero-extended into the u32 as it always was: a QE offset of -1 arrives as 0x0000FFFF. The client sign-extends it from `bits` before showing it.
- **FEATURE_SET still carries `u32 value`**. For a signed feature the client sends the low `bits` bits of the number it wants, so -1 on a 16-bit field is 0xFFFF, and qwark writes the field exactly as it does for an unsigned one. Nothing about the write path changed.
- A signed row leaves **`min` and `max` both 0**, which already means unbounded. The width is the range: a client offers -32768..32767 on a 16-bit field and -2147483648..2147483647 on a 32-bit one, and clamps what the user types to it.

A client that predates this revision reads `bits` as padding and the flag as a bit it does not know, so it goes on showing the raw field: the same behaviour it had before, not a new failure.

The signed VALUEs today:

| Game | Feature id | Label | Field |
|---|---|---|---|
| RaC2 | 10 | Health XP | signed 32-bit |
| RaC2 | 23 | QE save write-offset | signed 16-bit halfword |
| RaC3 | 9 | Health XP | signed 32-bit |
| RaC3 | 14 | QE offset | signed 16-bit halfword |

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

A whole category may withhold one. The four descriptor rows say what a game names, not what every row carries, so a client draws a column from the descriptors and then a cell only where the row's `fields` bit is set. In RaC3 the level, XP and ammo slots belong to the weapons: its gadgets, items and vid comics are owned or not owned and declare slot 0 alone.

An id is retired rather than renumbered when an entry goes: the remaining rows keep the ids they had, the list simply gets shorter, and UNLOCK_SET on the retired id answers BAD_ARG. RaC3's id 0, the Bomb Glove, is the one retired so far, dropped because UYA has no way to reach the item in game.

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

These are the transport of both libraries: the client uploads a mod's files with them (section 5.7), and it uploads, mirrors, renames and deletes savefiles with them (section 5.13). What a savefile no longer travels this way for is a *load*: the console keeps the file and copies it into the aside buffer itself.

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
| 0x0079 | FILE_RENAME | `u16 from_len, char from[from_len], char to[rest]` | none (revision 1.10) |

**FILE_RENAME** (revision 1.10) moves a file within `/dev_hdd0`. Both paths obey the rule above; the old path must exist (`NOT_FOUND` otherwise) and the new one must **not** (`BAD_ARG` if it does), because cellFs and Windows refuse to replace an existing destination while POSIX replaces it silently, and a rename that means one thing on a console and another in the simulator is worse than one that never overwrites anywhere. A client that means to replace a file deletes it first.

It sits at 0x0079 rather than at 0x0075, where a rename would naturally have gone: 0x0075 has been DIR_LIST since revision 1, and an opcode is never renumbered.

### 5.9 Combos (0x008x)

A combo fires once when `pad_mask` equals its mask exactly, and re-arms when `pad_mask` returns to 0. Mask 0 disables the combo. Persisted in config.

Actions: 0 SAVE_POSITION, 1 LOAD_POSITION, 2 DIE, 3 LOAD_PLANET, 4 LOAD_SETASIDE_FILE. LOAD_SETASIDE_FILE makes the game load whatever is in the savefile helper's aside buffer (section 5.12), and does nothing in a game that has no helper or where code cannot be patched.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0080 | COMBO_SET | `u8 action, u8 pad[3], u32 mask` | none |
| 0x0081 | COMBO_LIST | none | `u8 n, { u8 action, u8 pad[3], u32 mask }[n]` |
| 0x0082 | COMBO_SUSPEND | `u8 suspend` (1 hold every combo off, 0 resume) | none |

COMBO_SUSPEND (revision 1.8) exists for capture. A client that records a combo reads the buttons out of telemetry's `pad_mask`, which is the same pad the console is watching, so without the hold the press that records "load position" also loads a position. Send 1 when the capture starts and 0 when it commits, is cancelled or is dropped; it is not a game write, so it answers in any session state.

The hold expires by itself two minutes after the COMBO_SUSPEND that set it, and is dropped when the session leaves the game. A client that dies mid-capture therefore cannot leave the combos off for good, and a client that resumes at the two-minute mark just sends 1 again. Lifting a hold does not re-arm anything: a combo still held when the hold ends fires only after the pad has returned to 0, which is the ordinary arming rule.

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
| 0x00A1 | AUTOSPLIT_DESCRIBE | none | `u8 n, EventDesc[n]` (32 bytes a row since 1.5) for the running game. UNSUPPORTED when no game is running or the game has no watcher |

AUTOSPLIT_EVENTS answers whatever the session state is: a client that reconnects after a crash still wants the splits that happened while it was away. AUTOSPLIT_DESCRIBE is gated on INGAME the same way DESCRIBE is, because under BCES01503 the running game is not known until the fingerprint answers.

### 5.12 Save files (0x00Bx), revision 1.9

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x00B0 | SAVEFILE_INFO | none | `u8 supported, u8 installed, u8 running, u8 pending, u32 size, u32 done, u32 total, u8 error, u8 pad[3]` (20 bytes since revision 1.10) |
| 0x00B1 | SAVEFILE_READ | `u32 offset, u32 len` (len at most 65536) | the bytes of the aside buffer at that offset; short at the end |
| 0x00B2 | SAVEFILE_WRITE | `u32 offset, bytes` (at most 65536) | none |

All three answer **UNSUPPORTED** where the platform cannot patch code (RPCS3, `flags` bit2) and **NOT_INGAME** outside INGAME, and all three install the game's helper on demand: a client never asks for that and never sees it happen.

Since revision 1.10 READ and WRITE are the **debug and test path**, not what a save or a load does: the library ops of section 5.13 move a whole file between the console's own filesystem and this buffer without any of it crossing the wire. They still work exactly as they did, and a client that wants the bytes on the PC still reads them here.

- **supported** — 1 when qwark has a helper for the running game. 0 is an OK answer, not an error: it is how a client knows to hide its save-file panel. All four games are 1 today.
- **installed** — 1 when qwark has written the helper into this process. Since asking is what installs it, this is 1 whenever `supported` is.
- **running** — 1 when the helper's own byte reads 1. The helper writes it on every call, so this says the code is installed *and* that the game is reaching the hook. It is 0 for the first frame or two after an install, and it stays 0 for as long as the game is on a screen that does not run the hooked routine.
- **pending** — bit0: a set-aside request is still outstanding. bit1: a load request is still outstanding. bit2 (revision 1.10): qwark is copying between a file and the aside buffer, section 5.13. The helper clears its own request byte when it has done the work, so a client polls this rather than guessing at a delay.

  **A clear bit means the work is over, not that the byte happens to read 0.** Build 11 made that true at both ends. In the game, the helper clears the set-aside byte *after* its copy loop rather than before it, so the byte reading 0 is the copy having finished; it copies a megabyte or two in 32 KB steps, and clearing first meant a client could read the aside buffer while the copy was still walking it and save a file with a torn tail. In qwark, a bit goes up the moment the request byte is written and stays up until the byte has read 0 on **every tick of a settle window**, 30 ticks of the 120 Hz loop, a quarter of a second (`SAVEFILE_SETTLE_TICKS` in `src/core/savefile.h`). The window is there because one read of one byte of a running game is thin evidence: the helper puts no barrier between the copy and the store that clears the byte, and a memory read that fails answers with a zero of its own. A byte that goes back to non-zero inside the window starts it again.

  A client should therefore poll INFO until the bit clears and only then read (or, for a load, only then consider the load done). The quarter of a second is the price of the guarantee, on an operation a user waits seconds for.
- **size** — how many bytes the aside buffer holds, which is the size of a save file for that game. It is fixed per game and does not depend on the save.
- **done**, **total**, **error** (revision 1.10) — the transfer of section 5.13. `done` and `total` are its bytes, and `error` is why the last one stopped: 0 none, 1 file missing, 2 io error, 3 the file is not exactly `total` bytes, 4 the game went away underneath the copy, 5 the aside buffer was already spoken for. All three describe the **last** transfer until the next STORE or RESTORE starts, so a client that polls once more after `pending` bit2 clears reads how it ended rather than zeroes.

A client built against revision 1.9 reads the first eight bytes and is right about every one of them; the three new fields are appended, and nothing before them moved.

**A save, end to end.** FEATURE_TRIGGER the game's SAVE_ASIDE action; poll SAVEFILE_INFO until `pending` bit0 clears; SAVEFILE_READ the whole buffer in 64 KB chunks. **A load** is the reverse: SAVEFILE_WRITE the file into the buffer in chunks from offset 0, in order, each write answered before the next goes out, then FEATURE_TRIGGER the LOAD_ASIDE action and poll until `pending` bit1 clears. The bytes are opaque; nothing on the PC knows the save format.

A client is expected to hold the whole save until it has every chunk before it writes a file, and to send a file only when its length is exactly `size`: the console cannot tell a short file from a whole one, and a save that is neither is what the game crashes on later.

Bounds: a READ whose `offset` is past the end is BAD_ARG and one that runs off the end comes back short, so a client can ask for a round chunk at every offset. A WRITE that would run off the end is BAD_ARG rather than trimmed, because a client sending more than the buffer holds has the wrong file.

**Where the buffer comes from.** qwark carries a small piece of PowerPC code per game, compiled from one source in `src/games/sfhelper/` and embedded as bytes. Installing it writes one or two code caves and a branch word into the running game; from then on the game calls it once a frame, and it does nothing until a request byte changes. It is never reverted: taking a branch back out from under code that may be executing it is a crash, and with no request outstanding it costs a byte write and three comparisons a frame. Nothing is written to the console's filesystem at any point.

Before this revision the same two ACTIONs moved a `tempsave` file under `/dev_hdd0/game/<TITLEID>/USRDIR`, which the client then pulled over the FILE ops, and the helper was a mod the user had to load first. Both are gone. The FILE ops stay, for the mod library.

## 5.13 The savefile library on the console (0x00B3..0x00B7), revision 1.10

**Where a save lives.** The library of record is on the console, laid out like the mod library:

```
/dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav
/dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav.sum
```

The `.sum` sidecar holds eight lowercase hex digits of CRC32 over the `.sav` beside it, exactly as `qwark.sum` does for a mod. qwark writes it whenever it stores a file, and computes it once for a file the client uploaded itself, so a listing never costs a 2 MB read twice.

A save is up to 2 MB, and until this revision every load streamed one from the PC over SAVEFILE_WRITE. Now the console copies it out of its own filesystem: the PC's copy is a **mirror**, kept so nothing is lost when a console is wiped, and a file that exists only on the PC is uploaded once, on first use, and lives on the console from then on.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x00B3 | SAVEFILE_CATEGORIES | none | `u8 n, char[32] name[n]` |
| 0x00B4 | SAVEFILE_LIST | `char[32] category` | `u8 n, { char[32] name, u32 size, u32 crc32 }[n]` |
| 0x00B5 | SAVEFILE_STORE | `char[32] category, char[32] name` | none |
| 0x00B6 | SAVEFILE_RESTORE | `char[32] category, char[32] name` | none |
| 0x00B7 | SAVEFILE_CATEGORY | `u8 op` (0 create, 1 delete), `char[32] name` | none |

All five answer **UNSUPPORTED** where the platform cannot patch code and **NOT_INGAME** outside INGAME, for the same reason section 5.12 does: the library exists to feed the helper, and without a helper there is nothing to feed. The title id in the path is always the running game's, so a client never names one.

**Names.** A category or a file name is a NUL-padded 32-byte field. qwark refuses one that is empty, starts with a dot, holds `/`, `\` or `:`, or has a byte outside printable ASCII, and it refuses a file name that does not end in `.sav`, because the listing reports nothing else and a file stored under another name would be one nobody could ever see again. Everything else is taken as given.

**SAVEFILE_LIST** reports only `<name>.sav` files: the sidecars are qwark's own bookkeeping and never appear. `size` is the file's size and `crc32` is the CRC of its contents, which is the ordinary reflected CRC-32 that zlib, PNG and `qwark.sum` all use, so a client compares it against its own mirror to decide whether the two copies agree.

**SAVEFILE_STORE** raises the set-aside request, waits out the settle window of section 5.12 (so the copy inside the game is over before a byte of it is read), then copies the aside buffer into the file and writes the sidecar. **SAVEFILE_RESTORE** is the reverse: it copies the whole file into the buffer and only then raises the load request, so the game never sees a half-filled buffer. The file must be exactly `size` bytes, and a file that is not is refused with `BAD_ARG` and error 3 without anything being raised at the game; a file that is not there is `NOT_FOUND` with error 1, again with nothing raised.

A store that stops part way through leaves **nothing** behind: the half-written file is unlinked, because a file of the right name and the wrong length is what a user would take for their save. The copy it was replacing is gone either way, since opening the file for writing truncated it.

**One at a time.** There is one aside buffer, so a STORE or RESTORE while another is in flight, or while the game has a set-aside or load of its own outstanding, is answered `BUSY` and error 5. SAVEFILE_CATEGORIES, SAVEFILE_LIST and SAVEFILE_CATEGORY answer `BUSY` too while a transfer runs, because a listing taken half way through a write would report the size and the sum of half a save.

**The transfer, and how long it takes.** The copy runs on qwark's tick thread as a chunked state machine: 64 KB a chunk, two chunks a tick, so 128 KB per tick of the 120 Hz loop. A 2 MB save is 32 chunks, sixteen ticks, an eighth of a second; RaC1's 704 KB save is eleven chunks and six ticks. Telemetry, freezes, watches and every other request keep flowing throughout, which is the whole reason it is a state machine rather than a loop. On top of the copy, a STORE waits the settle window before it starts (a quarter of a second) and a RESTORE waits one after it (the same), so end to end either is well under a second for the largest save.

Poll SAVEFILE_INFO: `pending` bit2 is up from the moment the op is accepted until the copy **and** its request have both finished, `done` and `total` are the progress, and `error` says how the last one ended.

**Deletes and renames** go through FILE_DELETE and FILE_RENAME with the whole path, which the client builds from the layout above. Two rules follow from the sidecar:

- Deleting a save means deleting `<name>.sav` **and** `<name>.sav.sum`; `NOT_FOUND` on the second is not a failure, since a file the client uploaded may not have one yet.
- Renaming means renaming both, for the same reason.

Deleting a category (`op` 1) removes the folder. It sweeps up any `.sum` files left in it first, so a client that forgot a sidecar cannot end up with a category it is unable to remove, but a folder that still holds a `.sav` is refused: `IO_ERROR`, and the category stays. A category that is not there is `NOT_FOUND`.

**What a client does.**

- *Save:* SAVEFILE_STORE, poll INFO to completion showing `done`/`total`, then, if it mirrors, read the file down with FILE_OPEN/FILE_READ in the background.
- *Load:* if the console has the file and its CRC matches the PC's copy, SAVEFILE_RESTORE. If it is only on the PC, or the two differ, upload it once with FILE_OPEN/FILE_WRITE/FILE_CLOSE and then SAVEFILE_RESTORE. Poll INFO either way.

## 6. Pad mask layout

OG layout, shared by all four games: l2 0x1, r2 0x2, l1 0x4, r1 0x8, triangle 0x10, circle 0x20, cross 0x40, square 0x80, select 0x100, l3 0x200, r3 0x400, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000.

## 7. Connection lifecycle from the client side

1. Connect, send HELLO, read SessionInfo.
2. SUBSCRIBE with a bound UDP port, then DESCRIBE, AUTOSPLIT_DESCRIBE, PLANET_LIST, WATCH_LIST, FREEZE_LIST, PATCH_LIST, MOD_LIST, COMBO_LIST, SAVEFILE_INFO.
3. Render from telemetry. When `game` changes, repeat step 2 from DESCRIBE. When `PREVIOUS_PENDING` appears, PREVIOUS_LIST and prompt.
4. On any socket error: drop everything, reconnect with backoff, start again at step 1. There is no client state to restore.

## 8. Autosplitting (revision 1.5)

**qwark detects, the client decides.** qwark watches game memory on the tick thread and emits run events. It keeps **no timer**, applies **no user setting**, and knows nothing about LiveSplit: every split candidate goes out unconditionally with a reason code, and the PC decides which ones to act on. Anything the old ASL scripts gated on a *setting* is still emitted here; only what they gated on *game state* is a condition qwark evaluates.

The one thing that is **not** left to taste is timing. The old scripts adjusted game time at fixed points — a load screen that stopped counting after 7.56 s, a transition worth nine frames, a quit that always cost 14.8 s — and a run that does not reproduce those adjustments to the microsecond is a different time. Those adjustments travel as the `FLAT` and `NORMALISE` flags on an `EventDesc` row, and **a client applies them whenever the autosplitter is on**, whatever its split checkboxes say (section 8.3).

Only Deadlocked emits PAUSE and RESUME. Every game but Deadlocked emits LOAD_START; only RaC1 and RaC3 close one with LOAD_END.

Events are emitted only while the session is INGAME.

### 8.1 Event, 16 bytes

```
u32 seq        1, 2, 3 ... per qwark load; never reset while the module runs
               (a game reboot does not reset it)
u32 time_ms    milliseconds since the module started, wrapping. Revision 1.5:
               this was the session tick count. A client measures a load or a
               pause by subtracting two of these and never needs the tick rate;
               u32 subtraction gives the right answer across the wrap
u8  kind       1 START, 2 SPLIT, 3 RESET, 4 PAUSE, 5 RESUME,
               6 LOAD_START, 7 LOAD_END
u8  code       per-game reason (see AUTOSPLIT_DESCRIBE); 0 for START and RESET
               only. A LOAD_END carries the same code as the LOAD_START it
               closes, and a RESUME the same code as its PAUSE
u16 reserved   0
u32 arg        code-specific; for the planet-enter code it is the planet index in
               PLANET_LIST order
```

In every game the old script's `start` and `reset` blocks are the same expression, so qwark emits **RESET immediately followed by START** on that condition and the client applies whichever suits its timer, exactly as LiveSplit does with the two blocks.

### 8.2 EventDesc, 32 bytes

```
u8  code      1..255, unique per game; code 1 is RESERVED for "planet entered"
              in every game (arg = planet index in PLANET_LIST order)
u8  kind      the kind this code is emitted with. For a load row it is 6
              LOAD_START, and its LOAD_END carries the same code; for a pause
              row it is 4 PAUSE, and its RESUME carries the same code
u8  flags     bit0 = a client should enable it by default
              bit1 = "planet route" applies (only code 1 sets it)
              bit2 = FLAT       (section 8.3)
              bit3 = NORMALISE  (section 8.3)
u8  reserved  0
u32 param_us  the timing parameter, in microseconds; 0 when neither timing flag
              is set. A row never sets both flags, and a row that sets one always
              names a non-zero parameter
char label[24]  NUL-padded, what the client shows as a checkbox
```

The list is static per game and never changes within a session.

### 8.3 The two timing rules

A row carries at most one of these, and the client applies it **whenever the autosplitter is on** — they are not user options, and they are not gated on whether the row's checkbox is ticked.

- **FLAT** — when the described event fires, subtract `param_us` from game time. On a SPLIT row the subtraction happens *before* the split is taken, which is the order the old scripts wrote it in.
- **NORMALISE** — time the interval from the described event to the one that closes it (the matching LOAD_END, or the RESUME after a PAUSE) and subtract `max(0, duration - param_us)`. The interval therefore always costs exactly `param_us`, however long it really took. The `time_ms` on the two events is what to measure with.

A FLAT **load** row is paid on its LOAD_START; its LOAD_END, if the game sends one, only closes the pair and costs nothing. RaC2's three load rows send no LOAD_END at all, because the script they come from never timed a load — it only paid a toll on entering one.

### 8.4 The UDP push

On the tick an event is emitted, and again on each of the next two ticks, qwark sends a 20-byte datagram to every telemetry subscriber — the same UDP socket and subscriber list telemetry uses. Three sends cover a lost datagram without anyone keeping a timer; a client that sees the same `seq` three times ignores the repeats.

```
char magic[2]  'Q','E'
u8   version   1
u8   reserved  0
Event          16 bytes, section 8.1
```

The magic and the length tell it apart from a telemetry packet, which always starts `QWRK` and is never 20 bytes. An event is **never** carried inside the telemetry packet. Datagrams go out every tick rather than every fourth, because a split has to reach the PC in single-digit milliseconds. AUTOSPLIT_EVENTS over TCP is the catch-up read for anything that was dropped.

### 8.5 Reason codes per game

Ported from racman's `rac1-autosplitter.asl`, `rac2-autosplitter.asl`, `rac3-autosplitter.asl` and `rac4-LC-autosplitter.asl`. "Default" is bit0 of `flags`, and for a split row it matches whether the script's setting defaulted to true. Every timing row is default-on, because the adjustment it carries was never a setting. The rows are listed in the order DESCRIBE returns them.

**RaC1** — START/RESET when the game state goes 6 to 0 while on Veldin (planet 0).

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet |
| 2 | SPLIT | Veldin | yes | — | 0. Fires once per run; a latch stops the double split |
| 3 | SPLIT | Drek button | yes | — | 0. The player state reaching 34 within 1.7 of one of the four buttons |
| 4 | SPLIT | Gold bolt collected | no | — | the helper mod's counter |
| 5 | SPLIT | Skill point | no | — | the helper mod's counter |
| 6 | SPLIT | Item collected | no | — | the helper mod's counter |
| 7 | SPLIT | Infobot | no | — | the helper mod's counter |
| 8 | LOAD_START | Loading screen | yes | NORMALISE 7 560 000 | the loading-screen id it moved to; 4 on the LOAD_END |

Codes 4 to 7 count changes in four words the `gb_sp_as_helper` mod keeps. qwark now embeds that mod and writes it — four code caves and four hook words — every time RaC1 reaches INGAME, so the four collectable codes work without anyone loading anything; it is never reverted, because without a run in progress it is four counters nobody reads. Code 4 also fires for the Kalebo3 gold bolt, code 6 also for the codebot and the raritanium, exactly as the script had them.

Code 8 is the script's `isLoading` block. It started a 7.56 s timer when the loading-screen id left 4 and only called the game "loading" once that timer had run out, so the first 7.56 s of every load counted towards game time and the rest did not. qwark emits LOAD_START when the id leaves 4 and LOAD_END when it comes back, and the client subtracts `max(0, duration − 7.56 s)`.

**RaC2** — START/RESET when the player state goes 0 to 98 while on Aranos (planet 0).

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the planet just entered; never the Insomniac Museum (21) |
| 2 | SPLIT | Protopet defeated | yes | FLAT 116 667 | 0 |
| 3 | SPLIT | Aranos 2 Clank swap | yes | — | 0 |
| 4 | SPLIT | Maktar arena entry | no | — | 0 |
| 5 | SPLIT | Barlow race entry | no | — | 0 |
| 6 | SPLIT | Endako Clank entry | no | — | 0 |
| 7 | SPLIT | Endako Clank exit | no | — | 0 |
| 8 | SPLIT | Tabora caves | no | — | 0 |
| 9 | LOAD_START | Load transition: slide | yes | FLAT 16 667 | 0 |
| 10 | LOAD_START | Load transition: curved | yes | FLAT 150 000 | 0 |
| 11 | LOAD_START | Load transition: wipe | yes | FLAT 350 000 | 0 |

Codes 9 to 11 are the script's `update` block: every time the load-screen byte changed it subtracted a fixed number of frames at 60 fps, keyed on the value it changed *to* — 1 frame for 0, 9 for 1, 21 for 3, and nothing for any other value. One code each, emitted on the change, with no LOAD_END: the script never timed a load, it only paid a toll on entering one. Code 2 carries the seven frames the script took off immediately before returning true for the Protopet split.

**RaC3** — START/RESET when the game state goes 6 to 0 while on Veldin (planet 1).

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet |
| 5 | SPLIT | Biobliterator defeated | yes | — | 0. Armed by an odd Neffyrious phase at full health |
| 2 | SPLIT | LDF entered | no | — | 0 |
| 4 | SPLIT | Koros bolt 2 | no | — | 0. The second titanium bolt of the run on Koros |
| 3 | SPLIT | Tyhrraguise obtained | no | — | 0 |
| 6 | LOAD_START | Long load | yes | FLAT 1 000 000 | the destination planet |

Code 6 is the script's long load: a second off game time when the loading-screen id becomes 1, **unless** the destination planet or the origin planet is one of 20, 26, 27, 28 or 29, whose loading screens are different ones that only look long. qwark tracks the origin the way the script did — the planet you left when a planet change is under way, the planet you are standing on when one is not — and emits the LOAD_START only when both ends pass. LOAD_END follows when the id leaves 1, and only for a load that actually opened.

The script's split route and its strict-order mode are the client's business: qwark emits code 1 for every planet change and the PC filters.

**Deadlocked** — START/RESET when a load starts for Dread Zone (planet 1) with the tutorial flag clear.

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet |
| 2 | SPLIT | Vox defeated | yes | — | the destination planet, which is 0 by then |
| 3 | PAUSE | Quit to XMB | yes | NORMALISE 14 800 000 | 0 |

Deadlocked is the only game whose detection already ran on the console; codes 1 and 2 replace the old command stream's single "split" whose packet byte the LC script had to read as "0 means Vox". The script's AEC setting, which disables resetting, is the client's business: qwark still emits every RESET.

**Where Deadlocked's origin planet comes from (build 9).** Code 1 needs the planet the player is coming from as well as the one they are going to, and the game's own current-planet word at `0x009C3240` is saved with the file: after a quit to the XMB and a boot it still holds whatever planet the save was on, so the first load out of the main menu read as a planet change and split. qwark keeps its own origin instead. It is planet 0, the main menu, whenever a session is primed, and it becomes the destination of every load that starts after that — so a load back to the main menu puts it back to 0 on its own, and neither that load nor the load out of the menu after it is a split. The word itself is still read, for the Vox split and for the readout.

Code 3 is the quit to the XMB, which some categories use as a strategy. PAUSE goes out when the game leaves the running state — the quit hook fires, or the process disappears underneath qwark. RESUME carries the same code 3, and the LC script's arithmetic (stop game time for the whole quit and reload, then add 14.8 s back on the way in) is NORMALISE 14 800 000: the quit costs exactly 14.8 s however slow the console was.

**Where Deadlocked's RESUME comes from.** qwark reaches INGAME as soon as the process is back and its fingerprint reads, which is several seconds before the game is playable — it sits behind a loading screen and a warning screen first — so resuming there would hand the runner free time the old autosplitter never gave away. qwark therefore installs the old SPRX's **loading hook** alongside its quit hook when Deadlocked enters INGAME: ten words of trampoline at `0x11904`, a `b +0x80` at `0x11884` that reaches it, and a byte at `0x1710000` the game sets to `0xFF` once it has the SCE logo up. Both hooks go in permanently and are never reverted, and both bytes are cleared on entry so a stale one cannot fire. RESUME is emitted on the first tick that byte reads `0xFF`, and the byte is cleared again for the next quit — exactly the gate the SPRX's `STATUS_INGAME_PENDING` used before it sent `CMD_UNPAUSE`.
