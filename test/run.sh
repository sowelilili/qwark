#!/bin/sh
#
# Builds and runs the qwark host unit tests. Run from Git Bash.
#
set -e

cd "$(dirname "$0")/.."

CLANG="${QWARK_CLANG:-C:/ghcup/ghc/9.4.7/mingw/bin/clang.exe}"
OUT="test/qwark-test.exe"
ROOT="test/qwark-host-root"

SRC="src/core/util.c src/core/mem.c src/core/config.c src/core/features.c \
     src/core/mods.c src/core/session.c src/core/net.c src/core/autosplit.c src/core/manips.c \
     src/core/savefile.c src/core/switches.c src/core/menu.c src/core/uipak.c \
     src/games/sfhelper_bins.c src/games/switches_bins.c src/games/menuhelper_bins.c \
     src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
     src/games/rac2.c src/games/rac2_panel.c \
     src/games/rac3.c src/games/rac3_panel.c \
     src/games/rac4.c src/games/rac4_panel.c \
     src/games/games.c \
     src/plat/host/plat_host.c src/plat/host/backend_fake.c \
     src/plat/host/backend_pine.c \
     test/test_game.c test/test_pine.c test/test_menu.c test/test_menu_core.c test/test_menu_uya.c test/test_menu_rac2.c test/test_menu_rac1.c test/test_vshmem.c test/test_menu_help.c test/test_uipak.c \
     test/test_combo_fastloads.c test/test_qwark.c"

# backend_fake.c owns the plat_* game entry points here, so backend_pine.c is
# built without its own set and the PINE tests drive its pine_* half directly.
# QWARK_EMULATOR, as in build-host.sh: the tests cover the RPCS3 paths too.
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -DQWARK_HOST -DQWARK_EMULATOR -DQWARK_TEST -DQWARK_PINE_NO_PLAT"
# -static folds winpthreads into the executable. Without it both programs import
# libwinpthread-1.dll, which only exists on a PATH that has a mingw bin folder
# on it; started by the PC client from anywhere else, qwark-rpcs3.exe died at
# once with STATUS_DLL_NOT_FOUND (0xC0000135) and the client reconnected for ever.
LDFLAGS="-static -lws2_32 -lwinmm -lpthread"

echo "compiling $OUT"
"$CLANG" $CFLAGS $SRC -o "$OUT" $LDFLAGS

# A fresh fake HDD every run, with the real racman mod fixtures installed.
rm -rf "$ROOT"
mkdir -p "$ROOT/dev_hdd0/qwark/mods"
cp -r test/fixtures/mods/. "$ROOT/dev_hdd0/qwark/mods/"

# Build 55: the console menu's renderers as a console reads them, ui.pak in the
# fake HDD, packed by the same pak.py `make` runs. test_uipak.c makes its own
# bad copies of it (another build, cut short, damaged) and puts this one back.
PAK=""
for PY in python python3 py; do
	if "$PY" -I src/games/menuhelper/pak.py --out "$ROOT/dev_hdd0/qwark/ui.pak" >/dev/null 2>&1; then
		PAK="$ROOT/dev_hdd0/qwark/ui.pak"
		break
	fi
done
if [ -z "$PAK" ]; then
	echo "test/run.sh: no python to pack ui.pak with (src/games/menuhelper/pak.py)" >&2
	exit 2
fi

# A free port for this run. The suite used to listen on 9673, which is the port
# a qwark-rpcs3.exe serving the user's own client already holds; the bind
# succeeded anyway and the tests talked to that process. The port only has to be
# free for the moment the listener opens, so asking the OS for one and letting it
# go again is enough. Set QWARK_TEST_PORT yourself to pin it; with neither, the
# suite falls back to 9673 and a bare run still works.
PICK='import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'

if [ -z "${QWARK_TEST_PORT:-}" ]; then
	for PY in python python3 py; do
		QWARK_TEST_PORT=$("$PY" -c "$PICK" 2>/dev/null) && break
		QWARK_TEST_PORT=""
	done
fi
export QWARK_TEST_PORT

if [ -n "$QWARK_TEST_PORT" ]; then
	echo "running $OUT on port $QWARK_TEST_PORT"
else
	echo "running $OUT (no python to pick a port, falling back to 9673)"
fi
"./$OUT"
