# qwark

qwark is a PS3 VSH plugin (SPRX) that runs the RaCMAN trainer on the console itself: game detection, feature toggles, freezes, instruction patches, controller combos, position slots, mods and a small network protocol for the PC client, [RaCMAN Reloaded](../racman-reloaded/). It replaces Ratchetron plus the per-game autosplitter modules. The architecture is described in [DESIGN.md](../DESIGN.md); the wire contract in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Supported games in this release: Ratchet & Clank (NPEA00385), Going Commando (NPEA00386, and the RC2ILYOOO and BLMODYOOO repacks of it), Up Your Arsenal (NPEA00387), Deadlocked (NPEA00423), and the BCES01503 trilogy collection.

Every game has Freeze health under Cheats, which holds health at the player's current maximum, whatever the health upgrades have made it.

Anything else the console runs still gets the memory tools: a game qwark has no table for comes up with its title id and no features, and the memory viewer, watches, freezes and patches work on it.

## Requirements

- A jailbroken PS3 with webMAN MOD (full version) and PS3MAPI, the same setup Ratchetron needs.
- The console and the PC on the same network.

## Installing

1. Copy `qwark.sprx` to the console, for example to `/dev_hdd0/tmp/qwark.sprx` over FTP. RaCMAN Reloaded's Connection panel does this for you and loads the plugin through webMAN.
2. To load it by hand: open `http://<ps3-ip>/vshplugin.ps3mapi?prx=%2Fdev_hdd0%2Ftmp%2Fqwark.sprx&load_slot=5` in a browser. An XMB notification confirms the load.
3. Only once it has proven stable on your console: add the path to `/dev_hdd0/boot_plugins.txt` so it loads at boot. A VSH plugin that crashes at boot is only recoverable by disabling plugins, so do not do this first.

qwark listens on TCP port 9673 and streams telemetry over UDP to connected clients. It can coexist with Ratchetron (9671) and the old autosplitter modules (9672).

## When a game launches

The moment a game process appears, qwark leaves the console alone for one second: nothing reads the process, the XMB is not asked for its title, and nothing moves on the network, telemetry included. Ratchetron and the old autosplitter modules kept the same silence, and consoles that crashed at launch without it are why. After that second the fingerprint runs and the session comes up, usually within a second or two of the launch. Until it does, a client's small requests (the handshake, heartbeats, the state poll) keep working and anything larger is answered BUSY, and a connection made or a large request already in flight during the switch is dropped, which the client's reconnect handles. PROTOCOL.md section 1.1 has the exact rules.

## Save files

qwark carries a small savefile helper for each of the four games: a few hundred bytes of PowerPC code, built from one source in `src/games/sfhelper/` and embedded in the module. The first time an action needs the savefile helper, qwark writes that code into a code cave in the running game and branches the game into it; from then on the game calls it once a frame and it does nothing until asked. The user never loads a mod for it and never sees it happen.

Asked to set a save aside, the helper copies the game's live save buffer into a spare region of the game's own memory. Loading is the reverse: something fills that region and the helper hands it to the game's own loader. In Ratchet & Clank 1, 2 and 3, a load first takes the game into its pause, from whatever it is doing, and goes from there, and the three refuse a set-aside, or a load with nothing set aside, until they have a save of their own (on a new game, the first autosave). The old `tempsave` under `USRDIR` is gone, and the old `sfhelper`, `rc2-save`, `rc3-save` and `rc4-save` mods are not needed and should not be loaded alongside it.

The saves themselves live on the console, under `/dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav`, with the CRC32 of each one beside it as `<name>.sav.sum`. qwark copies between one of those files and the aside buffer itself, 64 KB at a time on its own tick thread, so a save that is already on the console is loaded without a byte of it crossing the network: a save is up to 2 MB, and it used to be streamed from the PC on every single load. The PC keeps a mirror of the library so nothing is lost if the console is wiped, and a file that only exists on the PC is uploaded once, the first time it is used. See PROTOCOL.md section 5.13.

The code and the addresses are ported from those mods and from their upstream sources, which are credited in the per-game headers under `src/games/sfhelper/`. Because it is code, qwark cannot put it into a game under RPCS3 itself; an RPCS3 patch can (see below).

## Trophies (Up Your Arsenal)

UYA runs use trophies: a trophy the console has not seen before pops up, and that holds the next load back. So the trophies a run relies on have to be won again in every run, and qwark can reset them, as the old trainer's Reset Trophies button did with the rc3-save mod.

**A reset deletes your console's UYA trophy data.** qwark deletes the game's trophy folder, `/dev_hdd0/home/<user>/trophy/NPWR02347_00`, which is where the console records the trophies you have won: every UYA trophy you have, the platinum included, is gone from the console afterwards. Copy the folder off first if you want them back later. If you are signed in to PSN, the trophy server may give them back.

The game lets go of its trophies first, qwark deletes the folder, and then the game re-reads its trophies, which takes a few seconds while it keeps running; in the run that follows it awards three trophies again, the RY3NO, the Metropolis blimp and the snowman skin, and no others, as the old mod did.

- **Reset trophies** does it now.
- **Reset trophies on Veldin load**, off by default, does it once per run, starting inside the load into Veldin that starts every run, before the timer starts.

If a trophy file cannot be deleted, or the game does not get its trophies back, the console shows a notification.

qwark finds the folder from the game and the signed-in user rather than guessing it, and refuses, deleting nothing, unless the folder's own trophy configuration says it is UYA's. It only ever deletes the files in that one folder, and logs what it did. Under RPCS3 both answer unsupported: the emulator's trophy data is out of qwark's reach. PROTOCOL.md section 5.14 has the details.

## What it does without a PC

Everything that was configured last time keeps working with no client attached: combos, auto-applied mods and toggles, position slots, and the manips of the selected category, which qwark applies on its own whenever a save is loaded or a new game starts. The console is the single source of truth; the client only displays and requests.

## Files on the console

All under `/dev_hdd0/qwark/`, plain text so they can be edited by hand:

| Path | Contents |
|---|---|
| `config.txt` | `key = value` lines: combos, per-game auto flags for toggles and mods, per-game manip options (`manip.<game>.*`), selected slot, per-game selected planet and its reset flags (`selected.<game>.*`), `log = 1`, `savefile_helper` |
| `positions/<game>.txt` | one line per position slot, `<planet>.<slot> = <hex bytes>`; keyed on the game (`rac1` to `rac4`) so the disc collection and the PSN release share slots |
| `mods/<TITLEID>/<mod>/` | mods uploaded by the client, same format as RaCMAN's `patch.txt` folders |
| `savefiles/<TITLEID>/<category>/` | the savefile library: `<name>.sav` and its `<name>.sav.sum`, eight hex digits of CRC32 |
| `qwark.log` | state transitions, when `log = 1` |

The log is there for working out why a console is crashing. When the module starts with a `qwark.log` over 1 MB, that log becomes `qwark.old.log` and a new one begins, so after a crash look at the end of both. `savefile_helper = 0` stops qwark ever writing the savefile helper into a game; the game then reports as having none and the client hides the panel.

## RPCS3

The same core also runs on the PC as `qwark-rpcs3.exe`, driving [RPCS3](https://rpcs3.net/) through its PINE IPC server instead of a console through PS3MAPI. There is no SPRX to load and no console on the network: the client connects to `127.0.0.1:9673` exactly as it connects to a PS3.

**Turn RPCS3's IPC server on.** Settings, I/O tab, tick **Enable IPC server**; the port beside it is 28012 unless you change it. RPCS3 binds it to 127.0.0.1 only, so nothing outside the machine can reach it.

**Run it.**

```
qwark-rpcs3.exe [--pine-port 28012] [--port 9673] [--bind 127.0.0.1] [--root DIR]
```

| Option | Meaning |
|---|---|
| `--pine-port N` | RPCS3's IPC port, if it is not 28012 |
| `--port N` | the port qwark listens on for the client, 9673 by default |
| `--bind ADDR` | the address it listens on, `127.0.0.1` by default, which is where the client looks and which Windows Firewall never asks about. `--bind 0.0.0.0` serves a client on another machine, at the cost of that prompt |
| `--root DIR` | where `/dev_hdd0` is mapped. By default a `qwark-rpcs3-root` folder beside the executable, holding the same `dev_hdd0/qwark/` layout the console uses: `config.txt`, `positions/`, `mods/` |
| `--pine-timeouts W,L,R` | the PINE clocks in milliseconds, for the tests only: how long a caller waits for a reply (100), how long a silent link lives (5000), how long a silent RPCS3 is left alone (5000) |

It prints one banner line with the versions and both ports, then waits. `status` and `version` on standard input print a line each; `exit` or Ctrl-C shuts it down. If RPCS3 is not running, or has no game booted, qwark reports XMB and retries the socket once a second, so the two can be started in either order.

qwark follows RPCS3's own status: a game counts as up while the emulator reports **Running** or **Paused** and a title id. Pausing the emulator changes nothing (the game, its memory and its title id are all still there); stopping the game, or closing RPCS3, ends the session, and booting again starts a new one that offers the previous-session record back, exactly as a real reboot would.

**One client at a time.** RPCS3's IPC server accepts a connection and serves it until it goes away; anyone else who connects meanwhile completes the TCP handshake and then hears nothing until the first client leaves. So if another program is on the port (a second copy of `qwark-rpcs3.exe`, say, or any other PINE tool), qwark connects, gets no answer, and after five seconds says so: `pine: RPCS3 accepted the connection but has not answered in 5 s; another program is probably connected to its IPC server`. It then stays out of the queue for five seconds at a time and picks the game up on its own once the port is free. The PC client stays fully served meanwhile: PINE traffic lives on a worker thread, and the tick thread never waits more than a tenth of a second for it. The same holds for an emulator that stalls: the last known state stands until a reply comes or the link has been silent for five seconds.

**What does not work.** RPCS3 recompiles PPU code ahead of executing it, so writing an instruction word into memory changes the word and nothing else: the game carries on running the translated block. qwark refuses everything that depends on a code patch rather than pretending it worked, and tells the client so in the session flags, which greys those rows:

- toggles marked "writes code" — fast loads and infinite ammo in RaC1, Deadlocked's crash patches and refill ammo, and their equivalents in RaC2 and RaC3 — until qwark's patches are in the game (below)
- loading or unloading a mod while the game runs: a mod is patch words, code caves, or both (but see below)
- PATCH_APPLY from the client's own patch panel
- UYA's trophy reset, either way: it deletes the console's trophy data, and the emulator's is out of qwark's reach

**Save files work once the RPCS3 patch is in the game.** The savefile helper is code too, so qwark cannot write it into a running game here. RPCS3 applies its own patch files as it loads a game, though, before it compiles anything, so the client writes the helper as one: it asks qwark for the helper's words and the three request bytes the patch zeroes at load (SAVEFILE_PATCH), writes them into RPCS3's per-title patch file `patches/<TITLE_ID>_patch.yml`, enables the patch in `patch_config.yml`, and the game is restarted. qwark then finds its own helper in the game and the save-file manager works exactly as on a console. A patch written by an older qwark build no longer matches and reads as not there, so the client writes the new one and the game is restarted again. PROTOCOL.md section 5.12.1 has the details.

**The nine code checkboxes work once qwark's patches are installed.** RaC1's fast loads and infinite ammo, RaC2's fast loads and infinite ammo, RaC3's infinite ammo and fast loads, and Deadlocked's crash patches, fast loads and refill ammo are instruction patches on a console. Under RPCS3 the same RPCS3 patch file carries them as code switches: each patched instruction becomes a branch to a few instructions of qwark's that run either the patched form or the game's own, depending on a flag byte, and the checkbox writes that byte. Install qwark's patches from the Connection screen and restart the game, and the checkboxes work, auto-apply included; until then they stay greyed as before. A qwark build that changes one of them asks for the patch again, as build 53 does for RaC2's switches, Deadlocked's fast loads and RaC3's, which gained one. RaC3's fast loads are new to the list: they used to be two data words that worked here with nothing installed, and they are a code patch now (the old ones could leave the screen black for good on a trip out of Aquatos; PROTOCOL.md section 5.12.3). RaC2's patch also carries two crash guards with no checkbox, which a console gets written by qwark as the game starts: they stop a crash only qwark's own planet loads can reach. PROTOCOL.md section 5.12.2 has the details.

**Mods work the same way.** The client uploads a mod as it would to a console, asks qwark for it as patch words (MOD_PATCH), writes it into the same RPCS3 patch file and enables it, and the game is restarted. qwark then finds the mod in the game's memory and the mods list shows it loaded; right after a boot it shows "checking" for a moment while qwark looks. A mod goes in or comes out only across a restart: loading or unloading one mid-session, auto-apply and the previous-session reapply do not exist here. Mods with a Lua automation cannot be patched in. PROTOCOL.md section 5.7.1 has the details.

Everything that is a plain data write or a freeze works exactly as it does on a console: health, bolts, unlocks, level flags, positions, colours, planet loads, combos, watches, freezes, the live toggles, and the autosplitter. Two autosplit details differ, because the helpers behind them are code patches: RaC1's four collectable splits (gold bolt, skill point, item, infobot) never fire, and Deadlocked's quit still pauses the timer — the session notices the game vanish on its own — but resumes on the way back into the game rather than on the SCE logo. And RaC1's, RaC2's, RaC3's and Deadlocked's planet loads, and RaC1's, RaC2's and RaC3's set-aside loads, go out without the fast loads a console forces on for them, so they take the game's own time, unless the code switches are installed, when they are forced through the switch.

## Building

PS3 module: the PS3 toolchain with `CELL_SDK` set and Cygwin, as for Ratchetron. Run `_Make.bat`, or `make` from a Cygwin login shell in this directory. The output is `qwark.sprx`.

Host builds, which need no console: `build-host.sh` builds both `qwark-host.exe` and `qwark-rpcs3.exe` with the clang listed in `CLAUDE.md` (`build-host.sh host` or `build-host.sh rpcs3` for one of them). `test/run.sh` runs the unit tests; `python test/smoke.py` drives both executables through the full wire protocol, the second against the fake PINE server in `test/fake_pine.py`.

Both PC executables bind their listener to `127.0.0.1` by default, so no build and no test run raises a Windows Firewall prompt; `--bind ADDR` on either of them moves it, and `--bind 0.0.0.0` is the old behaviour, which the SPRX keeps because the console's client is on another machine. Neither suite uses a fixed port: `test/run.sh` picks a free one and passes it in `QWARK_TEST_PORT` (set it yourself to pin it), and `test/smoke.py` picks one for each program it starts, so a run never attaches itself to a `qwark-rpcs3.exe` that is already serving a client on 9673.

The savefile helper is built separately, because it is PowerPC code that runs inside the *game* rather than inside qwark: `make sfhelper` compiles `src/games/sfhelper/sfhelper.c` once per game at that game's cave address and regenerates `src/games/sfhelper_bins.c`, which is committed. A plain `make` never needs to do that, but it does rebuild it when one of the helper's sources is newer, so an edit cannot be left out of a module by accident. The host builds and the tests compile the committed file and need no SDK.

`make dist` copies the built `qwark.sprx` and `qwark-rpcs3.exe` into `dist/`, which is committed: the PC client's release packaging takes both from there rather than from a working tree that may or may not have been built.

`qwark-host.exe` is the simulator: it takes `boot <TITLEID>`, `quit`, `pad <hex>`, `poke`, `peek`, `status` and `exit` on standard input and serves the same protocol as the real module, so the PC client can be developed against it with neither a console nor an emulator. The two executables share everything but one file: `src/plat/host/plat_host.c` is the common half, `backend_fake.c` is the fake console and `backend_pine.c` is RPCS3.

## Licence

The VSH and PS3MAPI glue under `src/plat/ps3/` and the build files come from webMAN MOD by way of Ratchetron and carry its GPL v3 notice.
