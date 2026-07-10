#
# circle-libsdl2 — SDL2-compatible shim over the Circle bare-metal framework.
# Builds libSDL2.a. Requires a configured+built ../circle-stdlib.
#

include ../circle-stdlib/Config.mk

OBJS = src/init.o src/error.o src/timer.o src/hints.o src/events.o src/video.o src/input.o src/audio.o

libSDL2.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

-include $(DEPS)
