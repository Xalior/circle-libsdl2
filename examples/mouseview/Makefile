#
# circle-libsdl2 mouseview test — SDL2 mouse input
#
# Each board has its own configured world and its own archive, so name the
# board to build against a different one:
#
#   make BOARD=rpi5
#
# Objects are built in place, as Circle's rules do, so switching boards means
# `make clean` first.
#
# This example carries a patchable-defaults block (defaults.cpp, and the
# linker script that pins it to image offset 0x800), so a network loader can
# stamp a switch into the image before it boots. The `image` gate below
# refuses to leave an image behind that lost the block.
#

BOARD ?= rpi4

CIRCLESTDLIBHOME ?= ../../circle-stdlib-$(BOARD)

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = main.o kernel.o defaults.o

STANDARD = -std=c++23 -Wno-volatile

# Stated explicitly because the first rule this file sees comes from an
# included makefile, and that would otherwise decide the default goal.
.DEFAULT_GOAL := image

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

LIBS := ../../libSDL2-$(BOARD).a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

# sdl-app.mk lets the caller choose the linker script, so this build supplies
# its own: the shim's script with room made at image offset 0x800 for the
# block. Everything else about the script is inherited from the shim's.
SDL_APP_LDSCRIPT = $(CURDIR)/mouseview-defaults.ld
include ../../sdl-app.mk

# The other half of the guarantee the linker script's ASSERTs begin: check
# the BYTES of the finished image, not just the link. A pre-boot writer
# refuses to stamp an image whose magic is not at 0x800, so an image that
# lost it would boot and then quietly ignore everything ever sent to it.
# Better to have no image than that one.
.PHONY: image
image: $(TARGET).img
	@if [ "`dd if=$(TARGET).img bs=4 skip=512 count=1 2>/dev/null`" = "PM8D" ]; \
	then \
		echo "  GATE  $(TARGET).img defaults block at 0x800: present"; \
	else \
		echo "  GATE  $(TARGET).img: defaults block ABSENT at 0x800 — image deleted"; \
		rm -f $(TARGET).img; \
		exit 1; \
	fi
