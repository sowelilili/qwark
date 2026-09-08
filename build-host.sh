#!/bin/sh
#
# Builds qwark-host.exe, the simulator the PC client is developed against.
# Run from Git Bash. The compiler is the clang that ships with GHC on this
# machine; it targets x86_64-w64-windows-gnu and has winsock and winpthreads.
#
set -e

cd "$(dirname "$0")"

CLANG="${QWARK_CLANG:-C:/ghcup/ghc/9.4.7/mingw/bin/clang.exe}"
OUT="${1:-qwark-host.exe}"

SRC="src/core/util.c src/core/mem.c src/core/config.c src/core/features.c \
     src/core/mods.c src/core/session.c src/core/net.c \
     src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
     src/games/rac2.c src/games/rac2_panel.c \
     src/games/rac3.c src/games/rac3_panel.c \
     src/games/rac4.c src/games/rac4_panel.c \
     src/games/games.c \
     src/plat/host/plat_host.c src/plat/host/host_main.c"

CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DQWARK_HOST"
LDFLAGS="-lws2_32 -lwinmm -lpthread"

echo "compiling $OUT"
"$CLANG" $CFLAGS $SRC -o "$OUT" $LDFLAGS
echo "built $OUT"
