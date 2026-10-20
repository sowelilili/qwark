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
# altogether, and so is everything else that only exists for RPCS3.
#
PPU_SRCS  = src/plat/ps3/libc.c src/plat/ps3/printf.c src/plat/ps3/plat_ps3.c
PPU_SRCS += src/main.c
PPU_SRCS += src/core/util.c src/core/mem.c src/core/config.c
PPU_SRCS += src/core/features.c src/core/mods.c src/core/session.c src/core/net.c
PPU_SRCS += src/core/autosplit.c src/core/savefile.c src/core/manips.c src/core/menu.c
#
# Build 55: not src/games/menuhelper_bins.c. The console menu's renderers go
# into ui.pak beside the module (below), and qwark reads the running game's out
# of /dev_hdd0/qwark/ui.pak (src/core/uipak.c); the module carries none of them.
#
PPU_SRCS += src/core/uipak.c
PPU_SRCS += src/games/sfhelper_bins.c
PPU_SRCS += src/games/classic.c src/games/rac1.c src/games/rac1_panel.c
PPU_SRCS += src/games/rac2.c src/games/rac2_panel.c
PPU_SRCS += src/games/rac3.c src/games/rac3_panel.c
PPU_SRCS += src/games/rac4.c src/games/rac4_panel.c
PPU_SRCS += src/games/games.c
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

ifeq ($(BUILD_TYPE), debug)
PPU_CFLAGS += -DDEBUG -DDEBUG_FILE
endif

all: sfhelper-if-stale switches-if-stale menuhelper-if-stale
	$(MAKE) $(PPU_OBJS_DEPENDS)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $(PPU_PRX_TARGET)
	./scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 -c SPRX --self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002 -e $(PPU_PRX_TARGET) $(PPU_SPRX_TARGET)
	$(MAKE) ui.pak
	@rm -rf objs qwark.prx qwark.sym

#
# Build 55: ui.pak, the console menu's renderers, packed out of the committed
# src/games/menuhelper_bins.c by pak.py and stamped with this build. It goes
# to /dev_hdd0/qwark/ui.pak beside the module's data, and a module takes only
# the pak built with it (src/core/uipak.h). Python, as for check.py.
#
ui.pak: src/games/menuhelper_bins.c src/games/menuhelper/pak.py src/core/proto.h
	python -I src/games/menuhelper/pak.py --out ui.pak

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

pkg: ui.pak
	@test -f qwark.sprx || { echo "make pkg: there is no qwark.sprx here to pack; run make first" >&2; exit 1; }
	$(INSTALLER_TIMEOUT) $(MAKE) -C installer elf
	@rm -rf $(INSTALLER_BUILD) $(INSTALLER_PKG)
	@mkdir -p $(INSTALLER_BUILD)
	$(INSTALLER_TIMEOUT) $(PPU_STRIP) -s -o $(INSTALLER_BUILD)/EBOOT.elf installer/installer.elf
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py stage --sprx qwark.sprx --pak ui.pak --out $(INSTALLER_BUILD)
	$(INSTALLER_TIMEOUT) ./scetool $(INSTALLER_SCETOOL) -f $$(python -I installer/pkg.py content-id) \
		-e $(INSTALLER_BUILD)/EBOOT.elf $(INSTALLER_BUILD)/pkg/USRDIR/EBOOT.BIN
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py build --root $(INSTALLER_BUILD)/pkg --out $(INSTALLER_PKG)
	$(INSTALLER_TIMEOUT) python -I installer/pkg.py check --pkg $(INSTALLER_PKG) --sprx qwark.sprx --pak ui.pak \
		--elf $(INSTALLER_BUILD)/EBOOT.elf --sdk-bin $(CELL_HOST_PATH)/bin
	@rm -rf installer/objs installer/installer.elf installer/installer.self

#
# dist/ is what the PC client's release workflow takes the console side from:
# the signed module, its ui.pak (build 55), the installer package built from
# those two, and the RPCS3 helper, all committed so a release can be cut
# without an SDK or a cross build. Run it after `make` and `./build-host.sh`.
# The module and the pak only ever go out together, and the package is made
# from the very two it sits beside.
#
dist: ui.pak pkg
	@mkdir -p dist
	@cp qwark.sprx dist/qwark.sprx
	@cp ui.pak dist/ui.pak
	@cp $(INSTALLER_PKG) dist/qwark-installer.pkg
	@cp qwark-rpcs3.exe dist/qwark-rpcs3.exe
	@ls -l dist

.PHONY: all sfhelper sfhelper-if-stale switches switches-if-stale menuhelper menuhelper-if-stale pkg dist

include $(CELL_MK_DIR)/sdk.target.mk
