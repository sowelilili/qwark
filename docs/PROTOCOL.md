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
Revision 1.11 (2026-09-15): `QWARK_MAX_PAYLOAD` is **16384**. One frame, request or reply, carries at most that, so every op that used to name 65536 — MEM_READ, FILE_READ, FILE_WRITE, SAVEFILE_READ, SAVEFILE_WRITE — is capped there too. No opcode, structure, status or field moved: this is the size of a frame and nothing else. The console holds one 16 KB request buffer and one 16 KB reply buffer for every connection to share, instead of allocating 192 KB of the machine's own memory per request in flight, which is what a user running qwark beside other VSH plugins actually feels. A client that sends chunks of 16000 bytes is safe against this build and against every older one.

Revision 1.12 (2026-09-16): COMBO_ENABLE at 0x0083, the user's own on/off for every combo. It is kept in the console's config as `combo.enabled`, so it survives a client restart and a console reboot the way the five masks do, and SessionInfo `flags` gained bit3 COMBOS_OFF, set while it is off, so a client draws the checkbox out of the info block it already reads. See section 5.9.

Revision 1.13 (2026-09-22): POS_EDIT at 0x004A and POS_STORE at 0x004B, the two ways a client writes a position slot without the player standing in it. POS_EDIT rewrites the three coordinates of a slot that is already filled and leaves the rest of that slot's blob - the rotation, and whatever else the game keeps in it - untouched, so a coordinate typed into a box does not throw away a facing captured in the game. POS_STORE replaces a slot's whole blob and will only take one of the running game's own length, so a blob of the wrong shape can never be stored for POS_LOAD to misread. Both act on the current planet and both persist immediately, as POS_SAVE does. See section 5.5.

Revision 1.15 (2026-09-29): the savefile helper under RPCS3. SAVEFILE_PATCH at 0x00B8 hands out the running game's helper as the (address, value) words a patch writes to put it there and the request bytes it zeroes, with a CRC stamp, and on a platform that refuses code patches the savefile block now works once an RPCS3 patch has put the helper in the game at load: qwark looks for it once a session and, finding every byte of it, behaves exactly as a console does after an install. SAVEFILE_INFO answers on that platform either way, where it used to be UNSUPPORTED, and says in `installed` whether the patch is in. Revision 1.14 is reserved for opcodes 0x0056 and 0x0057 on a parked branch and is not in this build. See sections 3.2 and 5.12.1.

Revision 1.16 (2026-09-29): mods under RPCS3, the way the savefile helper got there in 1.15. MOD_PATCH at 0x0066 hands out one mod as the (address, value) words an RPCS3 patch writes - every cave as consecutive words, then the mod's own words - and the tail bytes of any cave that is not whole words, in SAVEFILE_PATCH's layout and with the same CRC stamp. On a platform that refuses code patches MOD_LIST's `flags` bit0 LOADED now means the mod is in game memory, every cave byte and every word of it, which is how a patch RPCS3 applied at load shows up, and the new bit5 CHECKING says qwark has not looked yet this session; a console never sets bit5 and its LOADED is unchanged. MOD_LOAD, MOD_UNLOAD, auto-apply and the previous-session reapply behave there exactly as before. Separately, `flags` bit3 PREVIOUS was documented and never set: it is now set on a mod whose bit is in the previous-session record while it is not loaded again. Module build 49. See sections 3.2, 5.7 and 5.7.1.

Revision 1.17 (2026-09-29): code switches, the nine WRITES_CODE toggles under RPCS3. SWITCH_PATCH at 0x0025 hands out the running game's code switches - a trampoline in a code cave for every instruction those toggles patch on a console, and the branch that replaces each such instruction - as the (address, value) words an RPCS3 patch writes, with every flag byte the trampolines read as a byte entry at 0, in SAVEFILE_PATCH's layout and with the same CRC stamp. On a platform that refuses code patches qwark looks for them once a session, and while they are in the game SessionInfo `flags` bit4 CODE_SWITCHES is set and every WRITES_CODE toggle of the game works, as a write of its flag byte: FEATURE_SET, the read-back into `toggle_state`, auto-apply, PREVIOUS_REAPPLY and the fast loads RaC2's and Deadlocked's loads force. RaC2's, RaC3's and Deadlocked's fingerprints accept the branch at their site. Separately, RaC2's savefile helper keeps its request bytes at 0x1317728..0x131772A since this build, out of the executable's code segment, which RPCS3 makes read-only (section 5.12.1). Module build 50. See sections 3, 3.2 and 5.12.2.

Revision 1.18 (2026-09-29): the PS3 module carries nothing that only RPCS3 needs. The three patch ops, SAVEFILE_PATCH, MOD_PATCH and SWITCH_PATCH, and the looks for a savefile helper, a mod or the code switches an RPCS3 patch put in the game exist only in the emulator builds, `qwark-rpcs3.exe` and the `qwark-host.exe` simulator; they are compiled out of `qwark.sprx`, which runs where memory is tight. The PS3 module answers SAVEFILE_PATCH, MOD_PATCH and SWITCH_PATCH with `UNSUPPORTED`, where until now it answered them for information, and never sets SessionInfo `flags` bit4 CODE_SWITCHES, as before. A client only asks for the three under its RPCS3 target, so nothing a client does has to change; no opcode, structure or other status moved. Module build 51. See sections 3.2, 5.7.1, 5.12.1 and 5.12.2.

Revision 1.19 (2026-10-06): MOD_PATCH answers a page at a time, so a mod bigger than one frame can still be written as an RPCS3 patch. Until now a reply was one 16384-byte frame, at most 2046 rows, and a bigger mod (a 9.3 KB cave and a few words, say) answered `FULL`. The request is now `u8 index, u8 pad, u16 page`, still four bytes; a client before this revision sends three zero pads, which is page 0. The reply keeps its layout, with the pad after `n` now `u16 pages`. A page carries at most 2046 rows, word rows and byte rows together: page k carries word rows 2046·k up to 2046·(k+1) or the last, and the last page carries every byte row after its words. `pages` and `stamp` are the same on every page, the stamp being the CRC-32 of one unbounded reply after its stamp, so a mod that fits one page answers exactly what it did before with `pages` 1 where the pad was 0. A `page` at or past `pages` is `BAD_ARG`, and `FULL` now means only that a mod's byte rows alone would not fit one page. Module build 52. See section 5.7.1.

Revision 1.20 (2026-10-15): SAVEFILE_INFO's `error` gains 7, NO_SAVE: there was no save to act on. RaC1, RaC2 and RaC3 have no save until the game has saved or loaded once in the process (on a new game, the first autosave), and a set-aside before then - and a load with nothing set aside, which fills the aside buffer from the game's save - is refused with `BUSY` and error 7 rather than copied from wherever the game's pointer points, address 0 in RaC2; so is a load whose aside buffer holds something that is not a save of the running game, and a RESTORE whose file is not one stops with error 7 after the copy, without raising the load. No opcode, structure or field moved, and a client that does not know the value still gets the `BUSY`. Module build 53, which also makes RaC1's and RaC2's loads wait for the game's pause, moves RaC1's request bytes and aside buffer and RaC2's helper, and so changes both games' SAVEFILE_PATCH words. It moves RaC2's code switches too, out of the same routine planet Joba runs every frame, so RaC2's SWITCH_PATCH words change as well; RaC2's fingerprint goes on accepting the branch an earlier build's patch left at its site, so such a game is still recognised and reads as having switches from another build. See sections 5.12 and 5.12.2.

Revision 1.21 (2026-10-16): manip categories. MANIP_LIST, MANIP_SELECT, MANIP_OPTIONS and MANIP_APPLY at 0x00D0..0x00D3. A game describes groups of the RAM writes speedrunners call manips, each for one or more leaderboard categories; the client picks one, and qwark applies its setup once per game boot and its per-run writes on every file load (a save loaded or a new game started) while auto-reset is on, after checking the file's challenge-mode counter both ways. The options live in qwark's config.txt, so they work without a PC. The old manip buttons and toggles are retired, ids and all: RaC1 7, 8, 10, 11 and 16; RaC2 15 to 22; UYA 16, 18, 19 and 20; Deadlocked 12. MANIP_LIST also carries a list of headers, features that belong beside the manips: Deadlocked's "Fixes" (Crash patches and Fix reset softlocks, ids, behaviour and auto flags unchanged) and UYA's "Trophies" (Reset trophies, 21, and a new toggle, 39 Reset trophies on Veldin load, off by default), each also a DESCRIBE group of the same name. UYA's reset deletes the console's trophy data for the game and then has the game re-register its trophies on a thread of the savefile helper's, while the game goes on running; it is refused under RPCS3. The same build fixes Deadlocked's Fast loads, which hung the game on a black screen whenever the flight had a movie to play, and reworks them on RaC2's model: the screen goes black at the start of the load and stays black to the level's first frame, no flight is flown or shown, the level is read with no wait between its steps, and a queued movie plays first, at once, instead of after a segment of flight. Nothing on the wire changes, but the code switches behind that checkbox under RPCS3 changed, so a patch written by an earlier build is asked for again (section 5.12.2). RaC1's PLANET_LOAD and LOAD_ASIDE, and RaC2's LOAD_ASIDE as before, force fast loads on for the length of the load and put the checkbox's setting back when it is over (sections 3.2 and 5.12). RaC2 also gets two crash guards that are no feature: a planet change qwark makes to or from Dobbo Orbit, Damosel Orbit or the Jamming Array, with an in-space story cutscene queued, crashed the game. qwark writes the guards into the game itself as the session enters RaC2 on a console, and under RPCS3 RaC2's SWITCH_PATCH carries them as two more trampolines with no flag byte, so a patch from an earlier build is asked for again there too (section 5.12.2). UYA's Fast loads (38) become a code patch and gain `WRITES_CODE`, as RaC1's and RaC2's are: the old data words hung the game on a black screen on a trip that owes one of Aquatos's story scenes, and the patch lets the scene play and, as RaC2's does, keeps the screen black from the start of a load to the new level's first frame with no ship clip shown, so UYA's PLANET_LOAD forces fast loads for every planet now, Aquatos included, and its LOAD_ASIDE forces them as RaC1's and RaC2's do; under RPCS3 they are a second code switch, so UYA's SWITCH_PATCH words change and a Freeze-ammo-only patch reads as one from another build (sections 5.12.2 and 5.12.3). Same module build, 53, which is unreleased. See section 5.14.

Revision 1.21, the toggle tidy (2026-10-16, same build 53): RaC1's Max ammo, all weapons (17), RaC2's Weapon insta-upgrades (4) and UYA's One-hit KO (2) are retired, ids and all (section 3.1). Freeze health in all four games (RaC1 2, RaC2 2, UYA 1, Deadlocked 4) holds the hit points at the game's own maximum as it is now, health upgrades and all, rather than at a fixed number: it keeps a freeze entry on them, which FREEZE_LIST shows like any other, moves its value as the game's maximum moves, and while the game has no maximum yet holds nothing. RaC1's is id 2, which was Infinite health, the code patch at the fingerprint word 0x7F558: it keeps its id, so its combos, auto flag and previous-session record carry over, and loses `WRITES_CODE`; the Freeze health row RaC1 had for a while beside it, id 28, is retired. UYA's Freeze ammo (0) is labelled Infinite ammo, its patch unchanged. The four cheats every game has lead the Cheats group in all four, in one order, Fast loads, Ghost Ratchet, the ammo cheat (Infinite ammo, but Deadlocked's Refill ammo, which fills the ammo at a mission's start on purpose) and Freeze health, each game's other cheats after them; only the rows moved. A toggle's config.txt auto flag set under its old label still counts (section 3.1). And the first application of a manip group in a boot is announced whatever it wrote (section 5.14): a group with per-run writes alone, all of RaC1's and RaC2's Any% and 100%, never was. No opcode or structure changed.

Revision 1.21, loads during a cutscene (2026-10-17, same build 53): RaC2's savefile helper takes the game into its pause by writing the mode word, as RaC1's does, and loads in the same call, rather than asking the game's mode change for the pause and waiting a frame for it, which still failed during a cutscene. Before it writes the mode it closes a movie that is playing and empties the cutscene's queue, as the Start button's skip does, and drops the callback the cutscene's end would have run. A RaC2 load is now also refused while a trip in the ship is queued, and while the console holds the game in its own freeze, and no longer in debug mode. RaC1's and RaC3's loads still wait for a frame of the pause, and their wait now runs out after ten seconds of the console's clock rather than after 600 calls of the helper. No opcode or structure changed; RaC1's, RaC2's and RaC3's SAVEFILE_PATCH words and stamps did, so a patch written before this change is asked for again. See section 5.12.

Revision 1.21, the review's follow-ups (2026-10-17, same build 53): a load of anything qwark itself put in the aside buffer - a RESTORE's file, or SAVEFILE_WRITE - is refused with error 7 when its first word is 0, in all four games, rather than handed to the helper, which takes that word for an empty buffer and loads the game's own last save; a RESTORE of such a file used to report success having loaded the game's save. SAVEFILE_INFO's `error` describes the last request: a set-aside or a load clears it as it starts, as a STORE or a RESTORE always has. A set-aside or a load asked for while a STORE or a RESTORE runs is `BUSY` with error 5, where it used to go out over the buffer the transfer was copying. A load's fast-load force belongs to that load: a request that fails takes back its own force and no earlier load's (RaC1's, RaC2's and Deadlocked's PLANET_LOAD, and RaC1's and RaC2's set-aside loads); a set-aside load the helper drops puts back only its own force, and a RESTORE's dropped load none; RaC1 watches for the end of the load its force was for, so a set-aside load asked for in a planet's fly-in is no longer taken for that planet load's end; and under RPCS3 the end of a load nothing forced writes nothing, and a fast-load flag a force finds up is the user's, as RaC3's already were (sections 5.12 and 5.12.2). Deadlocked's file-load trigger acts only if the tutorial byte still reads 0 on the tick after its drop (section 5.14). Freeze health (the toggle tidy, above) leaves a freeze the user set on the same hit points alone, value and all, also when it goes off, and puts its own entry back once the user takes theirs out. UYA's Reset trophies (21) and its toggle (39) are left out of DESCRIBE where they can never work, under RPCS3 and with `savefile_helper = 0` (section 5.14). Under RPCS3 without the code switches' patch, which carries RaC2's crash guards there, RaC2's PLANET_LOAD refuses a trip the guards are for with `UNSUPPORTED` (sections 5.5 and 5.12.2). No opcode, structure or status changed, and no patch word.

Revision 1.21, planets (2026-10-17, same build 53): PLANET_LIST carries one more byte per planet after the names, the order a menu lists the planets in, which is the order the game visits them rather than the order it numbers them; the index a name sits at is still the planet's id, and every request still carries the id. UYA's and Deadlocked's orders are the old client's combo boxes, the others' are their ids in turn, and a client before this build reads the names and stops, so it sees no change. UYA's PLANET_LOAD now acts on bit1 of the planet flags, reset special bolts, which it ignored: it takes back the titanium bolts collected on the destination planet, as RaC1's takes back its gold bolts, so a client offers the box for UYA against a module of this build or later and not against one before it. UYA's Unlock all titanium bolts (24) and Reset all titanium bolts (25) cover the game's whole bolt table, 296 bytes from 0xECE54D, where they wrote 128 bytes from sixteen bytes before it and missed every planet from Koros on. No opcode, id or status changed. See section 5.5.

Revision 1.21, the planet selection per game (2026-10-17, same build 53): the planet PLANET_SELECT picks and the planet flags beside it, the two reset boxes, are one game's rather than one setting for the console, as the manip options are. config.txt keeps them as `selected.<game>.planet` and `selected.<game>.planet_flags`, `<game>` being `rac1` to `rac4` as in the manips' keys, and a game with no key of its own yet reads the console-wide `selected.planet` and `selected.planet_flags` an earlier build wrote, which are never written again, so a selection made before this build is not lost. SessionInfo's `selected_planet` and `planet_flags` are the running game's, PLANET_SELECT sets the running game's, and a PLANET_LOAD of 0xFF, the load-planet combo's among them, loads the running game's: a box ticked in RaC1 is no longer ticked in UYA. Outside a game - at the XMB, through the next boot, and in a title no game table claims - both fields and PLANET_SELECT are the last game's that was INGAME, which is the game a client goes on drawing there; the first block that says a game is INGAME carries that game's selection. Before any game has run since the module loaded both fields are 0 and PLANET_SELECT answers `UNSUPPORTED`. No opcode, structure or field moved; only what the two fields and the two ops mean did. See sections 3, 5.5 and 5.10.

Revision 1.21, the trophy status (2026-10-18, same build 53): an ACTION's `readout`, always 0xFF until now, may name a **status readout**, a word that says what the action is doing and how it last ended (section 5.3). UYA's Reset trophies (21) is the only one, and names readout 12, "Trophy reset", new and last in UYA's list: whether a reset is running and at which step (starting, deleting, registering, and which registration), whether the trophies are ready and how many seconds the reset took, or why it failed, with a count of the resets that have ended so a client can tell a new result from an old one (section 5.14). A boot puts it back to "no reset this boot". Where the reset can never work, under RPCS3 and with `savefile_helper = 0`, DESCRIBE leaves the readout's name out with the two rows, so UYA describes twelve readouts there and thirteen elsewhere. A reset that ends well no longer shows "Trophies reset: ready to win (N s)" on the console's screen, which would distract a runner mid-attempt; the failures still show theirs. A client before this revision takes a readout only from a VALUE, an ENUM or a COLOR, so it reads Reset trophies' row as it always did and sees one more readout name. No opcode or structure changed.

Revision 1.22 (2026-10-18, module build 54): the console menu, a menu on the TV for a console with no PC beside it (section 9). Combo action 5, OPEN_MENU, opens it, and closes it when it is up. It is the first combo with a mask of its own, L1 + R1 + L2 + R2 + L3 + R3 (0x060F), the one combo COMBO_ENABLE does not hold off, and COMBO_LIST's sixth entry (section 5.9). Two config.txt keys come with it, `combo.menu` and `menu.enabled` (section 5.10). The menu is drawn by a renderer qwark writes into the game a few seconds into a session, which also makes the game's own Help row in its pause menu the menu's way in, and the two talk through a mailbox in game memory; neither is on the wire, and section 9 describes both for information. Deadlocked and, since the menu's third round in the same build, Up Your Arsenal have renderers, and only on a console. No opcode, structure or status changed. A client that predates this revision reads the sixth COMBO_LIST entry as an action it does not know.

Revision 1.22, the console menu's round 3 (2026-10-19, same build 54): MANIP_SELECT writes nothing any more, whatever is in play: a group's setup goes in at the next start of a run that passes its check, with its per-run writes, or by MANIP_APPLY (section 5.14). Build 53 put the setup in at once when a compatible file was in play, so stepping through the categories on the console menu applied, or refused, each group on the way. On the console menu, which is not on the wire (section 9): the root and the pause-menu row that opens it say Trainer; the menu opens on its root, the cursor on its first row, whenever it was closed; a choice shows its bare value; a game may put the Unlocks page's actions on a page of their own (UYA's Collectables); a VALUE row with bounds carries its numbers for a native slider; and the renderer's core keeps two text sets so a screen going keeps its own text to its last frame. No opcode or structure changed.

This file is the contract between qwark (the PS3 SPRX) and every client. Both sides are written against it; when it changes, `QWARK_PROTOCOL_VERSION` changes with it.

All integers are big-endian. Floats are IEEE 754 single precision, big-endian. Fixed-width string fields are NUL-padded and need not be NUL-terminated when full. Variable strings are given with an explicit length unless stated.

Transport: TCP on port 9673 for commands, UDP telemetry sent to the port the client names in SUBSCRIBE. Up to 4 concurrent TCP clients (8 before revision 1.11); a connection arriving when they are all taken is closed as it arrives, which is what always happened one past the limit.

## 1. Frames

```
Request:  u32 length | u16 seq | u16 opcode | payload[length]
Reply:    u32 length | u16 seq | u16 status | payload[length]
```

- `length` is the payload size, at most `QWARK_MAX_PAYLOAD` = 16384 (65600 before revision 1.11). A larger length closes the connection.
- Every request gets exactly one reply carrying the same `seq`. Replies come back in request order, so clients may pipeline.
- An unknown opcode is answered with `UNKNOWN_OP` and an empty payload; the payload is drained and the connection stays open.
- A payload shorter than the opcode requires is answered with `BAD_ARG`.
- A reply listed as **none** below is a frame with status and an empty payload; every request still gets its frame.

### 1.1 While a game launches

The moment the console reports a game process, qwark leaves it alone for one second, measured by the clock rather than counted in ticks. Nothing reads the process, the XMB is not asked for its title, and nothing moves on the network: existing connections neither receive nor reply, and the telemetry and autosplit datagrams stop. Frames sent into that second are answered, in order, once it ends; nothing is buffered for them beyond what the socket holds. The OS still acknowledges TCP by itself, and other plugins are outside this. Ratchetron and the old autosplitter modules kept the same silence, and the consoles that crashed at launch without it are why. None of this depends on which game is starting, because none of it knows yet: the title is not asked for until the second is over, so a process qwark turns out not to know (section 3.3) is launched through exactly the same silence and reaches INGAME the same way.

For the rest of BOOTING, and through QUITTING, the shared request buffers are not handed out. HELLO, HEARTBEAT, SUBSCRIBE, UNSUBSCRIBE and GET_STATE, with at most two payload bytes, use per-connection fixed buffers and keep working. Any other request is drained without them and answered **BUSY**: retry once the session is INGAME. New connections are closed as they arrive. A connection that is holding the request buffers when a transition starts is closed, and the request in flight may get no reply; reconnect as usual.

Since revision 1.11 there is exactly one request buffer and one reply buffer on the console, 16384 bytes each, and one connection holds the pair for the length of one request: the payload is read into it, the command is run, the reply is sent, and it is handed on. A second client's bulk request therefore waits for the first to finish rather than being refused, and the five control ops above never wait at all, because they never touch it. Nothing about this is visible on the wire — the ordering guarantee is unchanged, every request still gets exactly one reply carrying its own `seq`, and a client that pipelines sees its replies in request order — but a client that wants two bulk transfers to overlap should know that the console runs them one after the other.

Connected sockets are non-blocking. A payload receive and a reply send each have a two-second deadline, not counting time paused for a launch, and a connection that misses one is closed and the buffers released. Waiting for the next request header has no deadline and holds no buffers.

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
| 8 | BUSY | Command ring is full, a save transfer is busy, boot is holding the request buffers back, or (revision 1.15) qwark has not yet been able to look for a patched-in savefile helper (section 5.12.1); retry. Revision 1.21: also MANIP_APPLY's refusal, whose reason is in MANIP_LIST's event (section 5.14) |

## 3. Session info block

Shared by HELLO, GET_STATE and telemetry. 164 bytes.

```
u8   protocol_version   = 1
u8   qwark_version      module build number (see below); a client compares it with the build it shipped beside
u8   state              0 XMB, 1 BOOTING, 2 INGAME, 3 QUITTING
u8   game               0 NONE, 1 RAC1, 2 RAC2, 3 RAC3, 4 RAC4 (Deadlocked)
                        BCES01503, the disc trilogy, reports 1, 2 or 3 depending
                        on which executable is mapped; the client never picks.
                        0 while INGAME is a process qwark does not know (section 3.3)
u32  generation         incremented every time a game enters BOOTING
u32  tick               tick-thread counter, 120 Hz
char title_id[12]       e.g. "NPEA00385"
u8   flags              bit0 PREVIOUS_PENDING: a previous-session record is waiting (section 4.1)
                        bit1 EMULATOR: qwark is driving an emulator, not a console (section 3.2)
                        bit2 NO_CODE_PATCHES: this platform refuses code patches (section 3.2)
                        bit3 COMBOS_OFF: the COMBO_ENABLE switch is off, so no combo fires (section 5.9)
                        bit4 CODE_SWITCHES (revision 1.17): the running game's code switches are in it,
                             so its WRITES_CODE toggles work on a platform that sets bit2 (section 5.12.2).
                             Never set by the PS3 module
u8   selected_slot      0..7, used by the save/load combos
u8   selected_planet    used by the load-planet combo. Since build 53 the running game's, and
                        outside a game the last game's (section 5.5)
u8   planet_flags       bit0 reset level flags, bit1 reset special bolts, used by the load-planet combo.
                        Which bits a game's load acts on is in section 5.5. The same game's as
                        selected_planet
u8   current_planet     game planet index, 0 when unknown
u8   pad[3]
f32  pos[3]             player x, y, z
u32  pad_mask           controller buttons, OG layout (section 10)
f32  analog[4]          rx, ry, lx, ly, each -1..1
u32  readout[16]        game-defined live values; DESCRIBE names them. readout[0] is the bolt count when the game has one. VALUE, ENUM and COLOR features name the readout that mirrors their current value
u64  toggle_state       bit i set = TOGGLE feature id i is on. A LIVE toggle's bit is whatever game memory says, re-read at 10 Hz (section 5.3.1)
u64  toggle_auto        bit i set = feature id i re-applies itself on game boot. Never set for a LIVE toggle
u64  freeze_active      bit i set = freeze id i is active
u32  mod_loaded         bit i set = console mod index i is loaded by qwark. A mod an RPCS3 patch put in
                        the game is not in here: MOD_LIST's LOADED bit says that (revision 1.16, section 5.7.1)
u32  mod_auto           bit i set = console mod index i auto-applies on game boot
u32  mod_previous       bit i set = mod i was loaded before the last same-game reboot and is neither re-applied nor dismissed
```

### 3.1 `qwark_version`, the build number

`qwark_version` is the build number of the module itself (`QWARK_BUILD` in `src/core/proto.h`), and is independent of `protocol_version`. The wire contract can hold still while what sits behind it moves, so the build number increments whenever the feature tables change — a feature added, retired or relabelled — or whenever any other user-visible behaviour changes. It never decrements and is never reused.

Retiring a feature leaves its id behind for good: ids are the wire contract for the toggle bitmaps, nothing is renumbered, and a client that still knows a retired id simply never sees it in DESCRIBE again.

Build 53 retired these besides the manip rows (section 5.14), and as with those, config.txt auto flags for them are ignored and the previous-session record never offers them:

| Game | Retired id |
|---|---|
| RaC1 | 17 Max ammo, all weapons (Infinite ammo covers it); 28 Freeze health, the row it had beside Infinite health for a while (id 2 is Freeze health now) |
| RaC2 | 4 Weapon insta-upgrades (the Unlocks panel sets weapon levels) |
| UYA | 2 One-hit KO |

A relabelled feature keeps its id. config.txt keeps a toggle's auto flag under its label (`<TITLE>.auto.<slug>`), so a game names the labels its toggles had, and a toggle reads its flag from the old label's key until one is written under the new label's, which wins from then on. Build 53 relabelled RaC1's Infinite health (2) as Freeze health and UYA's Freeze ammo (0) as Infinite ammo.

A client ships knowing the build it was developed against. When the console reports a *lower* `qwark_version` than that, the client warns the user that the SPRX on the console is out of date and should be re-uploaded; DESCRIBE still answers, so the client keeps working against whatever tables the older module actually has. A *higher* number is not an error: the console is newer than the client, and DESCRIBE remains the authority on what exists.

### 3.2 `flags` bit1 EMULATOR and bit2 NO_CODE_PATCHES (revision 1.6)

qwark also runs on a PC as `qwark-rpcs3.exe`, driving RPCS3 through its PINE IPC server instead of a console through PS3MAPI. The core, the game tables and every opcode in this document are the same; two things about the platform are not, and both travel in `flags`.

**bit1 EMULATOR** is set when the thing on the other side is an emulator. It is cosmetic: say "RPCS3" rather than "console" and do not offer to upload an SPRX.

**bit2 NO_CODE_PATCHES** is not cosmetic. RPCS3 recompiles PPU code ahead of execution, so writing an instruction word changes memory and the game carries on running the old instruction. It goes further than that once the game is loaded: RPCS3 applies its own patches to the executable and then takes write permission away from every segment of it that has none of its own, the code segment and whatever read-only data shares it. From then on a write into that segment fails, a MEM_WRITE through PINE and a store the game makes itself alike; data memory stays writable. Rather than let a checkbox lie, qwark refuses everything that depends on a code patch:

| Request | Answer when bit2 is set |
|---|---|
| FEATURE_SET or FEATURE_TRIGGER on a feature flagged `WRITES_CODE` | `UNSUPPORTED`, except FEATURE_SET on a toggle while bit4 CODE_SWITCHES is set (revision 1.17, section 5.12.2) |
| PATCH_APPLY | `UNSUPPORTED` |
| MOD_LOAD of a mod with patch words or code caves | `UNSUPPORTED`, and MOD_UNLOAD has nothing to unload. The mod goes in as an RPCS3 patch instead (revision 1.16, section 5.7.1) |
| SAVEFILE_READ, SAVEFILE_WRITE, and the SAVE_ASIDE and LOAD_ASIDE actions | `UNSUPPORTED` unless an RPCS3 patch put the savefile helper in the game when it loaded (revision 1.15, section 5.12.1) |
| Every op of the savefile library: SAVEFILE_CATEGORIES, SAVEFILE_LIST, SAVEFILE_STORE, SAVEFILE_RESTORE, SAVEFILE_CATEGORY | `UNSUPPORTED` under the same condition (section 5.13). The library is only reachable through the helper, so without one there is nothing to copy out of and nothing to hand a copy to |
| FEATURE_TRIGGER of UYA's Reset trophies (21), and FEATURE_SET of its toggle (39) to on | `UNSUPPORTED` always, patch or not (revision 1.21, section 5.14). The reset deletes the game's trophy data on the console first, and RPCS3 keeps its own where qwark cannot reach it; the game's half alone would change nothing a run can see |
| The OPEN_MENU combo (revision 1.22) | nothing but a notification: the console menu's renderer is code, so `qwark-rpcs3` installs none and there is no menu (section 9) |

SAVEFILE_INFO was in this table until revision 1.15. It answers on this platform now, and its `installed` byte says whether the helper is in the game, which is how a client knows whether to offer to write the patch; SAVEFILE_PATCH answers on this platform (section 5.12.1).

**The patch ops are the emulator builds' alone** (revision 1.18). SAVEFILE_PATCH, MOD_PATCH and SWITCH_PATCH, and the looks that find what their patches put in a game, are built into `qwark-rpcs3.exe` and the `qwark-host.exe` simulator and not into `qwark.sprx`: the PS3 module runs beside other VSH plugins in very little memory, and a console patches code itself. The PS3 module answers the three with `UNSUPPORTED`, where until revision 1.18 it answered them for information. A client asks for them only while bit2 is set, so it never sees that answer.

**Mods work here too, once the client has written them as RPCS3 patches and the game has been restarted** (revision 1.16). A mod is code - patch words, code caves or both - so qwark cannot put one into a running game on this platform, but RPCS3 applies its own patch files as it loads the executable, before it compiles anything. The client asks for each mod with MOD_PATCH, a page at a time since revision 1.19, writes it into RPCS3's per-title patch file, enables it in `patch_config.yml`, and the user restarts the game; qwark then finds the mod in game memory and MOD_LIST reports it LOADED. What does not exist here is everything that changes a mod's state inside a session: MOD_LOAD is refused and MOD_UNLOAD has nothing to unload, as the table says, auto-flagged mods are skipped at boot as they always were, and there is no previous-session reapply for them, because qwark never loaded them and the record never holds them. Taking a mod out is the reverse of putting it in: the client disables the patch and the game is restarted. Section 5.7.1 has the rules.

Everything else works unchanged: memory reads and writes, freezes, watches, positions, planet loads, unlocks, level flags, colours, values, and every toggle whose truth is a data byte rather than an instruction. One difference inside a planet load: on a console, RaC1's, RaC2's, RaC3's and Deadlocked's PLANET_LOAD, and RaC1's, RaC2's and RaC3's LOAD_ASIDE, force fast loads on first, which is a code patch, so here they leave that step out and the load takes the game's own time rather than being refused. With the code switches in (revision 1.17) they force them after all, through the fast-load flag byte. RaC3's fast loads were two data words until they became a code patch (section 5.12.3), and went through here with nothing installed; they need the code switches now, like the others. The resets a planet load's flags ask for are data writes and work here exactly as on a console, code switches or not: the level flags, and the special bolts, RaC1's gold bolts and, since build 53, UYA's titanium bolts (section 5.5).

**The WRITES_CODE toggles work here too, once the client has written the game's code switches as an RPCS3 patch and the game has been restarted** (revision 1.17). The patch gives the game both forms of every instruction those toggles change, and a flag byte in data memory that picks one, so a toggle is a byte write; qwark finds the switches in the game once a session and sets bit4 CODE_SWITCHES while they are there. Section 5.12.2 has the rules. Without them, everything in this section stands.

DESCRIBE is **unchanged**: the WRITES_CODE rows are still listed, with the same ids, labels, groups and flags. A client greys them itself from bit2 rather than discovering one refusal at a time, so the same DESCRIBE parsing works against both platforms and a client built before this revision still functions, it simply sees the refusals instead of predicting them. Since revision 1.17 a client offers them again while bit4 CODE_SWITCHES is set; DESCRIBE is the same then too.

Two side effects a client should know about, both inside qwark rather than on the wire:

- Auto-flagged WRITES_CODE toggles (Deadlocked's crash patches, for instance) are skipped when a game reaches INGAME. `toggle_state` reports them as off, which is the truth. Since revision 1.17 they wait for qwark's look for the code switches instead, and go on through their flag bytes when it finds them.
- The games' own embedded helpers are code patches too, so they are not installed. RaC1's autosplit helper is one, and without it the four collectable reason codes (gold bolt, skill point, item, infobot) never fire; every other RaC1 split is a plain memory read and is unaffected. Deadlocked's quit and loading hooks are the others: the PAUSE still fires when the game goes away, because the session also watches the process itself, and the RESUME then fires on the way back INGAME rather than on the SCE logo. The savefile helper is a third, and the one with a way back in (revision 1.15): RPCS3 applies its own patch files to the executable as it loads it, before it recompiles anything, so a patch that writes the helper's caves and hook words at load, and zeroes its request bytes, is code the game really runs. The client writes that patch from SAVEFILE_PATCH into RPCS3's per-title patch file, `patches/<TITLE_ID>_patch.yml`, enables it in `patch_config.yml`, and the game is restarted; from then on qwark finds its helper in the game and the whole of sections 5.12 and 5.13 works as it does on a console. A patch written by an older qwark build no longer matches this one's helper and reads as not present, so the client writes the new one. Section 5.12.1 has the rules.

### 3.3 INGAME with `game` 0: a process qwark does not know

A title id no game table claims still reaches INGAME. `state` is 2, `game` is 0 NONE, and `title_id` is the id the console reported. It is a real session against a real process: the memory tools work on it exactly as they do on a game.

- **MEM_READ, MEM_WRITE, WATCH_\*, FREEZE_\* and PATCH_\*** work, under the same rules as always. A watch reads live and its value travels in every telemetry packet; PATCH_APPLY is still refused where `flags` bit2 says the platform cannot patch code.
- **Everything that needs a game table answers `UNSUPPORTED`**: DESCRIBE, FEATURE_SET, FEATURE_TRIGGER, FEATURE_SET_AUTO, FEATURE_OPTIONS, PLANET_LIST, PLANET_LOAD, POS_SAVE, POS_LOAD, UNLOCK_LIST, UNLOCK_SET, LEVELFLAGS_GET, LEVELFLAGS_RESET, LEVELFLAGS_SET, DIE, MOBY_TABLE, AUTOSPLIT_DESCRIBE and every op of sections 5.12 and 5.13, SAVEFILE_PATCH included (there is no helper to describe). The ops that were never about the game answer as they always do: the listings, the config and combo ops, the file ops and AUTOSPLIT_EVENTS. PLANET_SELECT, and the planet selection in this block, are the last game's that was INGAME, as at the XMB (build 53, section 5.5).
- **No readout, position or pad data is filled in.** `current_planet`, `pos`, `pad_mask`, `analog` and every `readout` are 0 and stay 0, because reading them means knowing where they are. `toggle_state`, `toggle_auto` and `freeze_active` behave as usual; nothing fires the stored combos, since the pad is not read.

Everything else about the session is unchanged: the launch is quiet for its second (section 1.1), `generation` advances, QUITTING and the return to the XMB happen when the process goes away, and a same-title reboot keeps its watches and builds the previous-session record (section 4.1) out of what is actually there, which is freezes and client patches.

A client should offer the memory panel and hide the game panels when `game` is 0 while `state` is 2. DESCRIBE answering `UNSUPPORTED` is the same answer it gives at the XMB, so no client is surprised by it.

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
| 0x0010 | SUBSCRIBE | `u16 udp_port` | none. Packets go to the connection's remote address and this port. The subscription ends when the connection closes. A subscriber that has sent no frame for 5 s is not sent to, and its next frame of any kind starts the packets again |
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
| 0x0025 | SWITCH_PATCH | none | `u16 n, u16 pad, u32 stamp, { u32 addr, u32 value }[n], u16 nb, u16 pad, { u32 addr, u8 value, u8 pad[3] }[nb]`: the running game's code switches as the words and bytes an RPCS3 patch writes (revision 1.17, section 5.12.2). `UNSUPPORTED` from the PS3 module (revision 1.18) |

```
Feature (48 bytes):
u8   id
u8   kind
u8   group          index into DESCRIBE groups
u8   aux            ENUM: option count. 0 for every other kind
u8   flags          bit0 AUTO (same as toggle_auto), bit1 WRITES_CODE (instruction patch), bit2 SAVE_ASIDE (this ACTION asks the game to copy its current save into the savefile helper's aside buffer), bit3 LOAD_ASIDE (this ACTION asks the game to load what is in that buffer), bit4 LIVE (TOGGLE only, section 5.3.1), bit5 SIGNED (VALUE only, section 5.3.2)
u8   readout        VALUE, ENUM, COLOR: index into SessionInfo.readout[] that mirrors the current value, 0xFF if none. ACTION: 0xFF, or (revision 1.21) the index of its status readout, below. TOGGLE: 0xFF
u8   bits           VALUE: the width in bits of the field behind it, 8, 16 or 32. 0 means 32, so a row that names no width reads as it always did. 0 for every other kind (revision 1.7)
u8   pad
u32  min
u32  max            VALUE and ENUM range, inclusive; both 0 when unbounded. A SIGNED VALUE leaves both 0: `bits` already says what the range is
char label[32]
```

Toggle state travels in `toggle_state`; every other kind that has state travels through its readout, so a second client or a restarted one sees the real value.

**An ACTION's status readout** (revision 1.21). An ACTION whose effect outlasts its answer may name a readout that says what it is doing and how it last ended, for a client to show beside its button. The word's layout is the action's own and is given where the action is described; UYA's Reset trophies (21, section 5.14) is the only ACTION that names one. A client that does not know an action's layout ignores the readout, as every client before this revision does: they take a readout only from a VALUE, an ENUM or a COLOR.

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
| 0x0030 | MEM_READ | `u32 addr, u32 len` (len at most 16384 since revision 1.11, 65536 before it) | `len` bytes |
| 0x0031 | MEM_WRITE | `u32 addr, bytes` (at most 16380 bytes of data: the frame is 16384 and the address is four of them) | none |
| 0x0032 | WATCH_ADD | `u32 addr, u8 size` (1, 2, 4 or 8) | `u8 id`. Adding the same address and size again returns the existing id |
| 0x0033 | WATCH_REMOVE | `u8 id` | none |
| 0x0034 | WATCH_LIST | none | `u8 n, { u8 id, u8 size, u8 pad[2], u32 addr }[n]` |
| 0x0035 | FREEZE_ADD | `u32 addr, u8 size, u8 pad[3], u64 value` | `u8 id` |
| 0x0036 | FREEZE_REMOVE | `u8 id` | none |
| 0x0037 | FREEZE_LIST | none | `u8 n, { u8 id, u8 size, u8 pad[2], u32 addr, u64 value }[n]` |
| 0x0038 | PATCH_APPLY | `u16 nwords, u16 pad, { u32 addr, u32 word }[nwords]` (at most 64 words) | none. A client patch keyed by its first address; applying it twice is a no-op. Since revision 1.11 the sixteen client-patch slots share a pool of 256 words, so a patch that would run the pool over is answered `FULL`, the same status a client already gets when every slot is taken |
| 0x0039 | PATCH_REVERT | `u32 first_addr` | none |
| 0x003A | PATCH_LIST | none | `u8 n, { u32 first_addr, u16 nwords, u8 kind, u8 pad, char[32] name }[n]`. kind: 0 client, 1 feature, 2 mod |
| 0x003B | CLEAR_CLIENT | none | none. Removes every watch, freeze and client patch. Toggles and mods are untouched |

### 5.5 Positions and planets (0x004x)

Eight slots per planet per game. Slot contents are opaque game-defined blobs of at most 64 bytes.

POS_SELECT and PLANET_SELECT write straight through to `config.txt`, so the selection is remembered across a client restart and across a console reboot; no CONFIG_SAVE is needed. It comes back in `SessionInfo.selected_slot`, and in `selected_planet` and `planet_flags`.

**Whose planet selection it is (build 53).** The slot is one for the console. The planet and its flags are one game's, the way the manip options are (section 5.14): a planet id means something only inside its own game, and so do the resets beside it. PLANET_SELECT sets the running game's, SessionInfo reports the running game's, and a PLANET_LOAD of 0xFF, which is what the load-planet combo sends, takes the running game's, so a box ticked in one game is not ticked in the next. Outside a game - at the XMB, while the next game boots, and in a title no game table claims (section 3.3) - the selection is the last game's that was INGAME: that is the game a client goes on showing, greyed, until another arrives, so its boxes neither empty at the XMB nor fill again when it comes back. The first block that says a new game is INGAME is the first that carries its selection; a BOOTING block whose `game` already names a candidate still carries the last game's. Before any game has run since the module loaded there is no game to keep one for: both fields are 0 and PLANET_SELECT answers `UNSUPPORTED`. A game that has never had a selection of its own reads the console-wide pair from before build 53 (section 5.10), and 0 when there is none.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0040 | POS_SELECT | `u8 slot` | none |
| 0x0041 | POS_SAVE | `u8 slot`, 0xFF = selected | none |
| 0x0042 | POS_LOAD | `u8 slot`, 0xFF = selected | none |
| 0x0043 | POS_LIST | none | `u8 planet, u8 nslots, { u8 filled, u8 pad[3], f32 x, f32 y, f32 z }[nslots]` for the current planet |
| 0x0044 | POS_CLEAR | `u8 slot` | none |
| 0x0045 | PLANET_LIST | none | `u8 count, char[24] name[count], u8 order[count]`; index is the game's planet index. `order` since build 53, see below |
| 0x0046 | PLANET_SELECT | `u8 planet, u8 flags` (bit0 reset level flags, bit1 reset special bolts; which a game acts on, see below) | none. Build 53: the running game's selection, or the last game's; `UNSUPPORTED` before any game has run (above) |
| 0x0047 | PLANET_LOAD | `u8 planet` 0xFF = selected, `u8 flags` 0xFF = selected, the running game's selection since build 53 | none. The planet request goes out first, then the flags act on the planet being loaded. Build 53: RaC2 under RPCS3 without the code switches' patch answers `UNSUPPORTED` for a trip its crash guards are for (section 5.12.2) |
| 0x0048 | DIE | none | none |
| 0x0049 | MOBY_TABLE | none | `u32 table_ptr_addr, u32 table_end_ptr_addr, u16 stride, u16 pad`. The client reads the pointers and the entries with MEM_READ |
| 0x004A | POS_EDIT | `u8 slot, u8 pad[3], f32 x, f32 y, f32 z` | none. Revision 1.13 |
| 0x004B | POS_STORE | `u8 slot, u8 len, u8 pad[2], bytes[len]` | none. Revision 1.13 |

**The menu order (revision 1.21, build 53).** After the names, PLANET_LIST carries `count` more bytes: the planet indices in the order a menu should list them, every index from 0 to `count - 1` exactly once. It is the order the game visits its planets rather than the order it numbers them, and it changes nothing else: a name is still at its id, every request and every telemetry field still carries the id, and the autosplit route lines are still in id order. UYA's order is the old client's combo box (Veldin, Florana, Starship Phoenix, Marcadia, Daxx, Obani Gemini, Blackwater City, Annihilation Nation, Aquatos and its two mission levels, and so on, the vid comics last by their own number), and Deadlocked's is its combo box too, which is the numbering but for Orxon before Valix; RaC1's and RaC2's numbering is their order, and the bytes are 0, 1, 2 and on. The placeholder names, `(none)`, `(unused)` and `(infinite loop)`, come after every real planet. A module before build 53 sends the names alone, and a client that finds no bytes after them, or bytes that are not every index once, lists the planets in id order.

**Deadlocked: a locked planet is unlocked first (build 54).** Deadlocked crashes when it arrives on a planet the save has not unlocked: with no mission of the planet's unlocked or visible, it drops the player onto an empty Missions screen, and any press there brings the game down. So a Deadlocked PLANET_LOAD of a planet the save has not unlocked (bit 1 of its level save's status byte clear) first leaves the save as the game leaves it when that planet unlocks the long way, and only then sends the request: the console menu's planet picker and its Planets page load through the same code (section 9.1). From the game's own progress code (the PS2 prototype's `MF_UpdateLevelProgress`, `MF_UpdateMissionProgress`, `LevelBox::CheckUnlockLevel` and `CheckUnlockMission`; the PS3 code at 0x1D62E0 is the same), the unlock writes **two bytes of the save**, in the planet's `MF_LevelSave` (0xB27FF0 + (planet - 1) x 0x304):
- its **first mission's status** (+0x08, `mission[0].status`): bits 0x01 visible and 0x02 unlocked or'd in;
- then **the level's status** (+0x300): bits 0x01 visible and 0x02 unlocked or'd in.

Nothing else: no badges, Dread points, armour, completions, and not the next-mission word. That is what the game's rules leave in a planet that has just unlocked: every planet's first mission is a main mission that asks for nothing (the PS3 executable's MissionDefs at 0x9E06A4), and the arrival's own progress pass, which runs before the level picks its mission, makes the planet's other missions visible and unlocks those whose own rules allow, so the player arrives in the planet's first mission. A planet the save has unlocked already, DreadZone (which asks for nothing) and the placeholders are left as they are. The bytes are save data: **the next save or autosave keeps the unlock**, and nothing in the game ever locks a planet again. Build 54's Unlock all planets (feature 11) does the same for every planet it leaves locked, Interior included, and sends its load of DreadZone last, once everything is written.

**What the planet flags do in each game.** PLANET_LOAD sends the request first and only then acts on the flags, on the planet being loaded, and only when the request went out. bit0 zeroes the destination's level flags in RaC1, RaC2 and UYA. bit1 takes back the destination's special bolts: RaC1's four gold bolts, and since build 53 UYA's titanium bolts, every one collected on that planet. UYA keeps them eight bytes to a planet id from 0xECE54D, and a bolt belongs to the level it is found in: the five vid comic bolts the game keeps in Starship Phoenix's eight bytes are each taken back by a load of their comic, and a Phoenix load takes back the Phoenix's own two. Before build 53 UYA's load ignored bit1, so a client offers that box for UYA against build 53 or later only. RaC2 has no per-planet special bolt and ignores bit1; Deadlocked ignores both bits. Both are data writes and work under RPCS3 as on a console (section 3.2).

**Writing a slot the player is not standing in (revision 1.13).** POS_SAVE captures a blob from the live game and is the only way a slot was ever filled. POS_EDIT and POS_STORE are the other two a client needs: POS_EDIT for coordinates the user typed into a box, POS_STORE for a blob the client already has. Both act on the current planet, the one POS_LIST reports, and both persist the moment they succeed exactly as POS_SAVE does; no CONFIG_SAVE is needed. Both need the session to be INGAME and answer **NOT_INGAME** otherwise, and both answer **UNSUPPORTED** where the running game has no position slots. A slot outside 0..7 is **BAD_ARG**.

POS_EDIT replaces only the three coordinates of a slot that is already filled. The rest of the blob - the rotation, and the camera floats Deadlocked keeps - is left exactly as the game wrote it, so a position nudged sideways still comes back facing the way it was captured. It is the game that decides which bytes of its blob are x, y and z, the same knowledge POS_LIST's reply is built from, so the coordinates POS_EDIT writes are the ones the next POS_LIST reports and the ones the next POS_LOAD puts in the game. An empty slot is **NOT_FOUND**: there is no blob to preserve, and a client that wants to fill an empty slot from nothing uses POS_STORE.

POS_STORE replaces the whole blob of a slot and marks it filled, whether or not it was filled before. `len` must equal the running game's blob length to the byte, and anything else is **BAD_ARG**: a blob is opaque and self-describing only to the game that made it, so one of the wrong shape - another game's, or one saved before that game's blob changed - would be stored happily and then misread by POS_LOAD. The lengths themselves are not on the wire and never have been: a blob is only ever handed back to the game it came from, so nothing needed to name its size. A client importing old slots therefore keeps the lengths beside its own game knowledge and checks the running game first; a blob it cannot place is one to keep rather than to store, and the BAD_ARG is the console refusing to take its word for it.

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
| RaC2 | Owned, flag | **Level, number, max 4** | — | Ammo, number |
| RaC3 | Owned, flag | **Level, number, max 8** | **XP, number** | Ammo, number |
| Deadlocked | Owned, flag | **Level, number, max 99** | **Ammo, number** | — |

RaC2's slots are RaC3's bar the XP, because the two games keep the same item system: every item, gadget and weapon version has an item id, and the game keeps an owned byte, an ammunition word, an experience word and a version byte per id. Slot 1 is that version, and what crosses the wire is the step in the weapon's own chain — v1, v2, v3, v4 — not the item id the game records it with: a V2 Lancer holds the Lancer V2's item id, and UNLOCK_SET is what turns a level into it. `max` is 4, the game-wide maximum; sixteen weapons reach V4, the rest stop at V2, and UNLOCK_SET clamps to the entry's own count rather than refusing. A weapon whose version byte names no version of itself reads level **0**, a number the field never otherwise takes. Slot 3 is per BASE weapon: a V4 Lancer's rounds are still the Lancer's own word, and only the magazine size "Max all weapon ammo" fills to comes from the V4's stats. Only two weapons have no second version — the Zodiac and the RYNO II — and declare no level; the Clank Zapper and the five RaC1 carry-overs (Tesla Claw, Bomb Glove, Walloper, Visibomb Gun and Decoy Glove, whose second and last version is bought from Slim Cognito rather than earned) have two, and everything else four. RaC2's gadgets and items are owned or not owned and declare slot 0 alone. Ticking Owned leaves the version byte where it is, so a weapon taken away and handed back comes back on the version it had.

RaC2's slot 2 is unnamed. Build 33 offered it as an XP number — the raw experience word the game keeps, thirty-two times the figure the weapon's HUD bar counts — and build 34 withdrew it, because the column did not fit the table a client draws. The word is still in the game and qwark simply no longer reads or writes it: no row declares the slot, UNLOCK_LIST reports 0 there and UNLOCK_SET on it answers UNSUPPORTED. Ammo stayed in slot 3 rather than moving down, because slot numbers are part of this contract and a client draws only the named ones.

RaC3's slot 1 is the weapon version UYAUnlocks drew as a v1..v8 combo box, not a gold flag: gold weapons are a RaC1 idea and RaC3 has none. `max` is the game-wide maximum, so a client may offer 8 for every weapon; UNLOCK_SET clamps to the entry's own level count, and the R3YNO — the one weapon that stops at v5 — lands on v5. Writing 1 is a real downgrade, not "unset".

Deadlocked's slot 1 is the weapon version too, and its slot 2 is that weapon's ammunition rather than any experience the game keeps. The level that crosses the wire is the one the game shows, V1..V99, and a client needs to know nothing else: 1 is the lowest version a weapon can have, and a weapon the player does not have reads Owned 0 and level **0**, a number the field never otherwise takes. `max` is 99, the version a weapon reaches in challenge mode; a first playthrough stops at V10, and nothing in the entry says which mode the file is in. Unlike RaC3's version, that 99 is a hard limit rather than a clamp — the game misbehaves above V99, so UNLOCK_SET answers BAD_ARG for a larger level, and for 0, and writes nothing either time. Its bot upgrades are one owned byte each and declare slot 0 alone, so the level and ammo columns belong to the weapons.

The table has three categories, in this order: Weapons, then Gadgets, then Bot upgrades. Gadgets is the two pairs of boots, the Charge Boots and the Gravity Boots, which sit in the same gadget table as the weapons and are taken and given by the same halfword, but have no version and no ammunition of their own, so those two rows declare slot 0 alone the way a bot upgrade does.

Setting field 0 to 1 may carry its game's side effects: in RaC1 a weapon is handed its maximum ammo, as the old client did. In Deadlocked owning a locked weapon hands it V1, and a weapon that already has a version keeps the one it has, so the checkbox never undoes a level. Clearing it takes the weapon away again and leaves the ammunition where it was.

Deadlocked's feature id 16, "Reset all weapon levels", is retired and never reused, so DESCRIBE no longer lists it. "Max all weapon levels" (15) and "Max all weapon ammo" (17) keep their ids, and both pass over a weapon the player does not have rather than handing it out, and over the two gadget rows entirely. RaC2 has the same pair under ids 38 and 39, and RaC3's "Max all weapon levels" is id 26; the labels are word for word the same in all three, so a client can lay them out together.

An entry may withhold a slot its category otherwise offers, and UNLOCK_SET on that slot is then UNSUPPORTED: RaC3's Suck Cannon declares no ammo, because the game does not count any for it, and RaC2's Zodiac and RYNO II declare no level.

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
| 0x0066 | MOD_PATCH | `u8 index, u8 pad, u16 page` (the Mod row's `index`; a request before revision 1.19 is `u8 index, u8 pad[3]`, page 0) | One page: `u16 n, u16 pages, u32 stamp, { u32 addr, u32 value }[n], u16 nb, u16 pad, { u32 addr, u8 value, u8 pad[3] }[nb]`: the mod as the words and bytes an RPCS3 patch writes (revision 1.16), a page at a time (revision 1.19), section 5.7.1. `UNSUPPORTED` from the PS3 module (revision 1.18) |

```
Mod (120 bytes):
u8   index
u8   flags           bit0 loaded, bit1 auto, bit2 needs_lua, bit3 previous, bit4 parse_error,
                     bit5 checking (revision 1.16)
u8   pad[2]
u32  hash            content of qwark.sum, 0 when absent
char dirname[32]
char name[32]
char version[16]
char author[32]
```

**MOD_RESCAN** reads every mod folder of the current title again, and that is what makes the upload above land: the `qwark.sum` just written becomes the `hash` MOD_LIST reports, the `#-` lines become the name, version and author in the row, and the patch words become what the next MOD_LOAD writes into the game. It answers OK whatever it finds, including for a title with no mod folder at all.

A mod that is loaded at that moment is the one exception, and it is left exactly as it was parsed: its patch words are in the running game next to the originals its unload will put back, so re-reading its folder would leave the console unable to give the game its own instructions again. Its row therefore keeps the hash it was parsed with, and a client that uploads over a mod the console has loaded goes on seeing the old hash until it unloads the mod and rescans. Every other mod is re-read whatever is loaded, so a client that uploads a mod in order to load it — which is the only time the mods panel uploads one — always sees what it just wrote. A rescan used to skip *every* folder it already knew as soon as any one mod was loaded, so an uploaded mod kept the hash and the words of the copy it replaced for the rest of the session; that is fixed, and no client has to do anything differently for it.

**The flags.**

- bit0 **LOADED**: qwark has the mod in this process, by MOD_LOAD, an auto-apply at boot or a PREVIOUS_REAPPLY. On a platform that refuses code patches (SessionInfo `flags` bit2) it means instead that the mod is in game memory, every cave byte and every word of it, which is how a mod an RPCS3 patch put in at load shows up (revision 1.16, section 5.7.1).
- bit1 **AUTO**: the mod re-applies itself silently when the game boots (MOD_SET_AUTO).
- bit2 **NEEDS_LUA**: `patch.txt` names an `automation:` script, which qwark never runs. On a console the mod's words and caves still load.
- bit3 **PREVIOUS**: the mod's bit is set in the previous-session record (SessionInfo `mod_previous`, section 4.1) and the mod has not been loaded again since; it goes when the mod loads and when the record is re-applied or dismissed. Before build 49 this bit was documented and never set, so a client that read it read 0.
- bit4 **PARSE_ERROR**: `patch.txt` could not be read or does not fit qwark's tables. MOD_LOAD answers `IO_ERROR`.
- bit5 **CHECKING** (revision 1.16): only on a platform that refuses code patches, and only while INGAME: qwark has not yet looked for this mod in the running game, so bit0 says nothing yet. It is never set on a console.

### 5.7.1 Mods under RPCS3 (revision 1.16)

qwark cannot load a mod under RPCS3: a mod is patch words, code caves or both, and the emulator recompiles PPU code before it runs it, so words and caves written into a running game are memory and nothing more (section 3.2). RPCS3 applies its own patch files as it loads the executable, though, before it compiles anything, so a mod written as an RPCS3 patch is code the game runs from its first frame. This is the road the savefile helper takes in section 5.12.1, and it is the same here.

**Where the words come from.** MOD_PATCH, request `u8 index, u8 pad, u16 page` with the `index` of the mod's MOD_LIST row and the page wanted (revision 1.19; before it the request was `u8 index, u8 pad[3]`, whose zeros are page 0). All big-endian, laid out as SAVEFILE_PATCH's reply with `pages` where its first pad was:

```
u16 n       number of word rows on this page
u16 pages   how many pages the mod takes, at least 1, the same on every page (revision 1.19; 0 before it)
u32 stamp   the same on every page: CRC-32 of one unbounded reply after this field, below
{ u32 addr, u32 value }[n]                 be32 words
u16 nb      number of single bytes on this page: all of them on the last page, 0 on the others
u16 pad     0
{ u32 addr, u8 value, u8 pad[3] }[nb]      single bytes
```

Every cave comes first, in `patch.txt` order, each as consecutive words of its `.bin` - the whole file, streamed exactly as MOD_LOAD streams it into a console's memory - and then the mod's own words, in `patch.txt` order. That is the order a console writes them in, caves before the branches into them, and the order a patch file should list them in. A row's `value` is the four bytes the game holds at `addr`, read as a big-endian word, so a patch entry of type `be32` writes it as it stands.

A cave whose length is not a multiple of four ends in one to three bytes that make no whole word. They are not padded into one, which would write bytes the mod never names: they are the byte section, one entry each, every cave's tail together and sorted by address. NPEA00386's barlow_joba_trainer is the shipped example: its 534-byte `rack.bin` is 133 rows and two bytes. A patch that writes the rows and then the bytes, in reply order, puts exactly what MOD_LOAD would into the game. Should two entries ever name the same byte, the later one wins; no shipped mod does that.

**Dependencies are not flattened.** MOD_PATCH describes one mod and nothing it depends on. A client walks `#- depends` itself - it names other mods by their `#- name` - writes each mod as its own patch entry, and decides for itself what to do when two mods write the same address. NPEA00423's il-ghost depends on dl-cs; its reply is its own 745 rows, and dl-cs's 253 words are dl-cs's own reply.

The **stamp** is the ordinary reflected CRC-32 SAVEFILE_PATCH, SAVEFILE_LIST and `qwark.sum` use, over what one unbounded reply would hold after it: every word row of the mod, then `u16 nb, u16 pad` with the whole mod's `nb`, then every byte row. It is the same on every page. For a mod that fits one page that is everything after the stamp exactly as it is on the wire, the definition before revision 1.19, so such a mod's stamp has not changed. A client keeps it beside the patch entry it wrote and knows from it alone whether that entry is still the mod's current contents.

MOD_PATCH reads the mod table and the mod's files and nothing of the game, writes nothing, and answers in every session state MOD_LIST answers in. Since revision 1.18 it exists only in the emulator builds, `qwark-rpcs3.exe` and the `qwark-host.exe` simulator: the PS3 module answers it `UNSUPPORTED` whatever the payload, where it used to answer it for information, and never looks for a mod in the game (section 3.2). In the emulator builds it answers:

- `NOT_FOUND` for an index that is not in the list, as MOD_LOAD does for a mod it does not have
- `IO_ERROR` for a mod that failed to parse (flags bit4), as MOD_LOAD does, and for one whose `.bin` is missing, unreadable or empty
- `UNSUPPORTED` for a mod with a Lua automation (flags bit2): part of it is a script, and a patch cannot carry that
- `BAD_ARG` for a payload shorter than its four bytes, and for a `page` at or past the mod's `pages` (revision 1.19)
- `FULL` only for a mod whose byte rows alone would not fit one page, below. A cave has at most three tail bytes and a title at most 32 caves, so no mod qwark can parse comes near it. Before revision 1.19 `FULL` was any mod whose reply would not fit one frame; the size of a frame is no longer a reason for it

**Paging** (revision 1.19). A reply is one frame, at most `QWARK_MAX_PAYLOAD` = 16384 bytes, so a page carries at most (16384 − 8 − 4) / 8 = **2046 rows**, word rows and byte rows together. Take the whole mod as W word rows and B byte rows, in the order above. Then `pages` = ⌈(W + B) / 2046⌉, and at least 1. Page k carries word rows 2046·k up to 2046·(k+1) or W, whichever comes first, so every page but the last is full unless the words run out. The last page carries every byte row after its words; since W + B ≤ 2046·`pages`, it has at most 2046 − B words, and it may have none. A mod of 2045 words and 2 bytes, for example, is page 0 with the 2045 words and page 1 with the 2 bytes; one of exactly 2046 rows is one full page. A `page` at or past `pages` is `BAD_ARG`. Counted in what a mod is made of, its words plus a quarter of its caves' whole-word bytes plus its caves' tail bytes may come to 2046 before it takes a second page: a mod with no words fits 8184 bytes of cave on one. The largest shipped mod, NPEA00423's il-ghost, is a 2968-byte cave and three words, 745 rows on one page, and the largest in the old racman library, NPEA00387's coolcam, a 6312-byte cave and seven words, is 1585 rows on one; a 9.3 KB cave and sixteen words, about 2344 rows, is two pages, and answered `FULL` before this revision.

**What a client does with the pages.** It asks for page 0, then pages 1 to `pages` − 1 of the same index, concatenates the word rows in page order and takes the byte rows from the last page; together they are exactly the one unbounded reply described above, rows in the same order. Should any page's `pages` or `stamp` differ from page 0's, the mod changed underneath it (an upload and a MOD_RESCAN in between): it throws the pages away and starts again. qwark keeps nothing between the requests: every page reads the mod's files again and lays the whole mod out, keeping only its own slice, and the layout depends on nothing but those files, so the pages agree for as long as they do not change. A client written before revision 1.19 sends page 0 and reads `pages` as a pad. For a mod that fits one page it gets exactly the reply it always did. For a bigger one, which used to be `FULL`, it now gets page 0 alone and has no way to know there is more, so a client must read `pages` before it writes a patch; `qwark-rpcs3.exe` ships with its client, which is what keeps an older client from meeting this build.

**The presence rule.** On a platform that refuses code patches a mod counts as in the game when every byte of every cave and every one of its words reads back from game memory exactly as MOD_PATCH hands them out. qwark starts looking on the first INGAME tick and looks a little each tick - at most four reads of at most 2 KB a tick, a run of consecutive words being one read - so that no tick waits on the emulator for long; every mod of the shipped library is looked at within a few dozen ticks. While it looks, MOD_LIST says:

- bit5 CHECKING and not bit0: not looked at yet this session. The row changes by itself once there is an answer, so a client polls MOD_LIST.
- bit0 LOADED: all of the mod is there.
- neither: not all of it is there - a cave byte or a word differs, which is also what a patch from an older copy of the mod looks like - or its cave file is missing.

A mod that is never looked for never shows CHECKING: one that failed to parse, one with a Lua automation, and one with no words and no caves, which MOD_LOAD still loads, as nothing, exactly as it always has.

The answer holds for the session, because a patch goes in at load or not at all. It is looked for again when a game comes back, the same title included, since a patch written meanwhile counts from the next restart and not before, and after every MOD_RESCAN, since the files it was compared against may have changed: a client that uploads a new copy of a mod sees it CHECKING and then not LOADED until it rewrites the patch and the game is restarted. A read that fails, because the emulator is not answering, leaves the mod CHECKING rather than calling it absent. Outside INGAME no mod is CHECKING or LOADED on this platform. qwark writes nothing into the game for any of this, and SessionInfo's `mod_loaded` stays qwark's own loads, so it never carries a patched-in mod.

**What the client does with the reply.** It uploads the mod exactly as for a console - the files, `qwark.sum`, MOD_RESCAN - asks for it with MOD_PATCH, every page of it (revision 1.19), writes it into RPCS3's per-title patch file `patches/<TITLE_ID>_patch.yml` as its own entry, `be32` rows and `byte` entries, enables that entry in `patch_config.yml`, and has the user restart the game. Taking a mod out is disabling the entry and restarting. That half is the client's; nothing in qwark touches RPCS3's files.

### 5.8 Files (0x007x)

Paths are absolute, at most 511 bytes, sent as the remainder of the payload with no terminator. Only paths under `/dev_hdd0/` and `/dev_usb` are accepted. These run on the network thread and never touch game memory.

These are the transport of both libraries: the client uploads a mod's files with them (section 5.7), and it uploads, mirrors, renames and deletes savefiles with them (section 5.13). What a savefile no longer travels this way for is a *load*: the console keeps the file and copies it into the aside buffer itself.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0070 | FILE_OPEN | `u8 mode` (0 read, 1 write and truncate), `path` | `u32 handle` |
| 0x0071 | FILE_WRITE | `u32 handle, bytes` (at most 16380 since revision 1.11, 65536 before it) | none |
| 0x0072 | FILE_READ | `u32 handle, u32 len` (len is taken as 16384 when larger, since revision 1.11) | up to `len` bytes; fewer at end of file |
| 0x0073 | FILE_CLOSE | `u32 handle` | none |
| 0x0074 | FILE_DELETE | `path` | none |
| 0x0075 | DIR_LIST | `path` | `u16 n, { u8 type (0 file, 1 dir), u8 namelen, u32 size, char name[namelen] }[n]` |
| 0x0076 | DIR_CREATE | `path` | none. Creates missing parents |
| 0x0077 | DIR_DELETE | `path` | none. Recursive |
| 0x0078 | USER_ID | none | `u32 user_id` |
| 0x0079 | FILE_RENAME | `u16 from_len, char from[from_len], char to[rest]` | none (revision 1.10) |

**DIR_LIST** fills one reply frame and stops: it has always broken off at whatever the reply buffer held rather than paging, and since revision 1.11 that buffer is 16384 bytes, so a folder of about 1400 short names is where the listing now ends instead of about 5700. Every folder qwark's own libraries use — the mod folders under `/dev_hdd0/qwark/mods/<TITLEID>/` and the savefile categories under `/dev_hdd0/qwark/savefiles/<TITLEID>/` — is orders of magnitude smaller than that, and the two library listings a client actually reads, MOD_LIST and SAVEFILE_LIST, are bounded by their own tables and fit whole. A client browsing an arbitrary `/dev_hdd0` folder with thousands of files in it will see the listing cut short.

**FILE_RENAME** (revision 1.10) moves a file within `/dev_hdd0`. Both paths obey the rule above; the old path must exist (`NOT_FOUND` otherwise) and the new one must **not** (`BAD_ARG` if it does), because cellFs and Windows refuse to replace an existing destination while POSIX replaces it silently, and a rename that means one thing on a console and another in the simulator is worse than one that never overwrites anywhere. A client that means to replace a file deletes it first.

It sits at 0x0079 rather than at 0x0075, where a rename would naturally have gone: 0x0075 has been DIR_LIST since revision 1, and an opcode is never renumbered.

### 5.9 Combos (0x008x)

A combo fires once when `pad_mask` equals its mask exactly, and re-arms when `pad_mask` returns to 0. Mask 0 disables the combo. Persisted in config, one set of masks for the console rather than one per title.

Actions: 0 SAVE_POSITION, 1 LOAD_POSITION, 2 DIE, 3 LOAD_PLANET, 4 LOAD_SETASIDE_FILE, 5 OPEN_MENU (revision 1.22). LOAD_SETASIDE_FILE makes the game load whatever is in the savefile helper's aside buffer (section 5.12), and does nothing in a game that has no helper, or where code cannot be patched and no RPCS3 patch put the helper in (section 5.12.1). OPEN_MENU opens the console menu, or closes it when it is up (section 9).

**OPEN_MENU** (revision 1.22) differs from the other five in three ways:

- It has a mask before anybody sets one: **0x060F**, L1 + R1 + L2 + R2 + L3 + R3, which a hand cannot press by accident in play. COMBO_LIST reports it whether or not config.txt names it, and COMBO_SET of 0 turns it off for good, like any other combo. It is a shortcut: with it at 0 the menu still opens from the Help row of the game's pause menu (section 9.2).
- **COMBO_ENABLE does not hold it off.** A user who switched the combos off for a run keeps the quick way into the menu that would switch them back on. COMBO_SUSPEND does hold it off, so a client can capture it like any other.
- Where the session has no menu - a game with no renderer, RPCS3, `menu.enabled = 0`, or another mod on the renderer's hooks - it shows a console notification saying why, once a session per reason, and does nothing else.

COMBO_LIST therefore has **six** entries since revision 1.22, where it had five. A client reads `n` and takes what it knows: the client shipped before this revision draws its five rows and looks each one up, so the sixth is simply not drawn.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0080 | COMBO_SET | `u8 action, u8 pad[3], u32 mask` | none |
| 0x0081 | COMBO_LIST | none | `u8 n, { u8 action, u8 pad[3], u32 mask }[n]` |
| 0x0082 | COMBO_SUSPEND | `u8 suspend` (1 hold every combo off, 0 resume) | none |
| 0x0083 | COMBO_ENABLE | `u8 on` (1 combos fire, 0 every combo is held off) | none |

COMBO_ENABLE (revision 1.12) is the user's switch, the one the old RaCMAN had. Send 0 and no combo fires again until a later 1, whatever the masks say; any value other than 0 or 1 is `BAD_ARG`. It is not a game write, so it answers in any session state, exactly as COMBO_SET does: both do the same thing, which is write a key into `/dev_hdd0/qwark/config.txt`. The key is `combo.enabled`, it is 1 for a config that has never mentioned it, and it has the same scope as the five mask keys — one switch for the console, not one per title — so it survives a client restart, a game reboot and a console power cycle without the client storing anything. A hand edit picked up by CONFIG_RELOAD counts as a change like any other.

There is no op to read it back, because there is no need for one: `flags` bit3 COMBOS_OFF in the session info block (section 3) is set while the switch is off, and HELLO, GET_STATE and every telemetry packet carry it. COMBO_LIST is unchanged and still reports every mask whether the switch is on or off. Since revision 1.22 the switch holds off every combo but OPEN_MENU (above).

COMBO_SUSPEND (revision 1.8) exists for capture. A client that records a combo reads the buttons out of telemetry's `pad_mask`, which is the same pad the console is watching, so without the hold the press that records "load position" also loads a position. Send 1 when the capture starts and 0 when it commits, is cancelled or is dropped; it is not a game write, so it answers in any session state.

The hold expires by itself two minutes after the COMBO_SUSPEND that set it, and is dropped when the session leaves the game. A client that dies mid-capture therefore cannot leave the combos off for good, and a client that resumes at the two-minute mark just sends 1 again. Lifting a hold does not re-arm anything: a combo still held when the hold ends fires only after the pad has returned to 0, which is the ordinary arming rule.

The switch and the hold are independent, and either one on its own holds every combo off (the switch every combo but OPEN_MENU, since revision 1.22): a combo fires only while the switch is on and no hold is in force. Turning the switch back on under a live hold changes nothing until the hold ends, and a hold expiring while the switch is off changes nothing either. Re-enabling re-arms as little as lifting a hold does — a combo whose buttons are still held when the switch comes on fires only after the pad has returned to 0.

### 5.10 Config (0x009x)

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x0090 | CONFIG_RELOAD | none | none. Re-reads `/dev_hdd0/qwark/config.txt` after a user edited it |
| 0x0091 | CONFIG_SAVE | none | none. Config is also written on every change |

Config keys the module reads for itself: `log` (1 by default) writes a line per event to `/dev_hdd0/qwark/qwark.log`; `log = 0` silences it, which costs a file open, write and close per line on the console. `menu.enabled` (revision 1.22, 1 by default) is the console menu's own switch: `menu.enabled = 0` writes no renderer into any game, even when the menu combo is pressed, and takes out one already in at the first safe moment, the game's Help row its own again (section 9.2). No op writes it; a user edits the file, and CONFIG_RELOAD or a reboot picks it up.

Keys a client's own ops write. Console-wide rather than per title: the six combo masks `combo.save`, `combo.load`, `combo.die`, `combo.planet`, `combo.setaside` and, since revision 1.22, `combo.menu` (COMBO_SET; 0x060F when the file does not name it), `combo.enabled` (COMBO_ENABLE, revision 1.12, 1 when the file does not mention it) and `selected.slot` (POS_SELECT). Per game since build 53: `selected.<game>.planet` and `selected.<game>.planet_flags` (PLANET_SELECT), `<game>` being `rac1`, `rac2`, `rac3` or `rac4` as in the manips' keys (section 5.14), so the disc trilogy keeps three. Until build 53 the planet selection was one pair for the console, `selected.planet` and `selected.planet_flags`; qwark never writes those two again, and a game whose own key the file does not hold yet reads the old one in its place, so the first PLANET_SELECT in each game is where its own pair begins (section 5.5). Each key is written to the file the moment the op lands, so none of them needs a CONFIG_SAVE to survive a power cycle, and CONFIG_RELOAD picks up a hand edit to any of them without a game restart.

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
| 0x00B1 | SAVEFILE_READ | `u32 offset, u32 len` (len at most 16384 since revision 1.11, 65536 before it) | the bytes of the aside buffer at that offset; short at the end |
| 0x00B2 | SAVEFILE_WRITE | `u32 offset, bytes` (at most 16380 since revision 1.11, 65536 before it) | none |
| 0x00B8 | SAVEFILE_PATCH | none | `u16 n, u16 pad, u32 stamp, { u32 addr, u32 value }[n], u16 nb, u16 pad, { u32 addr, u8 value, u8 pad[3] }[nb]`: the running game's helper as the words and bytes a patch writes (revision 1.15, section 5.12.1). `UNSUPPORTED` from the PS3 module (revision 1.18) |

INFO, READ and WRITE answer **NOT_INGAME** outside INGAME. READ and WRITE answer **UNSUPPORTED** where the platform cannot patch code (RPCS3, `flags` bit2) unless an RPCS3 patch put the helper in the game when it loaded; INFO answers there either way (revision 1.15, section 5.12.1). Before revision 1.15 the three answered UNSUPPORTED on that platform ahead of the INGAME test, so it said UNSUPPORTED at the XMB too; it says NOT_INGAME there now, as a console does. None of the three installs the helper: that is done by the set-aside and load actions, by RaC1's Force autosave, and by a library STORE or RESTORE with a valid file, and a client never asks for it or sees it happen. INFO, the library's metadata ops and raw READ and WRITE of the aside buffer only ever look.

Since revision 1.10 READ and WRITE are the **debug and test path**, not what a save or a load does: the library ops of section 5.13 move a whole file between the console's own filesystem and this buffer without any of it crossing the wire. They still work exactly as they did, and a client that wants the bytes on the PC still reads them here.

- **supported** — 1 when qwark has a helper for the running game. 0 is an OK answer, not an error: it is how a client knows to hide its save-file panel. All four games are 1 today.
- **installed** — 1 when qwark has written the helper into this process. A supported game reports 0 until an action installs it, and while it is 0 so are `running` and `pending`, since the helper's request bytes are not read from a process that has no helper in it. Where code cannot be patched (revision 1.15) it is 1 when an RPCS3 patch put the helper in the game at load, and 0 when nothing did or what is there does not match; qwark writes nothing there, so it is the only way it is ever 1 on that platform (section 5.12.1).
- **running** — 1 when the helper's own byte reads 1. The helper writes it on every call, so this says the code is installed *and* that the game is reaching the hook. It is 0 for the first frame or two after an install, and it stays 0 for as long as the game is on a screen that does not run the hooked routine.
- **pending** — bit0: a set-aside request is still outstanding. bit1: a load request is still outstanding. bit2 (revision 1.10): qwark is copying between a file and the aside buffer, section 5.13. The helper clears its own request byte when it has done the work, so a client polls this rather than guessing at a delay.

  **A clear bit means the work is over, not that the byte happens to read 0.** Build 11 made that true at both ends. In the game, the helper clears the set-aside byte *after* its copy loop rather than before it, so the byte reading 0 is the copy having finished; it copies a megabyte or two in 32 KB steps, and clearing first meant a client could read the aside buffer while the copy was still walking it and save a file with a torn tail. In qwark, a bit goes up the moment the request byte is written and stays up until the byte has read 0 on **every tick of a settle window**, 30 ticks of the 120 Hz loop, a quarter of a second (`SAVEFILE_SETTLE_TICKS` in `src/core/savefile.h`). The window is there because one read of one byte of a running game is thin evidence: the helper puts no barrier between the copy and the store that clears the byte, and a memory read that fails answers with a zero of its own. A byte that goes back to non-zero inside the window starts it again.

  A client should therefore poll INFO until the bit clears and only then read (or, for a load, only then consider the load done). The quarter of a second is the price of the guarantee, on an operation a user waits seconds for.
- **size** — how many bytes the aside buffer holds, which is the size of a save file for that game. It is fixed per game and does not depend on the save.
- **done**, **total**, **error** (revision 1.10) — the transfer of section 5.13. `done` and `total` are its bytes, and `error` is why the last one stopped: 0 none, 1 file missing, 2 io error, 3 the file is not exactly `total` bytes, 4 the game went away underneath the copy, 5 the aside buffer was already spoken for, 6 (build 43) the game was not somewhere a save could be taken or put back, 7 (revision 1.20, build 53) there was no save to act on. All three describe the **last** transfer until the next STORE or RESTORE starts, so a client that polls once more after `pending` bit2 clears reads how it ended rather than zeroes. Since build 43 `error` is also why a set-aside or a load was refused, and since build 53 a set-aside or a load clears it as it starts, as a STORE or a RESTORE always has: it is the reason for the last of the four, and a request that goes out, or is refused for a reason with no code here (the `BUSY` of an emulator qwark cannot read yet, say), leaves 0 rather than an earlier refusal's reason. A set-aside or a load asked for while a STORE or a RESTORE runs is `BUSY` with error 5 (build 53): it would write over the save being stored, or hand the game a buffer still being filled.

**Error 6, the game was not idle (build 43).** A set-aside or a load only means anything while the game is sitting still. Ratchet: Deadlocked keeps working on its own save data from inside the game — the load is a menu, the autosave writes the same buffer, and every planet load, lobby screen and cutscene transition moves the game between modes that read or rewrite it — so a request raised across one of those either copies a save the game is still writing or hands the loader a buffer something else is about to walk over, which is how a stored file came back with the game's fingerprints in it. qwark now asks the running game before it raises either request, and answers `BUSY` with error 6 without writing anything into the game, so a client shows a reason at the moment the user pressed the button rather than a request that quietly comes to nothing. Since build 53 RaC1, RaC2 and RaC3 are asked too, with tests of their own (below). For Deadlocked the test is the game's own: `gameMode` must be 0 (ordinary play) or 3 (the pause the savefile helper parks the game in itself), and `pendingGameMode` must be -2, meaning no mode change is queued. The embedded helper makes the same test on the frame it would act, because qwark's tick and the game's frame are different clocks; a request it finds busy there is **dropped, not deferred** — a load that fired at some later moment the user did not ask for would be worse than one that plainly did not happen — and it leaves a code in the request byte that qwark clears and reports as error 6 in turn.

**Deadlocked's load waits for its own pause (builds 44 and 45).** The helper puts the game in its pause to run the load menu, and it runs part way through a frame, so the frame it pauses in still finishes and anything that frame queued - an autosave above all - takes the game out of the pause for a while and back. Build 43 handed the save over four frames after pausing regardless, and what the game did next went on writing into the save it had just been given. The helper now hands it over only once the game is in its pause with no mode change queued, on a later frame than the one it paused in (it keeps the game's own frame counter for the mode, gameModeTimer, from the pausing call), waiting up to ten seconds for that. Build 44 also made the game sit in the pause for fifteen frames in a row first; build 45 drops that quarter of a second. The game back in ordinary play before then means the pause is gone, and the load is dropped rather than raised later; running out of time drops it too and puts back the mode the pause replaced. Either way qwark reports error 6, as for a load the helper refused outright. While the load waits, `pending` bit1 stays up. A cancel in the menu waits the same way (build 45): the helper used to put the old mode back on the frame it saw the cancel, and the game's own close of the menu can queue a pause after that, which left the game paused with no menu. It now moves the load byte from 2 to 3 and puts the mode back on a later frame, once the game is in play or in its pause with nothing queued, and only if it is still paused; the byte goes to 0 then, so `pending` bit1 stays up through the cancel too.

**RaC1's, RaC2's and RaC3's loads go from the game's pause (build 53).** The three games' loaders write every field of the save over the live game the moment they run, the current planet first, while the planet's own data stays the old planet's until the planet load that follows. Up to build 52 the helper loaded wherever the request found the game, and RaC1 then updated and drew the rest of that frame with the new planet's number over the old planet's world: the world render picks a planet's sky routine by that number, and a Veldin save loaded on some planets drew sky layers the loaded sky did not have, through a null pointer. RaC3 had not been seen to crash and goes the same way as a precaution.

None of the three has a mode that runs nothing. Each has a freeze of sorts - RaC1's prompt, and in RaC2 and RaC3 the mode the game's mode enum calls freeze (4), a message over a stopped game - but it draws the world under the message, which is the render that crashed. The pause alone neither updates nor draws the world, so the helper loads from the pause, and it takes the game there itself, from whatever the game is doing and with none of the Start button's own tests:

- **RaC1** changes state by writing the state word, from dozens of places, and the helper does the same: it enters the start menu (state 3) through the game's own leaf routine for that (0x13D3E0, which the title uses), which also puts back the start menu's own words - the screen, the pending action and three more - so the menu does not repeat its last close; the screen is then set to the one the Start button opens.
- **RaC2** has its pause written the same way. Its helper runs last in the frame, after the frame's update and render, and everything the game runs after it in that frame reads the mode word afresh, so the helper writes the pause into it with no mode change queued behind it and loads in the same call: what is left of the frame is the pause's, the loader sets the load request, and the planet load follows at the end of that frame. No frame of the pause runs, so the pause menu is never entered. Before it writes the mode it puts away what a cutscene leaves behind that the planet load does not, the way the Start button's skip does: the movie player's Bink file is closed, the cutscene's queue of movies and scenes is emptied, the callback the cutscene's end would run is dropped without running, and a movie held under the console's freeze is let go; leaving the vendor, it also gives the renderer back the setting the vendor's entry zeroed, which nothing but the vendor's exit restores. Up to this change the helper asked mode_requestChange for the pause as RaC3's still does, and a load during a cutscene still failed on hardware: the request is refused while the cutscene has its next movie, scene or end queued, and once the change is made it runs the movie's or the scene's exit, which runs the cutscene's callback - a moby's continuation, which can itself start the next planet's load, after which the load was refused.
- **RaC3** changes mode through the game's mode change, which at the end of the frame runs the leaving mode's exit (a movie stopped, a scene ended, the vendor closed) and the pause menu's entry. Its helper runs first in the frame, and the frame then runs the update and render of the mode it began in whatever the helper writes, so writing the mode word is not enough there: the helper asks mode_requestChange for the pause as the Start button does (mode 3, pushed, screen 1, or screen 5 on the vid comics), and asks again on its next call while the game refuses for now - a mode change already queued, the player with no health, the game blocking its pause for the moment.

RaC1 and RaC3 load on the first call from a later frame that runs in the pause: the state or mode reads the pause (with nothing queued) and the game's own frame counter for it has moved since the call that entered it. A load asked for while the player is already in the pause goes at once. The planet load that follows starts the new planet in its ordinary opening mode - RaC1's main loop starts every planet in its arrival state, RaC2's and RaC3's mode stacks are reset and the new planet starts in ordinary play - so the pause ends with the load. `pending` bit1 stays up while the load waits, up to ten seconds by the console's clock (the timebase; until this change 600 calls of the helper, which came to five seconds or less with two controllers in); a load still waiting then, or one that finds the game somewhere it may not be paused while it waits, is dropped and reported as error 6, and leaves the player in the game's own pause. A RaC2 load never waits: the helper either refuses it at once or loads it.

What is left for **error 6** is a load the pause cannot be entered for at all, refused before anything is raised: a planet load already on its way (all three), and in RaC2 a trip already decided, the ship's mode queued or under way; RaC1 playing a movie, which runs in a frame loop of its own until it ends, or in a state its main loop runs nothing in; RaC2 on the title screen, or while the console holds the game in its own freeze (the PS button's menu, a lost controller), whose latch only the game's own release clears; RaC2 and RaC3 in space, the trip between planets, or in a mode past 9 (RaC3's lobby and multiplayer modes); RaC3 in debug mode, which its mode change traps on leaving. RaC2's debug mode, refused until this change for the same trap, loads now: its helper never leaves a mode through the mode change. The helper makes the same tests on its own frame.
qwark makes the same tests before it raises either request, so most refusals are a `BUSY` at once. A load also needs either a save in the aside buffer or, with the aside buffer empty, a save of the game's own to fill it from; a set-aside needs only that save, wherever the game is, since it copies the game's last save and not its live state. Without them the answer is error 7 (revision 1.20). The aside buffer is empty when its first word is 0, which is what every game's helper takes for nothing set aside, filling the buffer from the game's save instead - but only until qwark itself has written into it in the process, by a RESTORE's copy or SAVEFILE_WRITE (build 53). After that a first word of 0 is what the user put there and no save, so a load of it - the LOAD_ASIDE action, the combo, or the load a RESTORE raises after its copy - is refused with error 7 in all four games, Deadlocked included, before anything is raised; until build 53 such a RESTORE loaded the game's own last save and reported success. A save is recognised as the games' own restores recognise one, by the sizes of its blocks at its head (RaC3's are 0x37B0 and 0x84C0, or RaC2's older pair). RaC1's, RaC2's and RaC3's LOAD_ASIDE actions force fast loads on, and are refused before they do; a request that fails as it goes out puts back its own force, a load the helper drops on the game's frame puts them back on the next tick, and the end of the load puts back the checkbox's setting as a planet load's end does (build 53 for RaC1 and RaC3). In RaC1 and RaC2 a drop puts back only the dropped load's own force (build 53): once a PLANET_LOAD has gone out after it the force is that load's, and a RESTORE's load, which forces nothing, puts back nothing when it is dropped, whatever a planet load under way forced. RaC1's load is over once its request word is back at 0 and its game state has left 6, the planet transition's and the new level's own, because one of the fast-load sites runs in that state after the request word is cleared - and only once the request word has been seen up since that load went out (build 53): a set-aside load asked for in a planet's fly-in, the state still 6 from the planet load, is taken by the helper into the start menu, state 3, which until then read as the planet load's end and put the fast loads back before the set-aside load was raised.

**RaC1's request bytes and aside buffer moved, and RaC2's helper (build 53).** RaC1's request bytes were the mod's 0xB00070..0xB00073, inside the game's bump arena, and its aside buffer 0x1000000..0x10B0000 ended over game variables from 0x10A0230 on. Both live in a static buffer of the game's, 0xFB0230..0x10A0230: the aside buffer at 0xFC0000, still 0xB0000 bytes so every save file keeps its size, and the bytes at 0x1080000 (`api_mod`, `api_load`, `api_setaside`, Force autosave's byte at 0x1080003). One routine writes that buffer: the update function of moby class 1750 (0x2B1D98, registered for class 0x6D6 by the class map at 0x2A3B8), which serialises a save into it, 0xA4A14 bytes and never past 0x1054C44, on every frame such a moby updates; this paragraph called it a debug menu's command until the review that followed. The request bytes are clear of it. The aside buffer is not: in a level with that moby its first 0x94C44 bytes become the level's own save from 0xFDD0 on. The mod's buffer had the same exposure over its first 0x54C44 bytes, so it is not new in build 53. What class 1750 is, and which levels place one, is level data the executable does not hold. RaC2's helper was at 0x22A368, the head of a routine planet Joba runs every frame, so it ran in Joba's place there; it is in dead library code at 0x113EC64 now, still reached by a `bla` from the same hook site, and its request bytes and aside buffer did not move. RaC3's helper stays at 0x980AC0 with the same hook word, request bytes and aside buffer, but it grew with the pause, and it shares its dead function with the Infinite ammo (until build 53 Freeze ammo) code switch at 0x980F00 (section 5.12.2): the build now fails a RaC3 helper that would run into the switch, as it already did for RaC1's and RaC2's caves. Its two words for the wait are at 0xD9FF14 and 0xD9FF18, zero-filled data a patch need not write. It also carries UYA's trophy reset (section 5.14): a second cave at 0x9808C0..0x980AC0, the top of the strictly dead code below its own, and a third at 0x980308..0x9807F0 for the thread the reset runs on, which the build fails either of if it outgrows it (only 0x980304..0x980308 and 0x9807F0..0x9808C0 stay free of the helper; the console's fast-load cave takes 0x9807F0..0x9808B0 of the second, section 5.12.3), and a fourth request byte, `api_trophy` at 0xD9FF1C, which a console install zeroes with the other three, with the helper's busy byte for the reset beside it at 0xD9FF1D. SAVEFILE_PATCH's byte entries stay the three request bytes: the fourth is zero-filled data too, and nothing writes it where code cannot be patched. RaC1's, RaC2's and RaC3's SAVEFILE_PATCH words are all new, so a patch written by an earlier build for any of the three reads as one from another build (section 5.12.1) and the client writes it again; Deadlocked's did not change by a byte, and its patches still count.

A client built against revision 1.9 reads the first eight bytes and is right about every one of them; the three new fields are appended, and nothing before them moved.

**A save, end to end.** FEATURE_TRIGGER the game's SAVE_ASIDE action; poll SAVEFILE_INFO until `pending` bit0 clears; SAVEFILE_READ the whole buffer in chunks of at most 16384 bytes (16000 is the size a client that also talks to older builds should use). **A load** is the reverse: SAVEFILE_WRITE the file into the buffer in chunks from offset 0, in order, each write answered before the next goes out, then FEATURE_TRIGGER the LOAD_ASIDE action and poll until `pending` bit1 clears. The bytes are opaque; nothing on the PC knows the save format.

A client is expected to hold the whole save until it has every chunk before it writes a file, and to send a file only when its length is exactly `size`: the console cannot tell a short file from a whole one, and a save that is neither is what the game crashes on later.

Bounds: a READ whose `offset` is past the end is BAD_ARG and one that runs off the end comes back short, so a client can ask for a round chunk at every offset. A WRITE that would run off the end is BAD_ARG rather than trimmed, because a client sending more than the buffer holds has the wrong file.

**Where the buffer comes from.** qwark carries a small piece of PowerPC code per game, compiled from one source in `src/games/sfhelper/` and embedded as bytes. Installing it writes one or two code caves and a branch word into the running game; from then on the game calls it once a frame, and it does nothing until a request byte changes. It is never reverted: taking a branch back out from under code that may be executing it is a crash, and with no request outstanding it costs a byte write and three comparisons a frame. Nothing is written to the console's filesystem at any point.

Before this revision the same two ACTIONs moved a `tempsave` file under `/dev_hdd0/game/<TITLEID>/USRDIR`, which the client then pulled over the FILE ops, and the helper was a mod the user had to load first. Both are gone. The FILE ops stay, for the mod library.

### 5.12.1 The helper under RPCS3 (revision 1.15)

qwark cannot install the helper under RPCS3: the emulator recompiles PPU code before it runs it, so a cave and a branch written into a running game are memory and nothing more (section 3.2). RPCS3 does apply its own patch files, though, and it applies them to the executable as it loads it, before it compiles anything. A patch that writes the helper's caves and hook words at load, and zeroes its three request bytes, is therefore code the game runs from its first frame, and the savefile block works on top of it exactly as it does on a console.

**Where the words come from.** SAVEFILE_PATCH, request none, describes the helper for the running game. All big-endian:

```
u16 n       number of patch words
u16 pad     0
u32 stamp   CRC-32 over every byte of the reply after this field
{ u32 addr, u32 value }[n]                 be32 words
u16 nb      number of single bytes, 3 today
u16 pad     0
{ u32 addr, u8 value, u8 pad[3] }[nb]      single bytes the patch sets at load
```

Every cave comes first, as consecutive words in address order and the caves in the order qwark installs them, then the hook words, which is also the order a patch file should list them in: a reader applying them one at a time never has a branch in place before what it branches to. A word's `value` is the four bytes the game holds at `addr`, read as a big-endian word, so a patch entry of type `be32` writes it as it stands. A cave whose length is not a multiple of four would have its last word padded with zero bytes; every cave today is whole instructions, so none is.

The single bytes are the helper's three request bytes, `api_mod`, `api_load` and `api_setaside` in that order, each with `value` 0: the three a console install zeroes before it hooks anything, because whatever the executable holds there would fire the moment the hook goes in. Under RPCS3 there is no install and the hook is live from the first frame, so the patch zeroes them at load instead. It mattered: until build 50 RaC2's three sat in its executable's code segment and loaded as 0x80 0xFE 0xA4, which without the zeroing read from boot as a set-aside and a load outstanding that never cleared, with the helper's refusal code in the load byte. Since build 50 every game's request bytes load as zero - RaC1's, RaC3's and Deadlocked's are in zero-filled BSS, and RaC2's are in a gap of the read-write segment that is zero in the file - and the byte entries stay, for the next executable that does not.

**RaC2's request bytes moved in build 50.** rc2-save, which the RaC2 helper comes from, kept `api_mod`, `api_load`, `api_setaside` and its copy counter at 0x10CD71D..0x10CD723, inside the executable's code segment, and the helper stores to `api_mod` on every frame. A console does not mind. RPCS3 does: it makes that segment read-only once it has applied its patches (section 3.2), so the first frame through the patched-in hook would have faulted the game. They are at 0x1317728, 0x1317729 and 0x131772A now, with the counter at 0x131772C, in the gap between the end of the TLS template and the next data at the top of the read-write segment (0x1317714..0x131773F), which is part of the segment's file data, reads zero and is referred to by nothing in the executable; the code switches' two RaC2 flag bytes take the start of the same gap. SAVEFILE_PATCH names the new addresses, and a patch written by an earlier build differs in the helper's own code as well as in its bytes, so it reads as not present and the client writes the new one.

The stamp is the ordinary reflected CRC-32 that SAVEFILE_LIST and `qwark.sum` use, taken over everything after it exactly as it is on the wire - the word pairs, the `nb` head and the byte entries - so it changes exactly when anything the patch writes does: a client keeps it beside the patch it wrote and knows from it alone whether that patch is still the current build's.

It reads nothing out of the game and writes nothing into it. Since revision 1.18 it exists only in the emulator builds, `qwark-rpcs3.exe` and the `qwark-host.exe` simulator: the PS3 module answers it **UNSUPPORTED** in every session state, where it used to answer it for information - on a console qwark writes those very words itself, on the first action that needs them - and never looks for a patched-in helper (section 3.2). In the emulator builds it answers **NOT_INGAME** outside a game, **UNSUPPORTED** for a game with no helper (a title qwark does not know included) and with the helper switched off by `savefile_helper = 0` in `config.txt`. Since build 53 RaC3's is the largest reply, 658 words and three bytes in 5300 bytes with its trophy reset and the reset's thread; Deadlocked's is 2132, RaC1's 2116 and RaC2's 1972, all well inside a frame.

**The presence rule.** On a platform that refuses code patches, the running game's helper counts as present when every byte of every cave and every hook word SAVEFILE_PATCH describes reads back from the game exactly as qwark would have written it. The single bytes are no part of it: the helper and qwark change them from the first frame on, so what they read later says nothing about whether the patch went in. qwark looks once a session, on the first INGAME tick, and keeps the answer until the session ends; it never looks outside INGAME, and it looks again when a game comes back, since a patch goes in at load or not at all. So:

- A patch written while the game is running counts from the next restart, and not before.
- A patch from an older qwark build differs somewhere - a cave byte, or a hook word that branches to where the old helper began - and reads as **not present**. The client writes the current words over it and the game is restarted again.
- A read that fails, because the emulator is not answering, leaves the answer unknown rather than "not present": INFO and every op that needs the helper answer **BUSY** until a look succeeds. The three library ops that answer on the network thread rather than the tick (CATEGORIES, LIST, CATEGORY) never read the game themselves: they take the answer the tick found, and are BUSY for the tick or so before the first look as well.

**When it is present** everything behaves as on a console after an install. INFO reports `installed` 1, `running` is the helper's own `api_mod` byte reading 1, and `pending` and the transfer fields are what they always are; READ, WRITE, the two actions, RaC1's Force autosave, the combo and the whole library of section 5.13 work. The install step writes nothing at all - not the caves and the hook, which are there already, and not the three request bytes a console install zeroes first either, because here the patch zeroed them at load, the hook has been live since the game booted, and those bytes are the helper's own. RaC1's, RaC2's and RaC3's set-aside loads force fast loads as they do on a console when the code switches' patch is in as well (section 5.12.2), through the fast-load flag, and go at the game's own pace without it; before revision 1.17 RaC2's always went at the game's own pace here.

**When it is not**, INFO answers with `supported` as on a console and `installed` 0, and everything that needs the helper answers UNSUPPORTED as it did before this revision. A client tells the platform apart by SessionInfo `flags` bit2, which it already reads: with bit2 set and `installed` 0, it offers to write the patch.

**What the client does with the reply.** It writes the words and the bytes into RPCS3's per-title patch file, `patches/<TITLE_ID>_patch.yml` under RPCS3's folder, enables that patch for the game in `patch_config.yml`, and has the user restart the game. That half is the client's; nothing in qwark touches RPCS3's files.

### 5.12.2 Code switches under RPCS3 (revision 1.17)

Nine toggles are instruction patches on a console, flagged `WRITES_CODE`: RaC1's Fast loads and Infinite ammo; RaC2's Fast loads and Infinite ammo; RaC3's Infinite ammo (Freeze ammo until build 53) and Fast loads; Deadlocked's Crash patches, Fast loads and Refill ammo. (RaC3's Fast loads were two data words, and needed no switch, until they became a code patch; see 5.12.3. RaC1's Infinite health was the tenth until build 53, when it became Freeze health, a freeze that writes no code; its switch is still in RaC1's patch, below, and nothing writes its flag.) qwark cannot patch code under RPCS3 (section 3.2), so on that platform the game is given both forms of every instruction those toggles change, at load, by an RPCS3 patch - the road the savefile helper takes in section 5.12.1. Each patched instruction, a *site*, becomes a branch to a *trampoline* in a code cave; the trampoline reads a *flag byte*, runs the patched form of the instruction when it is 1 and the game's own when it is 0, and branches back to the instruction after the site. The flag bytes are in data memory, which RPCS3 leaves writable, so turning a toggle on or off there is a byte write.

Deadlocked's crash patches include one word of data, the last word of a vperm control table at 0x808830, which RPCS3 makes read-only with the code around it. Its switch hooks the one instruction that loads the table instead, and with the flag set loads a copy of the patched table out of the cave.

On a console none of this happens: the toggles patch code as they always have, qwark never looks for the switches, and bit4 is never set. Since revision 1.18 none of it is in the PS3 module at all, and SWITCH_PATCH, which used to answer there for information, answers `UNSUPPORTED`; it exists only in the emulator builds, `qwark-rpcs3.exe` and the `qwark-host.exe` simulator (section 3.2).

**Where the words come from.** SWITCH_PATCH, request none, describes the switches of the running game. All big-endian, laid out exactly as SAVEFILE_PATCH's reply:

```
u16 n       number of rows
u16 pad     0
u32 stamp   CRC-32 over every byte of the reply after this field
{ u32 addr, u32 value }[n]                 be32 words
u16 nb      number of flag bytes
u16 pad     0
{ u32 addr, u8 value, u8 pad[3] }[nb]      the flag bytes, each with value 0
```

Every cave comes first, as consecutive words, caves in address order - Deadlocked's last is the sixteen-byte table its bot-crash trampoline loads - and then every site's branch word, in address order: a reader applying the rows one at a time never has a branch in place before the trampoline it branches to. A row's `value` is the four bytes the game holds at `addr`, read as a big-endian word, so a `be32` patch entry writes it as it stands. The byte entries set every flag byte of the game to 0, so every switch is off at every boot whatever the executable holds there; every flag is zero in the file today, and the entries are the belt to that. The stamp is the reflected CRC-32 of section 5.12.1, over everything after it as it is on the wire.

| Game | Rows | Flag bytes |
|---|---|---|
| RaC1 | 44: five trampolines (39 words) and five sites | 0x9058B0 fast loads, 0x9058B1 infinite ammo, 0x9058B2 the old infinite health, which nothing writes since build 53 |
| RaC2 | 33: four trampolines (29 words) and four sites, the crash guards' two included | 0x1317720 fast loads, 0x1317721 infinite ammo |
| RaC3 | 58: three trampolines (55 words) and three sites, two of them fast loads' | 0xD9FF10 infinite ammo, 0xD9FF11 fast loads |
| Deadlocked | 151: fifteen trampolines, two of them in two halves, the routine that plays a queued movie, and the table (136 words), fifteen sites | 0x15CD780 crash patches, 0x15CD781 fast loads, 0x15CD782 refill ammo |

Deadlocked's is the largest reply, 1244 bytes. The words are built from one assembly source per game in `src/games/switches/`, linked at the game's own addresses so that every branch is computed by the linker, and they change only when a trampoline does. None of them shares a byte with the savefile helper's patch, so a client writes both.

SWITCH_PATCH reads nothing out of the game and writes nothing into it, and in the emulator builds it answers whether or not the platform patches code (the PS3 module answers `UNSUPPORTED`, above). **NOT_INGAME** outside a game, **UNSUPPORTED** for a game with no switches, a title qwark does not know included.

**The presence rule.** On a platform that refuses code patches the switches count as present when every byte of every cave and every site word reads back from the game exactly as SWITCH_PATCH hands them out. The flag bytes are no part of it: the patch zeroes them at load and qwark writes them from then on. qwark looks once a session, from the first INGAME tick, a few reads a tick - every game is looked through within four ticks - and keeps the answer until the session ends; it looks again whenever a game comes back. So, as for the helper:

- A patch written while the game is running counts from the next restart, and not before.
- A patch from another build, or one that is only partly there, differs somewhere and reads as not present.
- A read that fails, because the emulator is not answering, leaves the answer unknown rather than "not present", and the next tick looks again.

**SessionInfo `flags` bit4 CODE_SWITCHES** is set while the answer is yes, and only then: never on a console, never outside INGAME, never before the look has answered.

**While bit4 is set:**

- FEATURE_SET works on every WRITES_CODE toggle of the game, on and off, and writes 1 or 0 into the toggle's flag byte instead of an instruction. qwark writes no code word for any of it. FEATURE_TRIGGER on a WRITES_CODE action is still refused; no game has one.
- `toggle_state` for those toggles is read back out of their flag bytes at 10 Hz, as a LIVE toggle's is (section 5.3.1), so a flag something else wrote shows in the checkbox. They are not LIVE in any other respect: DESCRIBE flags them WRITES_CODE as before, FEATURE_SET_AUTO works on them, and they go into the previous-session record. The one exception to the read-back is the fast-load flag a load has forced on: for the length of that load the checkbox reports the user's setting, not the force.
- Auto-apply at INGAME and PREVIOUS_REAPPLY turn them on through their flags. Both can come before the look has answered - the auto-apply always does - so a WRITES_CODE toggle asked for before then waits for the answer, and goes on through its flag when the answer is yes, or is skipped as it always was when it is no. Deadlocked's crash patches, which ship auto-flagged, are therefore on a few ticks into every session that has the switches.
- RaC1's, RaC2's, RaC3's and Deadlocked's planet loads and RaC1's, RaC2's and RaC3's set-aside loads force fast loads on through the fast-load flag, and the games' own watchers put the flag back when the load is over, exactly as a console forces and reverts the patch. For the length of a forced load the checkbox reads the user's setting, not the flag. Only a forced load's end writes anything (build 53, all four games): the flag outlives qwark, so a qwark-rpcs3 started against a game that is already running finds it as the last one left it, and a flag at 1 that this process did not force is the user's - the checkbox reads it as on, a force leaves it up, and no load's end puts it down. Until build 53 RaC1's, RaC2's and Deadlocked's watchers wrote the checkbox's setting at the end of every load, and the first load after such a restart switched fast loads off.

**While it is not** - no patch in the game, one from another build, the look not yet answered - everything is as section 3.2 describes: FEATURE_SET on a WRITES_CODE toggle answers UNSUPPORTED, the auto-apply skips those toggles, and the loads go without fast loads.

**Fingerprints.** Three of the sites are fingerprint sites: RaC2's fast-load call at 0xBEA8A0, RaC3's ammo store at 0x182A88 and Deadlocked's refill-ammo branch at 0x1D7D18. With the patch in, those hold the branch into the trampoline, and each game's fingerprint accepts that word as a third form beside the game's own instruction and qwark's patched one. RaC1's Infinite health switch hooks the instruction after its fingerprint, 0x7F55C, where a console patched 0x7F558 itself, and runs the patched addic again from the trampoline, so RaC1's fingerprint is untouched. Since build 53 no toggle uses that switch, and at 0 its trampoline runs the game's own instructions; it stays in the patch so that RaC1's words and stamp do not change and a patch an earlier build wrote still counts. A patch from a build whose trampolines sit elsewhere would put a branch at those three sites that the fingerprint does not know and the game would not leave BOOTING, so a build that moves one keeps accepting the old word. RaC2's moved in build 53 (below), and its fingerprint accepts the old branch, 0x4B648960, as a fourth form. RaC3's Fast loads switch (5.12.3) did not move the ammo switch's trampoline or its site word, so a game carrying the ammo-only patch of builds 50 to 53 is still RaC3; it reads as having switches from another build - bit2 set, bit4 clear, and a SWITCH_PATCH stamp that is not the 0x0C909623 that patch was written with - and the client writes the patch again. Until it does, both of RaC3's switch toggles are refused, Infinite ammo (Freeze ammo until build 53) included, and its planet loads go at the game's own pace. A patch from build 53's first fast loads, whose one fast-load site was the transition loop's test at 0x27B428 and whose trampoline was at 0x980F20, reads the same way; this build's fast-load trampolines start at 0x980F80, past that one, so a game that has both in at once still runs a whole trampoline from every site.

**RaC2's switches moved (build 53).** Builds 50 to 52 put RaC2's two trampolines at 0x233200 and 0x233220, the end of the function at 0x22A368, which was taken for dead code. It is not: planet Joba's setup (0x5BB78) points the per-planet hook at 0x1462DAC to 0x205018, which calls 0x22A368 every frame, so with the switches patched in Joba could run into the trampolines' bytes - the same routine RaC2's savefile helper was moved out of (section 5.12). They are at 0x1141400 and 0x1141420 now, at the far end of the span of dead library code whose near end holds the helper (0x113EC64..0x11414FC). The trampolines are the same instructions and the flag bytes did not move; the two site words branch to the new place, 0xB30C7C = 0x486107A4 and 0xBEA8A0 = 0x48556B60. A patch written by an earlier build differs in those rows and in its stamp, so it reads as not present and the client offers to write the patch again; because RaC2's fingerprint accepts the old branch at 0xBEA8A0 as well, a game still carrying that patch comes up as RaC2 with bit2 set and bit4 clear rather than waiting in BOOTING. RaC1's and RaC3's switches did not change by a byte, and their patches still count; Deadlocked's changed in the same build (below).

**RaC2's crash guards (build 53).** Two more trampolines follow the toggles' two, at 0x1141440 and 0x1141470, from two more sites, 0x86960 = 0x490BAAE0 and 0xBDF30C = 0x48562164; 96 bytes of the dead span are left. They are no toggle's switch and have no flag byte: once the patch is in they run on every frame. A transition to or from Dobbo Orbit, Damosel Orbit or the Jamming Array does not empty the list of the ship's part mobys, and an in-space story cutscene it plays moves the parts still in it, which are the last level's, against the transition's moby table; UpdateMobyGrids (0x868B0) then searched a grid cell for an index that cannot be there until it ran into unmapped memory. Only a planet change qwark makes gets there. One guard leaves the grid alone for a moby whose index does not fit a cell's 16-bit entry, the other skips a part that is not inside the moby table; neither changes anything for a moby the table holds. On a console qwark writes the same words at the same addresses into the game when the session enters RaC2, trampolines first, and never takes them out, as it never takes the savefile helper out; it writes nothing where either site holds anything but the game's own word or the guard's branch. Neither form shows anywhere: no DESCRIBE feature, no PATCH_LIST entry. The presence rule counts the guards' rows like any other, so a patch written before them, the toggles' rows alone, reads as not present and the client offers to write the patch again; the fast-load site's branch did not change, so the game still comes up as RaC2. Under RPCS3 without the patch the game has no guards, so PLANET_LOAD there refuses, with `UNSUPPORTED` and before anything is written, a trip the guards are for: one to or from Dobbo Orbit (22), Damosel Orbit (23) or the Jamming Array (26), or to Aranos 2 (14) while the story flag at 0x148174B is clear, with a story cutscene to play - one already queued in 0x15D4670, or one the transition would queue itself, which it decides by seven rules over the story bytes at 0x1482512..0x1482534 and the seen bits in the words at 0x1481994, 0x1481998, 0x1481A28 and 0x1481A2C (0xBEA270..0xBEA400). Every other trip goes, and so does every trip once the patch is in, and every trip on a console. From the NPEA00386 executable; not yet run on hardware.

**Deadlocked's fast-load switches (build 53).** A planet load is Transition_DoTransition (0x2A8AA0). A trip out of a level first autosaves (memcard_Save at 0x2A8AF4), then sets up the flight, which can queue a movie (the word at 0x10B7FE0: a movie the save's progress has earned - Dread points, badges and the like - or the first trip to Stygia, that the save has not seen yet), and then loops: the transition's update (the `bcctrl` at 0x2A8C50 through `g_fnTransitionUpdate`), whose answer is copied into the loop's exit word (0x2A8C5C), a frame (BeginFrame, the flight's draw and a flip, from 0x2A8CC8), and a step of the level load, held back while a movie is queued. The flip is the loop's only wait; a load the loop leaves unfinished, a second loop after it finishes with no wait at all. The flight's post-update plays a queued movie when the update has flown the first segment.

Builds 50 to 52 nopped the autosave's call and the update's, which left the load's "almost done" byte as the answer: a queued movie then never played, the load never started and the game sat on a black screen until fast loads went off. Build 53 first let the update run and or'd that byte into the copy (`or r27, r3, r26` at 0x2A8C5C, under RPCS3 a site there branching to the six-word cave at 0x667C00): the movie played, but after a segment of flight, and every load showed the flight until the load cut it off. Fast loads are now RaC2's: on a console six words and a routine of thirteen at 0x667DB8, in the dead span the switches use. 0x2A8AE8, 0x2A8AEC and 0x2A8AF4 make the autosave's arguments and call `FadeToBlack(2, 0)`, so the screen goes black at once and nothing is saved; 0x2A8C50 calls the routine in place of the update; 0x2A8C5C sets the exit word to 1, so the loop goes round once; and 0x2A8CC8 branches past that one frame. The routine plays a queued movie the way the flight's post-update plays it at the end of a segment: it deletes the segment's actors first (0x2A6994), then plays the movie and clears the word (0x2AA004); with none queued it returns at once. The order matters because 0x2AA004 empties the joint cache's table as the movie starts, and an actor deleted after that looks its entry up in the empty table, a search with no end (0x1DCC50): build 53 as it first shipped called 0x2AA004 straight, the transition's post deleted the first segment's actors after the movie, and the game hung there, skipped movie or watched. The second loop reads the level, and the screen stays black to its first frame but for the movie. Under RPCS3 they are three sites on the fast-load flag, one for each group of words that runs together, so a flag that changes between two of them never leaves half the patch running: 0x2A8AF4 = 0x483BF0F4 and 0x2A8C50 = 0x483BEFB0 branch to the six-word caves at 0x667BE8 and 0x667C00 they always had, whose patched forms are second halves at 0x667D80 and 0x667D90, after the bot-crash table, and 0x2A8CC8 = 0x483BF0D4 branches to a trampoline at 0x667D9C; the update's second half calls the console's routine, which the switches carry at the same address byte for byte, and answers 1 for the game's own copy at 0x2A8C5C, which is no longer a site. Every crash trampoline, refill ammo's and the fingerprint site 0x1D7D18 are unchanged, byte for byte, and so are the flag bytes. 0x2A8C50's word is the one builds 50 to 52 put there; their cave behind it is not this build's, so their patch, and build 53's two earlier ones, differ in their rows and in their stamps (0xEF1A07C3, 0xBB16AF57 and 0xEEC44D7C, now 0xE8661079), read as not present, and the client offers to write the patch again. Meanwhile Deadlocked's code toggles are refused and its loads go out without fast loads, so no older trampoline is ever switched on.

**What the client does with the reply** is what it does with SAVEFILE_PATCH's: it writes the rows and the bytes into RPCS3's per-title patch file `patches/<TITLE_ID>_patch.yml` as their own entry, enables it in `patch_config.yml`, and has the user restart the game. With bit2 set and bit4 clear it offers to write the patch; with bit4 set it offers the WRITES_CODE rows. A mod that writes a site or a cave collides with the switches the way two mods can collide with each other, and the client checks for that as it does between mods. Nothing in qwark touches RPCS3's files.

### 5.12.3 RaC3's fast loads are a code patch

RaC3's Fast loads toggle (feature 38) used to be two data words: 0x134EBD4 = 3 when a load started and 0x134EE70 = 01 01 a fifth of a second later. It is a code patch now, flagged `WRITES_CODE`, like RaC1's and RaC2's, and under RPCS3 a code switch (5.12.2). Nothing about the row changed but that flag: the same id, label, group and kind.

What the patch does, as RaC2's does. RaC2's patch nops the call that draws its load's transition, so nothing is drawn or flipped from the fade to black its load starts with until the new level's first frame, and with no vblank to wait for the transition runs as fast as the loader reads. RaC3's is two sites in a pass of its transition loop (0x27B360) and a cave, 0x9807F0..0x9808B0 on a console and 0x980F80..0x98104C for the switch. One site, 0x27B550, takes each pass past the frame: nothing is drawn and nothing flips. The other, 0x27B49C, ahead of the transition's update: the first pass fades the screen to black with the game's own fade (0x123988, six frames); then, unless a story scene is owed, the transition stands still while the loader reads the level - no clip moves, so none starts a sound, which UYA's clips do not stop when they end - and once it has read it (stage 2) the transition is over: the transition's sound banks are unloaded as the game does when its closing clip starts, and the game's own finish runs the rest of the load behind the black. Every trip is fast but multiplayer's, which the patch leaves alone: the clip transitions (the ship, the dropships, Aquatos's sub and pipe), the vid comics' cards, Holostar Clank's way back and the black screen a pair the game does not list gets, so no planet is left out. A trip that owes a story scene runs its transition undrawn until the first clip's end plays the scene, at once, and then stands still as any other; the scene draws itself. Build 53's first patch, at the loop's "is the transition over" test 0x27B428, ended a clip transition when the level was read but showed its first clip until then, and left the cards alone.

The old words hung the game on a black screen on a trip that owes a story scene - the first trip out of Aquatos once its sewers have been visited, and the first trip into Aquatos, which qwark and the old client never armed: they skipped the clip count the scene waits for, so the loader never ran. No planet is left out any more, and PLANET_LOAD forces fast loads on for Aquatos too.

PLANET_LOAD forces them on before the request and puts the checkbox's setting back when the load is over, which for RaC3 is the load request word (0xEE9310) down again with the game out of mode 6, its space mode, at 0xEE9334, once the load has been seen to run - the word up with the mode at 6 - since the force: the galactic map's way of leaving takes the word down while the game is still paused, and raises it again later for the transition. While forced, `toggle_state` reports the checkbox, not the force (5.12.2). UYA's LOAD_ASIDE action (32) forces them the same way before its request, as RaC2's does, and puts the checkbox back at the end of that load; a load the action refuses forces nothing, a request that fails as it goes out takes its force back, and a load the helper drops on the game's frame puts them back on the next tick. Only a forced load's end writes anything: under RPCS3 a flag at 1 that qwark did not force is the user's, a qwark started against a running game included, and stays up. A library RESTORE (5.13) and the other savefile paths do not force them: to the game they are ordinary planet loads, fast while the checkbox is on.

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

All five answer **UNSUPPORTED** where the platform cannot patch code and **NOT_INGAME** outside INGAME, for the same reason section 5.12 does: the library exists to feed the helper, and without a helper there is nothing to feed. Since revision 1.15 an RPCS3 patch that put the helper in the game at load is a helper like any other, and all five work under RPCS3 once it is there (section 5.12.1). The title id in the path is always the running game's, so a client never names one.

**Names.** A category or a file name is a NUL-padded 32-byte field. qwark refuses one that is empty, starts with a dot, holds `/`, `\` or `:`, or has a byte outside printable ASCII, and it refuses a file name that does not end in `.sav`, because the listing reports nothing else and a file stored under another name would be one nobody could ever see again. Everything else is taken as given.

**SAVEFILE_LIST** reports only `<name>.sav` files: the sidecars are qwark's own bookkeeping and never appear. `size` is the file's size and `crc32` is the CRC of its contents, which is the ordinary reflected CRC-32 that zlib, PNG and `qwark.sum` all use, so a client compares it against its own mirror to decide whether the two copies agree.

**SAVEFILE_STORE** raises the set-aside request, waits out the settle window of section 5.12 (so the copy inside the game is over before a byte of it is read), then copies the aside buffer into the file and writes the sidecar. **SAVEFILE_RESTORE** is the reverse: it copies the whole file into the buffer and only then raises the load request, so the game never sees a half-filled buffer. The file must be exactly `size` bytes, and a file that is not is refused with `BAD_ARG` and error 3 without anything being raised at the game; a file that is not there is `NOT_FOUND` with error 1, again with nothing raised.

A store that stops part way through leaves **nothing** behind: the half-written file is unlinked, because a file of the right name and the wrong length is what a user would take for their save. The copy it was replacing is gone either way, since opening the file for writing truncated it.

**One at a time.** There is one aside buffer, so a STORE or RESTORE while another is in flight, or while the game has a set-aside or load of its own outstanding, is answered `BUSY` and error 5. Since build 43 a STORE or RESTORE is also `BUSY`, with error 6, when the running game is not somewhere a save can be taken or put back (section 5.12); it is refused before either op opens or creates a file, and a RESTORE whose game stops being idle during the copy stops with error 6 rather than raising a load at it. Since revision 1.20 a STORE is also `BUSY` with error 7 when the game has no save to set aside (RaC1, RaC2 and RaC3, section 5.12), and a RESTORE whose file is not a save of the running game stops with error 7 once it is copied, raising nothing - in all four games for a file whose first word is 0 (build 53), which the helpers would otherwise take for an empty buffer and load the game's own save in place of. While a STORE or a RESTORE runs, the set-aside and load actions are `BUSY` with error 5 as well (build 53). SAVEFILE_CATEGORIES, SAVEFILE_LIST and SAVEFILE_CATEGORY answer `BUSY` too while a transfer runs, because a listing taken half way through a write would report the size and the sum of half a save.

**The transfer, and how long it takes.** The copy runs on qwark's tick thread as a chunked state machine: 8 KB a chunk, sixteen chunks a tick, so 128 KB per tick of the 120 Hz loop. A 2 MB save is sixteen ticks, an eighth of a second; RaC1's 704 KB save is six. (Build 35 made the chunk 16 KB where it had been 64 KB and put eight of them in a tick where there had been two; build 37 made it 8 KB and sixteen of them. The bytes per tick and the number of ticks are the same all three times, out of a buffer an eighth the size it started at.) Telemetry, freezes, watches and every other request keep flowing throughout, which is the whole reason it is a state machine rather than a loop. On top of the copy, a STORE waits the settle window before it starts (a quarter of a second) and a RESTORE waits one after it (the same), so end to end either is well under a second for the largest save.

Poll SAVEFILE_INFO: `pending` bit2 is up from the moment the op is accepted until the copy **and** its request have both finished, `done` and `total` are the progress, and `error` says how the last one ended.

**Deletes and renames** go through FILE_DELETE and FILE_RENAME with the whole path, which the client builds from the layout above. Two rules follow from the sidecar:

- Deleting a save means deleting `<name>.sav` **and** `<name>.sav.sum`; `NOT_FOUND` on the second is not a failure, since a file the client uploaded may not have one yet.
- Renaming means renaming both, for the same reason.

Deleting a category (`op` 1) removes the folder. It sweeps up any `.sum` files left in it first, so a client that forgot a sidecar cannot end up with a category it is unable to remove, but a folder that still holds a `.sav` is refused: `IO_ERROR`, and the category stays. A category that is not there is `NOT_FOUND`.

**What a client does.**

- *Save:* SAVEFILE_STORE, poll INFO to completion showing `done`/`total`, then, if it mirrors, read the file down with FILE_OPEN/FILE_READ in the background.
- *Load:* if the console has the file and its CRC matches the PC's copy, SAVEFILE_RESTORE. If it is only on the PC, or the two differ, upload it once with FILE_OPEN/FILE_WRITE/FILE_CLOSE and then SAVEFILE_RESTORE. Poll INFO either way.

## 5.14 Manip categories (0x00Dx), revision 1.21

The PS3 ports keep many RAM globals across files and planets that the PS2 originals reset, and speedrunners call the ones that matter **manips**. In a challenge-mode (NG+) category a runner may set them before a run, as long as the values are reachable in game; in a new-game (any%-style) category they must be what a freshly booted game has. qwark owns all of it: the groups, their writes, the file-load trigger, the challenge-mode check and the options. A client renders what MANIP_LIST describes and sends selections; it holds no address and no group table.

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x00D0 | MANIP_LIST | none | below |
| 0x00D1 | MANIP_SELECT | `u8 group` (0 = None) | none. `BAD_ARG` for an id the game does not have. Selecting the group already selected does nothing. Since round 3 of revision 1.22 a selection writes nothing: the group goes in at the next start of a run, or by MANIP_APPLY |
| 0x00D2 | MANIP_OPTIONS | `u8 auto_reset, u8 remember` (each 0 or 1) | none. `BAD_ARG` for anything else |
| 0x00D3 | MANIP_APPLY | none | none. Applies the selected group's setup and per-run writes now, after the challenge-mode check. A refusal is `BUSY`, with the reason in the event. With None selected it writes nothing and answers `OK` |

All four answer `UNSUPPORTED` for a game without groups and outside a game (at the XMB, while a game is booting, for a process qwark does not know); MANIP_APPLY answers `NOT_INGAME` while a game that has groups is booting or quitting. MANIP_LIST answers on the network thread, the other three on the tick thread.

**MANIP_LIST reply**, big-endian:

```
u8   selected       the selected group, 0 = None
u8   auto_reset     0 or 1
u8   remember       0 or 1
u8   ngroups        the groups below; None is not one of them
u16  event_seq      bumped on every new event; never reset while the module runs
u8   event_kind     the last event, below; 0 none
u8   event_group    the group it was about
group[ngroups]:
  u8   id           1..31, stable
  u8   flags        below
  u8   name_len
  char name[name_len]
  u8   cats_len
  char cats[cats_len]   the leaderboard categories, joined with '\n'
u8   nheaders
header[nheaders]:
  u8   name_len
  char name[name_len]
  u8   nfeatures
  u16  feature_id[nfeatures]
```

**Headers** are features that belong beside the manips without being part of a category: a client draws each header's features under its name in the manips area rather than in their DESCRIBE groups. Deadlocked has one, "Fixes" (0 Crash patches, 1 Fix reset softlocks); UYA has one, "Trophies" (21 Reset trophies, 39 its auto toggle); RaC1 and RaC2 have none. The features keep their ids, kinds and flags, and DESCRIBE lists them under a group of the same name. A header may name a feature DESCRIBE does not list - UYA's two, where the reset can never work (below) - and a client draws a header only around the features DESCRIBE has.

The groups are listed in the order a client lists them: challenge-mode groups, then new-game ones, then the extensions.

**Group flags**

| Bit | Name | Meaning |
|---|---|---|
| bit0 | NGPLUS | 1 a challenge-mode (NG+) group, 0 a new-game group |
| bit1 | EXTENSION | a category extension: shown with less emphasis, still selectable |
| bit2 | PER_RUN | the group has per-run writes |
| bit3 | NO_CHALLENGE_CHECK | the group skips the challenge-mode check |

**Event kinds.** `event_seq` lets a client toast each event once: it toasts when the number changes, never twice for one number. A client reads only the latest event, and one application of a group records exactly one, so a group's first application in a boot, which is also when the console says so, is kind 1 rather than 2. That is the first application of any kind, a setup or the per-run writes alone: until build 53 it took a setup, and a group with per-run writes alone (all of RaC1's, RaC2's Any% and 100%) was never announced. A group with a setup is announced exactly as before, since its first application in a boot always carries the setup.

| Kind | Meaning |
|---|---|
| 0 | none (also after a game boot, which clears the kind and the group but not the sequence) |
| 1 | activated: the group's first application this boot, setup or per-run. The console shows `<group> manips activated` |
| 2 | applied again: the per-run writes on a later file load, or a setup put in again after the group was announced this boot (Apply now, or the group selected again) |
| 3 | refused: an NG+ group on a file that is not in challenge mode |
| 4 | refused: a new-game group on a challenge-mode file |
| 5 | refused: Apply now with no save file in play (the title screen or the menus) |

Kinds 3 to 5 also show a console notification, because the PC may not be connected.

**Setup and per-run.** A group has two lists of writes, each applied in order:

- **Setup** is what the port never resets until the game reboots. It is applied once per game boot: at the first compatible file load after the boot or after the selection, and by Apply now. Selecting another group, or the same one again after another, makes it due again. A selection itself writes nothing, checks nothing and records no event (revision 1.22, round 3: build 53 put the setup in at once when the group was selected with a compatible file in play). A game boot is a new game process; quitting to the XMB and starting the game again is one.
- **Per-run** writes are applied on every file load while auto-reset is on, and by Apply now.

A **file load** is the start of a run: a save loaded or a new game started, from the title screen or from inside the game, including a load through the savefile helper (section 5.12) and a challenge-mode start. Not a planet change, a death or a respawn. Every run starts on its game's first planet and none comes back to it, so RaC1, RaC2 and UYA take the end of a planet load whose planet is the first one; Deadlocked goes back to DreadZone all the time and takes the tutorial flag instead, which a new game and a challenge-mode start reset. The first tick after qwark attaches to a running game never counts as one.

| Game | File load |
|---|---|
| RaC1 | the end of a planet load into Veldin (planet 0): the load request word seen set, then clear, the front end closed and the game state out of the transition |
| RaC2 | the load screen's counter reaching 2 on a load into Aranos (planet 0), the moment the old auto-resets used |
| UYA | the end of a planet load into Veldin (planet 1): a load seen under way to Veldin (the request word set, destination 1, game mode 6), then the mode leaving 6 on Veldin and the level running a frame |
| Deadlocked | the tutorial flags' low byte (0xB1F46F) dropping to 0, the edge the Fix reset softlocks watcher and the autosplitter's run start use, acted on the tick after, and only if the byte still reads 0 then (build 53: memcard_ResetGame, which zeroes it, is also called from the memory card screens): a new game or a challenge-mode start. A save keeps its own value of the byte, so loading one counts only when that takes the byte from non-zero to 0 |

The game files in `src/games/` give the evidence for each.

**The challenge-mode check, both ways.** Nothing is written when it fails, and the refusal is an event. An NG+ group writes only to a file whose challenge counter is 1 or more; a new-game group only to one whose counter is 0. RaC2's NG+ No Specials runs on an overflowed counter and skips the check (bit3). A group with nothing due on a file load is not checked and raises nothing.

| Game | Challenge counter |
|---|---|
| RaC1 | 0x96C9FC u32 |
| RaC2 | 0x1329AA2 u8 |
| UYA | 0xC1E50E u8 |
| Deadlocked | 0x9C330E u8 |

**Options**, per game in qwark's own `config.txt`, so they work with no PC connected. The keys are per game rather than per title id, so the disc trilogy keeps three sets:

| Key | Default | Meaning |
|---|---|---|
| `manip.<game>.auto_reset` | 1 | per-run writes on every file load |
| `manip.<game>.remember` | 0 | the category survives a game boot |
| `manip.<game>.category` | 0 | the selected group, written on every MANIP_SELECT |

The console menu (section 9.1, revision 1.22 round 5) shows the two options as one row, **Auto-reset**, Off, On or On boot: Off is `auto_reset` 0 and `remember` 0, On is 1 and 0, On boot 1 and 1 (the category put in on every file load and back after a game boot). `remember` 1 with `auto_reset` 0, which only MANIP_OPTIONS sets, reads Off there, since nothing is put in on a file load then; the menu's next step from it writes one of its three. The wire is unchanged: MANIP_OPTIONS still takes the two bytes, MANIP_LIST still reports them apart, and a client keeps its two boxes.

`<game>` is `rac1`, `rac2`, `rac3` or `rac4`. With `remember` 0 a new game process starts at None; with 1 the category is restored. Turning `remember` on also writes the current selection, so what comes back is what was selected.

**The groups.** Their writes, in order and big-endian, are in each game's file under `src/games/`; a client never needs them.

| Game | id | Name | Flags | Categories |
|---|---|---|---|---|
| RaC1 | 1 | NG+ (poki skip) | NGPLUS, PER_RUN | NG+ |
| RaC1 | 2 | NG+ (no poki skip) | NGPLUS, PER_RUN | NG+ |
| RaC1 | 3 | All Missions | PER_RUN | All Missions |
| RaC1 | 4 | All Checkmarks | PER_RUN | All Checkmarks |
| RaC1 | 5 | NG+ GBSP | NGPLUS, EXTENSION, PER_RUN | NG+ GBSP |
| RaC2 | 1 | NG+ no IMG | NGPLUS, PER_RUN | NG+ no IMG |
| RaC2 | 2 | NG+ | NGPLUS, PER_RUN | NG+ |
| RaC2 | 3 | Any% | PER_RUN | Any%, All Platinum Bolts |
| RaC2 | 4 | 100% | PER_RUN | Max%, 100% |
| RaC2 | 5 | NG+ All Missions | NGPLUS, EXTENSION, PER_RUN | NG+ All Missions |
| RaC2 | 6 | NG+ No Specials | NGPLUS, EXTENSION, PER_RUN, NO_CHALLENGE_CHECK | NG+ No Specials |
| UYA | 1 | NG+ no QE | NGPLUS | NG+ no QE |
| UYA | 2 | NG+ | NGPLUS | NG+ |
| UYA | 3 | NG+ CC Early | NGPLUS, EXTENSION, PER_RUN | NG+ CC Early |
| Deadlocked | 1 | NG+ | NGPLUS | NG+ no LC, NG+ |

Deadlocked's "Fixes" header lists feature ids 0 (Crash patches) and 1 (Fix reset softlocks). They are ordinary toggles, with the same ids, behaviour and auto flags as before, described under DESCRIBE's new group "Fixes".

UYA's "Trophies" header lists feature ids 21 (Reset trophies, an ACTION) and 39 (Reset trophies on Veldin load, a TOGGLE, new in this revision), described under DESCRIBE's new group "Trophies". UYA runs use trophy unlocks to lengthen in-game loads: a trophy the console has not recorded before pops up, and the console's work to record and show it holds the game's next load back. So the trophies a run relies on have to be won afresh in every run.

**What the reset does** (module build 53). Two halves, interleaved: the helper destroys the game's trophy context, qwark deletes the folder, and the helper registers the game again.

1. **qwark deletes the console's data for the game's trophy set**, `/dev_hdd0/home/<user>/trophy/<NP communication id>`, `NPWR02347_00` for NPEA00387. The PS3 keeps a user's unlocks there (TROPUSR.DAT among the set's files), and while that folder records a trophy as won, the game's unlock of it is answered SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED and nothing shows, however often the game re-registers. **This deletes the console's record of every UYA trophy the signed-in user has won, the platinum included.** A modded console has ways to restore them, a copy of the folder for one, and a user signed in to PSN may have them merged back by the trophy server when the game next registers.
2. **The savefile helper resets the game's side**: it marks every trophy won in the game's own list of what is won and destroys the game's trophy context; only then does qwark delete the folder (the helper's thread says so with 6 in the request byte, and qwark answers 7 when the folder is done with); then the helper waits a second and registers the game again, as the game's boot does on a console with no data for the set, and marks 10 (the RY3NO), 25 (the Metropolis blimp) and 32 (the snowman skin) unwon. The registration installs the set afresh, nothing won. The helper makes the registration's calls itself, the ones the game's register thread makes (sceNpTrophyCreateContext, CreateHandle, GetRequiredDiskSpace, RegisterContext), leaving out the game's boot check (cellGameBootCheck and cellGameContentPermit) that the register thread repeats on every registration, and puts the game's "registered" byte up itself the moment RegisterContext succeeds; one that fails is made again, a destroy and a second's wait first, up to three in all. On every console log so far the first registration after the delete fails a second in (RegisterContext answers 0x80022D1E) and the second installs the set in about ten seconds. The game's own reader of the game and trophy info, which its register thread runs next, is left out too: after a fresh install it took about thirty-five seconds, a second a call, and with the cache fill a nop it writes nothing the reset keeps. So the trophies are back about thirteen seconds after the request, which the reset's status readout then says (below; build 53 showed "Trophies reset: ready to win (N s)" on the console's screen until revision 1.21); until then the game awards nothing, and a trophy earned in that time is lost. It takes the request on the game's main thread, for a few microseconds of one frame, and makes the calls on a thread of its own; **the game goes on running** while it does, a few seconds, most of them the console installing the set. Every reset logs each step as it happens, with how long the reset has run, and at the end every call's answer in hex (`trophies: registration N of M: ...`). A trophy won meanwhile is not awarded, as one won before the game has registered at boot is not. Build 53 as first tested on hardware made the calls on the game's main thread with a second's sleep before each step, as the mod did, and the game stood still for three seconds. Run on a thread, with qwark deleting the folder first under the game's live context as the old trainer did and the game's register thread doing the registration, the console's log showed the first registration after the delete fail about a second in on every reset, and the next one still inside its call forty-five seconds on. Before it asks, qwark writes a nop over the one instruction that fills that list from the console as the game registers (0x985318, in the game's trophy register thread), so the registration leaves the list as the reset set it: those three are the only trophies the game will try to award in the run that follows, and it does try.

That is the old trainer's Reset Trophies button with its rc3-save 1.1 mod loaded, in what it does: the button deleted the same folder over Ratchetron, first, and wrote 1 to the word at 0xD9E020, and the mod's per-frame hook did the second half, nop included. qwark no longer writes 0xD9E020. Its request is the helper's fourth byte, `api_trophy` at 0xD9FF1C, and the second half lives in two more caves of RaC3's helper, 0x9808C0..0x980AC0 and 0x980308..0x9807F0 (section 5.12).

**How the folder is found.** It is never a fixed path. Its name is the communication id the running game registers its trophies under, read out of the game and checked for the shape `NPWR` and five digits, with the set number after an underscore; its parent is the home folder of the user the XMB has signed in, the number USER_ID reports, as eight digits; and it counts as the game's only if its TROPCONF.SFM or TROP.SFM names that same id in its `<npcommid>` and it holds no folder of its own. It is found and checked when the reset is asked for, and anything less refuses the whole reset with `NOT_FOUND` before the game is asked anything or a file is touched: no id in the game yet, an id of another shape, no user, no such home or `trophy` folder, a folder whose configuration names another set, or one holding a folder. A folder that is not there at all is a set with nothing to delete, and the reset goes on with the game's half. An empty folder of that name is a set with nothing in it, and only the folder goes. Only the files directly in that one folder are deleted, eight a tick once the game's context is down, and then the folder; nothing is walked into, and every step goes to the log (`qwark.log`, with `log = 1`). The two files that say whose set it is, TROPCONF.SFM and TROP.SFM, are deleted last, and the first file that will not go stops the delete there: the folder keeps what identifies it, and the next reset finds it again and finishes. The game's context is down by then, so the helper registers the game again all the same, over what is left. The console shows a notification when that happens, since the trigger has answered by then.

**Answers.** FEATURE_TRIGGER 21 answers `OK` once the reset is under way: the request and the delete run on qwark's tick, and the trigger does not wait for the game. It answers `BUSY` while a reset is already running (qwark's half, or the helper's thread from the last one), while one of the game's own trophy threads runs, or before the game has set its trophies up, all of which are checked before the folder is looked for; `NOT_FOUND` as above; and `UNSUPPORTED` when the instruction at 0x985318 is neither the game's own nor the nop (not the executable this was written for), when the savefile helper is switched off (`savefile_helper = 0`) or cannot be installed, and under RPCS3. None of these refusals deletes a file. On the game's frame the helper refuses a request it finds while a trophy thread of the game's or a reset of its own runs, for the toggle's request outside a planet load, or when it cannot start its thread; qwark logs that and the reset ends there, with the folder untouched. A reset none of whose registrations took is logged and shown as a console notification, and so is one the console has not answered in ten minutes, which qwark then stops watching (the helper's busy byte still holds the next one off until its thread ends). A reset that ends well shows nothing on the console's screen: the status readout says it. Whatever the request byte is left holding when qwark is idle and the helper's thread is not running, an answer nobody read, is put back to 0 by the next reset rather than holding it off. The nop at 0x985318 is never put back and is not one of PATCH_LIST's patches: only a registration runs that store, and only the game's boot and this reset start one, so it changes nothing else, and the next boot loads the executable's own word.

**The toggle.** Toggle 39 is qwark's own state: off after every boot unless its auto flag (FEATURE_SET_AUTO, config.txt as for any toggle, under the key its label makes) brings it back, and independent of the selected category, None included. While it is on, qwark resets once per run, from the first tick it sees a load into Veldin under way (the load request word set, destination 1, game mode 6), and asks again on the following ticks of that load for as long as the game's trophy threads keep it `BUSY`. The game's half is asked for only inside that load, and the helper refuses it outside one, so the reset always starts inside the load into Veldin: after the last run's trophies, before this run can win one, and before the run's timer starts, which is when that load ends (the autosplit START, section 8, is Veldin with the mode going from 6 to 0). The load's opening transition, which keeps the mode at 6, calls the helper every frame, and qwark asks on the tick it sees the load, so the helper takes the request within a frame or two. The helper's thread, and the delete in it, may go on for a few seconds after the load, with the game running; nothing it leaves to be won is on Veldin. A load that ends before the helper has taken the request, which a short load with fast loads on could, leaves the whole reset, folder and all, to the next load into Veldin. The toggle will not turn on under RPCS3 or with `savefile_helper = 0`.

**The status** (revision 1.21). Reset trophies' row names readout 12, "Trophy reset", as its status readout (section 5.3), and SessionInfo carries it in every block, so a client can show under the button what the reset is doing and how the last one went without asking. qwark makes the word on every tick from its own state; it reads nothing from the game for it.

```
bits 28..31  state   0 NONE: no reset this boot. 1 RUNNING. 2 READY: the game has its trophies
                     back and they can be won. 3 FAILED
bits 24..27  detail  RUNNING: the step, 1 STARTING (the game is asked, and lets go of its trophies),
                     2 DELETING (qwark deletes the trophy data), 3 REGISTERING (the game registers
                     its trophies again). FAILED: the reason, below. NONE and READY: 0
bits 20..23  arg     RUNNING in REGISTERING: the registrations the game has started, 0..3 (0 in the
                     second's wait before the first; the first after the delete fails on every
                     console log so far, so 2 is the usual one). FAILED with reason 7: the status
                     the reset was refused with. Otherwise 0
bits 8..19   secs    RUNNING: seconds since the request went out to the game. READY and FAILED: the
                     seconds the reset ran, rounded, 0 for a refusal. 4095 at most
bits 0..7    seq     the resets that have ended, READY or FAILED, since the module started, modulo
                     256. A boot does not put it back, so a client can tell a new result from the
                     one it saw before even when the words are the same
```

| Reason | Name | Meaning |
|---|---|---|
| 1 | DELETE_STOPPED | a trophy file would not go; the game registered again over what was left, so the trophies won before are still won (the console showed "Trophy reset stopped" as it happened) |
| 2 | UNREGISTERED | every registration failed: the game has no trophies to award until a reset or a restart |
| 3 | NO_ANSWER | the console had not answered the game in ten minutes, and qwark stopped watching |
| 4 | LOAD_ENDED | the toggle's reset: the load into Veldin ended before the game took the request, and nothing was done; the next load into Veldin asks again |
| 5 | GAME_REFUSED | the helper refused the request on the game's frame (its trophies not set up, a trophy thread of its own running, no thread to be had) |
| 6 | NOT_TAKEN | the game did not pick the request up in five seconds, and it was withdrawn |
| 7 | REFUSED | qwark refused the reset before anything was done, with the status FEATURE_TRIGGER answered, or would have, in `arg`: `NOT_FOUND` when the folder was not found as UYA's (above), otherwise `UNSUPPORTED`, `IO_ERROR` or what the savefile helper's install answered. A `BUSY` is not a result and leaves the word as it was: a reset is under way, or the game is not ready and the toggle asks again on the next tick |
| 8 | STOPPED | the game changed under the reset: 0x985318 held neither word when qwark came to ask, or something else wrote the request byte, or it could not be read for ten minutes |

READY means the reset ended with the trophies back and the delete whole. A refusal counts as a result because the toggle's has nobody else to tell: without it a READY left from the last run would stand for this one. A boot, a reboot of the same title included, puts the state back to NONE and leaves `seq`; under RPCS3 and with `savefile_helper = 0` the word stays 0, and DESCRIBE leaves the readout's name out with the row (UYA then describes twelve readouts), so a client shows nothing. A client shows nothing for NONE either, and goes by the row rather than by the readout's index or name: an older module names no status readout, and the client then has nothing to show.

**Under RPCS3** the reset cannot work, and since build 53 DESCRIBE leaves 21 and 39 out there, as it does on a console with `savefile_helper = 0`, so a client has nothing to draw for them, the Trophies header and group included, and since revision 1.21 their status readout's name; FEATURE_TRIGGER 21 and FEATURE_SET 39 answer `NOT_FOUND`, as for any id DESCRIBE does not list. Until then both rows were described, and answered `UNSUPPORTED` (section 3.2). The emulator keeps a trophy set's data in its own `dev_hdd0`, which `qwark-rpcs3` does not reach (its file ops are rooted in a folder of its own, and nothing in qwark touches RPCS3's files), and the game's half alone would leave every won trophy won and change nothing a run can see. RaC3's SAVEFILE_PATCH carries the reset's caves with the rest of the helper, where nothing ever asks for them.

RaC2's NG+ No Specials writes M, the NG+ per-run list, without its first write, the shortcut cursor: the shortcuts live in the specials menu, which the category never uses.

**Retired features.** The old manip buttons and toggles are gone from DESCRIBE. Their ids stay spoken for and are never reused, config.txt auto flags for them are ignored, and the previous-session record never offers them.

| Game | Retired ids |
|---|---|
| RaC1 | 7 Drek skip, 8 Drek cutscene, 10 Reset shooting skill points, 11 Setup shooting skill points, 16 Any% all-missions reset |
| RaC2 | 15 Reset any% manips, 16 Reset NG+ manips/menus, 17 NG+ setup, 18 NG+ No IMG setup, 19 NG+ All Missions setup, 20 Maktar slots, 21 Auto-reset (any%), 22 Auto-reset (NG+) |
| UYA | 16 Setup NG+ manips, 18 CC early setup, 19 Untune bosses, 20 Reset dropship health |
| Deadlocked | 12 Act tune bosses |

RaC2's charge buffer is no longer written on every load into Aranos: it is part of the NG+ per-run writes. Turning infinite ammo off on a load into Aranos is not a manip and stays. The per-death resets (RaC2 13 and 14) are not manips and stay.

## 6. Pad mask layout

OG layout, shared by all four games: l2 0x1, r2 0x2, l1 0x4, r1 0x8, triangle 0x10, circle 0x20, cross 0x40, square 0x80, select 0x100, l3 0x200, r3 0x400, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000.

## 7. Connection lifecycle from the client side

1. Connect, send HELLO, read SessionInfo.
2. SUBSCRIBE with a bound UDP port, then DESCRIBE, AUTOSPLIT_DESCRIBE, PLANET_LIST, WATCH_LIST, FREEZE_LIST, PATCH_LIST, MOD_LIST, COMBO_LIST, SAVEFILE_INFO, MANIP_LIST (revision 1.21; `UNSUPPORTED` for a game without manip groups).
3. Render from telemetry. When `game` changes, repeat step 2 from DESCRIBE. When `PREVIOUS_PENDING` appears, PREVIOUS_LIST and prompt.
4. On any socket error: drop everything, reconnect with backoff, start again at step 1. There is no client state to restore.

## 8. Autosplitting (revision 1.5)

**qwark detects, the client decides.** qwark watches game memory on the tick thread and emits run events. It keeps **no timer**, applies **no user setting**, and knows nothing about LiveSplit: every split candidate goes out unconditionally with a reason code, and the PC decides which ones to act on. Anything the old ASL scripts gated on a *setting* is still emitted here; only what they gated on *game state* is a condition qwark evaluates.

The one thing that is **not** left to taste is timing. The old scripts adjusted game time at fixed points — a load screen that stopped counting after 7.56 s, a transition worth nine frames, a quit that always cost 14.8 s — and a run that does not reproduce those adjustments to the microsecond is a different time. Those adjustments travel as the `FLAT` and `NORMALISE` flags on an `EventDesc` row, and **a client applies them whenever the autosplitter is on**, whatever its split checkboxes say (section 8.3).

Two things a client must *not* do with a split follow from the scripts rather than from the wire, because both used to lose a runner a segment. A script's `split` block ran once per update and returned at most one `true`, so **a client takes at most one split per moment the console stamped**, however many candidates that moment carried; and a script checked `reset` before `split` and never ran `split` at all on the update a run began, so **a split the console stamped in the same moment as a START or a RESET is not a split**. Neither rule waits for anything: both are decided on the `time_ms` the events already carry, so nothing is held back and no split reaches LiveSplit a millisecond later than it would have.

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
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet; never Veldin (0), at either end |
| 2 | SPLIT | Veldin | yes | — | 0. Fires once per run; a latch stops the double split |
| 3 | SPLIT | Drek button | yes | — | 0. The player state reaching 34 within 1.7 of one of the four buttons |
| 8 | LOAD_START | Loading screen | yes | NORMALISE 7 560 000 | the loading-screen id it moved to; 4 on the LOAD_END |

Codes 4 to 7 (gold bolt, skill point, item, infobot) are reserved and neither advertised nor emitted. They counted words that the `gb_sp_as_helper` mod kept, and qwark used to write that mod into the game on every launch; written that early it blanked the screen, so it is gone, and with it the four splits. A client only ever sees the rows DESCRIBE returns, so a saved preference for one of those codes enables nothing.

Code 8 is the script's `isLoading` block. It started a 7.56 s timer when the loading-screen id left 4 and only called the game "loading" once that timer had run out, so the first 7.56 s of every load counted towards game time and the rest did not. qwark emits LOAD_START when the id leaves 4 and LOAD_END when it comes back, and the client subtracts `max(0, duration − 7.56 s)`.

**RaC2** — START/RESET when the player state goes 0 to 98 while on Aranos (planet 0).

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the planet just entered; never Aranos (0) and never the Insomniac Museum (21) |
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

**Why Aranos is not a planet entered.** A new game loads Aranos, and the planet byte gets there first: it reads 0 for as long as the load takes, and only then does the player state flip to 98 and the START/RESET above go out. The script split on that, so the run it was about to throw away ended on a segment nobody ran, and LiveSplit kept the gold. Arriving on Aranos is the beginning of a run in this game and never anything else, so it is not a planet change. **In every game the planet the run starts on is excluded from code 1 for this reason**: RaC1's script did it already, by testing `destinationPlanet != 0` where its Veldin is planet 0; RaC3's says in a comment that it does and does not, and qwark's does.

**RaC3** — START/RESET when the game state goes 6 to 0 while on Veldin (planet 1).

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet; never Veldin (1) |
| 5 | SPLIT | Biobliterator defeated | yes | — | 0. Armed by an odd Neffyrious phase at full health |
| 2 | SPLIT | LDF entered | no | — | 0 |
| 4 | SPLIT | Koros bolt 2 | no | — | 0. The second titanium bolt of the run on Koros |
| 3 | SPLIT | Tyhrraguise obtained | no | — | 0 |
| 6 | LOAD_START | Long load | yes | FLAT 1 000 000 | the destination planet |

Code 6 is the script's long load: a second off game time when the loading-screen id becomes 1, **unless** the destination planet or the origin planet is one of 20, 26, 27, 28 or 29, whose loading screens are different ones that only look long. qwark tracks the origin the way the script did — the planet you left when a planet change is under way, the planet you are standing on when one is not — and emits the LOAD_START only when both ends pass. LOAD_END follows when the id leaves 1, and only for a load that actually opened.

The script's split route and its strict-order mode are the client's business: qwark emits code 1 for every planet change and the PC filters.

**Why Veldin is not a planet entered.** The script's comment on the planet-change line reads "we are changing levels (but not reloading, and not going to or from veldin)", but the two tests under it — `destinationPlanet != 0 && planet != 0` — are its RaC1 ancestor's, where Veldin is planet 0. In RaC3 it is planet 1 and neither test catches it, so the load that starts a run over split the run the game-state edge was about to reset a moment later. qwark excludes the destination only: leaving Veldin for Florana is the run's first real split, and RaC3 has no Veldin split of its own to carry it the way RaC1 does.

**Deadlocked** — START/RESET when a load starts for DreadZone (planet 1) with the tutorial flag clear.

| Code | Kind | Label | Default | Timing | `arg` |
|---|---|---|---|---|---|
| 1 | SPLIT | Planet entered | yes (route) | — | the destination planet |
| 2 | SPLIT | Vox defeated | yes | — | the destination planet, which is 0 by then |
| 3 | PAUSE | Quit to XMB | yes | NORMALISE 14 800 000 | 0 |

Deadlocked is the only game whose detection already ran on the console; codes 1 and 2 replace the old command stream's single "split" whose packet byte the LC script had to read as "0 means Vox". The script's AEC setting, which disables resetting, is the client's business: qwark still emits every RESET.

**Where Deadlocked's origin planet comes from (build 9).** Code 1 needs the planet the player is coming from as well as the one they are going to, and the game's own current-planet word at `0x009C3240` is saved with the file: after a quit to the XMB and a boot it still holds whatever planet the save was on, so the first load out of the main menu read as a planet change and split. qwark keeps its own origin instead. It is planet 0, the main menu, whenever a session is primed, and it becomes the destination of every load that starts after that — so a load back to the main menu puts it back to 0 on its own, and neither that load nor the load out of the menu after it is a split. The word itself is still read, for the Vox split and for the readout.

The origin is also what makes code 1 a planet *change* rather than a load. The SPRX watched the load request word for a plain 0-to-something edge, and one flight can raise it more than once: the second edge carried the same destination and split a second time, so one transition took two splits. By then the origin is already that destination, so **a load whose destination is the origin is not a planet change** and emits nothing.

Code 3 is the quit to the XMB, which some categories use as a strategy. PAUSE goes out when the game leaves the running state — the quit hook fires, or the process disappears underneath qwark. RESUME carries the same code 3, and the LC script's arithmetic (stop game time for the whole quit and reload, then add 14.8 s back on the way in) is NORMALISE 14 800 000: the quit costs exactly 14.8 s however slow the console was.

**Where Deadlocked's RESUME comes from.** qwark reaches INGAME as soon as the process is back and its fingerprint reads, which is several seconds before the game is playable — it sits behind a loading screen and a warning screen first — so resuming there would hand the runner free time the old autosplitter never gave away. qwark therefore installs the old SPRX's **loading hook** alongside its quit hook when Deadlocked enters INGAME: ten words of trampoline at `0x11904`, a `b +0x80` at `0x11884` that reaches it, and a byte at `0x1710000` the game sets to `0xFF` once it has the SCE logo up. Both hooks go in permanently and are never reverted, and both bytes are cleared on entry so a stale one cannot fire. RESUME is emitted on the first tick that byte reads `0xFF`, and the byte is cleared again for the next quit — exactly the gate the SPRX's `STATUS_INGAME_PENDING` used before it sent `CMD_UNPAUSE`.

## 9. The console menu (revision 1.22)

A menu on the TV that gives a console with no PC beside it what the client offers. qwark owns it: the pages, what is on them, what a row does, where the player is. A small **renderer** qwark writes into the game draws the current page with the game's own menu widgets and posts back what the player pressed, through a **mailbox** in game memory. Nothing of this crosses the network: a client sees what the menu changes the way it sees any change, in telemetry, and this section is for information.

Deadlocked (section 9.5), Up Your Arsenal (section 9.7), Going Commando (section 9.8) and Ratchet & Clank (section 9.9) have renderers in build 54, and only on a console. Under RPCS3 there is no menu at all (section 3.2).

### 9.1 What the menu shows

Pages come in two **types**, which the mailbox carries with each page (section 9.4):

- **MAIN**, a short list of links and buttons, drawn with the game's own list of links. Each row is one line, "label: value".
- **CONFIG**, a list of labels and controls in two columns, drawn with a two-column screen of the game's own (Deadlocked's Options > GAME SETTINGS, UYA's Options > AUDIO / VISUAL, RaC2's Special > Cheats, RaC1's Goodies > Cheats). Every game's CONFIG look scrolls, so a CONFIG page shows more rows at once; no game's MAIN look does (round 4), so a list that can be longer than a MAIN look holds is a CONFIG page (below).

A renderer says which it has; one with only MAIN is sent every page as MAIN.

The pages, for a game with a menu table (Deadlocked; `struct game_menu` in src/games/game.h):

| Page | Type | Rows |
|---|---|---|
| The root | MAIN | titled Trainer (round 3; it was RaCMAN): the game's sections (Game, Cheats), then Planets, Manips, Unlocks and Save files where the game has them, then a grey row with the module's build ("qwark build 54"), for the user's testing: the release builds it out (round 5, `MENU_BUILD_ROW` in src/core/menu.h) |
| Game | CONFIG | the quick block of the client's Game panel: the position slot, Save position and Load position, a blank row (round 5), then Set aside file and Load set-aside file (RaC1: and Force autosave). Round 5 left out Die, which the combos and the client have, and the game's values, which Cheats has now; round 4 had Die first and the values last (UYA: Bolts, Challenge mode, Health XP, Health, File time) |
| Cheats | CONFIG | the four cheats every game has, then the game's other cheats (UYA: the quick-select pause; RaC1: the Goodies menu; RaC2: Reset bosses on death, Reset platinum bolts on death); round 5: then Challenge mode as a switch (RaC2, UYA, Deadlocked), then the buttons: a Max button for each of the game's values (Max bolts in every game; RaC2: Max raritanium; Deadlocked: Max Dread points), then the game's other actions (Deadlocked: Unlock all planets). RaC2's Set respawn point and Store Swingshot are left out (round 5) |
| Planets | CONFIG (round 4; MAIN before) | the planets in the game's order, each a button that loads it once the menu has closed: every planet PLANET_LIST names, in its menu order (UYA's vid comics last), the placeholders left out. Deadlocked listed only the planets the save had unlocked until build 54's planet load learnt to unlock a locked one first (section 5.5). Where the renderer opens the game's own planet picker (mailbox version 3, below), the root's Planets row opens that instead, and this page is the fallback |
| Manips | CONFIG | the category (None or a group), Auto-reset (round 5: Off, On or On boot, MANIP_OPTIONS' auto-reset and remember as one row; Remember had a row of its own) and Apply now, as MANIP_SELECT, MANIP_OPTIONS and MANIP_APPLY have them (section 5.14); then, where MANIP_LIST's headers have features DESCRIBE has (Deadlocked's Fixes, UYA's Trophies), a blank row (round 5) and those features, an ACTION with a status readout followed by a grey row of that status in the game's words, labelled with the readout's name (UYA: Reset trophies, then "Trophy reset: Ready, 13 s", then Reset trophies on Veldin load). RaC1 and RaC2 have no headers, so no blank row |
| Unlocks | MAIN | UNLOCK_LIST's categories that have rows, each a link, then the game's actions on the whole table: on this page as buttons (Deadlocked: Max all weapon levels, Max all weapon ammo), or, where the game's menu table names a page for them (`unlock_actions_title`, round 3), one link to that page (UYA, RaC2 and RaC1: Collectables) |
| The Unlocks actions' page | CONFIG | round 3: the game's actions on the whole table as buttons, their labels in the label column (UYA's Collectables: Max and Reset all weapon levels, Unlock and Reset all skill points and all titanium bolts; RaC2's: Max all weapon levels and ammo, Unlock and Reset all platinum bolts, nanotech boosts and skill points; RaC1's: Unlock and Reset all gold bolts and all skill points) |
| An unlock category | CONFIG | each unlock a selector: LOCKED or UNLOCKED, or for a row with the game's level field the game's own values, its version shown as the game numbers it (Deadlocked LOCKED, V1, V10, V99; UYA LOCKED, V1 and the weapon's own last version, V8 or the R3YNO's V5; RaC2 Locked, V1 and the weapon's own last version, V4, or V2 for the Clank Zapper and the five RaC1 carry-overs; round 5, the user's call: UYA and RaC2 said UNLOCKED or Unlocked and MAX or Max); a gadget or an item, which has no level, LOCKED or UNLOCKED in the game's casing; RaC1 Locked, Unlocked and Gold on the gold flag, an item with no gold version Locked or Unlocked. A version between shows as itself ("V3") and steps to the next on the list; an owned item whose version cannot be read back (UYA's GameCube weapons) shows UNLOCKED, and Right on it writes its last version, which it goes on showing as UNLOCKED |
| Save files | CONFIG (round 4; MAIN before) | the console library's categories (section 5.13), each a link, its name the label |
| A save category | CONFIG (round 4; MAIN before) | its saves, each a button, its name the label and its value blank: SAVEFILE_RESTORE once the menu has closed, which leaves the save in the aside buffer, so Load set-aside file loads it again |

Cosmetics are left out, and so are mods and combos for now. A game with no menu table gets round 1's root of DESCRIBE groups, each a page of its features, before Planets and the rest.

**Round 5** (the user's hardware notes on round 4, build 54 as before). A game's menu table (`struct game_menu`, src/games/game.h) can list a VALUE feature as something other than a stepper (`struct menu_value`, `MENU_ITEM_VALUE(i)`): a **button** that writes one value, or a **switch**. Every game's values are listed so, and none is a stepper on its pages any more (round 1's pages of DESCRIBE groups still step them):

| Game | Switch | Buttons, and what they write |
|---|---|---|
| Ratchet & Clank | - | Max bolts 9,999,999: the game keeps no cap, and the HUD's bolt counter is registered with 9,999,999 as its limit wherever it shows (0x0C8444's last argument, from the game's update 0x1675C8, the vendor 0x16AB38 and seven more sites): seven digits |
| Going Commando | Challenge mode | Max bolts and Max raritanium 9,999,999 each: the game caps neither, and the HUD's counter prints with "%d" into a layout of one to seven characters (0xCC2678, the length less one held to 0..6) |
| Up Your Arsenal | Challenge mode | Max bolts 99,999,999: the game's own cap, which its update (0x286D08) puts on the bolts every frame (0x287C04) |
| Deadlocked | Challenge mode | Max bolts 99,999,999: the game's own cap (its update 0x2B1F30, from the frame's 0x1CE638 and three more, at 0x2B2400). Max Dread points 99,999,999: nothing caps them, and the game adds rewards to them and tests them signed against its unlocks (the highest 250,000), so the bolts' cap, well clear of a wrap negative |

A switch is On while its field reads above 0 and Off at 0; Off writes 0, On writes the game's first value, 1 (Challenge mode: a file not in challenge mode becomes its first run's, as the game's own challenge-mode start adds one, RaC2's 0xBBB798; a file on its seventh run reads On, and Off makes it 0). Cheats lists the toggles first, then the switch, then every button. A blank row, an INFO row that is not ENABLED with nothing in it, parts a page's parts: Game's position rows from its save-file rows, and Manips' category from the headers' features. The values left the Game page, which also left out Die and UYA's and RaC2's Health XP, Health and File time; RaC2's Cheats left out Set respawn point and Store Swingshot.

Round 4 moved Planets, Save files and a save category to CONFIG: no game's MAIN look scrolls (UYA's main pause holds nine rows, RaC1's panes and RaC2's bars seven, Deadlocked's list eight), and each of those lists can be longer. The MAIN pages left, the root and Unlocks, fit every game's MAIN look (seven rows and five at most). The Planets page stays built in one place (`page_items`' MENU_PAGE_PLANETS case), the native planet pickers' fallback.

**The game's own planet picker** (mailbox version 3, build 54; Deadlocked's Worlds so far, section 9.5). A renderer that can open its game's own planet picker says so in `caps` (bit4 PICKER), and qwark flags the root's Planets row PICKER (row flag bit6). The cross on that row opens the game's picker over the menu, the way the game's own pause opens it, with the game's sounds, its locks shown as the game shows them (but on what the picker loads anyway: Deadlocked's planets, since the pickers' round 2) and every planet qwark can load listed. Back from the picker returns to the menu's root, the cursor on Planets; Start or Select closes everything, as anywhere. Picking a planet closes the game's menu the way the picker's own Start closes it, and the renderer hands qwark the planet (`pick_planet`, then `pick_seq`, section 9.4); qwark loads it once the game plays again, exactly as a row of the Planets page loads it (PLANET_LOAD's code, the running game's selected flags, fast loads forced, a locked Deadlocked planet unlocked first). While the picker is up the menu's `hstate` reads 2, PICKER: the menu is not closed, so qwark keeps its stack and page, and the picker's going counts as the menu's close. Everything a renderer changes in the game for its picker acts only on a picker the menu opened: the game's own, from its own pause, is left exactly as it is. Where the renderer cannot open its picker there and then (Deadlocked: where the game's own pause greys its WORLDS row), the row posts ACTIVATE as any link does, and qwark opens the Planets page.

**The pickers' round 2** (still mailbox version 3 and build 54). A picker that cannot show every planet qwark loads can send the player to qwark's page of the others: the renderer posts PICKER_OTHERS (event kind 9) on the Planets row instead of ACTIVATE, and qwark opens that game's page of them, a CONFIG page of buttons that load as the Planets page's do (struct game_menu's `picker_others`; page id MENU_PAGE_PLANETS with argument 1). Back from it returns to the root, the cursor on Planets. A game that names no such page gets its whole Planets page. UYA's GALACTIC MAP has no vid comics; its Square, which says VID-COMICS in the Trainer's map, opens Vid-Comics, the base vid comics 1 to 5 (section 9.7).

The controls:

| Control | Row | What it does |
|---|---|---|
| Checkbox | TOGGLE: Off, On, and Auto for a toggle that has an auto flag (all but LIVE ones) | the cross and Right go to the next, Left to the one before, wrapping. Auto is on with its auto flag ticked, the client's "A" box; Off clears both. Through FEATURE_SET and FEATURE_SET_AUTO's own code. Round 5, the same control for a switch over a VALUE (Challenge mode: Off or On, as above, through FEATURE_SET) and for Manips' Auto-reset (Off, On, On boot, through MANIP_OPTIONS) |
| Selector | CHOICE: the current option | Left and Right go to the previous or next, wrapping; the cross goes to the next on a CONFIG page, as the game's Cheats screen does, and opens the options as a page of their own on a MAIN page |
| Button | ACTION | the cross runs it, and its value stays blank (round 5, the user's call: the game's own sound for the cross is the cue; round 4 showed "done", and "after closing" for a load). A refusal stands in place of its value for two seconds ("busy", "not in game", "not available", "refused", "failed", "not found", "full"), and in the page's hint as a sentence; so does a refused step of any other row. A button flagged CLOSES (a load: the set-aside file, a planet, a save) closes the menu: qwark asks the renderer to, and runs it once the game is back in ordinary play, or drops it five seconds on with a console notification. A Max button (round 5) writes its value through FEATURE_SET |
| Value | VALUE, a stepper (round 5: on round 1's pages only; no game's menu table lists one) | Left and Right step it; the cross changes the step, 1, 10, 100 up to 1,000,000 but never past the field's range. A renderer without Left and Right shows the number only. Round 3: a VALUE with bounds narrower than its 32-bit field (explicit ones, or an 8- or 16-bit field) is flagged RANGE and carries its value, its bounds and its step as numbers (section 9.4), and one whose range spans 1000 or fewer is flagged SLIDER as well, for a renderer that draws a native slider (Challenge mode's 0..255, not Bolts). Round 4: a SLIDER row's Left and Right move it a tenth of its range, rounded up, so ten presses cross it end to end (Challenge mode: 26, 0, 26 .. 234, 255), and the cross switches between that and a fine step of 1, which its value shows as "[1]" |
| Text | INFO | nothing. Not ENABLED, so every renderer's cursor passes over it, Up and Down alike (round 5: the blank row between a page's parts is one, drawn as an empty row in the grey of a row the cursor skips) |

Every change goes through the code the protocol's ops go through, so `toggle_state`, the auto flags, the manip options and the previous-session record (section 4.1) are exactly what they would be had a client made the change, and a connected client sees it in its next packet. A VALUE or ENUM row shows what the menu last wrote until the readout agrees, at most a quarter of a second.

qwark sends text as it is written ("Ghost Ratchet", "Off"); a game's renderer cases it the way that game cases its own menus (section 9.6).

**Navigation.** qwark keeps the stack of pages, eight deep. Select on a link opens its page; back goes up a page, the cursor on the row the page was opened from; back on the root leaves the menu, which the renderer does itself. Round 3: whenever the menu closes - Start or Select, the combo, back off the root, a load that closes it, the game closing its pause - the stack goes back to the root, and the next open, from the game's row or the combo, shows the root with the cursor on its first row. qwark resets it when the mailbox's `hstate` goes to closed, and posts the root at once, so it is there before the menu can open again; an opening it did not see the close of (the renderer's `frames` back near 0 with `hstate` open all along) goes back to the root too, a moment late. The values' steps and a load waiting for the close stay. (Round 2 opened the menu again on the page it was closed on.)

**Pages longer than a renderer takes at once** (round 4) are posted a window at a time, a window of as many rows as the renderer takes of that type (`max_rows`, `cfg_rows`: Deadlocked 16, UYA 24, RaC2 32, RaC1 24 on a CONFIG page), with no More and no Previous row and the page's own title: `pflags` says whether the page goes on above the window (MORE_UP) or below it (MORE_DOWN). When the cursor comes to the window's last row it may stand on and the page goes on below, the renderer posts SCROLL_DOWN with that row (SCROLL_UP for its first, the page going on above), and qwark posts the window that continues the page: going down, the cursor's row second from the window's top, going up second from its bottom, as far as the page's ends allow, so the cursor never lands on an edge the page goes on past; the window moves at least a row. PAGE_PREV and PAGE_NEXT (L1 and R1) move the cursor `m - 2` rows up or down and the window with it, on a page that goes on that way. Every game's CONFIG look scrolls its window natively, so only a page longer than the renderer's window slides: a save category of the console's library, and in Deadlocked, UYA and RaC1 nothing else (UYA's 34 planets once, at 24). A page lists 128 rows at most. Round 1 to 3 had a More row last and a Previous row first, and "Skin 2/4" in the title. A renderer with no back control of its own gets a **Back** row at the end of every page but the root, and one row fewer for the page's own.

### 9.2 Opening, closing and switching off

There are two ways in, and no renderer adds a row to any of the game's own lists:

- **The game's own Help row.** Each game's pause menu has a Help entry with the basic controls, which a RaCMAN user has no use for. The renderer takes it over: it keeps its place, says TRAINER in the casing of its list (round 3; it said RACMAN), and opens the menu instead of Help. Back from the menu's root returns to the pause menu with the cursor on it, and Start or Select closes both.
- **The OPEN_MENU combo** (section 5.9), a shortcut from play. It asks the renderer through the mailbox's request byte: 1 to open while the menu is down, 2 to close while it is up. The renderer takes the request on the game's next frame that can act on it and clears the byte. A request it has not taken half a second later is withdrawn - the game was somewhere the menu cannot open, a cutscene, a load, the game's own pause - so the menu never opens later on its own. With the combo at 0 the Help row still opens the menu.

`menu.enabled = 0` in config.txt (section 5.10) switches the menu off. No renderer goes into a game, and one already in has its hook words put back - the Help row's too, which is the game's own Help again - at the first moment it is safe: the menu down, the game not paused, no mode change queued, and the game's current menu screen not the menu's. A menu in use stays usable until the player closes it. With the switch at 0 the combo installs nothing and only says so; at 1 again, the renderer goes back in at the next moment the game is in ordinary play.

### 9.3 The renderer's install

On a console qwark writes a game's renderer in a few seconds into each session (three), at the first tick after that with the game in ordinary play - gameMode 0 and no mode change queued, in Deadlocked and in UYA alike; in RaC1 its state 0 and no reopen of the pause queued - so nothing is written while one of the game's menus is up, and the Help row is the menu's before the player first opens the pause. A press of the menu combo before then installs at once, in the tick that saw it, and its open request goes out right after the install. Before it writes anything it reads every word the renderer replaces (section 9.5 lists them). Each must hold the game's own word or the renderer's:

- **All the game's own**: the data images go in (the mailbox, the renderer's state and its read-only data, section 9.4), then the first page, then the code, then the hook words, in the order the table gives, so nothing can branch into a half-written cave and the Help row can only open a screen that is already the menu's. For Deadlocked that is twenty-two writes, 19.8 KB (build 54's planet picker added six words); for UYA twenty-two, 21.2 KB; for RaC2 fifteen, 20.2 KB; for RaC1 sixteen, 22.1 KB (its code in four areas) (round 4's sizes). The pickers' round 2 adds two words to Deadlocked's (twenty-four writes, 19.9 KB).
- **Some already the renderer's**: this module was loaded again into a running game, or an install was cut short. The code in the game must be this build's renderer to the byte. If it is, qwark takes the mailbox over as it stands, answers any event left in it unread, writes the hook words still missing, and carries on; if it is not, the install is refused, since rewriting code the game may be running is a crash. Such a game gets its menu back with a restart. A qwark and a renderer always come from the same build, so they always speak the same mailbox version.
- **Anything else** at any of them is another mod's: nothing at all is written and the log says which word. qwark tries again every ten seconds, so the menu comes once the other mod is unloaded; a press of the combo tries at once and shows a notification saying why, once a session.

A renderer's caves are never taken out, and its hook words only for `menu.enabled = 0`, as above. A process that ends takes them with it, and the next session installs afresh.

qwark carries the renderers inside the module in build 54 (`src/games/menuhelper_bins.c`, built from `src/games/menuhelper/`), the zeros of the mailbox and the state as counts rather than bytes; a later build may load them from a file on the HDD instead.

### 9.4 The mailbox

A block at a fixed address per game, big-endian, laid out the same for every renderer. The renderer's install writes its header; after that each field has one writer.

| Offset | Field | Writer |
|---|---|---|
| 0x00 | u32 magic, `QMNU` | install |
| 0x04 | u16 version, 3 | install |
| 0x06 | u16 size of the whole block | install |
| 0x08 | u8 `row_cap`, the row slots in the block | install |
| 0x09 | u8 `row_size`, 0x40 (round 4; version 2 had 0x80 as well, for rows with a description) | install |
| 0x0A | u8 `max_rows`, the rows the renderer takes at once on a MAIN page, at most `row_cap` | renderer |
| 0x0B | u8 `caps`, below | renderer |
| 0x0C | u8 `hstate`: 0 closed, 1 open, 2 PICKER (version 3: the menu's screen has stopped under the game's own planet picker, which it opened; not a close) | renderer |
| 0x0D | u8 `request`: 0 none, 1 open, 2 close | qwark sets, renderer clears (qwark clears to withdraw) |
| 0x0E | u8 the renderer's game id | install |
| 0x0F | u8 `cfg_rows`, the rows it takes at once on a CONFIG page; 0 for a renderer with no CONFIG page | renderer |
| 0x10 | u32 `frames`: frames the menu has been up since it last opened (round 3: 0 again at each opening, which tells qwark the menu opened) | renderer |
| 0x14 | u32 `ev_seq`: bumped after the event record below | renderer |
| 0x18 | u8 `ev_kind`, below | renderer |
| 0x19 | u8 `ev_row`: the row's index on the page | renderer |
| 0x1A | u16 `ev_id`: the row's id, echoed | renderer |
| 0x1C | u32 `ev_key`: the page key the event was made on | renderer |
| 0x20 | u32 `ev_ack`: the last `ev_seq` qwark has dealt with | qwark |
| 0x24 | u32 `page_seq`: odd while qwark writes the page, even once it is whole | qwark |
| 0x28 | u32 `pick_seq`: bumped after `pick_planet`, a barrier between (version 3, the planet pickers) | renderer |
| 0x2C | s32 `pick_planet`: the planet picked in the game's own picker, qwark's planet id (PLANET_LIST's index) | renderer |
| 0x30 | u32 `page_key`, below | qwark |
| 0x34 | u8 `nrows`, 1 .. `row_cap` | qwark |
| 0x35 | u8 `cursor`: the row the cursor goes to on a new key | qwark |
| 0x36 | u8 `pflags`: bit0 ROOT, back here leaves the menu; bit1 MORE_UP and bit2 MORE_DOWN (round 4), the page goes on above or below this window | qwark |
| 0x37 | u8 `ptype`: 0 MAIN, 1 CONFIG | qwark |
| 0x38 | char `title`[32] | qwark |
| 0x58 | char `hint`[40]: a line about the page, the last answer as a sentence (no renderer shows it since round 4) | qwark |
| 0x80 | rows[`row_cap`], `row_size` bytes each | qwark |

Version 2 (round 2, still revision 1.22 and build 54) added `cfg_rows` and `ptype` in what were two reserved bytes; version 1 never left a console with a PC beside it. Version 3 (round 4, the same build) drops the description: a row is 0x40 bytes and nothing else, which no renderer missed (none showed one after round 3, and the client has its own); and a long page has no MORE or PREV row any more, `pflags` saying where it goes on and the renderer asking for the next window with SCROLL_UP and SCROLL_DOWN (section 9.1). The kinds 7 MORE and 8 PREV stay reserved and are never sent. Also in version 3, for the planet pickers (build 54): `pick_seq` and `pick_planet` at the end of the header qwark polls (which is 0x30 bytes now), the page from `page_key` on 8 bytes further down and its hint 8 bytes shorter; `hstate` 2, PICKER; caps bit4 PICKER; row flag bit6 PICKER. qwark takes each new `pick_seq` once and loads `pick_planet` once the game plays again (section 9.1); the `pick_seq` it finds in the mailbox at an install is not a pick. The pickers' round 2 (the same version and build) adds event kind 9, PICKER_OTHERS.

A row:

| Offset | Field |
|---|---|
| +0x00 | u8 kind: 0 INFO, 1 LINK, 2 TOGGLE, 3 ACTION, 4 CHOICE, 5 VALUE, 6 OPTION, 7 MORE and 8 PREV (retired in round 4, never sent), 9 BACK |
| +0x01 | u8 flags: bit0 ENABLED (the cursor may land on it), bit1 ON (active: a toggle that is on or Auto, an owned unlock, the current option), bit2 STEPS (Left and Right do something on it), bit3 CLOSES (the menu closes before it acts), bit4 RANGE (round 3: a VALUE with bounds, its numbers at +0x1C), bit5 SLIDER (with RANGE: few enough steps from end to end for a bar), bit6 PICKER (version 3: the root's Planets row, for a renderer whose caps say PICKER: the cross opens the game's own planet picker) |
| +0x02 | u16 id, opaque to the renderer and echoed in its events |
| +0x04 | char label[40]; on a RANGE row the label is 23 characters at most and ends inside its first 24 bytes |
| +0x1C | RANGE rows only, the label's last 16 bytes: u32 value, u32 lowest, u32 highest, u32 step (the step Left and Right move it by). `value - lowest` and `highest - lowest`, modulo 2^32, are the distances along a bar whatever the field's sign |
| +0x2C | char value[20] |

Every string is a fixed slot, NUL-padded and not necessarily terminated when full, so a renderer finds row i's label and value at the same place whatever draws them.

`caps`: bit0 STEPS, it posts LEFT and RIGHT; bit1 PAGE_KEYS, it posts PAGE_PREV and PAGE_NEXT; bit2 BACK, it has a back control of its own; bit3 REQUEST, it opens and closes the menu when asked; bit4 PICKER (version 3), it opens the game's own planet picker from the Planets row and posts the pick. Event kinds: 1 ACTIVATE, 2 LEFT, 3 RIGHT, 4 BACK (on a page that is not the root), 5 PAGE_PREV, 6 PAGE_NEXT (L1 and R1, on a page that goes on that way), 7 SCROLL_UP and 8 SCROLL_DOWN (round 4: the cursor at the window's first or last row, the page going on that way; `ev_row` is the cursor's row), 9 PICKER_OTHERS (the pickers' round 2: on the root's Planets row, `ev_row` and `ev_id` that row's, from a renderer whose picker cannot show every planet; qwark opens the game's page of the others, section 9.1, and refuses it on any other row).

**A page** goes in as three writes: `page_seq` made odd, the page from `page_key` to its last row, `page_seq` made even. A renderer copies a page whole and keeps the copy only if `page_seq` read the same even number before and after it; otherwise it tries again on its next frame. It never hands the game a pointer into the mailbox: the games' text widgets keep the pointer they are given and read it at every draw, while qwark writes the mailbox from another thread whenever it likes. It draws from its own copy, which only it changes, on the game's thread.

**The page key** is the page's id in the top sixteen bits, the first row of the window in the next eight, and a count of qwark's moves between pages and windows in the low eight. A renderer that sees a key it has not shown puts the cursor on `cursor`; the same key again is the same page refreshed, and the cursor stays where the player left it.

**An event** is the record and then `ev_seq` one higher, with a barrier between, and one at a time: a press while `ev_seq` differs from `ev_ack` is dropped by the renderer. qwark reads the header every tick while the menu is up and every fourth tick while it is down. It acts on an event only if `ev_key` is the page it last posted and `ev_id` is the id of row `ev_row` on it; anything else is stale and changes nothing. Either way it writes `ev_ack`, and then the next page, which for a refused or stale event is the same page.

**Live values.** qwark builds a page of features, manips or unlocks after every event, ten times a second while the menu is up and once a second while it is down, and writes it only when its bytes changed. A page of links, planets or saves is built only after an event, since nothing on it moves while the game is paused; Save files reads the console's library once per event, not ten times a second. The page is in the mailbox before the menu opens, so a renderer has something to draw on its first frame.

### 9.5 Deadlocked's renderer (NPEA00423)

It shows qwark's pages as screen slot 36 of GuiMenu_Screens, which the game leaves empty, so the game pauses itself for it, draws it in its pause, holds gameplay input off and plays its own sounds:

- **MAIN** pages with GuiSupport's button list, the list Options and Extras use: up to eight rows, `max_rows` 8. A row is "label: value" in the capitals of the game's lists, the current option of a pick-one list marked "*", a row that is not ENABLED grey with no callback. When a page has another number of rows the renderer calls SetMenuTarget again, the call the game uses to place a list, which lays the rows out again and fits the frame round them, so a page of three rows has the frame of three rows.
- **CONFIG** pages (round 3) with the look of the game's Options > GAME SETTINGS (screen 12): its screen panel, template 37, on canvas layer 15, with its plates, separators, frame, title and button legend, in a GuiState thread of the renderer's own run the way GAME SETTINGS runs its own. GAME SETTINGS' eight rows are fixed places holding five value texts and three sliders, so the renderer hides them (elements 0x15..0x29) and shows the page as a CtlList of the game's two-column row (template 28, the Cheats screen's row, in the same font and size as GAME SETTINGS' rows) over their area, made as wide as the Cheats screen's list: eight rows on screen, up to sixteen, `cfg_rows` 16, scrolling. Labels and values are in GAME SETTINGS' capitals, without its colon (round 4, the user's call; round 3 had "GHOST RATCHET:"), an action with nothing to say blank (round 4; it was "..."). The cursor's row is lit (0x80D0D0D0), a row that is not ENABLED grey (0x80404040) and passed over by the cursor, the rest the game's grey (0x80808080): GAME SETTINGS has no colour for an active row, and round 2's gold is gone. A VALUE row flagged MROWF_RANGE and MROWF_SLIDER (section 9.4) is drawn as GAME SETTINGS draws its volumes, in place of the number: the row's outline and swatch rectangles reshaped into an outline 0.3555 by 0.0288 coloured with the row, a dark track 0.3477 by 0.0192 and a fill in the team colours cut at (value - lowest) / (highest - lowest). GAME SETTINGS has no description line, so the hint does not show; an answer shows in its row's value for its two seconds. Sixteen rows hold every list of Deadlocked's whole (the sixteen bot upgrades, the twelve planets, the weapons); a save category longer than that slides sixteen at a time (section 9.1).

Every new page is the game's own way between screens (round 3; round 2 did it only for a page of the other type): GuiMenu_GoToScreen to screen 36 again, whose stop ends the widget shown and whose start begins the new page's. A list closes with MenuMain_Stop and the next opens with ShowMenu inside the same frame, as Main > Options does, the frame gliding to the new rows and the cursor onto its row; the renderer then holds nothing back (`mh_core_hold(0)`), since the same list shows again at once. A CONFIG page's thread is told to end, plays GAME SETTINGS' closing sound and clears its canvas layer on its way out, and the next starts on the second of two GuiStates with GAME SETTINGS' opening sound. A page refreshed in place keeps its screen. GoToScreen pushes nothing, so back goes where it would have gone.

`row_cap` is 16, `row_size` 0x40 (round 4), `caps` all five bits (bit4 PICKER since build 54's planet picker, below).

| Address | Replaces | Becomes |
|---|---|---|
| 0x9E8B58 .. 0x9E8B67 | screen 36's {fcInit, fcReset, fcStart, fcStop}: 0, 0, 0x991D40, 0 | 0, 0, the renderer's start, the renderer's stop |
| 0x368620 | `bl HandleMenuInput` in GuiSupport's UpdateMenus | a call to the renderer's frame, which goes on to HandleMenuInput unless it took the frame's input |
| 0x35E074 | `li r4,1` before GuiMenu_GoToScreen in GuiHud's single-player and co-op Start path | 36 in the frame the renderer pressed Start for an open request, 1 otherwise |
| 0x35DFBC | the nop after GuiHud's call to GB_GetLocalHeroFromSlot, before it tests Start | the open request taken, and Start pressed for the game in that frame |
| 0x3A649C | `bl msg_string` for the label of the pause main menu's fifth row, Help, in MenuMain_StartMain | a call that answers "TRAINER" (round 3; "RACMAN" before) |
| 0x3B2844 | `bl MenuPlanet_HandleSelection` in MenuPlanet_HandleInput: X on Worlds | a call to the renderer's select stub: the menu's pick while the menu opened Worlds, HandleSelection otherwise |
| 0x3B177C | `beq` in MenuPlanet_UpdatePlanets that leaves Ghost Station out unless a byte of the save says otherwise | a branch to a gate: listed while the menu opened Worlds, the game's test otherwise |
| 0x3B1424 | `li r3,15` in MenuPlanet_SetupPlanetTourneyList, the entries it answers | a branch to a gate: 16 while the menu opened Worlds, so its sixteenth entry, Interior, is listed; 15 otherwise |
| 0x3B27C0 | `beq` in HandleInput that ignores Triangle with no mission active | a branch to a gate: Triangle always goes back while the menu opened Worlds; the game's test otherwise |
| 0x3B2898 | the same `beq` for Start and Select | the same gate |
| 0x3B1590 | `stbx r5,r7,r24` in MenuPlanet_UpdatePlanets: a planet entry's bLocked, 1 where its level's status byte has no 0x02 (the pickers' round 2) | a branch to a gate: 0 while the menu opened Worlds, so no planet there has a lock beside it; the game's value otherwise. A tournament's lock (0x3B16C4) stays the game's |
| 0x3B2794 | `beq` in HandleInput to the Skills screen on command 4, Circle (the pickers' round 2) | a branch to a gate: the press ignored while the menu opened Worlds; Skills otherwise |
| 0x9E8954 | GuiMenu_Screens[3].fcStop, MenuPlanet_Stop's descriptor 0x994400 (a data word) | the renderer's descriptor: Worlds leaving for anything but screen 36 ends the menu's picker, then the game's stop |
| 0x3A8584 | `li r4,30` in MenuMain_MainCMDHelpCB, the Help row's callback, before GuiMenu_PushScreen | `li r4,36`: the row pushes the menu's screen over the main menu |

The Help callback's descriptor (0x993FD0) is named by that one row and nothing else, so no other list changes; the multiplayer pause menu builds rows of its own with no Help, and keeps every word. The main menu keeps its eight rows in their order. The six Worlds words (build 54; eight since the pickers' round 2, the lock's and Circle's gates after the four) go in after the Start path's and before the Help row's; each acts only while the core's picker byte says the menu opened Worlds, and does exactly the game's own otherwise, so the pause's own WORLDS row opens the game's Worlds as it always has. Probe PL4 holds two of them (X and the stop) and its list mod the four first gates, so the install refuses while either is loaded.

**The planet picker** (build 54, mailbox version 3; probe PL4 ran it on hardware). The root's PLANETS row pushes the pause menu's own Worlds screen (screen 3) over screen 36, GuiMenu_PushScreen(p, 3), as the main menu's WORLDS row does (0x3A8368), with the row's 7 and Worlds' own opening sound; screen 36's stop on the way is not a close. Worlds lists what the game lists - its planets and DreadZone's tournaments, a locked tournament greyed with its requirements (a locked planet too until the pickers' round 2, below) - and, opened from the menu, Ghost Station and Interior too. On it:
- **X** takes the focused entry: a planet hands its level to qwark (co-op's 21..35 as 1..15), locked or not; an unlocked tournament hands DreadZone (1), which holds them, and a locked one gets the game's refusal sound (4) and nothing more. Then Worlds closes the way its own Start closes it (the main menu's sound flag, g_fromPlanet, g_fromOther and g_fromMissionScreen cleared, CloseMenu(p, -1); Worlds' stop plays its 6), and qwark loads the planet once the game plays again, unlocking a locked one first (section 5.5). There is no travel dialog and no Missions screen: the planet's arrival puts the player in its first open campaign mission, or on its Missions screen when every one is done.
- **Triangle** goes back to the menu's root, the cursor on PLANETS, even with no mission active (the game's own Worlds then ignores Triangle and Start: the trap after a planet's last mission). **Start** and **Select** close everything, with no mission active too. **Circle** (Skills) does nothing (the pickers' round 2, the user's call; in round 1 it opened the game's Skills screen, back from which Worlds was the game's own).
- **No locks** (the pickers' round 2): every planet is drawn as an unlocked one, with no lock beside it (the game's NEW mark where the save has never visited it) and its own panel of text rather than its requirements, since X loads any of them. Locked tournaments keep their locks, X on one being refused.
- Where the main menu greys its WORLDS row (MenuMain_StartMain, 0x3A619C..0x3A63B0) - in Interior, and in DreadZone until its mission 37 has been completed (the byte at 0xB281B5) - PLANETS opens qwark's Planets page instead.

The code is at 0x97B000..0x97F000, in the empty tail of the executable's code segment, the mailbox at 0x17DD600..0x17DDF00 (0x480 bytes of it since round 4) and the state, read-only data, TOC and descriptors at 0x17DF0C0..0x17DFFC0, past the end of its read-write segment. None of them overlaps any mod known to the client, nor qwark's own writes, and all keep clear of probe A's and probe N0's caves; N0 hooks screen 36's slot and the Start path too, by design, so the install refuses while it is loaded. MenuMain_StartOptions keeps every word.

The controls are the game's. On a MAIN page: the cross selects, triangle goes back, and on the root triangle is the game's own back (to the pause main menu when the menu was opened from Help, out of the pause when from the combo); Start or Select closes the menu from any page. Left, Right, L1 and R1, which the game's list ignores, the renderer takes from player one's presses of the frame (`hudBitsOn`), only when nothing the game ranks higher is pressed in the same frame, and clears them as the game clears the ones it takes. On a CONFIG page all of them come from GuiMain_GetMenuCommand, one a frame, as GAME SETTINGS reads them: Up and Down move the list's focus with the cursor's tick, and Left, Right and the cross change a row. Presses are edges: holding a direction steps once. A Japanese copy swaps the cross and the circle, as the game does everywhere.

**Sounds** (round 3), as the game's own screens play them. On a list: the cross 7, triangle 8, a step 0x10, a window turned the cursor's tick, and nothing as a list starts. On a CONFIG page, as GAME SETTINGS: a toggle or a choice changed 0x10, a value stepped 0x11, or 4 when a slider stands at the end it is pushed against, the cross on a button, a link or a window row 7, and triangle nothing of its own; the panel plays 0xB as it starts and 0xC as it stops. The menu opening from the combo plays the pause's own sound, 1, under MenuMain_StartMain's rule: when the main menu sound flag (the byte at 0x9E88A4, which it then clears) is set, or else g_bMenuExit (0x9EAE34) is; from the Help row the row's callback has played 7 and cleared g_bMenuExit, so nothing more sounds. Start or Select closes a list with Options' back callback, CloseMenu(p, 5), and a CONFIG page as GAME SETTINGS closes, CloseMenu(p, -1), which plays nothing (the panel's stop does) and sets the sound flag.

**Opening from the combo.** The renderer takes the open request in the next frame of ordinary play the game would take Start in, presses Start for it and has the Start path open screen 36 instead of the main menu, so the game's own gate applies, and nothing happens in multiplayer. GuiHud's Start path is GoToScreen(0, 36) and then PushState(0, 2), whose menu thread empties the screen stack as it starts (0x36270C), so nothing is under screen 36 and back at the root closes the menu (Options' own back callback 0x3A809C: PopScreen 0x362584, with nothing under, calls CloseMenu 0x36249C). Closing pops GuiMain's menu state, which stops screen 36, clears the pad ring and ends the pause (0x35E3FC).

**When qwark does not answer.** An event unanswered for half a second turns the title into "WAITING FOR QWARK..." until the answer comes. Before qwark has posted any page the menu shows one grey row, "WAITING FOR QWARK", and back leaves it.

The renderer's state begins with the core's counters (section 9.6), for whoever reads them with the memory tools at 0x17DF0C0: +0x04 the times the screen started, +0x08 open requests taken, +0x0C the times the Start path answered 36, +0x10 events posted, +0x14 pages taken, +0x18 pages caught half written, +0x1C presses dropped with an event in flight; +0x38 is 1 while the menu's Worlds is up. The shim's byte at 0x17DFAA7 counts the times PLANETS opened Worlds (wrapping at 256), and the mailbox's `pick_seq` (0x17DD628) the picks.

From the NPEA00423 executable and probe N0 on hardware, round 1's MAIN page and round 2's on hardware, and GAME SETTINGS' layout read from the PS3 build's own bonus.ps3 (template 37); round 3's CONFIG page and its sounds have not yet run on a console.

### 9.6 A renderer's core and its shim

Every game's renderer is two parts, built together for each game the way the savefile helper's one source serves four (`src/games/menuhelper/build.sh`):

- **The core**, `mh_core.c`, the same source for every game: reading the mailbox into buffers that stay put, the page and cursor state, the window sliding under the cursor (round 4), casing and decorating the text, the presses into events with one in flight, the "waiting" title, the requests, which widget a page is drawn with (a CONFIG page goes to a game with no CONFIG widget as MAIN), whether a page is shown in place or the menu's screen started again for it, and the counters. It names no game address and calls no game function, which `check.py` holds it to.
- **The shim**, `mh_<game>.c` with its stubs and its header `mh_<game>.h`: only what talks to that game's menus. It shows what the core composed with the game's widgets, reads the game's input and hands the core presses, plays the game's sounds, opens and closes the menu the game's way, and says through its header how the game cases its menus.

`mh_shim.h` is the interface between them, and `rewrite/SPEC-console-ui.md` section 3.5 describes it for whoever writes the next shim.

**Round 3: two text sets.** Everything the core composes for a page (its rows' kinds, flags, ids and text, its title, its number of rows) is a text set, and there are two. A page the screen on show takes in place goes into its set, so the widgets keep their pointers and the new text shows at once (composed first into the other set and copied over, so nothing half written ever shows). A page that needs the screen started again goes into the other set, and the screen going keeps the old page's text until its last frame; the new set takes over as the new screen starts. A screen that still draws for a while after the next one has started holds the other set that many frames (`MH_HOLD_MAIN`, `MH_HOLD_CFG`, or the shim's own `mh_core_hold` for one switch), and a page that would need that set again waits in the mailbox meanwhile. The core composes straight out of the mailbox, keeping a page only if `page_seq` held, as it kept its copy of the page before. When the menu closes the core forgets where the cursor was, and at each opening it counts `frames` from 0. A game's header says which of a new page and another number of rows its widgets cannot take in place (`MH_RESTART_MAIN`, `MH_RESTART_CFG`): for those the core asks the shim to start the menu's screen again, as it does for a page of the other type, rather than to show the page. UYA's main pause list lays its rows and frame out only as it starts, so UYA starts its screen again for every new page; Deadlocked's widgets could take every page in place, and did in round 2, but since round 3 a new page starts its screen again too, which is how the game moves between its own screens (section 9.5).

**Round 3: a colon.** A header's `MH_STYLE` may carry `MH_STYLE_COLON` (0x10): the core ends a CONFIG row's label with ":" when the row has a value beside it, keeping a byte of the label's slot for it, as Deadlocked's GAME SETTINGS writes "MUSIC VOLUME:". Deadlocked and UYA set it in round 3; since round 4 no game does (the user's call).

**Round 4.** No description line: the core reads none and the shims have no `shim_desc`, the mailbox's rows being 0x40 bytes (section 9.4). An action with nothing to say is blank in every game: `MH_STYLE_DOTS` is retired, as `MH_STYLE_ARROWS` was in round 3. And the window slides under the cursor: on a page whose `pflags` say it goes on past the window, the core looks at the shim's cursor every frame, and when it stands on the window's last row it may land on with the page going on below (its first, going on above) posts SCROLL_DOWN (SCROLL_UP) with that row, once a row and only with no event in flight; the next window comes as a new page, the cursor on the same row of the page. L1 and R1 post PAGE_PREV and PAGE_NEXT only on a page that goes on that way. A shim needs nothing for either but a `shim_cursor` that answers its widget's cursor.

**Mailbox version 3: the game's own planet picker** (build 54). A shim that can open its game's planet picker puts PICKER in its header's caps, which builds the core's part of it (`MH_PICKER`), and defines `shim_picker(p)`. The core then calls it for the cross on a row flagged PICKER, with its picker byte (`ST_PICKER`, +0x38 of the state; `mh_picker_up()`) already set: the shim opens the picker over the menu's screen and answers 1, or answers 0 and the core posts ACTIVATE as for any link. The menu's screen stopping while the picker is up is not a close (`hstate` PICKER, the page and the cursor kept), and its start again afterwards, back from the picker, is no opening (the frames counting on, the cursor back on the row). A pick is `mh_core_pick(p, planet)`, with qwark's planet id: `pick_planet`, then `pick_seq`, and the menu closed (`hstate` closed, the root next time); the shim then closes the game's menu its own way. The picker leaving any other way than back to the menu's screen is `mh_core_picker_gone(p)`, the same close with no pick, and the shim must see every such way out. A picker that is a page inside the menu's own screen need not stop it (`hstate` then stays open, also no close) and comes back with `mh_core_picker_back(p)` rather than through the start; one that can serve the combo's close while it is up takes it with `mh_core_close_request()` (both macros, so a game without them carries nothing). While the picker is up the core takes no press. qwark asks for no close for a pick, the game's menu closing already, and drops a planet the game has not. Everything a shim changes in the game for its picker acts only while `mh_picker_up()`. The pickers' round 2: a shim whose game header sets `MH_PICKER_OTHERS` may answer `shim_picker` with MH_PICK_OTHERS (2) rather than open its picker, and the core then posts PICKER_OTHERS on the row instead of ACTIVATE (section 9.4); `shim_picker`'s answers are named MH_PICK_REFUSED (0), MH_PICK_OPENED (1) and MH_PICK_OTHERS, and a game without `MH_PICKER_OTHERS` builds the core byte for byte as before. `mh_shim.h` has the contract; rewrite/SPEC-console-ui.md, "Planet pickers: Deadlocked and the shared part" and "Planet pickers, round 2", the design.

**Round 5: a word after leading punctuation.** In title case (`MH_CASE_TITLE`, RaC1's and RaC2's) a word starts after a space, and goes on starting after the punctuation that opens one, the characters below "0" in ASCII ("(", the quotes, "-" and the rest): "(premade savefiles)", a category of the console's library, reads "(Premade Savefiles)", where round 4 made it "(premade Savefiles)". A letter or a digit ends the start, and punctuation inside a word starts none ("Load Set-aside File" as before). Capitals (`MH_CASE_UPPER`, Deadlocked's and UYA's) case every letter and the game's own casing none, so neither had the fault. The rule is in `put_mem`, which cases everything from the mailbox; the core's own strings ("Waiting for qwark...", the entry label) open no word with punctuation and keep the space rule. It costs every renderer 8 bytes of code.

### 9.7 Up Your Arsenal's renderer (NPEA00387)

UYA's single-player pause runs on the same IGE widget engine as Deadlocked's, but without a table of screens: each pause screen is a GuiState, a "thread" with an init, an update and a deinit, which the pause launcher or another screen starts with SwapTo while the running one resets itself, and the game's thread runner ends the one and starts the other. The renderer brings two GuiStates of its own, which are the menu's screen, so the game pauses for it, draws it in its pause and plays its sounds, with two of the game's own widgets, as probe N3b showed on the console:

- **MAIN** pages on the main pause's own list: the main pause's init, update and deinit run on the renderer's row table, so the frame is sized to the rows, and the cursor, its sound, the colours of an enabled and a grey row and the Start close are the game's. Up to nine rows, `max_rows` 9, as the game's own nine-row tables have, each "label: value" in capitals; the labels are text ids that the renderer's hook on msg_string answers with its rows. A row wider than the frame (0.44 of the screen; the frame's end pieces sit at 0.258 and 0.747) shrinks to fit it, down to 0.6 of its size, through the text widget's own box and fit flag, which the Options screens' lists use. The game's list lays its rows and frame out only as it starts, so a new page, or one with another number of rows, starts the menu's screen again on the other GuiState; a page refreshed in place only has its rows' texts set again.
- **CONFIG** pages in the look of the Options screen's AUDIO / VISUAL page (getter 0x2DB6B8, init 0x2DD218), the settings list of the game's own Options, rebuilt with that screen's own calls and numbers: the submenu frame with the page's title, its banner and the title's scale fitted to the title as the Options screens fit theirs (0x2D4DB0), "SELECT" and "BACK" hints; the picture panel AUDIO / VISUAL has above its list (its widget table at 0xC4FC60), in which the renderer shows one of that screen's own pictures, the one its EFFECTS VOLUME row has (picture 4, the user's choice for a header; round 3 had the description line there), set up as that screen sets up its own: two of the pause screens' picture buffers (0x2D3A4C), picture source 1 reading AUDIO / VISUAL's picture set out of the hud.wad (0x2CCD48 with the word at 0xD60F74 and the set at 0xD63550, the wad's table being in memory from the game's start), the panel's rectangle 0x1E0003 tied to the source (0x2AFF84), and the picture shown (0x2AE710, then SetVisibility) thirty frames after the page starts, as AUDIO / VISUAL shows a row's once the cursor has rested that long; the screen's end stops the source (its vtable's second entry, 0x2CCD38), gives the buffers back (0x2D3B94) and waits for the disc's reads (0x234CA8), as AUDIO / VISUAL's deinit does, and without both buffers or with the source busy no picture shows; and the grid list of raw strings with AUDIO / VISUAL's configuration (rows at 0.445, 0.0532 apart, labels at 0.115 in a box 0.53 wide, values right-aligned at 0.885 in one 0.24 wide, each text shrinking to fit its box), its frame, cursor bar and rules, and its cells in font 1, the font of the Options screens' lists (CHEATS draws in font 0, with boxes 0.35 wide that shrink a long label). Labels are in capitals, without a colon (round 4, the user's call; round 3 had AUDIO / VISUAL's "SUBTITLES:"), values in capitals: a toggle OFF, ON or AUTO; a choice its bare value, as STEREO and MONO; an action blank until it answers; a link its label alone; a value its number; a grey row its text. A VALUE row qwark flags MROWF_RANGE and MROWF_SLIDER (section 9.4) is a slider, AUDIO / VISUAL's own: the groove, the track and the knob (0x2DD218), the knob at (value - lowest) / (highest - lowest) along the bar (0.653 to 0.885, as 0x2DBE70 places it), past an end at that end; the value cell blank, as that screen's MUSIC VOLUME, the number after the label instead ("CHALLENGE MODE 51"), where the label's box has room for it. The renderer keeps one slider slot for each of the eight rows on screen, shows in each the slider of the row the grid has there, and none while the rows slide. Up to 24 rows (round 4; sixteen before), `cfg_rows` 24, eight of them on screen and the grid scrolling to the rest; a longer page (the 34 planets, a save category) slides 24 at a time (section 9.1). Every row is in the list's own colour but one that is not ENABLED, in the grid's grey (colour 4, drawn as colour 1 is, 0x80808080), which the grid's Up and Down skip (round 4: round 3 drew a row that was on white, as CHEATS does). The grid's cells, 24 rows of two cells of 0x44 bytes and a word, have a span of their own (0x1ADAC00..0x1ADB900), which the install does not write: GridBuild clears them itself.

`row_cap` is 24, `row_size` 0x40 (round 4: no descriptions), `caps` all five bits (bit4 PICKER since build 54's planet picker, below, "The Planets row").

| Address | Replaces | Becomes |
|---|---|---|
| 0x14BCF0 | `lis r4,0`, the first word of msg_string | `b` to the label stub: text id 0xF000 is "TRAINER", 0xF001 to 0xF009 the rows of the MAIN page in the text set on show, 0xF040 + p (p below 30; the pickers' round 2) PLANET_LIST's name of planet p, any other id goes on into msg_string |
| 0x2F4284, 0x2F41FC, 0x2F4250 | `li r6,8`, `li r6,9`, `li r6,9`: the row count of the main pause's three tables, after its init picks one by planet | a call that hands over the renderer's table, its count and the band of the frame's end pieces while the renderer runs that init, and the game's otherwise |
| 0x287184 | `rlwinm r6,r6,0,20,20`, the mode 0 update's test of the pad for Start this frame | a call that makes the test true in the frame that takes an open request, for that test alone: no Start is written to the pad, so nothing else of the game sees one, and the game's own gate after it decides whether the pause opens |
| 0x287264 | `li r5,1`, the pause reason of the Start path's mode_requestChange(3, 1, 1, 0, 0) | a call that keeps the reason and tells the launcher whether this pause is the menu's |
| 0x2D4444 | `bl 0x2F3C18`, the pause launcher's call of the main pause's getter for reason 1 | a call that answers the menu's GuiState when the Start path opened this pause for an open request, and calls the getter otherwise |
| 0x2CE838 | `cmpwi cr1,r3,30`, the first test of the GALACTIC MAP's list build (0x2CE7B8) on the save's planet `r3 = 0xDA5A94[r7]` | a call that, for the map the Trainer opened and only while its init runs, hands the build the r7th planet of the Trainer's list instead and takes it past the build's filter to 0x2CE894, where the entry is made (a 0 past the list's end goes back into the filter, which skips it); for every other map, the game's own word |
| 0xC52844, 0xC528F8, 0xC529AC | 0x106, the text id of the HELP row of each of the main pause's three tables | 0xF000, "TRAINER" |
| 0xC5284C, 0xC52900, 0xC529B4 | 0xBDE430, that row's callback, HELP's | the renderer's own callback, which starts the menu's screen over the main pause |

The tables are the main pause's for an ordinary planet (eight rows), for the challenge planets with END CHALLENGE and for the arenas (nine rows each); the renderer writes the HELP row's two words in each and nothing else of them, so every row keeps its place. HELP's callback descriptor is named by those three rows and nothing else. qwark writes the fourteen words in this order, the HELP rows' last, and takes them out in the reverse order.

The code is at 0xBC4000..0xBCC000 (0xBC8000 until the planet picker, whose code outgrew it), in the empty tail of the executable's code segment (which ends at 0xBC1B68), the grid's cells at 0x1ADAC00..0x1ADB900 and the mailbox at 0x1ADB900..0x1ADC000 (round 4: the mailbox was at 0x1ADB000, and the cells in the renderer's state, which 24 rows would have run 2 KB past its span), and the state, the read-only data, the TOC and the descriptors at 0x1ADC900..0x1ADE000, past the end of its read-write segment (0x1ADAB10). None of them overlaps any mod known to the client, nor qwark's own writes (the fast-load cave at 0x9807F0..0x9808B0, the savefile helper's and the trophy reset's caves round it), and all keep clear of the hardware probes' caves (probe A's at 0xBC2000 and 0x1ADC000, N3's and N3b's at 0xBCC000 and 0x1ADE000). N3 and N3b hook the same words by design, so the install refuses while either is loaded.

**Opening.** From the main pause's HELP row, which says TRAINER: the menu starts over the main pause on its root, and back at the root goes back to it with the cursor on TRAINER (the renderer keeps the main pause's cursor, which its list shares, and puts it back). From the combo: in the next frame of ordinary play whose Start test the game reaches, the renderer takes the open request and answers that test with Start, and the Start path's own gate (the frames since the last load, the player's state, gameMode 0) decides; the launcher then starts the menu's root instead of the main pause, and back at the root leaves the pause. Nothing opens in multiplayer, whose pause is the online lobby's, nor in a vid comic, whose pause is a screen of its own (planets 30 to 36, reason 5): the request is taken and dropped there.

**The controls.** On a MAIN page the main pause's own: Up and Down, the cross calls the row's callback (and the main pause plays its sound), Start closes the pause; Triangle is the menu's back, which the renderer takes before the main pause's update would leave the pause with it; Left, Right, L1 and R1, which the main pause ignores, step and turn windows. On a CONFIG page as AUDIO / VISUAL reads them (0x2DDA54): the grid's Up and Down with its sound, then one of the cross, Left, Right, L1, R1 and Triangle on the grid's row, each with the pause screens' change sound but a slider's step, which ticks as AUDIO / VISUAL's sliders do, then the Start close as every pause screen has it. The confirm button is the game's (`*0xCBA7F0`: the cross, or the circle where the game swaps them). Presses are the pad's edges, `*(0xC1D574)` + 0xC4.

**The Planets row: the GALACTIC MAP.** The root's Planets row (`MROWF_PICKER`) opens the game's own planet picker over the menu, the ship's GALACTIC MAP (getter 0x2CE130, REPORT-planet-pickers.md section 5), on the menu's other GuiState: the map's own init (0x2CED28), update (0x2CFB60) and deinit (0x2D0600), each wrapped by the renderer, and its own pre- and post-render (0x2D0744, 0x2D0768). A push-style picker, as Deadlocked's Worlds: the menu's screen stops without closing (`hstate` 2, PICKER, section 9.6), and back from the map is its start again. In the map:
- **the list** is every planet qwark loads that the map has a picture for (0xC4EFFC[planet], planets 1 to 29), in PLANET_LIST's menu order: Veldin, Florana, Starship Phoenix, Marcadia, Daxx, Obani Gemini, Blackwater City, Annihilation Nation, Aquatos and its two mission levels, Tyhrranosis, Zeldrin Starport, Holostar and its Clank level, Obani Draco, Metropolis, Crash Site, Aridia, Qwark's Hideout, Phoenix Rescue, Koros, Command Center, Launch Site, the Insomniac Museum and the two ranger battles, 27 entries of the map's 30. The build reads it through the word at 0x2CE838 instead of the save's list of unlocked planets, which is never written. The vid comics (30..36) have no picture (0xC4EFFC past 29 is the map's own variables) and are on qwark's page that Square opens. Every entry is named as the client's planet picker names it, PLANET_LIST's name (the pickers' round 2, the user's call): while the Trainer's map builds itself, both words of each listed planet in the game's name table (0xC409E4, {location, planet} text ids, whichever the build and the init read) say 0xF040 + p, which the label stub answers from a table in the renderer's read-only data (0x1ADD900: thirty pointers, planets 0 to 29, then the map's list, then the names); the game's words go back as the init returns. (Round 1 of the pickers changed five of the words to five of the game's own texts instead: Lot 43, Deep Sea Hideout, Sewers, Island Battle, The Phoenix Attacked.) Square's hint, MISSIONS, says VID-COMICS ("\x13 VID-COMICS", in the glyph and the capitals of the map's own hints).
- **the cross** on a planet: the renderer hands qwark the pick (`mh_core_pick`: `pick_planet`, then `pick_seq`, section 9.4), plays the map's travel sound (0x14) and closes the pause plainly (the pause exit with its kind 0, no travel of the game's). qwark then runs its planet load of that planet, with the fast-load force and the selected reset flags, once the game plays again, as the Planets list's row would. Veldin, the planet you are on and the Phoenix from Phoenix Rescue, which the map's own pick refuses, are loaded too.
- **Triangle**: back to the menu's root, the cursor on Planets.
- **Square** (VID-COMICS; the pickers' round 2): back to the root, then the cross on Planets again, which the renderer's picker answers that once with MH_PICK_OTHERS, so the core posts PICKER_OTHERS (section 9.4): qwark's Vid-Comics page, a CONFIG page of the base vid comics 1 to 5 (planets 31, 33, 34, 32 and 35, PLANET_LIST's names; rac3.c's `picker_others`), each a button that loads the comic through the planet load once the menu has closed. Triangle there returns to the root, the cursor on Planets. Vid Comic 6 and Vid Comic 1 SE stay on the Planets page. (Round 1 had Square open the whole Planets page, ALL PLANETS.)
- **Start**, Select and the combo (`mh_core_close_request`): close everything (`mh_core_picker_gone`), the menu back on its root next time.
- everything else (the cursor, the planet views, the bolt counts) is the map's own update.

The renderer takes the cross, Triangle, Square and the Start close before the map's update would see them: every one of the map's own exits travels from play (the pick's callback 0x2D078C sets the travel; Triangle and Start "return" to the current planet, a reload, or on Blackwater City and Aridia go to BATTLE MISSIONS, 0x2DECB4; Square starts MISSIONS, whose travel and whose Triangle back to the game's map do the same). The game's own map, the ship's and every other, keeps the game's GuiState and is never touched: the list word acts only while the Trainer's map's init runs. The map refuses to open (the cross posts the event, and qwark shows its list) with a Start close pending in that frame or in a pause whose Start does not leave it (0xEEED08). The map and a CONFIG page share the pause screens' picture sources and buffers (the map takes sources 0 to 2 and four buffers); the runner ends the menu's screen, and with it the CONFIG page's picture, before the map's init, and the menu's next CONFIG page sets its own up again.

**When qwark does not answer**, as in Deadlocked: "WAITING FOR QWARK..." in a CONFIG page's title after half a second (a MAIN page has no title), and before any page one grey row.

**Switching off.** `menu.enabled = 0` takes the words out (the map's list word with them) once the menu is down (`hstate` 0: not while the map is up over it), gameMode is not 3 (the pause), no mode change is queued (0xC1E840 is -2) and the renderer's own word at 0x1ADD754 (round 4; it moved with the state) says none of its GuiStates runs.

The renderer's state begins with the core's counters, at 0x1ADC900, as section 9.5 lists them: +0x04 the times the menu's screen started (every page the main pause list starts again counts), +0x08 open requests the Start test took, +0x0C the times the launcher answered the menu, +0x10 events, +0x14 pages, +0x18 pages caught half written, +0x1C presses dropped.

From the NPEA00387 executable and probes N3 and N3b on hardware. The renderer's first version, with CHEATS' look on its CONFIG pages, ran on the user's console (2026-10-19); the Options look, the sliders and the MAIN rows' fit ran on it after round 3 (2026-10-19: "perfect"); round 4's picture, its 24 rows and the sliding window have not yet. The GALACTIC MAP as the Planets row has not run on the console yet (probe PL3, its prototype, has not either). The disc trilogy's RaC3 (BCES01503) shares UYA's addresses in qwark, and gets the renderer only if every one of the fourteen words holds the game's own.

### 9.8 Going Commando's renderer (NPEA00386)

RaC2's pause is the PS2 game's own pause manager (0x15B2F50) with the HD port's pages drawing it. A pause screen is a static struct, {fourteen rects, a parent, an id, a focus item, fourteen items}, and an item four descriptors, {update, draw, enter, exit}; a screen opens when its address goes into the manager's next screen, and the manager then calls the old screen's exits, plays the screen sound (18), and on the following frame makes the new screen current and calls its enters. The renderer brings six screens of its own, in its data, each with the renderer's one item, so the game pauses for the menu, draws it in its pause and plays its sounds, with two of the game's own looks, as probe N2c showed on the console:

- **MAIN** pages on the pause root's vertical bars (the HD port's object at `*0x133E8A8` + 0x4A670): the renderer gives them the page's rows as text ids that its hook on msg_string answers with its rows, says which bars the cursor may land on, and the root's own draw draws them. The bars' own update moves the cursor (Up and Down, the tick, past a grey bar). Up to seven rows, `max_rows` 7, as the root's own seven, each "label: value" in title case.
- **CONFIG** pages drawn the way Special > Cheats is drawn (its draw, 0xBC1A50): the frame's three sprites, the page's title, Cheats' "Toggle" and "Exit" hints, and the rows in two columns from y 112, 19 apart, the label at the left in title case and the value centred on x 405, an action with nothing to say blank (round 4; it was "..."), a choice its value alone. The cursor's row is in Cheats' highlight, a row that is not ENABLED grey and skipped by the cursor; a label that would reach its value is cut short with "..", measured with the game's own text width. Up to 32 rows (round 4; sixteen before), `cfg_rows` 32, the 27 planets and the 24 weapons whole: nine lines fit inside the frame, y 112 to 264 (round 4, from the user's photo: the tenth, at 283, fell off its bottom), so a page of up to nine rows shows them all, and a longer one shows eight and scrolls, with "..." over the first (y 93) or under the last (y 264) where more are out of sight. There is no description line, and an answer shows in the row's value.

A new page, or one with another number of rows, opens the next of the renderer's screens of its look (two of each, used in turn), the game's own way between its pause screens, with its screen sound; a page refreshed in place has its texts, colours and enabled bars set again. Nothing of the menu is drawn on the frame a screen becomes the current one before its enter has run.

The two text sets (section 9.6) need no hold here (`MH_HOLD_MAIN` and `MH_HOLD_CFG` 0): the manager calls the screen's exit and makes no screen current in the frame the next is asked for, and the next draws nothing of the menu until its enter, where the new set takes over. The bars keep the pointers msg_string gave them; the label stub answers the set on show, and the bars ask it only as the renderer lays them out, after that enter, so a screen going keeps its own set's text to its last frame. A VALUE row with a range (Challenge mode's 0 to 255) shows its number; a bar for it would be drawn with the game's raw box call inside the pause's draw, which only the console can show is safe, so it waits.

`row_cap` is 32, `row_size` 0x40, `caps` all four bits and MBOX_CAP_PICKER (0x10, mailbox version 3: the planet picker, below).

| Address | Replaces | Becomes |
|---|---|---|
| 0xB28F38 | `lis r4,0x134`, the first word of msg_string | `b` to the label stub: text id 0xF000 is "Trainer", 0xF001 to 0xF007 the rows of a MAIN page, 0xF010 + p the planet picker's name of qwark's planet p (p below 27), each of them also sign-extended, 0xFFFFF0xx (the pickers' round 2: the planet list's draw passes a row's ids so), any other id goes on into msg_string |
| 0xBF6110 | `rlwinm r4,r4,0,20,20`, the mode 0 update's test of the pad for Start this frame | a call that, with a request in the mailbox, asks the renderer: an open request, where the pause would open on the root with Help, is answered with Start for that test alone (no Start is written to the pad, and the game's own gate after it decides whether the pause opens); any other request is taken and dropped |
| 0xBF6190 | `li r5,1`, the pause reason of the Start path's mode_requestChange(3, 1, 1, 0, 0) | a call that keeps the reason and tells the pause manager's root whether this pause is the menu's |
| 0xBB8958 | `addic r3,r3,24804`, the root screen (0x13460E4) the manager opens for reason 1 | a call that answers the renderer's combo screen when the Start path opened this pause for an open request and the pause's setup gave the bars the ordinary root's table (0x135218C), and the root otherwise |
| 0x13467F8 | 0x01348594, the screen of the root's entry 4, Help | the renderer's Help screen |
| 0x135212C | 11605, the text id of the root's fifth label, Help | 0xF000, "Trainer" |

qwark writes the six words in this order, Help's two last, and takes them out in the reverse order. The root's Help entry is the only one of its kind: the bars of the root in a hoverbike race, a ship level, an orbit or on the hoverbike have no Help (0xBB6970 gives them labels of their own), and the renderer's combo does not open the menu there either: it asks the same questions the root's setup asks (the six functions it calls, 0x5BAC4 .. 0x5BB00) before it answers Start.

The code is at 0x12D9000..0x12DD000, in the empty tail of the executable's code segment (which ends at 0x12D6668), the mailbox at 0x1FD3000..0x1FD3900 (round 4: 0x1FD3800 before) and the state (the core's two text sets, then the renderer's own), the six screens (from 0x1FD6320; 0x1FD5A20 before round 4), the read-only data, the TOC and the descriptors at 0x1FD5000..0x1FD8000, past the end of its read-write segment (0x1FD1090). None of them overlaps any mod known to the client, nor qwark's own writes (the savefile helper's cave and the switches' and crash guards' caves at 0x113EC64..0x11414FC, the aside buffer), and all keep clear of the hardware probes' caves (probe A's at 0x12D7000 and 0x1FD2000, N2's, N2b's and N2c's at 0x12DD000 and 0x1FD4000). N2, N2b and N2c hook the same label, combo, root and Help words by design, so the install refuses while one is loaded.

**Opening.** From the pause root's Help bar, which says Trainer: the menu's Help screen opens over the root, and back at the menu's root goes back to the root with the game's own bars set up again and the cursor on Trainer. From the combo: in the next frame of ordinary play whose Start test the game reaches, on a planet where the root has Help, the renderer takes the open request and answers that test with Start, and the Start path's own gate (the frames since the last load, the player's state, gameMode 0) decides; the manager then opens the menu's combo screen instead of the root, and back at the root leaves the pause. Nowhere else does the mode 0 update run, so nothing opens in a cutscene (gameMode 1 and 2), in space (6) or in the other modes.

**The planet picker** (mailbox version 3, section 9.4). The root's Planets row opens the game's own ship planet select, its galaxy and its list, in place of qwark's Planets page, which stays the fallback. The game opens that select for pause reason 4 as screen 15 (0x134DD90): item 0 the galaxy (0x134DEC0, the HD page at `*0x133E8A8` + 0x4C680), item 1 the PS2 list of the planets the save has discovered (0x134E0DC: update 0xBBE528, draw 0xBCBC58, enter 0xBD2488), item 4 the picture of the planet under the cursor (0x134DEF0, `gmap_ss[p]`, which the disc has for every planet 0 to 26). The cross on a planet there opens screen 16, the planet's page, whose cross requests the game's own travel (mode_requestChange(6, ...) at 0xBC983C) and whose code tells that screen by its address. The picker stops before it:
- **Its screen** is a copy of screen 15 in the renderer's data (0x1FD6AA0): screen 15's rects and id, the game's own galaxy and picture items where screen 15 has them, and in place of the list a copy of the list item (0x1FD6BD0) with the game's draw and flags (0x538) and the renderer's update, enter and exit. Its rows (0x1FD6C20) are every planet qwark's PLANET_LIST loads, in qwark's order, Aranos (0) to the Jamming Array (26), row i planet i: the game's location for the planet (its own text ids, 0x1345930 + 8p + 4) over qwark's name for it (text id 0xF010 + p, the label stub's), or the name alone where the location says the same or there is none (the Insomniac Museum, Wupash, the Jamming Array); every row of a kind (0x101) the game's row actions (0xBB77EC, kinds 3 to 13) ignore. Nothing of the game's own planet select is written: the ship's select is the game's as it was, its list the discovered planets. The list's draw (0xBCBC58) reads a row's text ids as halfwords and sign-extends them before msg_string (0xBCC124, 0xBCC384), whose answer for an id it does not know is "(unavailable)"; so the label stub takes our ids in that form too (the pickers' round 2: round 1's rows said "(unavailable)" where the planet's name should be). The game's own rows carry the game's ids, below 0x8000, which the stub sends on as before, so the ship's select says exactly what it always said.
- **Opening.** The cross on Planets, a row qwark flags MROWF_PICKER for a renderer with MBOX_CAP_PICKER: the renderer writes the picker's parent (the menu's screen it is on) and makes the picker the manager's next screen, the game's way between pause screens, with its screen sound (18); no event goes to qwark. The core has marked the picker up first (ST_PICKER), so the menu's screen stopping is no close: `hstate` reads PICKER, the page and the cursor kept. Only from the bars, so only from the root, wherever the Trainer menu opens: no ship, space mode or level of a kind is needed, the pause being the Trainer's (reason 1).
- **In the picker.** Up and Down (and L1 and R1, which the list takes the same way) are the game's list's own, with its tick; the renderer then shows the planet under the cursor in the galaxy and the picture through the game's own call (0xB4DEF8, which sets 0x157D110), since the list's update shows the planet of the game's discovered list (0x1345AB4) at that row. The cursor starts on the planet shown, the current one, as the select's own enter puts it.
- **The cross on a planet** is the pick, taken before the game's list sees it (with no Start, Select or Triangle in the same frame, bit 0 of 0x156A00C clear, the list the focus, as the list's own test): the sound the planet page plays for its own cross (4), the planet to qwark in the header's pick words (`pick_planet`, then `pick_seq`, through mh_core_pick, which counts as the menu's close), and the pause closes. In a reason 1 pause an item's close is exit action 1 (0xBB91FC), a plain mode_pop(0, 0), so nothing travels by itself. qwark takes a pick it has not seen, and once the game plays again runs its planet load for it (PLANET_LOAD's, section 5.5: the fast-load force, the reset flags and the manip watchers), as for a planet of its own Planets page; the current planet loads it again.
- **Back.** Triangle is the list's own: it shows the current planet again and goes to the picker's parent, the menu's screen it came from, whose start sees the picker up and takes it down: the root again, the cursor on Planets, with no new opening for qwark (the frames go on); a Triangle there then goes where it would have, to the pause's root from Help or out of the pause from the combo.
- **Closing.** Start and Select are the list's own close of the pause; a close request (the combo) is taken by the picker's next frame (mh_core_close_request) and closes it the same way. Any close of the pause from the picker - a pick, Start, Select, the combo - is the menu's close (mh_core_pick, or mh_core_picker_gone from the list's exit): `hstate` closed, the root next time, and the renderer's word free for `menu.enabled = 0`.

**The controls.** On a MAIN page: Up and Down the bars' own; the cross, Left, Right, L1 and R1 on the bar the cursor is on; Triangle back; Start and Select close the pause, with the sound the root makes as it closes. On a CONFIG page as Cheats reads them: Up and Down with the tick, past a grey row and round, the page scrolled; then one of the cross, Left, Right, L1 and R1 on the cursor's row, a change with Cheats' change sound (4), and Left or Right on a row they do nothing on with Cheats' refusal (5); Triangle back; Start and Select close. The confirm button is the game's (`*0x141ABA8`: the cross, or the circle where the game swaps them). Presses are the pad's edges, 0x147A444 (pad +0xC4).

**When qwark does not answer**, as in the other games: "Waiting for qwark..." in a CONFIG page's title after half a second (the bars have no title), and before any page one grey bar, "Waiting For Qwark". Whenever the menu closes, the next opening, from Trainer or the combo, shows the root with the cursor on its first bar (section 9.1).

**Switching off.** `menu.enabled = 0` takes the words out once the menu is down, gameMode (0x156B064) is not 3, no mode change is queued (0x1329F90 is -2) and the renderer's own word at 0x1FD62D4 (round 4; it moved with the state) says none of its screens is the manager's.

The renderer's state begins with the core's counters, at 0x1FD5000, as section 9.5 lists them: +0x04 the times one of its screens entered (every page the bars or Cheats' look start again counts), +0x08 open requests the Start test took, +0x0C the times the root stub answered the menu, +0x10 events, +0x14 pages, +0x18 pages caught half written, +0x1C presses dropped. Its own word at 0x1FD62E0 (round 4) counts the labels of the CONFIG page shown that were cut short; the next three are the planet picker's: 0x1FD62E4 the times it opened, 0x1FD62E8 the planets picked, 0x1FD62EC the last planet picked.

From the NPEA00386 executable and probes N2, N2b and N2c on hardware; the renderer has run on a console since (its scrolling the user's "works well"), the planet picker not yet. The disc trilogy's RaC2 (BCES01503) and the repacks qwark knows share RaC2's addresses, and get the renderer only if every one of the six words holds the game's own.

### 9.9 Ratchet & Clank's renderer (NPEA00385)

RaC1's pause is a data-driven framework of pages and items in the game's data. A page is 0x88 bytes, {a pose for each of the fourteen pane mobys, a parent, a screen number, the item that takes input, an item for each pane}, and an item four descriptors, {update, draw, enter, exit}, then a tail of its widget's. A page goes up when its address goes into the menu's next page (0xA52BB8): the game plays its sound, has the panes play the new page's poses forwards (or the current page's backwards when the next is its parent), runs twelve frames with no page current, which draw nothing of the pages' items, and then makes the new page current. Every item of the current page is updated every frame, and only the page's focus item reacts to the pad; an item whose update answers non-zero closes the pause. The renderer brings three pages of its own, in its state, and its own items on them, so the game pauses for the menu, poses and draws its panes and plays its sounds, with two of the game's own looks, as probes N1, N1b and N1c showed on the console:

- **MAIN** pages on the main pause's own panes and poses (the main page, 0x729A80): one row a pane, each a one-row generic list (the main page's own rows' widget, 0x7075F0 and 0x7075F8, flags 0x84) whose label is a text id the renderer's hook on get_ui_string answers with its own copy of the row, "label: value" in title case. A row that does not fit its pane at the main page's text size takes the small size, and is cut short with ".." only if it still does not fit; a row that is not ENABLED is the generic list's grey and out of the panes' Up and Down links, which do not wrap, as the main page's do not. Up to seven rows, `max_rows` 7, as the main page's seven panes; the render draws the seventh only with Goodies unlocked (0xA52C88), so a page of seven sets that byte while it shows and puts the game's own back. The game has no poses from the main page's layout to itself, so the renderer moves the panes itself between MAIN pages: a page's panes play the main page's poses forwards (out) and the next page's backwards (in), as the pause's own opening has them.
- **CONFIG** pages in the layout of Goodies > Cheats (screen 38, 0x72F5D4), probe N1c's CONFIG look: Cheats' poses for panes 1 to 4 (its list pane 0x9A, its hint pane 0x9B, and 0x9C and 0x9D, which Cheats gives no item) and, for the title pane, the pose of Goodies > In-Level Movies (0x72FA74, pose 0xA4), the sibling page that shares every other pose with Cheats and whose title pane is made for the longest of their titles; a copy of Cheats' title item (the label item 0x72F65C, flags 0x0F, no enter) with the page's title; Cheats' hint item ("Toggle", "Exit", 0x72F718) as it is; and in the list pane, which Cheats, Cinematics and In-Level Movies share and in which the game draws Cinematics' 29 rows, the renderer's own list draw with the geometry of Cheats' (0x14A86C): the blend that draw sets, the medium font (0x7368B0) at scale 1.0, the rows sharing the pane's height with the first at h / (n + 1) - 8, the label at x 12 and the value right-aligned at w - 12, the cursor's row in 0x8020FFFF and the others in 0x80FFA888, a row that is not ENABLED in the generic list's grey, 0x80303030, and skipped by the cursor. A label that would reach its value is cut short with "..", measured with the game's glyph widths. A VALUE row with RANGE and SLIDER (section 9.4) is drawn as Options > Camera Options draws its 3D depth slider (0x14AD88): the label 4 lower, the value right-aligned before the bar, the frame (0x80696969) and track (0x80383838) boxes and the fill (sprite group 0xE99E, frame 8) to the value's place along its range. Cheats' own list cannot scroll; the renderer's shows as many rows as the pane holds at 20 units or more each (the pane's height is the game's, read every frame; a pane that holds Cinematics' 29 rows should hold well over eight of the renderer's) and scrolls the rest. Up to 24 rows (round 4; sixteen before), `cfg_rows` 24, the nineteen planets whole; a longer page slides 24 at a time (section 9.1). An action with nothing to say is blank (round 4). There is no description line, and an answer shows in the row's value. The renderer's first version had Options > Camera Options' layout, whose list pane held three rows on the console: a pane's size is its pose's, from the level data, so a page gets a larger list pane only by taking another layout's pose for it.

The game's own Cheats list flips a row with the cross and has no Left or Right; the renderer's list takes the cross, Left, Right, L1, R1 and Triangle for the core, and moves Up and Down itself, with the menus' auto-repeat (pad +0xB4) and no wrap, as that list moves.

Moving between pages: from a MAIN page the panes fly out first; the next MAIN page's panes then fly in, or the game's transition goes to one of the renderer's two CONFIG pages (their poses forwards, from Goodies' layout). From a CONFIG page the game's transition goes to the MAIN page (those poses backwards, to Goodies' layout, after which the MAIN page's panes fly in) or to the other CONFIG page. A page refreshed in place has its rows' texts and colours again. The two text sets (section 9.6) need no hold (`MH_HOLD_MAIN` and `MH_HOLD_CFG` 0): the panes flying out show the renderer's own copies of their rows, and no CONFIG page is drawn during the game's transition.

**The planet picker** (mailbox version 3, `MBOX_CAP_PICKER`). The cross on the root's Planets row, which qwark flags `MROWF_PICKER`, has the core mark the picker up and ask the renderer, which shows the Galactic Map's own look in the same pause, on a copy of the map's page (0x72D87C, screen 15) in its state under its own screen 50: the map's poses (0x63 .. 0x67), its title ("Galactic Map"), its hint ("Select", "Exit"), its planet picture (0x72D904, the gmap_ss pictures) and its galaxy (the map's draw, "Galactic Map Draw", 0x14B5F4, without the update that goes on to the planet's page), and a list in the map list's look (the scroll list's draw, flags 0x138) of the nineteen planets of qwark's PLANET_LIST, Veldin to Veldin2, each with the game's name and subtitle from the planet table (0x7352B0); planet 18 is named Veldin2 through text id 0x7E09, the game naming both Veldins alike. The cursor starts on the planet the player is on and sets the map's selected planet (0xA206E4), which the galaxy labels and the picture shows. The map's planet page (0x72DC78) and its loader of planet maps (0x72DBC8) are left out: both use memory the pause lays out only when it opens on the map (0x13B108, screens 15 and 11).

Moving: the root's panes fly out, then the game's transition goes to the map page as the root's parent, which plays the current page's poses backwards, the map's for that moment, so the panes fly in to the map's layout (0x13C03C: the next page the current one's parent; 0x13C10C .. 0x13C150: its poses from their last frame at -1.0). Triangle flies the map's panes out, and the transition back to the root as the map page's parent, on the main page's poses, flies the root's panes in, the cursor on Planets. The menu's screen stays started while the picker is up, hstate open, the core taking no page and no press, so the push is no close; back on the root the renderer takes the picker down (`mh_core_picker_back`) and the menu goes on on the page it left. The cross hands the planet (0 .. 18) to the core (`mh_core_pick`: `pick_planet`, then `pick_seq` one up, and the menu counts as closed) and closes the pause, a plain close (0x13C208: only screens 15 and 16 travel); qwark loads the planet once the game is back in play. Start, Select, R3 and the combo's close request (`mh_core_close_request`) close the pause with no pick (`mh_core_picker_gone`), and so does any other close of the pause while the picker is up, which the play hook sees. Up and Down, L1 and R1 move the cursor without wrapping, as the map list does, with the cursor's tick (1); the cross plays the select click (0), Triangle the page sound (4). In play the renderer puts the pages' poses and the selected planet back as they were. The picker adds no word to the install: it is data in the renderer's state, calling the game through its descriptors, and the game's own map, reached from the ship, is never touched. Its counters, in the renderer's own words: +0x7BC the times it opened, +0x7C0 picks, +0x7C4 the planet last picked (0x160C60C .. 0x160C617).

`row_cap` is 24, `row_size` 0x40, `caps` all five bits (the picker's since version 3).

| Address | Replaces | Becomes |
|---|---|---|
| 0x07B7C8 | `lis r3,0x72`, get_ui_string's not-found path | `b` to the text stub: text id 0x7E00 is "Trainer", 0x7E01 to 0x7E07 the rows of a MAIN page, 0x7E08 a CONFIG page's title and 0x7E09 the planet picker's "Veldin2", from the renderer's table; any other id, and one of these with no entry, "MESSAGE DOES NOT EXIST" (0x720B94), as before. The user's charge-boots mod hooks the same function's first word, and both work |
| 0x0E14C8 | the nop after the main loop's call of the play update (state 0) | a call of the play stub: the renderer's pages set up the first time, the menu's screen stopped when the pause has closed, and an open request taken |
| 0x729B68 | 0x4EE60003, the main page's Help entry: text 0x4EE6, kind 3 (go to a page) | 0x7E000003, "Trainer", kind 3 |
| 0x729B6C | 0x0072AF48, that entry's page, Help's | the renderer's MAIN page |

qwark writes the four words in this order, the Help entry's two last, and takes them out in the reverse order. Nothing but the main page's Help item (0x729CF0) names that entry, so no other list changes.

The code is in four runs of code nothing reaches, the text segment's tail being the user's charge-boots mod's: 0x57DD40..0x57F854 (the stubs, then the core, then shim functions), 0x4FA800..0x4FB500, 0x5F0AA0..0x5F1788 and 0x609CF4..0x60A9B4 (the rest of the shim's functions, which the build spreads over the areas, the largest first). Each runs from a function's start to the next's; nothing outside it branches into it, no lis/addi pair names it, and the only words naming it are its own functions' descriptors, which nothing names. The mailbox is at 0x160A000..0x160A800 (0x680 bytes of it since round 4) and the state (the core's two text sets, then the renderer's own: its pages, items and text table), the read-only data, the TOC and the descriptors at 0x160B000..0x160F000, past the end of its read-write segment (0x1606310). None of them overlaps any mod known to the client, nor qwark's own writes (the savefile helper's caves, hook and request bytes, the code switches, the aside buffer), and all keep clear of the hardware probes' areas (probe A's at 0x50D240 and 0x1607000, N1's, N1b's and N1c's at 0x53C800, 0x628110 and 0x1608000). N1, N1b and N1c hook the same words by design, so the install refuses while one is loaded.

**Opening.** From the main pause's Help row, which says Trainer: the game's transition goes to the menu's MAIN page (the main page's poses forwards, all seven panes out), and the root's panes fly in. Back at the menu's root flies them out and goes back to the main page (the main page's poses backwards, all seven in), the cursor on Trainer. From the combo: in a frame of play in which Start itself could open the pause (state 0, eight frames into it, the player not in states 29, 50 or 114, the hand not locked, 0x96BD91 clear, health above 0, no debug camera, no planet load and no reopen queued, and no load of qwark's savefile helper waiting), the play stub takes the open request and writes the game's own reopen fields (0xA52CDC from 0x969CD4, the screen 50 at 0xA52CA0, then the menu's MAIN page at 0xA52C9C), which the game uses after a Goodies movie: the next frame of play opens the pause on the menu's root. Fields the game has not taken half a second later, or by a frame outside play, are taken back. Back at a root the combo opened closes the pause. Nothing opens where the pause cannot: in a cutscene, the Galactic Map, a movie or a load.

**The controls.** On a MAIN page: Up and Down the generic list's own, from pane to pane; the cross, Left, Right, L1 and R1 on the pane the focus is on; Triangle back; Start, Select and R3 close the pause, the list's own close. On a CONFIG page: Up and Down as above, with the cursor's tick; the cross, Left, Right, L1 and R1 on the cursor's row; Triangle back; Start, Select and R3 close. Sounds are play_moby_sound's (0x15D12C): the select click (0) for the cross, the cursor's tick (1) for a step or a window turned, the page sound (4) for back between MAIN pages; the game's transitions play their own. The confirm button is the game's (`*0x902ED4`: the cross, or the circle as well in Japanese). Presses are the pad's edges, 0x964B04 (pad +0xC4).

**When qwark does not answer**, as in the other games: "Waiting for qwark..." in a CONFIG page's title after half a second, and before any page one grey row, "Waiting For Qwark". Whenever the menu closes, the next opening, from Trainer or the combo, shows the root with the cursor on its first row (section 9.1).

**Switching off.** `menu.enabled = 0` takes the words out once the menu is down, the game's state (0xA10708) is not 3, no reopen of the pause is queued (0xA52C9C is 0) and the renderer's own word at 0x160BE54 (round 4; it moved with the state) says none of its pages is up.

The renderer's state begins with the core's counters, at 0x160B000, as section 9.5 lists them: +0x04 the times its screen started, +0x08 open requests the play stub took, +0x0C the times the pause opened on the menu for one, +0x10 events, +0x14 pages, +0x18 pages caught half written, +0x1C presses dropped. Its own words at 0x160BE50 (round 4; 0x160B9D0 before): +0x1C the text stub's answers, +0x20 opens written and taken back unused, +0x24 rows shrunk or cut short to fit, +0x28 the first one-row pane's width and height, +0x2C the CONFIG list pane's (two halves each, in the game's draw units).

From the NPEA00385 executable and probes N1, N1b and N1c on hardware. The renderer's first version, with Camera Options' layout on its CONFIG pages, ran on the user's console (2026-10-19); Cheats' layout has run only as probe N1c's. The disc trilogy's RaC1 (BCES01503) shares RaC1's addresses, and gets the renderer only if every one of the four words holds the game's own.
