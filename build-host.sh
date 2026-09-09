#!/bin/sh
#
# Builds the two host executables. Run from Git Bash. The compiler is the clang
# that ships with GHC on this machine; it targets x86_64-w64-windows-gnu and has
# winsock and winpthreads.
#
#   qwark-host.exe    the simulator the PC client is developed against: the same
#                     core over a fake console driven from stdin
#   qwark-rpcs3.exe   the same core over RPCS3's PINE IPC server
#
# Both link src/plat/host/plat_host.c and differ only in the backend beside it.
#
#   ./build-host.sh                 both
#   ./build-host.sh host            only qwark-host.exe
#   ./build-host.sh rpcs3           only qwark-rpcs3.exe
#   ./build-host.sh host out.exe    qwark-host.exe under another name
#
set -e

cd "$(dirname "$0")"

CLANG="${QWARK_CLANG:-C:/ghcup/ghc/9.4.7/mingw/bin/clang.exe}"

CORE="src/core/util.c src/core/mem.c src/core/config.c src/core/features.c \
      src/core/mods.c src/core/session.c src/core/net.c src/core/autosplit.c \
      src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
      src/games/rac2.c src/games/rac2_panel.c \
      src/games/rac3.c src/games/rac3_panel.c \
      src/games/rac4.c src/games/rac4_panel.c \
      src/games/games.c \
      src/plat/host/plat_host.c"

HOST_SRC="$CORE src/plat/host/backend_fake.c src/plat/host/host_main.c"
RPCS3_SRC="$CORE src/plat/host/backend_pine.c src/plat/host/rpcs3_main.c"

CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DQWARK_HOST"
# -static folds winpthreads into the executable. Without it both programs import
# libwinpthread-1.dll, which only exists on a PATH that has a mingw bin folder
# on it; started by the PC client from anywhere else, qwark-rpcs3.exe died at
# once with STATUS_DLL_NOT_FOUND (0xC0000135) and the client reconnected for ever.
LDFLAGS="-static -lws2_32 -lwinmm -lpthread"

build_host() {
	out="${1:-qwark-host.exe}"
	echo "compiling $out"
	"$CLANG" $CFLAGS $HOST_SRC -o "$out" $LDFLAGS
	echo "built $out"
}

build_rpcs3() {
	out="${1:-qwark-rpcs3.exe}"
	echo "compiling $out"
	"$CLANG" $CFLAGS $RPCS3_SRC -o "$out" $LDFLAGS
	echo "built $out"
}

case "${1:-all}" in
	host)  build_host "$2" ;;
	rpcs3) build_rpcs3 "$2" ;;
	all)   build_host; build_rpcs3 ;;
	*)
		# Backwards compatible: a bare argument is still the host output name.
		build_host "$1"
		;;
esac
