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
     src/core/mods.c src/core/session.c src/core/net.c src/core/autosplit.c \
     src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
     src/games/rac2.c src/games/rac2_panel.c \
     src/games/rac3.c src/games/rac3_panel.c \
     src/games/rac4.c src/games/rac4_panel.c \
     src/games/games.c \
     src/plat/host/plat_host.c src/plat/host/backend_fake.c \
     src/plat/host/backend_pine.c \
     test/test_game.c test/test_pine.c test/test_qwark.c"

# backend_fake.c owns the plat_* game entry points here, so backend_pine.c is
# built without its own set and the PINE tests drive its pine_* half directly.
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -DQWARK_HOST -DQWARK_TEST -DQWARK_PINE_NO_PLAT"
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

echo "running $OUT"
"./$OUT"
