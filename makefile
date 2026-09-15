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

PPU_SRCS  = src/plat/ps3/libc.c src/plat/ps3/printf.c src/plat/ps3/plat_ps3.c
PPU_SRCS += src/main.c
PPU_SRCS += src/core/util.c src/core/mem.c src/core/config.c
PPU_SRCS += src/core/features.c src/core/mods.c src/core/session.c src/core/net.c
PPU_SRCS += src/core/autosplit.c src/core/savefile.c
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

all: sfhelper-if-stale
	$(MAKE) $(PPU_OBJS_DEPENDS)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $(PPU_PRX_TARGET)
	./scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 -c SPRX --self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002 -e $(PPU_PRX_TARGET) $(PPU_SPRX_TARGET)
	@rm -rf objs qwark.prx qwark.sym

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
# dist/ is what the PC client's release workflow takes the console side from:
# the signed module and the RPCS3 helper, both committed so a release can be cut
# without an SDK or a cross build. Run it after `make` and `./build-host.sh`.
#
dist:
	@mkdir -p dist
	@cp qwark.sprx dist/qwark.sprx
	@cp qwark-rpcs3.exe dist/qwark-rpcs3.exe
	@ls -l dist

.PHONY: all sfhelper sfhelper-if-stale dist

include $(CELL_MK_DIR)/sdk.target.mk
