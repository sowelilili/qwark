# qwark

PS3 VSH plugin (SPRX) that owns the RaCMAN trainer logic for the four original Ratchet & Clank games. Read `../DESIGN.md` (architecture, section 3) and `docs/PROTOCOL.md` (the wire contract) before changing anything.

## Build

PS3 (`qwark.sprx`): run `_Make.bat` from Windows, or in a Cygwin login shell:

```
/c/cygwin64/bin/bash.exe --login -c "cd '/cygdrive/c/Users/atune/Documents/RaCMAN Development/qwark' && make"
```

Do not override `CELL_SDK`. Delete the `.prx` and `.sym` leftovers after a build. The compiler is ppu-lv2-gcc 4.1.1: gnu99 only, no C11, no `_Static_assert`, no anonymous unions, no floating-point printf (`printf2` from `printf.c` handles integers and strings only).

Host (`qwark-host.exe`, the simulator the PC client is developed against): `./build-host.sh`
from Git Bash, or `build-host.bat` from Windows. It uses

```
C:\ghcup\ghc\9.4.7\mingw\bin\clang.exe   (x86_64-w64-windows-gnu; link -lws2_32 -lwinmm -lpthread)
```

Host tests: `./test/run.sh` builds and runs the unit tests against the host platform.
Integration: `python test/smoke.py` starts `qwark-host.exe`, drives its fake console over
stdin, talks the real wire protocol to it and prints PASS/FAIL per step.

## Rules

- Only the tick thread in `src/core/session.c` calls `plat_mem_read` / `plat_mem_write`. Network threads post commands to the ring.
- Nothing under `src/core/` or `src/games/` includes a PS3 header; everything platform-specific goes through `src/plat/plat.h`.
- Static tables, no `malloc` in steady state. Stack budgets: 48 KB tick thread, 16 KB per client thread, so no large locals.
- Every multi-byte integer on the wire is big-endian; the PS3 is big-endian, the host is not, so use the `be16`/`be32`/`be64` helpers everywhere.
- Game knowledge (addresses, patch words, planet names) lives only in `src/games/<game>*`. A game may split into `<game>.c` (numbers, hot block, descriptors, vtable), `<game>.h` (the address table and the seam) and `<game>_panel.c` (the handlers), as RaC1 does. `classic.c` holds only what all four games genuinely share.
- Do not commit or add attribution trailers unless asked.
