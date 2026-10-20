#
# The installer's EBOOT, as a plain ELF: installer.elf. The top-level makefile's
# `make pkg` runs this, signs the ELF with ../scetool and packs it. Built with
# the SDK's own rules, as qwark.sprx is; run it from a Cygwin login shell.
#
CELL_MK_DIR = $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

# The build the package carries: QWARK_BUILD from ../src/core/proto.h. The
# module and the pak in it are checked against it as they are installed.
QWARK_BUILD := $(shell sed -n 's/^.define[ \t]*QWARK_BUILD[ \t]*\([0-9][0-9]*\).*/\1/p' ../src/core/proto.h)
ifeq ($(QWARK_BUILD),)
$(error no QWARK_BUILD in ../src/core/proto.h)
endif

PPU_SRCS	= main.c install.c bootlist.c ifs_ps3.c
PPU_TARGET	= installer.elf

PPU_INCDIRS	+= -I$(CELL_TARGET_PATH)/ppu/include/sysutil
PPU_CFLAGS	+= -Os -std=gnu99 -DINST_QWARK_BUILD=$(QWARK_BUILD)
PPU_LIBS	+= $(CELL_TARGET_PATH)/ppu/lib/libgcm_cmd.a $(CELL_TARGET_PATH)/ppu/lib/libgcm_sys_stub.a
PPU_LDLIBS	+= -lsysutil_stub -lsysmodule_stub -lfs_stub

# Only the ELF: the SDK's own target would also make a debug fself nothing uses.
elf: $(PPU_TARGET)

.PHONY: elf

include $(CELL_MK_DIR)/sdk.target.mk
