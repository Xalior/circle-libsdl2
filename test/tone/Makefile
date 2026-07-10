#
# circle-libsdl2 tone test — SDL2 callback audio
#

include ../../../circle-stdlib/Config.mk

OBJS = main.o kernel.o

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I ../../include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

LIBS := ../../libSDL2.a $(CIRCLEHOME)/lib/sound/libsound.a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)
