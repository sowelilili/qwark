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
     src/core/mods.c src/core/session.c src/core/net.c \
     src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
     src/games/rac2.c src/games/rac2_panel.c \
     src/games/rac3.c src/games/rac3_panel.c \
     src/games/rac4.c src/games/rac4_panel.c \
     src/games/games.c \
     src/plat/host/plat_host.c \
     test/test_game.c test/test_qwark.c"

CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -DQWARK_HOST -DQWARK_TEST"
LDFLAGS="-lws2_32 -lwinmm -lpthread"

echo "compiling $OUT"
"$CLANG" $CFLAGS $SRC -o "$OUT" $LDFLAGS

# A fresh fake HDD every run, with the real racman mod fixtures installed.
rm -rf "$ROOT"
mkdir -p "$ROOT/dev_hdd0/qwark/mods"
cp -r test/fixtures/mods/. "$ROOT/dev_hdd0/qwark/mods/"

echo "running $OUT"
"./$OUT"
