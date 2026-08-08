# sdl-app.mk — kernel image link rule for circle-libsdl2 applications.
#
# Include AFTER Circle's Rules.mk. Links with sdl-app.ld, which keeps the
# TLS sections adjacent (binutils 2.44+ refuses PT_TLS otherwise, and
# libc++'s threading support carries TLS). The overridden-recipe warning
# from make is expected.
#
# sdl-app.ld is derived from Circle's circle.ld and remains GPLv3 (see its
# header); the rest of this project is zlib-licensed.

# This directory, captured before anything else can change MAKEFILE_LIST. It
# is both where the default script lives and where the linker is told to look
# for INCLUDEd fragments, so an application that overrides the script with its
# own can still pull sdl-app-init.ld in by name.
SDL_APP_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

SDL_APP_LDSCRIPT ?= $(SDL_APP_DIR)sdl-app.ld

# THE CROSS COMPILER GOES THROUGH ccache WHEN THERE IS ONE.
#
# Circle's Rules.mk, which an application includes before this file, names the
# compiler directly ($(PREFIX)g++). Nothing was cached as a result: a game's
# sources were fully recompiled every time, however many times the same source
# had already been built from the same headers.
#
# Only if ccache is installed, so an application depends on nothing new. AS is
# left alone: Rules.mk sets it from CC before this point.
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
CPP := $(CCACHE) $(CPP)
CC  := $(CCACHE) $(CC)
endif


# The shim's audio backend needs Circle's sound library; carry it here so
# applications only ever list libSDL2.a.
LIBS += $(CIRCLEHOME)/lib/sound/libsound.a

# THE C++ THREADING RUNTIME IS THIS LIBRARY'S, NOT circle-stdlib's.
#
# circle-stdlib ships liblibcxx-threading.a: libc++'s threading built on
# Circle's cooperative scheduler, which its own doc/multicore.txt specifies as
# core 0's alone. This library runs applications on another core, so it
# supplies that ABI itself (src/libcxxthreading.cpp) — every symbol, futex-
# shaped, valid on every core. Nothing vendored is edited; the link is simply
# given one implementation instead of two.
#
# The filter belongs HERE and nowhere else. CIRCLE_STDLIB_LIBS comes from the
# world's Config.mk, which an application includes and must not edit; this
# fragment is the one piece of build machinery every consumer of this library
# already includes, and it is included AFTER the application has set LIBS. So
# one line here settles it for every port, and a port that has never heard of
# any of this gets a working runtime by changing nothing.
#
# Leaving the archive in the list would not usually break the link — an
# archive member is pulled only to resolve something still undefined, and
# these symbols are already defined by then — which is exactly why it is
# taken out rather than left to sit there. Which implementation a program got
# would depend on the order the libraries happened to be listed in, and that
# is not a thing to leave to chance in a program that boots a board.
LIBS := $(filter-out %/liblibcxx-threading.a,$(LIBS))

# LIBS is passed to the linker as it stands, so an application may put linker
# flags in it — --whole-archive around the shim, say, so that a stale stub in
# the application cannot silently shadow a symbol the library implements for
# real. A flag is not a file, so the prerequisite list takes the file subset:
# left in, make would try to build the flag and stop.
LIBS_FILES = $(filter-out -%,$(LIBS))

$(TARGET).img: $(OBJS) $(LIBS_FILES) $(SDL_APP_LDSCRIPT)
	@echo "  LD    $(TARGET).elf (sdl-app.ld)"
	@$(LD) -o $(TARGET).elf -Map $(TARGET).map $(LDFLAGS) \
		-L$(SDL_APP_DIR) -T $(SDL_APP_LDSCRIPT) $(CRTBEGIN) $(OBJS) \
		--start-group $(LIBS) $(EXTRALIBS) --end-group $(CRTEND)
	@echo "  DUMP  $(TARGET).lst"
	@$(OBJDUMP) -d $(TARGET).elf | $(CPPFILT) > $(TARGET).lst
	@echo "  COPY  $(TARGET).img"
	@$(OBJCOPY) $(TARGET).elf -O binary $(TARGET).img
	@echo -n "  WC    $(TARGET).img => "
	@wc -c < $(TARGET).img
