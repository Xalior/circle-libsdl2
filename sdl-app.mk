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

# The shim's audio backend needs Circle's sound library; carry it here so
# applications only ever list libSDL2.a.
LIBS += $(CIRCLEHOME)/lib/sound/libsound.a

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
