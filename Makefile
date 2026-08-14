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
#   make                       list the targets below (also the default goal)
#   make deps                  configure+build all three worlds, then all archives
#   make libSDL2-rpi4.a        build one board's archive against its world
#   make BOARD=rpi3 libSDL2-rpi3.a   the same, another board (default rpi4)
#   make all-boards            (re)build all three archives (worlds already built)
#   make rebuild               drop one board's objects and archive and build both
#                              from nothing
#   make rebuild-all           the same for all three boards
#   make examples              build every example under examples/, against a
#                              freshly rebuilt archive for one board
#   make audit BOARD=rpi5      report symbols the archive promises but does not
#                              define
#
# The consumer picks a board's archive+world explicitly; nothing here assumes
# a single board. `make CIRCLE_WORLDS=/path/to/worlds` puts every board's
# world under one directory instead of this repository's own submodules, so
# several consumers can share them.
#

# GNU make 4.0 or later. macOS ships 3.81 as `make`, and Homebrew installs a
# current one as `gmake`.
#
# 3.81 gets three things wrong that this build cannot survive, and all three
# were paid for here:
#
#   It compares file timestamps to the SECOND. A source rewritten within the
#   same second its object was compiled in is never seen as newer, so the
#   object stays in the archive carrying the older text and a symbol audit run
#   against that archive reports functions as missing that the source plainly
#   defines. 4.x compares to the nanosecond, which APFS records, and rebuilds.
#
#   A dependency file naming a header that has since moved or been deleted
#   stops it with exit 2 and no output at all — nothing names the file.
#
#   A -MG dependency on a header with no rule stops it the same silent way.
#
# The last two no longer apply to this makefile (dependencies are written with
# -MD -MP as a side effect of compiling; see DEPFLAGS below), but the timestamp
# comparison is make's own and cannot be worked around from here.
ifeq ($(filter 1.% 2.% 3.%,$(MAKE_VERSION)),$(MAKE_VERSION))
$(error this build needs GNU make 4.0 or later; this is '$(MAKE)' version '$(MAKE_VERSION)'. Homebrew installs one as gmake.)
endif

BOARDS      = rpi3 rpi4 rpi5
BOARD      ?= rpi4
RASPPI_rpi3 = 3
RASPPI_rpi4 = 4
RASPPI_rpi5 = 5

# An unknown board name otherwise reaches every rule below as an empty RASPPI
# and a world directory that does not exist, and surfaces as make reporting no
# rule for an archive it was never going to be able to name.
ifeq ($(filter $(BOARD),$(BOARDS)),)
$(error BOARD must be one of: $(BOARDS) — not `$(BOARD)`)
endif

# `make -n` EXECUTES any recipe line containing $(MAKE): make marks such a line
# always-run so a dry run can descend into the sub-make. Every recursive target
# here would therefore build for real. They refuse instead, from a line
# prefixed `+` so that it too runs under -n.
DRY_RUN     := $(findstring n,$(firstword -$(MAKEFLAGS)))
NOT_DRY_RUN  = $(if $(DRY_RUN),echo "$@: no dry run — this recipe drives sub-makes and make -n executes those for real." >&2; exit 1,:)

# THE CROSSING COUNT: how many drawing commands may cross to the presentation
# core as a LIST. A build-time value baked into the archive — not a boot
# option and not decided by the board.
#
# A frame whose draw list fits within the count crosses as a list and is
# composed on the far side. A frame that does not fit crosses as a finished
# picture instead.
#
#   0    every frame crosses as a picture. THE DEFAULT, and it costs nothing
#        extra: the usual clear-plus-one-blit is recognised as being the
#        application's own texture already, so the picture that crosses is
#        that texture where it sits and nothing is painted.
#   n    frames of up to n commands compose on the presentation core.
#
#   make PRESENT_CMDS=8
#
# The ceiling is the recorder's own capacity (SDL2CIRCLE_RECORD_MAX_CMDS in
# src/sdl2circle.h); past that a frame has no list left to send.
PRESENT_CMDS ?= 0

ifneq ($(PRESENT_CMDS),$(filter $(PRESENT_CMDS),0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
$(error PRESENT_CMDS must be a whole number from 0 to 16, not `$(PRESENT_CMDS)`)
endif

# The Arm GNU aarch64-none-elf cross toolchain.
#
# Established here so that building this library needs nothing but this
# makefile. Driven from a consumer's own root makefile the toolchain is
# usually already exported into the environment, and then this changes
# nothing; run on its own (`make -C circle-libsdl2 libSDL2-rpi5.a`) it is
# what stops every object failing at once with `aarch64-none-elf-g++:
# command not found`, which reads like a broken source tree rather than a
# PATH that was never set.
#
# PATH is honoured first, so a machine that already has the toolchain
# installed is left alone. Failing that, in order:
#
#   $RAPI_TOOLCHAIN_DIR   names where the toolchain lives
#   toolchains/           unpacked into this checkout
#   ../toolchains/        and two levels above, which is where it is found
#   ../../toolchains/     when this repository is used as a submodule
#
# Each may be the unpacked toolchain itself (it has a bin/) or a directory
# holding one or more unpacked releases. Get release 15.2.Rel1 for the
# aarch64-none-elf target, built for the machine you compile ON, from
# https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
TOOLCHAIN_SEARCH := $(RAPI_TOOLCHAIN_DIR) $(CURDIR)/toolchains \
                    $(CURDIR)/../toolchains $(CURDIR)/../../toolchains

ifeq ($(shell command -v aarch64-none-elf-gcc 2>/dev/null),)
TOOLCHAIN_BIN := $(firstword \
	$(wildcard $(addsuffix /arm-gnu-toolchain-*-aarch64-none-elf/bin,$(TOOLCHAIN_SEARCH))) \
	$(wildcard $(addsuffix /bin,$(TOOLCHAIN_SEARCH))))
ifneq ($(TOOLCHAIN_BIN),)
export PATH := $(TOOLCHAIN_BIN):$(PATH)
endif
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

# WHERE THE PER-BOARD WORLDS LIVE.
#
# The default is this repository, where they are submodules of it: a plain
# clone and a CI runner are self-contained with nothing set, which is the
# behaviour every consumer has today.
#
# Point several consumers at one directory and each board's world is built
# once and shared. A world is gigabytes and its sources are pinned, so two
# consumers on the same pin with the same configuration produce the same
# bytes twice over; whoever shares them owns initialising them, because a
# world outside this repository is not this repository's submodule to fetch.
#
# Same shape as CIRCLE_LLVM below, and for the same reason.
CIRCLE_WORLDS    ?= $(CURDIR)
CIRCLE_STDLIB     = $(CIRCLE_WORLDS)/circle-stdlib-$(BOARD)
CIRCLESTDLIBHOME ?= $(abspath $(CIRCLE_STDLIB))

# Per-board, per-count object tree, so all three archives coexist without one
# board's objects clobbering another's — no `make clean` between boards, each
# is its own cacheable unit. The crossing count is in the path for the same
# reason and a sharper one: it changes what the objects contain but not their
# timestamps, so sharing a tree between counts would leave
# `make PRESENT_CMDS=...` with nothing to rebuild and quietly hand back the
# previous count's archive.
OBJDIR = build/$(BOARD)-cmds$(PRESENT_CMDS)

# Plain `make` names no target, so it has to say what is available rather
# than pick one archive to build silently. Nothing in this repository or the
# one enclosing it calls `make` here without naming a target, so this is safe
# to change without breaking an existing caller.
.DEFAULT_GOAL := help

# Every application under examples/, discovered from the directory rather
# than listed here, so adding one needs no matching line in this file.
EXAMPLES := $(patsubst examples/%/,%,$(sort $(dir $(wildcard examples/*/Makefile))))

.PHONY: help
help:
	@echo "circle-libsdl2 - SDL2-compatible shim over the Circle bare-metal framework."
	@echo ""
	@echo "  make help                        this text (also the default goal)"
	@echo "  make deps                        configure+build all three worlds, then all archives"
	@echo "  make libSDL2-$(BOARD).a              build BOARD's archive against its world (BOARD=$(BOARD))"
	@echo "  make BOARD=rpi3 libSDL2-rpi3.a   the same, another board"
	@echo "  make all-boards                  (re)build all three archives (worlds already built)"
	@echo "  make rebuild                     drop BOARD's objects and archive, build both from nothing"
	@echo "  make rebuild-all                 the same for all three boards"
	@echo "  make examples                    build every example under examples/, against a freshly"
	@echo "                                   rebuilt archive for BOARD"
	@echo "  make audit BOARD=rpi5            symbols the archive references but does not define, and"
	@echo "                                   symbols its headers declare but src/ does not"
	@echo "  make world BOARD=rpi3            fetch and configure+build one board's world alone"
	@echo ""
	@echo "boards: $(BOARDS)"
	@echo "examples: $(EXAMPLES)"

# One BOARD is configured per invocation, so the other boards' archives have
# no rule here. Asking for one by name got "Nothing to be done" and an exit
# status of zero — a build that never happened, reported as success. Say what
# to run instead, and fail.
OTHER_ARCHIVES := $(filter-out libSDL2-$(BOARD).a,$(BOARDS:%=libSDL2-%.a))
.PHONY: $(OTHER_ARCHIVES)
$(OTHER_ARCHIVES):
	@echo "$@ is not built by BOARD=$(BOARD)."
	@echo "Run: $(MAKE) BOARD=$(patsubst libSDL2-%.a,%,$@)"
	@exit 1

# LLVM/libc++ comes from a git checkout at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball: Codeberg regenerates its archives,
# drifting their SHA from the pin, so a clean --libcxx build fails its hash
# check. An immutable tag reproduces from a fresh clone. The checkout lands in
# the gitignored libs/llvm-project that --libcxx-repo reads.
#
# It is on Codeberg because that is the only place it exists: the tag names a
# tree carrying the bare-metal patches, and neither the tag nor its commits
# are in llvm/llvm-project. Codeberg is a small volunteer-run forge and it
# times out often enough to fail a build that would otherwise have worked, so
# override LLVM_REPO to clone from a mirror of your own.
LLVM_REPO ?= https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  ?= circle-stdlib-22.1.3-v2

# How many times to try that clone before giving up. A gateway timeout on the
# fetch is a bad reason to lose a whole dependency build.
LLVM_CLONE_TRIES ?= 3

# ONE checkout of that tag, shared by every world. The worlds differ in how
# they are CONFIGURED, never in these sources, so three copies of one
# immutable tag was three downloads of identical bytes and three times the
# disk.
#
# Sharing was previously ruled out because a shallow clone cannot be a
# --reference source, and a full clone to share objects is larger than three
# shallow ones. A sparse checkout answers both: it is its own shallow clone,
# so nothing references anything, and it is far smaller than even one full
# checkout. The worlds then symlink to it.
#
# The subset is the three runtime libraries, the cmake modules
# runtimes/CMakeLists.txt reaches for (../cmake, ../llvm/cmake, ../third-party,
# and ../llvm for llvm/utils/llvm-lit), and libc -- libc++'s from_chars
# includes libc/shared, which in turn reaches libc/src/__support. Around
# 525 MB against a 2.8 GB worktree; clang, lldb, mlir, flang and the rest
# never arrive.
#
# Every entry is here because cmake or a compile named the thing it could not
# find. Trim one and the build tells you which, some minutes in.
#
# CIRCLE_LLVM names the checkout. The default sits beside this repository, so
# a plain clone and a CI runner are each self-contained with nothing set;
# point several projects at one path to share the fetch across all of them.
CIRCLE_LLVM ?= $(abspath $(CURDIR)/../circle-llvm)
LLVM_SPARSE  = libcxx libcxxabi libunwind runtimes cmake llvm/cmake \
               llvm/utils third-party libc

# Fetched once, then left alone: the tag is immutable, so a checkout that
# already carries the runtimes tree is finished by definition.
.PHONY: llvm-cache
llvm-cache:
	@[ -f $(CIRCLE_LLVM)/runtimes/CMakeLists.txt ] || { \
		n=1; \
		echo "  LLVM  $(LLVM_TAG) -> $(CIRCLE_LLVM) (once)"; \
		until git clone --quiet --depth 1 --branch $(LLVM_TAG) --no-checkout \
					--sparse $(LLVM_REPO) $(CIRCLE_LLVM) \
			&& git -C $(CIRCLE_LLVM) sparse-checkout set --cone $(LLVM_SPARSE) \
			&& git -C $(CIRCLE_LLVM) checkout --quiet; do \
			rm -rf $(CIRCLE_LLVM); \
			if [ $$n -ge $(LLVM_CLONE_TRIES) ]; then \
				echo "  LLVM  clone failed $$n times from $(LLVM_REPO)"; \
				exit 1; \
			fi; \
			echo "  LLVM  clone failed, retrying ($$n of $(LLVM_CLONE_TRIES))"; \
			n=$$((n+1)); sleep 15; \
		done; }

# deps splits into two phases so a parallel build is safe: the git-heavy FETCH
# (nested-submodule init + LLVM clone) is lock-prone and runs serially, one
# board at a time; the COMPILE (configure + make) touches only that world's own
# isolated checkout, so the three can run concurrently (e.g. one agent/board).
.PHONY: deps
deps:
	+@$(NOT_DRY_RUN)
	@for b in $(BOARDS); do $(MAKE) world-fetch BOARD=$$b || exit 1; done
	@for b in $(BOARDS); do $(MAKE) world-build BOARD=$$b || exit 1; done
	@$(MAKE) all-boards

# FETCH (git, run serially): populate one board's world source. Idempotent.
# The world's libc++ sources are a symlink to the one shared checkout above,
# which every board and every project beside this one points at.
#
# THE LIBC++ FETCH IS ASKED FOR ONLY WHEN THE WORLD HAS NO libc++ ALREADY.
# It used to be a prerequisite, so it ran first, unconditionally — and a world
# whose symlink already resolved then declined to use what had just been
# fetched. Run from a directory where CIRCLE_LLVM points somewhere that does
# not exist yet, that is 525 MB downloaded and immediately ignored, left
# behind as an untracked directory nothing reads.
.PHONY: world-fetch
world-fetch:
	@if [ "$(CIRCLE_WORLDS)" = "$(CURDIR)" ]; then \
		git submodule update --init --recursive circle-stdlib-$(BOARD); \
	elif [ ! -d "$(CIRCLE_STDLIB)/libs/circle" ]; then \
		echo "no $(BOARD) world at $(CIRCLE_STDLIB)." >&2; \
		echo "CIRCLE_WORLDS names a directory someone else owns, so this" >&2; \
		echo "makefile will not fetch into it. Populate it, or unset" >&2; \
		echo "CIRCLE_WORLDS to use this repository's own submodules." >&2; \
		exit 1; \
	fi
	@[ -f $(CIRCLE_STDLIB)/libs/llvm-project/runtimes/CMakeLists.txt ] || { \
		$(MAKE) llvm-cache \
		&& ln -sfn $(CIRCLE_LLVM) $(CIRCLE_STDLIB)/libs/llvm-project; }

# Every core's stack, including the one an application runs on: 2 MB, four
# cores, 8 MB of a board's memory. A consumer that needs more says so:
#
#   make world CIRCLE_KERNEL_STACK_SIZE=0x400000
#
# 2 MB RATHER THAN CIRCLE'S 128 KB, AND THE SAME FOR EVERY CONSUMER.
#
# The stacks are laid out one after another WITH NO GUARD PAGE BETWEEN THEM.
# A core that runs off the bottom of its stack does not fault: it writes into
# the core below's. Under this library's core split that is the application
# core writing over core 0's, where the host kernel object itself lives — a
# Circle kernel being a local of main(). What is seen is the picture
# corrupting for a frame, and then a device handler on core 0 dereferencing
# something the application overwrote, a fault nowhere near the code that
# caused it and on a core that did nothing wrong.
#
# That is why this is not left to each consumer to discover. A too-small
# stack does not report itself; it reports as a fault somewhere else, days
# later, in someone else's code. TyrQuake's software renderer allocas its
# edge and surface arrays every frame — about 198 KB at the engine's own
# minimum limits on a 64-bit target, and its source says it expects at least
# a megabyte — so the first frame of real geometry ran a core off the bottom
# of 128 KB. Nothing about that is exotic; it is an ordinary renderer written
# for machines with megabytes of stack, which is most of them.
CIRCLE_KERNEL_STACK_SIZE ?= 0x200000

# COMPILE (isolated per world, safe to run in parallel across boards). Idempotent:
# skips re-configure when Config.mk is already present.
.PHONY: world-build
world-build:
	+@$(NOT_DRY_RUN)
	@[ -f $(CIRCLE_STDLIB)/Config.mk ] || \
		( cd $(CIRCLE_STDLIB) && bash ./configure -r $(RASPPI_$(BOARD)) -p aarch64-none-elf- \
			--libcxx-repo --kernel-max-size 256 -o ARM_ALLOW_MULTI_CORE \
			-o KERNEL_STACK_SIZE=$(CIRCLE_KERNEL_STACK_SIZE) && $(MAKE) MAKEINFO=true )

# Convenience: fetch then build one board's world.
.PHONY: world
world:
	+@$(NOT_DRY_RUN)
	@$(MAKE) world-fetch BOARD=$(BOARD)
	@$(MAKE) world-build BOARD=$(BOARD)

.PHONY: all-boards
all-boards:
	+@$(NOT_DRY_RUN)
	@for b in $(BOARDS); do $(MAKE) libSDL2-$$b.a BOARD=$$b || exit 1; done

# One board from nothing: its objects and its archive are removed before the
# build, so nothing on disk can answer for a source make did not read.
#
# An incremental build is a decision made from timestamps, and a timestamp is
# evidence about when a file was written rather than about what is in it. Any
# reading taken off an incrementally built archive — a symbol list, a size, a
# member listing — is a reading of whatever the last build happened to put
# there. Take a measurement that has to be right off one of these.
.PHONY: rebuild rebuild-all
rebuild:
	+@$(NOT_DRY_RUN)
	@rm -rf $(OBJDIR) libSDL2-$(BOARD).a
	@$(MAKE) libSDL2-$(BOARD).a BOARD=$(BOARD) PRESENT_CMDS=$(PRESENT_CMDS)

rebuild-all:
	+@$(NOT_DRY_RUN)
	@for b in $(BOARDS); do $(MAKE) rebuild BOARD=$$b || exit 1; done

# Every example, built standalone (each has its own Makefile under
# examples/, and links against ../../libSDL2-$(BOARD).a the way any consumer
# of this library would). This only walks the list and refuses to be fooled
# by anything already sitting on disk:
#
#   the archive is rebuilt from nothing first (`rebuild`, above), so every
#   example that follows links a library this invocation actually produced,
#   not one left over from an earlier build with different sources;
#
#   each example's own image is deleted before it is asked to build again,
#   so a build that fails, or that make decides needs nothing done, cannot
#   leave yesterday's image sitting there to be mistaken for today's.
#
# It keeps going past a failure — one broken example must never hide the
# other ten — and reports at the end which examples built and which did not.
.PHONY: examples
examples: rebuild
	+@$(NOT_DRY_RUN)
	@built=""; broke=""; \
	for e in $(EXAMPLES); do \
		echo ""; echo "===== $$e ====="; \
		rm -f examples/$$e/*.img; \
		if $(MAKE) -C examples/$$e BOARD=$(BOARD) \
			&& ls examples/$$e/*.img >/dev/null 2>&1; then \
			built="$$built $$e"; \
		else \
			broke="$$broke $$e"; \
		fi; \
	done; \
	echo ""; echo "===== built =====$$built"; \
	[ -z "$$broke" ] || { echo "===== DID NOT BUILD =====$$broke" >&2; exit 1; }

# THE TWO WAYS A SYMBOL CAN BE MISSING, and only one of them shows up in an
# ordinary build.
#
#   undefined  the archive REFERENCES a symbol it does not define. A
#              selective link may never pull the member that needs it, so
#              this hides until someone links the archive whole.
#
#   declared   a public header PROMISES a function that src/ never defines.
#              Nothing inside the archive refers to it, so the undefined
#              check above cannot see it: only a consumer calling it finds
#              out, one consumer at a time, at the very end of its build.
#
# The second is the one that kept costing. It is a promise this library makes
# in its own headers, so it is this library's business to know which promises
# it is not keeping — rather than learning it from whoever tried to call one.
#
#   make audit BOARD=rpi5
#
# Neither list is required to be empty. Plenty of SDL2 belongs to hardware
# that is not here, and is honestly absent. The point is that the list is
# KNOWN and deliberate, not discovered by accident.
.PHONY: audit
audit: rebuild
	@echo "== referenced by the archive, defined nowhere in it =="
	@$(PREFIX)nm --defined-only libSDL2-$(BOARD).a | awk '{print $$3}' | sort -u > .audit-def
	@$(PREFIX)nm -u libSDL2-$(BOARD).a | awk '$$1=="U"{print $$2}' | sort -u > .audit-und
	@comm -23 .audit-und .audit-def | grep -E '^(SDL_|IMG_|Mix_)' || echo "  (none)"
	@echo
	@echo "== declared by include/SDL2, defined nowhere in src =="
	@grep -hoE 'extern[[:space:]]+DECLSPEC[[:space:]]+[^;]*SDLCALL[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
		include/SDL2/*.h \
		| grep -oE 'SDLCALL[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' | awk '{print $$2}' \
		| sort -u > .audit-decl
	@comm -23 .audit-decl .audit-def | sed 's/^/  /' || true
	@echo
	@echo "declared $$(wc -l < .audit-decl), defined $$(grep -cE '^(SDL_|IMG_|Mix_)' .audit-def), not defined $$(comm -23 .audit-decl .audit-def | wc -l)"
	@rm -f .audit-def .audit-und .audit-decl

# The shim targets need the selected board's Config.mk + Rules.mk; guard them so
# `make deps` can parse and run before that world has been configured.
ifneq ($(wildcard $(CIRCLESTDLIBHOME)/Config.mk),)

include $(CIRCLESTDLIBHOME)/Config.mk

SRCS = src/init.cpp src/error.cpp src/timer.cpp src/hints.cpp src/events.cpp \
       src/video.cpp src/surface.cpp src/input.cpp src/joystick.cpp \
       src/gamecontroller.cpp src/rwops.cpp src/audio.cpp src/perf.cpp \
       src/split.cpp src/log.cpp src/console.cpp src/stdio.cpp src/coreruntime.cpp src/hardware.cpp \
       src/mouse.cpp src/pixels.cpp src/blit.cpp src/bmp.cpp src/rect.cpp \
       src/threads.cpp src/stdinc.cpp src/filesystem.cpp src/clipboard.cpp \
       src/messagebox.cpp src/keyname.cpp src/platform.cpp src/image.cpp src/audiocvt.cpp src/mixer.cpp src/gl.cpp src/bootargs.cpp src/ctors.cpp src/libcxxthreading.cpp
OBJS = $(SRCS:src/%.cpp=$(OBJDIR)/%.o)
DEPS = $(OBJS:.o=.d)

# Dependency files are written as a side effect of compiling rather than by
# rules of their own, and Circle's Rules.mk is told to keep its hands off
# (CHECK_DEPS) so its `-M -MG` .d rules are not defined for these objects.
#
# -MG lists a header make has no rule for as a prerequisite of the object,
# which stops the build; GNU make 3.81, which macOS ships, stops with exit 2
# and prints not one line saying why. A .d left over from an earlier build
# naming a header that has since moved or been deleted stops it the same way,
# and is why "delete the object directory" kept being the cure.
#
# -MD writes the dependency file alongside the object it belongs to, so the
# two can never disagree about the flags that produced them, and -MP adds a
# rule with no recipe for every header named, so a header that disappears
# means a rebuild instead of a dead stop.
CHECK_DEPS = 0
DEPFLAGS   = -MD -MP

# Which crossing count the archive on disk was last built with.
#
# The objects live in per-count trees, so changing PRESENT_CMDS always
# recompiles. The ARCHIVE does not: it has one name whatever count made it,
# and the other tree's objects are older than it, so make finds it up to date
# and leaves the previous count's build sitting under the new name. Two builds
# differing only by this value then produce byte-identical artifacts and
# nothing says so — the value silently ignored.
#
# So when the recorded count does not match the requested one, the archive is
# DELETED here, at parse time, before make decides anything. A missing target
# has to be rebuilt; there is no timestamp comparison left to get wrong, and
# no dependence on filesystem clock granularity — which a prerequisite on the
# record file alone does not survive, because the record and the archive it
# invalidates can land in the same second.
#
# The record is rewritten only when the answer changes, so a repeat build at
# the same count stays fully incremental.
# Skipped under `make -n`, which expands this the same as a real run and
# would otherwise have a dry run delete a build artifact.
CMDS_TAG = build/.cmds-$(BOARD)
ifeq (,$(findstring n,$(firstword -$(MAKEFLAGS))))
$(shell mkdir -p build; \
        [ "$$(cat $(CMDS_TAG) 2>/dev/null)" = "$(PRESENT_CMDS)" ] \
        || { echo $(PRESENT_CMDS) > $(CMDS_TAG); rm -f libSDL2-$(BOARD).a; })
endif

libSDL2-$(BOARD).a: $(OBJS)
	@echo "  AR    $@ (present cmds $(PRESENT_CMDS))"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

STANDARD = -std=c++23 -Wno-volatile

# Before Rules.mk, which folds DEFINE into the compiler flags.
DEFINE += -DSDL2CIRCLE_PRESENT_MAX_CMDS=$(PRESENT_CMDS)

include $(CIRCLEHOME)/Rules.mk

# THE CROSS COMPILER GOES THROUGH ccache WHEN THERE IS ONE.
#
# Circle's Rules.mk names the compiler directly ($(PREFIX)g++), so nothing
# here was cached: ccache saw only the host compiles cmake runs while
# building libc++, and answered 15 of 2990 calls. Every cross compile — this
# archive, and every consumer's game and kernel through sdl-app.mk — was a
# full compile every time, however many times the same source had already
# been built from the same headers.
#
# Only if ccache is installed, so nothing here depends on it. AS is left
# alone: Rules.mk sets it from CC before this point, and assembling is not
# worth caching.
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
CPP := $(CCACHE) $(CPP)
CC  := $(CCACHE) $(CC)
endif

# src/libcxxthreading.cpp implements libc++'s external-threading ABI against
# the <__external_threading> header the world was built with, which
# Config.mk's CFLAGS already put on the include path. Nothing private to
# circle-stdlib's own implementation is needed: this is a second, complete
# implementation of the same published interface, and sdl-app.mk gives the
# link exactly one of the two.
INCLUDE := -I include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

# Per-board compile into $(OBJDIR) (Circle's Rules.mk %.o rule builds in-place;
# this more-specific rule wins for the board-scoped object paths). Same recipe
# as Rules.mk, plus $(DEPFLAGS) and a redirected output dir.
$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

-include $(DEPS)

else

# The board's world has not been configured, so not one of the rules above
# exists — including the rule for this board's archive. The archive FILE is
# still on disk from whenever it was last built, so make would find the
# requested target up to date, print "Nothing to be done" and exit zero: a
# build that never ran, reported as a success, with a stale archive left for
# the next kernel to link against. Phony so the file cannot answer for it.
.PHONY: libSDL2-$(BOARD).a
libSDL2-$(BOARD).a:
	@echo "$(CIRCLESTDLIBHOME)/Config.mk is missing: the $(BOARD) world is not configured."
	@echo "Run: $(MAKE) world BOARD=$(BOARD)"
	@exit 1

endif
