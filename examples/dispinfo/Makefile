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
# The probe calls no SDL function, but it still links through sdl-app.ld,
# and that script makes every image's entry point the boot-argument
# trampoline in src/bootargs.cpp (see the header there). That file is only
# ever compiled into libSDL2-$(BOARD).a, so the archive is on this probe's
# LIBS line too — not because the probe uses SDL, but because the trampoline
# it needs to be bootable at all lives nowhere else.
#

BOARD ?= rpi4

CIRCLESTDLIBHOME ?= ../../circle-stdlib-$(BOARD)

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = main.o kernel.o

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

# libSDL2-$(BOARD).a for the boot trampoline (see above); kernel.cpp still
# calls no SDL function and never calls SDL2Circle_ArmCoreRuntime, so the
# probe stays single-core and throws no C++ exception whether or not that
# call is made.
LIBS := ../../libSDL2-$(BOARD).a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

include ../../sdl-app.mk
