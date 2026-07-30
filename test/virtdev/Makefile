#
# circle-libsdl2 virtdev test — the declared virtual display device
#
# Each board has its own configured world and its own archive, so name the
# board to build against a different one:
#
#   make BOARD=rpi5
#
# Objects are built in place, as Circle's rules do, so switching boards means
# `make clean` first.
#

BOARD ?= rpi4

CIRCLESTDLIBHOME ?= ../../circle-stdlib-$(BOARD)

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = main.o kernel.o

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

LIBS := ../../libSDL2-$(BOARD).a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

include ../../sdl-app.mk
