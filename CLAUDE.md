# qwark

PS3 VSH plugin (SPRX) that owns the RaCMAN trainer logic for the four original Ratchet & Clank games. Read `../DESIGN.md` (architecture, section 3) and `docs/PROTOCOL.md` (the wire contract) before changing anything.

## Build

PS3 (`qwark.sprx`): run `_Make.bat` from Windows, or in a Cygwin login shell:

```
/c/cygwin64/bin/bash.exe --login -c "cd '/cygdrive/c/Users/atune/Documents/RaCMAN Development/rewrite/qwark' && make"
```

Do not override `CELL_SDK`. Delete the `.prx` and `.sym` leftovers after a build. The compiler is ppu-lv2-gcc 4.1.1: gnu99 only, no C11, no `_Static_assert`, no anonymous unions, no floating-point printf (`printf2` from `printf.c` handles integers and strings only).

Host: `./build-host.sh` from Git Bash, or `build-host.bat` from Windows, builds both
PC executables (`build-host.sh host` or `build-host.sh rpcs3` for one of them):

- `qwark-host.exe`, the simulator the PC client is developed against, over a fake
  console driven from stdin (`src/plat/host/backend_fake.c`)
- `qwark-rpcs3.exe`, the same core over RPCS3's PINE IPC server
  (`src/plat/host/backend_pine.c`). Code patches are refused there, see the README.

Both link `src/plat/host/plat_host.c`, which holds everything that is not the game.
The build uses

```
C:\ghcup\ghc\9.4.7\mingw\bin\clang.exe   (x86_64-w64-windows-gnu; link -lws2_32 -lwinmm -lpthread)
```

The savefile helper (`src/games/sfhelper/`) is PowerPC code that runs inside the *game*, built by `make sfhelper` into the committed `src/games/sfhelper_bins.c`. It needs the SDK; a plain `make` rebuilds it only when a source is newer, and the host builds compile the committed file. `make dist` copies `qwark.sprx` and `qwark-rpcs3.exe` into the committed `dist/`, which is where the client's release packaging takes them from.

Host tests: `./test/run.sh` builds and runs the unit tests against the host platform.
Integration: `python test/smoke.py` starts `qwark-host.exe`, drives its fake console over
stdin, talks the real wire protocol to it and prints PASS/FAIL per step; it then does the
same to `qwark-rpcs3.exe` against the fake PINE server in `test/fake_pine.py`.

## Rules

- Only the tick thread in `src/core/session.c` calls `plat_mem_read` / `plat_mem_write`. Network threads post commands to the ring.
- Nothing under `src/core/` or `src/games/` includes a PS3 header; everything platform-specific goes through `src/plat/plat.h`.
- Anything that writes an instruction word first asks `plat_can_patch_code()`. It is 0 under RPCS3, which recompiles PPU code, so patches, WRITES_CODE features, mods and the games' embedded helpers are refused there rather than half-applied. SessionInfo flags bit2 tells the client.
- Static tables, no `malloc` and no `sys_memory_allocate` anywhere: the module asks the console for no memory after it has loaded. Stack budgets: 16 KB tick thread, 16 KB per client thread, 8 KB accept thread, so no large locals. The sizes are measured, not guessed — `src/plat/ps3/qwark_ps3.h` says how.
- Every multi-byte integer on the wire is big-endian; the PS3 is big-endian, the host is not, so use the `be16`/`be32`/`be64` helpers everywhere.
- Game knowledge (addresses, patch words, planet names) lives only in `src/games/<game>*`. A game may split into `<game>.c` (numbers, hot block, descriptors, vtable), `<game>.h` (the address table and the seam) and `<game>_panel.c` (the handlers), as RaC1 does. `classic.c` holds only what all four games genuinely share.
- Do not commit or add attribution trailers unless asked.
