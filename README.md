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

## What it does without a PC

Everything that was configured last time keeps working with no client attached: combos, auto-applied mods and toggles, position slots. The console is the single source of truth; the client only displays and requests.

## Files on the console

All under `/dev_hdd0/qwark/`, plain text so they can be edited by hand:

| Path | Contents |
|---|---|
| `config.txt` | `key = value` lines: combos, per-game auto flags for toggles and mods, selected slot and planet, `log = 1` |
| `positions/<game>.txt` | one line per position slot, `<planet>.<slot> = <hex bytes>`; keyed on the game (`rac1` to `rac4`) so the disc collection and the PSN release share slots |
| `mods/<TITLEID>/<mod>/` | mods uploaded by the client, same format as RaCMAN's `patch.txt` folders |
| `qwark.log` | state transitions, when `log = 1` |

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

It prints one banner line with the versions and both ports, then waits. `status` and `version` on standard input print a line each; `exit` or Ctrl-C shuts it down. If RPCS3 is not running, or has no game booted, qwark reports XMB and retries the socket once a second, so the two can be started in either order.

qwark follows RPCS3's own status: a game counts as running only while the emulator reports **Running** and a title id. Pausing the emulator therefore looks like the game quitting and un-pausing like it booting again — the session bumps its generation and offers the previous-session record back, exactly as a real reboot would.

**What does not work.** RPCS3 recompiles PPU code ahead of executing it, so writing an instruction word into memory changes the word and nothing else: the game carries on running the translated block. qwark refuses everything that depends on a code patch rather than pretending it worked, and tells the client so in the session flags, which greys those rows:

- toggles marked "writes code" — fast loads, infinite ammo and infinite health in RaC1, Deadlocked's crash patches and refill ammo, and their equivalents in RaC2 and RaC3
- mods, all of them: a mod is patch words, code caves, or both
- PATCH_APPLY from the client's own patch panel

Everything that is a plain data write or a freeze works exactly as it does on a console: health, bolts, unlocks, level flags, positions, colours, planet loads, combos, watches, freezes, the live toggles, and the autosplitter. Two autosplit details differ, because the helpers behind them are code patches: RaC1's four collectable splits (gold bolt, skill point, item, infobot) never fire, and Deadlocked's quit still pauses the timer — the session notices the game vanish on its own — but resumes on the way back into the game rather than on the SCE logo.

## Building

PS3 module: the PS3 toolchain with `CELL_SDK` set and Cygwin, as for Ratchetron. Run `_Make.bat`, or `make` from a Cygwin login shell in this directory. The output is `qwark.sprx`.

Host builds, which need no console: `build-host.sh` builds both `qwark-host.exe` and `qwark-rpcs3.exe` with the clang listed in `CLAUDE.md` (`build-host.sh host` or `build-host.sh rpcs3` for one of them). `test/run.sh` runs the unit tests; `python test/smoke.py` drives both executables through the full wire protocol, the second against the fake PINE server in `test/fake_pine.py`.

`qwark-host.exe` is the simulator: it takes `boot <TITLEID>`, `quit`, `pad <hex>`, `poke`, `peek`, `status` and `exit` on standard input and serves the same protocol as the real module, so the PC client can be developed against it with neither a console nor an emulator. The two executables share everything but one file: `src/plat/host/plat_host.c` is the common half, `backend_fake.c` is the fake console and `backend_pine.c` is RPCS3.

## Status

The core and the four game modules build cleanly and pass the host tests, but nothing has run on a console yet. Treat the first live sessions as testing.

## Licence

The VSH and PS3MAPI glue under `src/plat/ps3/` and the build files come from webMAN MOD by way of Ratchetron and carry its GPL v3 notice.
