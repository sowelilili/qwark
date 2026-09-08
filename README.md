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

## Building

PS3 module: the PS3 toolchain with `CELL_SDK` set and Cygwin, as for Ratchetron. Run `_Make.bat`, or `make` from a Cygwin login shell in this directory. The output is `qwark.sprx`.

Host simulator and tests, which need no console: `build-host.sh` builds `qwark-host.exe` with the clang listed in `CLAUDE.md`; `test/run.sh` runs the unit tests; `python test/smoke.py` drives the simulator through the full wire protocol. The simulator takes `boot <TITLEID>`, `quit`, `pad <hex>`, `poke`, `peek`, `status` and `exit` on standard input and serves the same protocol as the real module, so the PC client can be developed against it.

## Status

The core and the four game modules build cleanly and pass the host tests, but nothing has run on a console yet. Treat the first live sessions as testing.

## Licence

The VSH and PS3MAPI glue under `src/plat/ps3/` and the build files come from webMAN MOD by way of Ratchetron and carry its GPL v3 notice.
