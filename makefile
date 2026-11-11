CELL_MK_DIR = $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

BUILD_TYPE			= release

LIBSTUB_DIR			= ./lib
PRX_DIR				= .
INSTALL				= cp
PEXPORTPICKUP		= ppu-lv2-prx-exportpickup
SDKLIB_DIR			= $(CELL_SDK)/target/ppu/lib
#
# --strip-unused-data IS the dead stripping: ps3ppuld answers --gc-sections with
# "L0153: --gc-sections is deprecated: using --strip-unused-data for dead
# stripping" and links exactly the same bytes, so adding it buys nothing and
# costs a build warning. -ffunction-sections and -fdata-sections below are what
# give it something to strip.
#
PRX_LDFLAGS_EXTRA	= -L $(LIBSTUB_DIR) -L $(SDKLIB_DIR) -Wl,--strip-unused-data

CRT_HEAD += $(shell ppu-lv2-gcc -print-file-name'='ecrti.o)
CRT_HEAD += $(shell ppu-lv2-gcc -print-file-name'='crtbegin.o)
CRT_HEAD += $(shell ppu-lv2-gcc -print-file-name'='ecrtn.o)
CRT_TAIL += $(shell ppu-lv2-gcc -print-file-name'='crtend.o)

#
# No QWARK_EMULATOR here (src/plat/plat.h): a console patches code, so the
# code switches (src/core/switches.c, src/games/switches_bins.c) are left out
# altogether, and so is everything else that only exists for RPCS3. Revision
# 1.24: but not switches_own.c, the pieces a console writes itself (Deadlocked's
# HUD colours), which build.sh cuts out of the same link.
#
PPU_SRCS  = src/plat/ps3/libc.c src/plat/ps3/printf.c src/plat/ps3/plat_ps3.c
PPU_SRCS += src/main.c
PPU_SRCS += src/core/util.c src/core/mem.c src/core/config.c
PPU_SRCS += src/core/features.c src/core/mods.c src/core/session.c src/core/net.c
PPU_SRCS += src/core/autosplit.c src/core/savefile.c src/core/manips.c src/core/menu.c
PPU_SRCS += src/core/runwarn.c
#
# Build 55: not src/games/menuhelper_bins.c. The console menu's renderers go
# into ui.pak beside the module (below), and qwark reads the running game's out
# of /dev_hdd0/qwark/ui.pak (src/core/uipak.c); the module carries none of them.
#
PPU_SRCS += src/core/uipak.c
#
# Revision 1.26: the title index and the game's arrival, in every build. The
# games themselves are modules of their own (`games`), or, in a
# QWARK_GAME_MODULES=0 module, linked in below.
#
PPU_SRCS += src/core/gamemod.c src/games/titles.c
PPU_PRX_TARGET = qwark.prx
PPU_PRX_LDFLAGS += $(PRX_LDFLAGS_EXTRA)
PPU_PRX_STRIP_FLAGS = -s
PPU_PRX_LDLIBS	=	-lfs_stub -lnet_stub -lrtc_stub -lio_stub -lstdc_export_stub \
					-lvshcommon_export_stub \
					-lpaf_export_stub -lxsetting_export_stub \
					-lvshmain_export_stub \
					-lvshtask_export_stub -lsdk_export_stub -lallocator_export_stub

PPU_CFLAGS +=	-Os -ffunction-sections -fdata-sections \
				-fno-builtin-printf -nodefaultlibs -std=gnu99 \
				-Wno-shadow -Wno-unused-parameter \
				-Wno-format-nonliteral \
				-Wno-strict-prototypes \
				-Wno-missing-prototypes \
				-Wno-missing-declarations
PPU_CFLAGS +=	-Wno-bad-function-cast
PPU_CFLAGS +=	-Wno-inline -Wno-redundant-decls

#PPU_CFLAGS += -finline-limit=20

#
# Test builds and release builds (revision 1.24, PROTOCOL.md 3.1.1). Every
# build between two releases reports the same QWARK_BUILD, so the module also
# reports a build ID, the CRC of its sources (tools/buildid.py), and a test
# build says it is one in SessionInfo flags2 bit4 DEV. The client replaces a
# DEV module whose ID is not the ID of the module it ships, so a console never
# keeps an older test build of the same number; a release module carries no
# DEV bit and is judged by its build number alone, as every build was before.
#
# A plain `make` is a TEST build, QWARK_DEV=1, because that is what nearly
# every build is, and a test build left unmarked is the bug this fixes. A
# release is `make release` (a QWARK_DEV=0 module, then `make dist`), and the
# client's release.ps1 refuses a dist/ whose qwark.build says dev 1. The ID is
# worked out once, as the makefile is read (the SDK's rules compile the objects
# before `all`'s own recipe runs), and handed to every make below this one;
# `all` writes it to qwark.build beside the module once the module is signed,
# so the two always describe each other.
#
QWARK_DEV			?= 1
ifndef QWARK_BUILD_ID
QWARK_BUILD_ID		:= $(shell python -I tools/buildid.py)
endif
ifeq ($(strip $(QWARK_BUILD_ID)),)
$(error no build ID: python -I tools/buildid.py did not run)
endif
export QWARK_DEV QWARK_BUILD_ID
PPU_CFLAGS +=	-DQWARK_DEV=$(QWARK_DEV) -DQWARK_BUILD_ID=$(QWARK_BUILD_ID)

#
# Build 56's preview features (src/core/proto.h, QWARK_PREVIEW: the settings
# profiles, the USB mods and the console menu's Mods page). Build 57 ships them
# in its release, so every build has them: QWARK_PREVIEW is 1 unless it is
# given itself, and no longer follows QWARK_DEV. QWARK_PREVIEW=0 still compiles
# them out (no modusb.c), a module no release ships. It is never exported.
#
QWARK_PREVIEW		?= 1
PPU_CFLAGS +=	-DQWARK_PREVIEW=$(QWARK_PREVIEW)
ifneq ($(strip $(QWARK_PREVIEW)),0)
PPU_SRCS += src/core/modusb.c
endif

#
# Revision 1.26, game modules (src/games/module.h, PROTOCOL.md 9.11). The games
# are not in qwark.sprx: each is an SPRX of its own named for its game,
# rac1.sprx (RaC1) to rac4.sprx (Deadlocked) and tod.sprx, built by `games`
# below, which the module loads while that game runs and the VSH has the memory
# for it. The core then hands the games its functions through a table
# (src/core/core_export.c) and its context through r14, which the core and
# every module are built to leave alone. Build 57: a release as well as a test
# build (`make release` ships the modules and Tools of Destruction), so this is
# 1 unless given itself and no longer follows QWARK_DEV. QWARK_GAME_MODULES=0
# still links the four original games in and calls them directly, as builds 51
# to 56 did, with no ToD: a module no release ships. Like QWARK_PREVIEW, it is
# never exported.
#
QWARK_GAME_MODULES	?= 1
PPU_CFLAGS +=	-DQWARK_GAME_MODULES=$(QWARK_GAME_MODULES)
ifneq ($(strip $(QWARK_GAME_MODULES)),0)
PPU_SRCS += src/core/core_export.c
#
# Revision 1.27: the core's half of a game's own Lua (src/core/luacore.c, the
# mailbox's state machine, the helper's install and its RPCS3 patch builder),
# which only a build with Tools of Destruction has. Revision 1.29: the
# helper's bytes and its Lua chunks are in ui.pak (PAK_TOD below), and tod.sprx
# carries only their numbers.
#
PPU_SRCS += src/core/luacore.c src/core/luamb.c src/games/luahelper_patch.c
PPU_CFLAGS +=	-ffixed-r14
GAME_MODULES_ALL = games
else
PPU_SRCS += src/games/sfhelper_bins.c src/games/switches_own.c
PPU_SRCS += src/games/classic.c src/games/rac1.c src/games/rac1_panel.c
PPU_SRCS += src/games/rac2.c src/games/rac2_panel.c
PPU_SRCS += src/games/rac3.c src/games/rac3_panel.c
PPU_SRCS += src/games/rac4.c src/games/rac4_panel.c
PPU_SRCS += src/games/games.c
GAME_MODULES_ALL = games-none
endif

ifeq ($(BUILD_TYPE), debug)
PPU_CFLAGS += -DDEBUG -DDEBUG_FILE
endif

all: sfhelper-if-stale switches-if-stale menuhelper-if-stale luahelper-if-stale
	@rm -f qwark.build
	$(MAKE) $(PPU_OBJS_DEPENDS)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $(PPU_PRX_TARGET)
	./scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 -c SPRX --self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002 -e $(PPU_PRX_TARGET) $(PPU_SPRX_TARGET)
	python -I tools/buildid.py --dev $(QWARK_DEV) --id $(QWARK_BUILD_ID) --out qwark.build
	@tail -n 3 qwark.build
	$(MAKE) ui.pak
	@rm -rf objs qwark.prx qwark.sym
	$(MAKE) $(GAME_MODULES_ALL)

#
# A release: the module with no DEV bit, its game modules and the full ui.pak
# (Tools of Destruction's Lua helper included), and dist/ made from them.
# Commit dist/ afterwards, as after any `make dist`. Objects a failed build
# left behind, and game modules a test build left, were made with the other
# DEV bit, so they go first and are built again. Build 57:
# a release differs from a test build by QWARK_DEV alone; the preview features
# and the game modules are pinned on here, so a QWARK_PREVIEW=0 or a
# QWARK_GAME_MODULES=0 on the command line cannot leave them out of a release.
#
release:
	@rm -rf objs $(GAME_MODULE_FILES)
	$(MAKE) all QWARK_DEV=0 QWARK_PREVIEW=1 QWARK_GAME_MODULES=1
	$(MAKE) dist QWARK_DEV=0 QWARK_PREVIEW=1 QWARK_GAME_MODULES=1

#
# Build 55: ui.pak, the console menu's renderers, packed out of the committed
# src/games/menuhelper_bins.c by pak.py and stamped with this build. It goes
# to /dev_hdd0/qwark/ui.pak beside the module's data, and a module takes only
# the pak built with it (src/core/uipak.h). Python, as for check.py.
#
# Revision 1.29: a build with Tools of Destruction (QWARK_GAME_MODULES: since
# build 57 a release as well as a test build) packs ToD's Lua helper too, its
# code and the three Lua chunks the core sends, out of src/games/luahelper_bins.c
# (pak.py --tod), as entries outside the header's identities; only a
# QWARK_GAME_MODULES=0 module, which no release ships, packs the pak without
# them. Packed every time, so one build never ships another's pak.
#
ifneq ($(strip $(QWARK_GAME_MODULES)),0)
PAK_TOD = --tod
endif
ui.pak: src/games/menuhelper_bins.c src/games/sfhelper_bins.c src/games/luahelper_bins.c 		src/games/menuhelper/pak.py src/core/proto.h
	python -I src/games/menuhelper/pak.py --out ui.pak $(PAK_TOD)

#
# Revision 1.26: the game modules, rac1.sprx to rac4.sprx (src/games/
# module.h, PROTOCOL.md 9.11), the games of a test build and a release. Each is the game's own
# sources, classic.c, the savefile helpers' numbers, the wrappers its calls
# into the core land on (src/games/core_import.c, tools/coreapi.py) and its
# start and stop (src/plat/ps3/game_module.c), built the SDK's way for a PRX:
#
#   ppu-lv2-gcc -mprx -c         QWARK_GAME_MODULE: state in the lent area,
#                                r14 left alone
#   tools/gamemodule.py objs     no object holds writable data or TLS
#   ppu-lv2-gcc -mprx -zgenprx   prelink, libgen and fixup: two LOAD segments,
#                                R-E and RW-, the latter only .opd, .toc and
#                                the relocated tables
#   ppu-lv2-prx-strip            as qwark.sprx's
#   tools/gamemodule.py merge    one R-E segment (the SDK cannot make one:
#                                SPEC-game-modules.md 7.2), which lv2 relocates
#   tools/gamemodule.py check    one R-X LOAD segment, at most 61,440 bytes, no
#                                writable or TLS section, no import, and the
#                                stamp naming this build and its ID
#   scetool                      signed exactly as qwark.sprx is
#
# `make` and `make release` build them after the module (a QWARK_GAME_MODULES=0
# module has none: `games-none` deletes any left over), and `make dist` copies
# them, all five or none.
#
# Each module is named for its game, never for its number: GAME_MODULES lists
# them, GAME_ID_<name> is the game id it is built for (src/core/proto.h) and
# GAME_SRCS_<name> its own sources. The names are the title index's
# (src/games/titles.c, title_module_name), which the core loads them by. A
# later game is a name of its own in both places (Tools of Destruction's is
# tod.sprx), whatever its number.
#
# Revision 1.27: Tools of Destruction is game 5, tod.sprx (since build 57 in a
# release too; a QWARK_GAME_MODULES=0 module links rac1 to rac4 and has no room
# for a fifth). GAME_VTABLE_<name>
# is the vtable the module's start hands back (src/plat/ps3/game_module.c).
#
GAME_MODULES		= rac1 rac2 rac3 rac4 tod
GAME_MODULE_FILES	= $(GAME_MODULES:%=%.sprx)
GAME_ID_rac1		= 1
GAME_ID_rac2		= 2
GAME_ID_rac3		= 3
GAME_ID_rac4		= 4
GAME_ID_tod			= 5
GAME_VTABLE_rac1	= rac1_game
GAME_VTABLE_rac2	= rac2_game
GAME_VTABLE_rac3	= rac3_game
GAME_VTABLE_rac4	= rac4_game
GAME_VTABLE_tod		= tod_game
# Revision 1.29: the pages a module may take, 1 unless given here
# (GAME_PAGES_<name>); the core's memory gate reads the pages from the file
# (src/core/gamemod.c), and the PRX probe's D4 loaded a two-page RX-only module
# on CFW and HEN. Tools of Destruction's took two with the cutscene cache, and
# one again once the debug pages went and its level-flag names moved to the
# client (56,224 of 61,440 bytes); it was 60,608 bytes with the galactic map and
# the fast-loaded planet loads, and 38,656 once its Lua helper and glue moved
# into ui.pak (revision 1.29).
GAME_COMMON_SRCS	= src/games/classic.c src/games/sfhelper_bins.c src/games/core_import.c \
					  src/plat/ps3/game_module.c
GAME_SRCS_rac1		= src/games/rac1.c src/games/rac1_panel.c
GAME_SRCS_rac2		= src/games/rac2.c src/games/rac2_panel.c
GAME_SRCS_rac3		= src/games/rac3.c src/games/rac3_panel.c
GAME_SRCS_rac4		= src/games/rac4.c src/games/rac4_panel.c src/games/switches_own.c
GAME_SRCS_tod		= src/games/tod.c src/games/tod_game.c src/games/tod_panel.c src/games/tod_flags.c src/games/tod_autosplit.c src/games/tod_save.c src/games/tod_cache.c src/games/luahelper_bins.c src/games/switches_own.c
GAME_CFLAGS			= -mprx -Os -ffunction-sections -fdata-sections -fno-builtin-printf -std=gnu99 \
					  -ffixed-r14 -Wall -Wno-unused-parameter -Wno-shadow -Wno-strict-prototypes \
					  -Wno-missing-prototypes -Wno-missing-declarations -Wno-bad-function-cast \
					  -Wno-inline -Wno-redundant-decls -Wno-format-nonliteral -Werror \
					  -DQWARK_DEV=$(QWARK_DEV) -DQWARK_BUILD_ID=$(QWARK_BUILD_ID) \
					  -DQWARK_PREVIEW=$(QWARK_PREVIEW) -DQWARK_GAME_MODULES=1 -DQWARK_GAME_MODULE=1
GAME_SCETOOL		= -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 APP \
					  -6 0003004000000000 -A 0001000000000000 -c SPRX \
					  --self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002
GAME_TIMEOUT		= timeout 300
GAME_DEPS			= $(GAME_COMMON_SRCS) $(wildcard src/games/*.h src/core/*.h src/plat/*.h) \
					  src/games/rac1.c src/games/rac1_panel.c src/games/rac2.c src/games/rac2_panel.c \
					  src/games/rac3.c src/games/rac3_panel.c src/games/rac4.c src/games/rac4_panel.c \
					  src/games/switches_own.c src/games/tod.c src/games/tod_game.c src/games/tod_panel.c src/games/tod_flags.c src/games/tod_autosplit.c src/games/tod_save.c src/games/tod_cache.c \
					  src/games/luahelper_bins.c tools/gamemodule.py makefile

games: $(GAME_MODULE_FILES)
	@ls -l $(GAME_MODULE_FILES)

games-none:
	@rm -f $(GAME_MODULE_FILES)

# A static pattern: % is a module's name (rac1 ..), and the rule matches those
# files alone, never qwark.sprx.
$(GAME_MODULE_FILES): %.sprx: $(GAME_DEPS)
	@test -n "$(GAME_ID_$*)" || { echo "make games: $@ has no GAME_ID_$*" >&2; exit 1; }
	@test -n "$(GAME_VTABLE_$*)" || { echo "make games: $@ has no GAME_VTABLE_$*" >&2; exit 1; }
	@rm -rf objs/$* $@
	@mkdir -p objs/$*
	@for f in $(GAME_COMMON_SRCS) $(GAME_SRCS_$*); do \
		echo "  $*: $$f"; \
		$(GAME_TIMEOUT) ppu-lv2-gcc $(GAME_CFLAGS) -DQWARK_MODULE_GAME=$(GAME_ID_$*) \
			-DQWARK_MODULE_VTABLE=$(GAME_VTABLE_$*) -c $$f \
			-o objs/$*/$$(basename $$f .c).o || exit 1; \
	done
	$(GAME_TIMEOUT) python -I tools/gamemodule.py objs objs/$*/*.o
	$(GAME_TIMEOUT) ppu-lv2-gcc -mprx -zgenprx -nodefaultlibs -Wl,--strip-unused-data \
		objs/$*/*.o -o objs/$*/game.sym.prx
	cp objs/$*/game.sym.prx objs/$*/game.prx
	$(GAME_TIMEOUT) $(PPU_PRX_STRIP) --strip-debug --strip-section-header objs/$*/game.prx
	$(GAME_TIMEOUT) python -I tools/gamemodule.py merge objs/$*/game.prx objs/$*/game.one.prx
	$(GAME_TIMEOUT) python -I tools/gamemodule.py merge objs/$*/game.sym.prx objs/$*/game.sym.one.prx
	$(GAME_TIMEOUT) python -I tools/gamemodule.py check objs/$*/game.one.prx objs/$*/game.sym.one.prx \
		--game $(GAME_ID_$*) --id $(QWARK_BUILD_ID) --name $@ --pages $(or $(GAME_PAGES_$*),1)
	$(GAME_TIMEOUT) ./scetool $(GAME_SCETOOL) -e objs/$*/game.one.prx $@ > objs/$*/scetool.txt 2>&1 \
		|| { head -c 4000 objs/$*/scetool.txt; exit 1; }
	@test -s $@ || { echo "make games: scetool wrote no $@" >&2; exit 1; }
	@rm -rf objs/$*
	@rmdir objs 2>/dev/null || true

#
# The savefile helper is PowerPC code that runs inside the game rather than
# inside qwark, so it is built on its own and committed as the byte arrays in
# src/games/sfhelper_bins.c. A plain `make` never needs to rebuild it - the host
# builds and the unit tests compile the committed file with no SDK in sight -
# but it does rebuild it when one of its sources is newer, so an edit to the
# helper cannot be left out of an SPRX by accident.
#
sfhelper:
	sh src/games/sfhelper/build.sh

sfhelper-if-stale:
	@sh src/games/sfhelper/build.sh --if-stale

#
# The console menu's renderers (revision 1.22) are the same kind of thing again:
# PowerPC code that runs inside the game, built by src/games/menuhelper/build.sh
# and committed as src/games/menuhelper_bins.c, which the SPRX and the host
# builds both compile.
#
menuhelper:
	sh src/games/menuhelper/build.sh

menuhelper-if-stale:
	@sh src/games/menuhelper/build.sh --if-stale

#
# The code switches (build 50) are the same kind of thing: PowerPC trampolines
# an RPCS3 patch puts in the game, assembled and linked at the game's addresses
# by src/games/switches/build.sh and committed as src/games/switches_bins.c. The
# same two targets, for the same reasons. The SPRX does not link the file (build
# 51), but the SDK that assembles it is only here, and the host builds do.
#
switches:
	sh src/games/switches/build.sh

switches-if-stale:
	@sh src/games/switches/build.sh --if-stale

#
# Revision 1.27: the Lua helper (src/games/luahelper/), the same again: PowerPC
# code built at the game's addresses into the committed src/games/luahelper_bins.c,
# whose bytes the host builds carry and (revision 1.29) the module's ui.pak,
# tod.sprx only their numbers. build.sh writes their identity in the pak,
# LH_PAK_ID, into luahelper_bins.h with pak.py --lh-id, so an edit to pak.py
# regenerates it too.
#
luahelper:
	sh src/games/luahelper/build.sh

luahelper-if-stale:
	@sh src/games/luahelper/build.sh --if-stale

#
# qwark-installer.pkg: qwark for a console with no PC. A homebrew app,
# QWRK00001, whose EBOOT.BIN copies the qwark.sprx and ui.pak it carries to
# where the PC client's boot install puts them, puts qwark first in
# /dev_hdd0/boot_plugins.txt and offers to restart (installer/, docs/installer.md).
# Built from the qwark.sprx and ui.pak beside this makefile, so run `make`
# first; `make dist` does this as well.
#
#   installer/makefile      installer.elf, with the SDK's own rules
#   ppu-lv2-strip           its debug information off
#   installer/pkg.py stage  PARAM.SFO, ICON0.PNG and USRDIR/ in installer/build/pkg
#   scetool                 EBOOT.BIN, signed as a free NPDRM application
#   installer/pkg.py build  a debug package, the SDK's format, which CFW and
#                           PS3HEN both install (the SDK's own packager takes
#                           no EBOOT.BIN signed this way: see pkg.py)
#   installer/pkg.py check  the package read back and held against what went
#                           in, read and listed by the SDK's own
#                           make_package_npdrm, and the SDK's own package of
#                           the same files reproduced byte for byte
#
# The EBOOT's signing, flag by flag: a SELF of NPDRM type (-5), compressed
# (-1), with its sections (-s FALSE); key revision 0A (-2), the 3.55 keys, the
# newest scetool holds a private key for and what homebrew for PS3HEN is
# signed with; the retail-game auth id and vendor (-3, -4); the firmware field
# 3.40 (-6); the control flags qwark.sprx has (-8), and the capability flags
# webMAN MOD's installer has (-9), with which an app may restart the console;
# a free licence (-b), an executable (-c), the package's content id (-f) and
# the name the console knows it by (-g).
#
INSTALLER_BUILD		= installer/build
INSTALLER_PKG		= qwark-installer.pkg
INSTALLER_SCETOOL	= -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 NPDRM \
					  -A 0001000000000000 -6 0003004000000000 \
					  -8 4000000000000000000000000000000000000000000000000000000000000002 \
					  -9 00000000000000000000000000000000000000000000007B0000000100000000 \
					  -b FREE -c EXEC -g EBOOT.BIN

#
# Every step has a deadline, and the check holds the SDK's tools to an output
# and a disk bound as well (pkg.py): one of them can run for ever.
#
INSTALLER_TIMEOUT	= timeout 300

#
# Revision 1.26: the package carries the game modules too, which the installer
# puts beside ui.pak: a test build's and, since build 57, a release's (only a
# QWARK_GAME_MODULES=0 module has none, `games-none`).
#
INSTALLER_GAMES		= $(wildcard $(GAME_MODULE_FILES))

#
# The package carries RaCMAN Reloaded's mods and premade savefiles too, for a
# console with no PC: installer/pkg.py takes them from the client's checkout by
# the client's own rules (what its publish.ps1 ships, as its uploads send them)
# into USRDIR/library.bin, and the installer adds whichever the console does not
# have yet (docs/installer.md). CLIENT_DIR is that checkout, beside this one by
# default; `make pkg CLIENT_DIR=` builds a package without them.
#
CLIENT_DIR		?= ../racman-reloaded
ifneq ($(strip $(CLIENT_DIR)),)
INSTALLER_CLIENT	= --client "$(CLIENT_DIR)"
endif

pkg: ui.pak
	@test -f qwark.sprx || { echo "make pkg: there is no qwark.sprx here to pack; run make first" >&2; exit 1; }
	$(INSTALLER_TIMEOUT) $(MAKE) -C installer elf
	@rm -rf $(INSTALLER_BUILD) $(INSTALLER_PKG)
	@mkdir -p $(INSTALLER_BUILD)
	$(INSTALLER_TIMEOUT) $(PPU_STRIP) -s -o $(INSTALLER_BUILD)/EBOOT.elf installer/installer.elf
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py stage --sprx qwark.sprx --pak ui.pak --out $(INSTALLER_BUILD) \
		--games $(INSTALLER_GAMES) $(INSTALLER_CLIENT)
	$(INSTALLER_TIMEOUT) ./scetool $(INSTALLER_SCETOOL) -f $$(python -I installer/pkg.py content-id) \
		-e $(INSTALLER_BUILD)/EBOOT.elf $(INSTALLER_BUILD)/pkg/USRDIR/EBOOT.BIN
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py build --root $(INSTALLER_BUILD)/pkg --out $(INSTALLER_PKG)
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py check --pkg $(INSTALLER_PKG) --sprx qwark.sprx --pak ui.pak \
		--elf $(INSTALLER_BUILD)/EBOOT.elf --sdk-bin "$(CELL_HOST_PATH)/bin" --games $(INSTALLER_GAMES) \
		$(INSTALLER_CLIENT)
	@rm -rf installer/objs installer/installer.elf installer/installer.self

#
# dist/ is what the PC client's release workflow takes the console side from:
# the signed module, its ui.pak (build 55), qwark.build (the module's build
# number, DEV bit and ID, revision 1.24), the game modules (revision 1.26; a
# release's too since build 57), the installer package built from those, and
# the RPCS3 helper, all committed so a release can be cut
# without an SDK or a cross build. Run it after `make` and `./build-host.sh`.
# The module and the pak only ever go out together, and the package is made
# from the very two it sits beside.
#
dist: ui.pak pkg
	@test -f qwark.build || { echo "make dist: there is no qwark.build beside qwark.sprx; run make first" >&2; exit 1; }
	@mkdir -p dist
	@cp qwark.sprx dist/qwark.sprx
	@cp qwark.build dist/qwark.build
	@cp ui.pak dist/ui.pak
	@cp $(INSTALLER_PKG) dist/qwark-installer.pkg
	@cp qwark-rpcs3.exe dist/qwark-rpcs3.exe
	@rm -f $(GAME_MODULE_FILES:%=dist/%)
	@if [ "$(strip $(QWARK_GAME_MODULES))" != "0" ]; then \
		for m in $(GAME_MODULE_FILES); do test -f $$m || { echo "make dist: no $$m; run make first" >&2; exit 1; }; \
		cp $$m dist/$$m; done; fi
	@ls -l dist
	@if grep -q '^dev 1' dist/qwark.build; then echo "dist/ holds a TEST build (dev 1); a release needs make release"; \
	else echo "dist/ holds a RELEASE build (dev 0)"; fi

.PHONY: ui.pak all release games games-none sfhelper sfhelper-if-stale switches switches-if-stale menuhelper menuhelper-if-stale luahelper luahelper-if-stale pkg dist

include $(CELL_MK_DIR)/sdk.target.mk
