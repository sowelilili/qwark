#!/bin/sh
#
# Builds and runs the installer's host tests: the same bootlist.c and install.c
# the console's EBOOT.BIN is built from, over a folder that stands in for the
# console's HDD (ifs_host.c). Run from Git Bash; test/run.sh runs it too.
#
set -e

cd "$(dirname "$0")/../.."

CLANG="${QWARK_CLANG:-C:/ghcup/ghc/9.4.7/mingw/bin/clang.exe}"
OUT="installer/test/installer-test.exe"
ROOT="installer/test/root"

# The build the package carries, as the makefile reads it.
BUILD=$(sed -n 's/^#define[ 	]*QWARK_BUILD[ 	]*\([0-9][0-9]*\).*/\1/p' src/core/proto.h)
if [ -z "$BUILD" ]; then
	echo "installer/test/run.sh: no QWARK_BUILD in src/core/proto.h" >&2
	exit 2
fi

# The SDK's own warnings, as far as clang has them, so the host build finds
# what the console build would complain about.
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings \
        -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wno-format-nonliteral \
        -DINST_QWARK_BUILD=$BUILD"

echo "compiling $OUT (build $BUILD)"
"$CLANG" $CFLAGS -Wno-missing-prototypes \
	installer/bootlist.c installer/install.c installer/test/ifs_host.c installer/test/test_installer.c \
	-o "$OUT" -static

rm -rf "$ROOT"
"./$OUT" "$ROOT"
