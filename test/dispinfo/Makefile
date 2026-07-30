#
# circle-libsdl2 dispinfo probe — firmware display tags and framebuffer
# allocation results, over serial only.
#
# Each board has its own configured world and its own archive, so name the
# board to build against a different one:
#
#   make BOARD=rpi5
#
# Objects are built in place, as Circle's rules do, so switching boards means
# `make clean` first.
#
# The probe uses no SDL, so it links against Circle and the C++ runtime only.
# It still links through sdl-app.ld, which keeps the TLS sections adjacent as
# libc++'s threading support requires.
#

BOARD ?= rpi4

CIRCLESTDLIBHOME ?= ../../circle-stdlib-$(BOARD)

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = main.o kernel.o

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

LIBS := $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

include ../../sdl-app.mk
