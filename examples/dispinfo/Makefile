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

# No libSDL2-$(BOARD).a here — deliberately: this probe links no SDL and
# uses none of this library's threading. That is also why kernel.cpp never
# calls SDL2Circle_ArmCoreRuntime: the symbol lives in that archive, so
# calling it would need this library linked in to arm a thread pointer
# nothing here reads (the probe throws no C++ exception and starts no
# thread). Adding the call without adding the archive is an undefined
# reference at link time.
LIBS := $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

include ../../sdl-app.mk
