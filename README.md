# qwark

qwark is a PS3 VSH plugin (SPRX) that runs the RaCMAN trainer on the console itself: game detection, feature toggles, freezes, instruction patches, controller combos, position slots, mods and a small network protocol for the PC client, [RaCMAN Reloaded](../racman-reloaded/). It replaces Ratchetron plus the per-game autosplitter modules. The architecture is described in [DESIGN.md](../DESIGN.md); the wire contract in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Supported games in this release: Ratchet & Clank (NPEA00385), Going Commando (NPEA00386), Up Your Arsenal (NPEA00387), Deadlocked (NPEA00423), and the BCES01503 trilogy collection.

## Requirements

- A jailbroken PS3 with webMAN MOD (full version) and PS3MAPI, the same setup Ratchetron needs.
- The console and the PC on the same network.

## Installing

1. Copy `qwark.sprx` to the console, for example to `/dev_hdd0/tmp/qwark.sprx` over FTP. RaCMAN Reloaded's Connection panel does this for you and loads the plugin through webMAN.
2. To load it by hand: open `http://<ps3-ip>/vshplugin.ps3mapi?prx=%2Fdev_hdd0%2Ftmp%2Fqwark.sprx&load_slot=5` in a browser. An XMB notification confirms the load.
3. Only once it has proven stable on your console: add the path to `/dev_hdd0/boot_plugins.txt` so it loads at boot. A VSH plugin that crashes at boot is only recoverable by disabling plugins, so do not do this first.

qwark listens on TCP port 9673 and streams telemetry over UDP to connected clients. It can coexist with Ratchetron (9671) and the old autosplitter modules (9672).

## Save files

qwark carries a small savefile helper for each of the four games: a few hundred bytes of PowerPC code, built from one source in `src/games/sfhelper/` and embedded in the module. The first time a client asks anything about save files, qwark writes that code into a code cave in the running game and branches the game into it; from then on the game calls it once a frame and it does nothing until asked. The user never loads a mod for it and never sees it happen.

Asked to set a save aside, the helper copies the game's live save buffer into a spare region of the game's own memory. Loading is the reverse: something fills that region and the helper hands it to the game's own loader. The old `tempsave` under `USRDIR` is gone, and the old `sfhelper`, `rc2-save`, `rc3-save` and `rc4-save` mods are not needed and should not be loaded alongside it.

The saves themselves live on the console, under `/dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav`, with the CRC32 of each one beside it as `<name>.sav.sum`. qwark copies between one of those files and the aside buffer itself, 64 KB at a time on its own tick thread, so a save that is already on the console is loaded without a byte of it crossing the network: a save is up to 2 MB, and it used to be streamed from the PC on every single load. The PC keeps a mirror of the library so nothing is lost if the console is wiped, and a file that only exists on the PC is uploaded once, the first time it is used. See PROTOCOL.md section 5.13.

The code and the addresses are ported from those mods and from their upstream sources, which are credited in the per-game headers under `src/games/sfhelper/`. Because it is code, it does not work under RPCS3 (see below).

## What it does without a PC

Everything that was configured last time keeps working with no client attached: combos, auto-applied mods and toggles, position slots. The console is the single source of truth; the client only displays and requests.

## Files on the console

All under `/dev_hdd0/qwark/`, plain text so they can be edited by hand:

| Path | Contents |
|---|---|
| `config.txt` | `key = value` lines: combos, per-game auto flags for toggles and mods, selected slot and planet, `log = 1`, `trace_ops`, `savefile_helper` |
| `positions/<game>.txt` | one line per position slot, `<planet>.<slot> = <hex bytes>`; keyed on the game (`rac1` to `rac4`) so the disc collection and the PSN release share slots |
| `mods/<TITLEID>/<mod>/` | mods uploaded by the client, same format as RaCMAN's `patch.txt` folders |
| `savefiles/<TITLEID>/<category>/` | the savefile library: `<name>.sav` and its `<name>.sav.sum`, eight hex digits of CRC32 |
| `qwark.log` | state transitions, when `log = 1` |

Two of them are for working out why a console is crashing, and both are off by default. `trace_ops = 1` puts two lines in the log for every request a client makes, one as it arrives and one when it is answered with what it cost the game, so the last line before a crash names the operation. It costs a file write per line, which is why it is not on. `savefile_helper = 0` stops qwark ever writing the savefile helper into a game; the game then reports as having none and the client hides the panel.

## RPCS3

The same core also runs on the PC as `qwark-rpcs3.exe`, driving [RPCS3](https://rpcs3.net/) through its PINE IPC server instead of a console through PS3MAPI. There is no SPRX to load and no console on the network: the client connects to `127.0.0.1:9673` exactly as it connects to a PS3.

**Turn RPCS3's IPC server on.** Settings, I/O tab, tick **Enable IPC server**; the port beside it is 28012 unless you change it. RPCS3 binds it to 127.0.0.1 only, so nothing outside the machine can reach it.

**Run it.**

```
qwark-rpcs3.exe [--pine-port 28012] [--port 9673] [--root DIR]
```

| Option | Meaning |
|---|---|
| `--pine-port N` | RPCS3's IPC port, if it is not 28012 |
| `--port N` | the port qwark listens on for the client, 9673 by default |
| `--root DIR` | where `/dev_hdd0` is mapped. By default a `qwark-rpcs3-root` folder beside the executable, holding the same `dev_hdd0/qwark/` layout the console uses: `config.txt`, `positions/`, `mods/` |
| `--pine-timeouts W,L,R` | the PINE clocks in milliseconds, for the tests only: how long a caller waits for a reply (100), how long a silent link lives (5000), how long a silent RPCS3 is left alone (5000) |

It prints one banner line with the versions and both ports, then waits. `status` and `version` on standard input print a line each; `exit` or Ctrl-C shuts it down. If RPCS3 is not running, or has no game booted, qwark reports XMB and retries the socket once a second, so the two can be started in either order.

qwark follows RPCS3's own status: a game counts as up while the emulator reports **Running** or **Paused** and a title id. Pausing the emulator changes nothing (the game, its memory and its title id are all still there); stopping the game, or closing RPCS3, ends the session, and booting again starts a new one that offers the previous-session record back, exactly as a real reboot would.

**One client at a time.** RPCS3's IPC server accepts a connection and serves it until it goes away; anyone else who connects meanwhile completes the TCP handshake and then hears nothing until the first client leaves. So if another program is on the port (a second copy of `qwark-rpcs3.exe`, say, or any other PINE tool), qwark connects, gets no answer, and after five seconds says so: `pine: RPCS3 accepted the connection but has not answered in 5 s; another program is probably connected to its IPC server`. It then stays out of the queue for five seconds at a time and picks the game up on its own once the port is free. The PC client stays fully served meanwhile: PINE traffic lives on a worker thread, and the tick thread never waits more than a tenth of a second for it. The same holds for an emulator that stalls: the last known state stands until a reply comes or the link has been silent for five seconds.

**What does not work.** RPCS3 recompiles PPU code ahead of executing it, so writing an instruction word into memory changes the word and nothing else: the game carries on running the translated block. qwark refuses everything that depends on a code patch rather than pretending it worked, and tells the client so in the session flags, which greys those rows:

- toggles marked "writes code" — fast loads, infinite ammo and infinite health in RaC1, Deadlocked's crash patches and refill ammo, and their equivalents in RaC2 and RaC3
- mods, all of them: a mod is patch words, code caves, or both
- PATCH_APPLY from the client's own patch panel
- the save-file manager: the helper behind it is a code cave and a branch into it, so the client hides its Save files panel here

Everything that is a plain data write or a freeze works exactly as it does on a console: health, bolts, unlocks, level flags, positions, colours, planet loads, combos, watches, freezes, the live toggles, and the autosplitter. Two autosplit details differ, because the helpers behind them are code patches: RaC1's four collectable splits (gold bolt, skill point, item, infobot) never fire, and Deadlocked's quit still pauses the timer — the session notices the game vanish on its own — but resumes on the way back into the game rather than on the SCE logo.

## Building

PS3 module: the PS3 toolchain with `CELL_SDK` set and Cygwin, as for Ratchetron. Run `_Make.bat`, or `make` from a Cygwin login shell in this directory. The output is `qwark.sprx`.

Host builds, which need no console: `build-host.sh` builds both `qwark-host.exe` and `qwark-rpcs3.exe` with the clang listed in `CLAUDE.md` (`build-host.sh host` or `build-host.sh rpcs3` for one of them). `test/run.sh` runs the unit tests; `python test/smoke.py` drives both executables through the full wire protocol, the second against the fake PINE server in `test/fake_pine.py`.

The savefile helper is built separately, because it is PowerPC code that runs inside the *game* rather than inside qwark: `make sfhelper` compiles `src/games/sfhelper/sfhelper.c` once per game at that game's cave address and regenerates `src/games/sfhelper_bins.c`, which is committed. A plain `make` never needs to do that, but it does rebuild it when one of the helper's sources is newer, so an edit cannot be left out of a module by accident. The host builds and the tests compile the committed file and need no SDK.

`make dist` copies the built `qwark.sprx` and `qwark-rpcs3.exe` into `dist/`, which is committed: the PC client's release packaging takes both from there rather than from a working tree that may or may not have been built.

`qwark-host.exe` is the simulator: it takes `boot <TITLEID>`, `quit`, `pad <hex>`, `poke`, `peek`, `status` and `exit` on standard input and serves the same protocol as the real module, so the PC client can be developed against it with neither a console nor an emulator. The two executables share everything but one file: `src/plat/host/plat_host.c` is the common half, `backend_fake.c` is the fake console and `backend_pine.c` is RPCS3.

## Licence

The VSH and PS3MAPI glue under `src/plat/ps3/` and the build files come from webMAN MOD by way of Ratchetron and carry its GPL v3 notice.
