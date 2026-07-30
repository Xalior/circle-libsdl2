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

# THE CORE BRIDGE: what crosses from the application core to the presentation
# core. A build-time choice, baked into the archive — not a boot option and
# not decided by the board.
#
#   frame      one finished pixmap per frame, reduced on the application
#              core. The usual frame is a clear plus one opaque blit, which
#              reduces to the application's own texture where it already
#              sits, so what crosses is a few hundred kilobytes.
#   commands   the recorded draw list, composed on the presentation core.
#              Nothing is reduced; the composing is what moves across.
#
#   make BRIDGE=commands
#
# Both work on any framebuffer grant. `frame` is the default: it is what the
# product board ships today, and it is the mode a frame too complicated to
# record falls back to in either case. See src/sdl2circle.h.
BRIDGE ?= frame

ifeq ($(BRIDGE),frame)
BRIDGE_DEFINE = -DSDL2CIRCLE_BRIDGE=SDL2CIRCLE_BRIDGE_FRAME
else ifeq ($(BRIDGE),commands)
BRIDGE_DEFINE = -DSDL2CIRCLE_BRIDGE=SDL2CIRCLE_BRIDGE_COMMANDS
else
$(error BRIDGE must be `frame` or `commands`, not `$(BRIDGE)`)
endif

# GNU getopt for circle-stdlib's configure (macOS BSD getopt drops long opts ->
# wrong toolchain prefix). ccache is build/ccache.sh's job (mandatory source).
GETOPT_BIN := $(firstword $(wildcard /opt/homebrew/opt/gnu-getopt/bin /usr/local/opt/gnu-getopt/bin))
ifneq ($(GETOPT_BIN),)
export PATH := $(GETOPT_BIN):$(PATH)
endif

# A modern bash (5+) for `bash ./configure` (macOS ships 3.2, which lacks
# mapfile; the invocation is PATH-resolved for exactly this reason). Same
# conditional shape as gnu-getopt: prepend brew's bin only where a brew bash
# exists; no-op on Linux/CI.
BASH5_BIN := $(firstword $(wildcard /opt/homebrew/bin/bash /usr/local/bin/bash))
ifneq ($(BASH5_BIN),)
export PATH := $(patsubst %/,%,$(dir $(BASH5_BIN))):$(PATH)
endif

CIRCLE_STDLIB     = circle-stdlib-$(BOARD)
CIRCLESTDLIBHOME ?= $(CURDIR)/$(CIRCLE_STDLIB)

# Per-board, per-bridge object tree, so all three archives coexist without one
# board's objects clobbering another's — no `make clean` between boards, each
# is its own cacheable unit. The bridge is in the path for the same reason and
# a sharper one: it changes what the objects contain but not their timestamps,
# so sharing a tree between the two modes would leave `make BRIDGE=...` with
# nothing to rebuild and quietly hand back the other mode's archive.
OBJDIR = build/$(BOARD)-$(BRIDGE)

.DEFAULT_GOAL := libSDL2-$(BOARD).a

# LLVM/libc++ comes from a git checkout at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball: Codeberg regenerates its archives,
# drifting their SHA from the pin, so a clean --libcxx build fails its hash
# check. An immutable tag reproduces from a fresh clone. The checkout lands in
# the gitignored libs/llvm-project that --libcxx-repo reads.
#
# It is on Codeberg because that is the only place it exists: the tag names a
# tree carrying the bare-metal patches, and neither the tag nor its commits
# are in llvm/llvm-project. Codeberg times out often enough to fail a build
# that would otherwise have worked, so override LLVM_REPO to clone from a
# mirror of your own.
LLVM_REPO ?= https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  ?= circle-stdlib-22.1.3-v2

# How many times to try that clone before giving up. A gateway timeout on a
# 3 GB fetch is a bad reason to lose a whole dependency build.
LLVM_CLONE_TRIES ?= 3

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
	@[ -f $(CIRCLE_STDLIB)/libs/llvm-project/runtimes/CMakeLists.txt ] || { \
		n=1; \
		until git clone --depth 1 --branch $(LLVM_TAG) $(LLVM_REPO) \
				$(CIRCLE_STDLIB)/libs/llvm-project; do \
			rm -rf $(CIRCLE_STDLIB)/libs/llvm-project; \
			if [ $$n -ge $(LLVM_CLONE_TRIES) ]; then \
				echo "  LLVM  clone failed $$n times from $(LLVM_REPO)"; \
				exit 1; \
			fi; \
			echo "  LLVM  clone failed, retrying ($$n of $(LLVM_CLONE_TRIES))"; \
			n=$$((n+1)); sleep 15; \
		done; }

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
       src/video.cpp src/surface.cpp src/input.cpp src/joystick.cpp \
       src/gamecontroller.cpp src/rwops.cpp src/audio.cpp src/perf.cpp \
       src/split.cpp src/log.cpp src/coreruntime.cpp src/hardware.cpp
OBJS = $(SRCS:src/%.cpp=$(OBJDIR)/%.o)
DEPS = $(OBJS:.o=.d)

# Which bridge the archive on disk was last built with.
#
# The objects live in per-bridge trees, so switching BRIDGE always
# recompiles. The ARCHIVE does not: it has one name whichever bridge made
# it, and the other tree's objects are older than it, so make finds it up to
# date and leaves the previous bridge's build sitting under the new name.
# Two builds differing only by this switch then produce byte-identical
# artifacts and nothing says so — which is the whole point of the switch,
# lost silently.
#
# So when the recorded bridge does not match the requested one, the archive
# is DELETED here, at parse time, before make decides anything. A missing
# target has to be rebuilt; there is no timestamp comparison left to get
# wrong, and no dependence on filesystem clock granularity — which a
# prerequisite on the record file alone does not survive, because the record
# and the archive it invalidates can land in the same second.
#
# The record is rewritten only when the answer changes, so a repeat build in
# the same mode stays fully incremental.
BRIDGE_TAG = build/.bridge-$(BOARD)
$(shell mkdir -p build; \
        [ "$$(cat $(BRIDGE_TAG) 2>/dev/null)" = "$(BRIDGE)" ] \
        || { echo $(BRIDGE) > $(BRIDGE_TAG); rm -f libSDL2-$(BOARD).a; })

libSDL2-$(BOARD).a: $(OBJS)
	@echo "  AR    $@ ($(BRIDGE) bridge)"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

STANDARD = -std=c++23 -Wno-volatile

# Before Rules.mk, which folds DEFINE into the compiler flags.
DEFINE += $(BRIDGE_DEFINE)

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
