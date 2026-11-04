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
     src/core/mods.c src/core/modusb.c src/core/session.c src/core/net.c src/core/autosplit.c src/core/runwarn.c src/core/manips.c \
     src/core/savefile.c src/core/switches.c src/core/menu.c src/core/uipak.c \
     src/games/sfhelper_bins.c src/games/switches_bins.c src/games/switches_own.c src/games/menuhelper_bins.c \
     src/games/classic.c src/games/rac1.c src/games/rac1_panel.c \
     src/games/rac2.c src/games/rac2_panel.c \
     src/games/rac3.c src/games/rac3_panel.c \
     src/games/rac4.c src/games/rac4_panel.c \
     src/games/tod.c src/games/tod_game.c src/games/tod_panel.c src/games/tod_flags.c src/games/tod_autosplit.c src/games/tod_save.c src/games/tod_cache.c src/games/luahelper_bins.c \
     src/core/luacore.c src/core/luamb.c src/games/luahelper_patch.c \
     src/games/games.c src/games/titles.c src/games/core_import.c \
     src/core/gamemod.c src/core/core_export.c \
     src/plat/host/plat_host.c src/plat/host/backend_fake.c \
     src/plat/host/backend_pine.c \
     test/test_game.c test/test_gamemod.c test/test_pine.c test/test_menu.c test/test_menu_core.c test/test_menu_uya.c test/test_menu_rac2.c test/test_menu_rac1.c test/test_tod_menu.c test/test_vshmem.c test/test_menu_help.c test/test_uipak.c test/test_usbmods.c test/test_menu_mods.c \
     test/test_combo_fastloads.c test/test_runwarn.c test/test_config_files.c test/test_profiles.c test/test_tod_game.c test/test_qwark.c"

# backend_fake.c owns the plat_* game entry points here, so backend_pine.c is
# built without its own set and the PINE tests drive its pine_* half directly.
# QWARK_EMULATOR, as in build-host.sh: the tests cover the RPCS3 paths too.
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -DQWARK_HOST -DQWARK_EMULATOR -DQWARK_TEST -DQWARK_PINE_NO_PLAT \
        -DQWARK_DEV=1 -DQWARK_PREVIEW=1 -DQWARK_BUILD_ID=0x51A7C0DE"
# Revision 1.24: a test build with a fixed build ID, which test_qwark.c reads
# back out of HELLO and SessionInfo flags2 bit4, and the features a release
# leaves out (QWARK_PREVIEW, src/core/proto.h), which test_profiles.c and
# test_usbmods.c cover.
# -static folds winpthreads into the executable. Without it both programs import
# libwinpthread-1.dll, which only exists on a PATH that has a mingw bin folder
# on it; started by the PC client from anywhere else, qwark-rpcs3.exe died at
# once with STATUS_DLL_NOT_FOUND (0xC0000135) and the client reconnected for ever.
LDFLAGS="-static -lws2_32 -lwinmm -lpthread"

echo "compiling $OUT"
"$CLANG" $CFLAGS $SRC -o "$OUT" $LDFLAGS

# Revision 1.26 (src/games/module.h, tools/coreapi.py): core_api's generated
# files match core_api.txt, and every game object reaches the core through
# core_api's wrappers and nothing else, which is what a game module must do.
PYOK=""
for PY in python python3 py; do
	if "$PY" -I tools/coreapi.py --check; then PYOK="$PY"; break; fi
done
if [ -z "$PYOK" ]; then
	echo "test/run.sh: core_api's generated files are stale or there is no python (tools/coreapi.py)" >&2
	exit 2
fi
NM="$(dirname "$CLANG")/llvm-nm.exe"
GOBJ="test/gameobjs"
rm -rf "$GOBJ"
mkdir -p "$GOBJ"
for f in src/games/classic.c src/games/rac1.c src/games/rac1_panel.c src/games/rac2.c \
         src/games/rac2_panel.c src/games/rac3.c src/games/rac3_panel.c src/games/rac4.c \
         src/games/rac4_panel.c src/games/sfhelper_bins.c src/games/switches_own.c \
         src/games/tod.c src/games/tod_game.c src/games/tod_panel.c src/games/tod_flags.c src/games/tod_autosplit.c src/games/tod_save.c src/games/tod_cache.c src/games/luahelper_bins.c; do
	"$CLANG" $CFLAGS -c "$f" -o "$GOBJ/$(basename "$f" .c).o"
done
"$PYOK" -I tools/coreapi.py nmcheck "$NM" "$GOBJ"/*.o
rm -rf "$GOBJ"

# A fresh fake HDD every run, with the real racman mod fixtures installed.
rm -rf "$ROOT"
mkdir -p "$ROOT/dev_hdd0/qwark/mods"
cp -r test/fixtures/mods/. "$ROOT/dev_hdd0/qwark/mods/"

# Build 55: the console menu's renderers as a console reads them, ui.pak in the
# fake HDD, packed by the same pak.py `make` runs (revision 1.29: with --tod,
# as a test build packs it, for ToD's Lua helper). test_uipak.c makes its own
# bad copies of it (another build, cut short, damaged) and puts this one back.
PAK=""
for PY in python python3 py; do
	if "$PY" -I src/games/menuhelper/pak.py --out "$ROOT/dev_hdd0/qwark/ui.pak" --tod >/dev/null 2>&1; then
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

# The release configuration (src/core/proto.h, QWARK_PREVIEW): the core as
# `make release` builds it, with no DEV bit, no preview features and no
# modusb.c, against test/test_release.c alone, on a fresh fake HDD and a port
# of its own. It checks that the settings profiles and the USB mods are gone.
REL_OUT="test/qwark-test-release.exe"
REL_SRC=$(for f in $SRC; do case "$f" in test/*|src/core/modusb.c|src/plat/host/backend_pine.c) ;; *) printf '%s ' "$f" ;; esac; done)
REL_CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DQWARK_HOST -DQWARK_EMULATOR -DQWARK_TEST -DQWARK_DEV=0 -DQWARK_PREVIEW=0 -DQWARK_BUILD_ID=0x51A7C0DE"
echo "compiling $REL_OUT"
"$CLANG" $REL_CFLAGS $REL_SRC test/test_game.c test/test_config_files.c test/test_release.c -o "$REL_OUT" $LDFLAGS
rm -rf "$ROOT"
mkdir -p "$ROOT/dev_hdd0/qwark"
if [ -n "${QWARK_TEST_PORT:-}" ]; then
	for PY in python python3 py; do
		QWARK_TEST_PORT=$("$PY" -c "$PICK" 2>/dev/null) && break
	done
	export QWARK_TEST_PORT
fi
echo "running $REL_OUT"
"./$REL_OUT"

# Tools of Destruction's groundwork (docs/tod-poc.md): the Lua mailbox's codec
# and request state machine, the Lua helper's RPCS3 patch, presence rule and
# install, and the fact sheet, as a program of its own against a fake game
# memory. The game and the Lua ops themselves are test/test_tod_game.c's, in
# qwark-test.exe.
TOD_OUT="test/qwark-test-tod.exe"
TOD_SRC="src/core/util.c src/core/luamb.c src/games/luahelper_bins.c src/games/luahelper_patch.c \
         src/games/tod.c test/test_tod.c"
echo "compiling $TOD_OUT"
"$CLANG" $CFLAGS $TOD_SRC -o "$TOD_OUT" $LDFLAGS
echo "running $TOD_OUT"
"./$TOD_OUT"

# Revision 1.29, the third console test (docs/tod-menu.md 11): ToD's pause
# menu glue and the helper's look, run by a real Lua against a stand-in pause
# menu (test/glue_harness.lua), the source and the bytes qwark sends. It needs
# a Lua DLL on the PC (QWARK_TEST_LUA, or a known place) and says SKIP
# without one.
echo "running test/test_glue.py"
"$PYOK" -I test/test_glue.py

# The installer package's EBOOT (installer/): its boot_plugins.txt edit and its
# install over a fake HDD, a program of its own.
sh installer/test/run.sh
