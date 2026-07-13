#
# circle-libsdl2 tone test — SDL2 callback audio
#

CIRCLESTDLIBHOME ?= ../../circle-stdlib

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = main.o kernel.o

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

LIBS := ../../libSDL2.a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)

include ../../sdl-app.mk
