#
# circle-libsdl2 — SDL2-compatible shim over the Circle bare-metal framework.
#
# RASPPI is baked into a circle-stdlib checkout at configure time (the hard
# rule forbids reconfiguring a Circle world in place), so each Pi board needs
# its OWN world, and the shim its own per-board archive built against it:
#
#   circle-stdlib-rpi3 (RASPPI 3) -> libSDL2-rpi3.a
#   circle-stdlib-rpi4 (RASPPI 4) -> libSDL2-rpi4.a
#   circle-stdlib-rpi5 (RASPPI 5) -> libSDL2-rpi5.a
#
# All three worlds are multicore (ARM_ALLOW_MULTI_CORE): the shim marshals
# platform calls back to core 0 so an application runs on another core, and
# offers a presentation worker for one — neither is possible single-core.
#
#   make deps            configure+build all three worlds, then all archives
#   make                 build the default board's archive (BOARD=rpi4)
#   make BOARD=rpi3      build one board's archive against its world
#   make all-boards      (re)build all three archives (worlds already built)
#
# The consumer picks a board's archive+world explicitly; nothing here assumes
# a single board. Override a world's location with
# `make CIRCLESTDLIBHOME=/path/to/world` if needed.
#
BOARDS      = rpi3 rpi4 rpi5
BOARD      ?= rpi4
RASPPI_rpi3 = 3
RASPPI_rpi4 = 4
RASPPI_rpi5 = 5

# GNU getopt for circle-stdlib's configure (macOS BSD getopt drops long opts ->
# wrong toolchain prefix). ccache is build/ccache.sh's job (mandatory source).
GETOPT_BIN := $(firstword $(wildcard /opt/homebrew/opt/gnu-getopt/bin /usr/local/opt/gnu-getopt/bin))
ifneq ($(GETOPT_BIN),)
export PATH := $(GETOPT_BIN):$(PATH)
endif

CIRCLE_STDLIB     = circle-stdlib-$(BOARD)
CIRCLESTDLIBHOME ?= $(CURDIR)/$(CIRCLE_STDLIB)

# Per-board object tree, so all three archives coexist without one board's
# objects clobbering another's — no `make clean` between boards, each is its
# own cacheable unit.
OBJDIR = build/$(BOARD)

.DEFAULT_GOAL := libSDL2-$(BOARD).a

# LLVM/libc++ comes from a git checkout at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball: Codeberg regenerates its archives,
# drifting their SHA from the pin, so a clean --libcxx build fails its hash
# check. An immutable tag reproduces from a fresh clone. The checkout lands in
# the gitignored libs/llvm-project that --libcxx-repo reads.
LLVM_REPO = https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  = circle-stdlib-22.1.3-v2

# deps splits into two phases so a parallel build is safe: the git-heavy FETCH
# (nested-submodule init + LLVM clone) is lock-prone and runs serially, one
# board at a time; the COMPILE (configure + make) touches only that world's own
# isolated checkout, so the three can run concurrently (e.g. one agent/board).
.PHONY: deps
deps:
	@for b in $(BOARDS); do $(MAKE) world-fetch BOARD=$$b || exit 1; done
	@for b in $(BOARDS); do $(MAKE) world-build BOARD=$$b || exit 1; done
	@$(MAKE) all-boards

# FETCH (git, run serially): populate one board's world source. Idempotent.
# LLVM is cloned per world at --depth 1: a shallow clone can't be a --reference
# source ("reference repository is shallow"), and a full clone to share objects
# is larger than three shallow ones — so each world fetches its own shallow copy.
.PHONY: world-fetch
world-fetch:
	git submodule update --init --recursive $(CIRCLE_STDLIB)
	@[ -f $(CIRCLE_STDLIB)/libs/llvm-project/runtimes/CMakeLists.txt ] || \
		git clone --depth 1 --branch $(LLVM_TAG) $(LLVM_REPO) $(CIRCLE_STDLIB)/libs/llvm-project

# COMPILE (isolated per world, safe to run in parallel across boards). Idempotent:
# skips re-configure when Config.mk is already present.
.PHONY: world-build
world-build:
	@[ -f $(CIRCLE_STDLIB)/Config.mk ] || \
		( cd $(CIRCLE_STDLIB) && bash ./configure -r $(RASPPI_$(BOARD)) -p aarch64-none-elf- \
			--libcxx-repo --kernel-max-size 256 -o ARM_ALLOW_MULTI_CORE && $(MAKE) MAKEINFO=true )

# Convenience: fetch then build one board's world.
.PHONY: world
world:
	@$(MAKE) world-fetch BOARD=$(BOARD)
	@$(MAKE) world-build BOARD=$(BOARD)

.PHONY: all-boards
all-boards:
	@for b in $(BOARDS); do $(MAKE) libSDL2-$$b.a BOARD=$$b || exit 1; done

# The shim targets need the selected board's Config.mk + Rules.mk; guard them so
# `make deps` can parse and run before that world has been configured.
ifneq ($(wildcard $(CIRCLESTDLIBHOME)/Config.mk),)

include $(CIRCLESTDLIBHOME)/Config.mk

SRCS = src/init.cpp src/error.cpp src/timer.cpp src/hints.cpp src/events.cpp \
       src/video.cpp src/input.cpp src/audio.cpp src/perf.cpp src/split.cpp
OBJS = $(SRCS:src/%.cpp=$(OBJDIR)/%.o)
DEPS = $(OBJS:.o=.d)

libSDL2-$(BOARD).a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

# Per-board compile into $(OBJDIR) (Circle's Rules.mk %.o rule builds in-place;
# this more-specific rule wins for the board-scoped object paths). Same recipe
# as Rules.mk, just a redirected output dir.
$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/%.d: src/%.cpp | $(OBJDIR)
	@$(CPP) $(CPPFLAGS) -M -MG -MT $(@:.d=.o) -MT $@ -MF $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

-include $(DEPS)

endif
