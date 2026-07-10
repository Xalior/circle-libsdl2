# sdl-app.mk — kernel image link rule for circle-libsdl2 applications.
#
# Include AFTER Circle's Rules.mk. Links with sdl-app.ld, which keeps the
# TLS sections adjacent (binutils 2.44+ refuses PT_TLS otherwise, and
# libc++'s threading support carries TLS). The overridden-recipe warning
# from make is expected.
#
# sdl-app.ld is derived from Circle's circle.ld and remains GPLv3 (see its
# header); the rest of this project is zlib-licensed.

SDL_APP_LDSCRIPT ?= $(dir $(lastword $(MAKEFILE_LIST)))sdl-app.ld

# The shim's audio backend needs Circle's sound library; carry it here so
# applications only ever list libSDL2.a.
LIBS += $(CIRCLEHOME)/lib/sound/libsound.a

$(TARGET).img: $(OBJS) $(LIBS) $(SDL_APP_LDSCRIPT)
	@echo "  LD    $(TARGET).elf (sdl-app.ld)"
	@$(LD) -o $(TARGET).elf -Map $(TARGET).map $(LDFLAGS) \
		-T $(SDL_APP_LDSCRIPT) $(CRTBEGIN) $(OBJS) \
		--start-group $(LIBS) $(EXTRALIBS) --end-group $(CRTEND)
	@echo "  DUMP  $(TARGET).lst"
	@$(OBJDUMP) -d $(TARGET).elf | $(CPPFILT) > $(TARGET).lst
	@echo "  COPY  $(TARGET).img"
	@$(OBJCOPY) $(TARGET).elf -O binary $(TARGET).img
	@echo -n "  WC    $(TARGET).img => "
	@wc -c < $(TARGET).img
