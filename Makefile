#
# circle-libsdl2 — SDL2-compatible shim over the Circle bare-metal framework.
# Builds libSDL2.a against its OWN circle-stdlib submodule, configured
# ARM_ALLOW_MULTI_CORE: the shim's core split runs the application on one core
# and presentation on another, so a single-core world cannot serve it.
# `make deps` builds that world then the shim; a plain `make` builds the shim
# once the world is configured. Override the world's location with
# `make CIRCLESTDLIBHOME=/path/to/circle-stdlib` if needed.
#
CIRCLESTDLIBHOME ?= $(CURDIR)/circle-stdlib

.DEFAULT_GOAL := libSDL2.a

# LLVM/libc++ comes from a git checkout at a fixed tag via --libcxx-repo, NOT
# circle-stdlib's default --libcxx tarball: Codeberg regenerates its archives,
# drifting their SHA from the pin, so a clean --libcxx build fails its hash
# check. An immutable tag reproduces from a fresh clone. The checkout lands in
# the gitignored libs/llvm-project that --libcxx-repo reads.
LLVM_REPO = https://codeberg.org/larchcone/llvm-project.git
LLVM_TAG  = circle-stdlib-22.1.3-v2

.PHONY: deps
deps:
	@[ -f circle-stdlib/libs/llvm-project/runtimes/CMakeLists.txt ] || \
		git clone --depth 1 --branch $(LLVM_TAG) $(LLVM_REPO) circle-stdlib/libs/llvm-project
	cd circle-stdlib && bash ./configure -r 4 -p aarch64-none-elf- --libcxx-repo \
		--kernel-max-size 256 -o ARM_ALLOW_MULTI_CORE && $(MAKE) MAKEINFO=true
	$(MAKE) libSDL2.a

# The shim targets need circle-stdlib's Config.mk + Rules.mk; guard them so
# `make deps` can parse and run before that world has been configured.
ifneq ($(wildcard $(CIRCLESTDLIBHOME)/Config.mk),)

include $(CIRCLESTDLIBHOME)/Config.mk

OBJS = src/init.o src/error.o src/timer.o src/hints.o src/events.o src/video.o src/input.o src/audio.o src/perf.o src/split.o

libSDL2.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

STANDARD = -std=c++23 -Wno-volatile

include $(CIRCLEHOME)/Rules.mk

INCLUDE := -I include $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

-include $(DEPS)

endif
