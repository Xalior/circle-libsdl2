# Building circle-libsdl2

## Prerequisites

- The Arm GNU toolchain for `aarch64-none-elf` (bare-metal AArch64) on your `PATH`. Get it from the [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
- `bash` 5 or later and GNU `getopt` on your `PATH`. circle-stdlib's `configure` uses `mapfile` and GNU-style option parsing. macOS ships bash 3.2 and BSD getopt. `brew install bash gnu-getopt` gives you both.
- GNU `make` 4.0 or later. Version 3.x compares file timestamps to the second, so a source file rewritten in the same second its object was compiled is never seen as newer, and the stale object goes into the link. macOS ships 3.81 as `make`. `brew install make` installs a current one as `gmake`, and every `make` in this document means that one. The library's Makefile and every example refuse to run under 3.x and say so.

## Building the library

```sh
git clone --recursive https://github.com/Xalior/circle-libsdl2.git
cd circle-libsdl2
make deps       # builds every Circle world, then every archive
```

The library carries its own runtime world, the configured `circle-stdlib` build it compiles and links against. `circle-stdlib` is the Circle framework plus newlib and libc++. It is a nested submodule here. You do not fetch or configure it yourself.

There is one world and one archive per board. Each is compiled for its own processor and its own `RASPPI` value, so an object built for one board does not work on another.

| Board | World | Archive |
|---|---|---|
| Pi 3 | `circle-stdlib-rpi3` | `libSDL2-rpi3.a` |
| Pi 4 | `circle-stdlib-rpi4` | `libSDL2-rpi4.a` |
| Pi 5 | `circle-stdlib-rpi5` | `libSDL2-rpi5.a` |

`make deps` does all of them. For each board it fetches the world's sources, configures the world, builds it, and then builds this library against it. libc++ comes from a git checkout at a fixed LLVM tag rather than a tarball, because Codeberg regenerates its archives and the tarball fails its hash check on a clean build. The configure line is `-r <board> -p aarch64-none-elf- --libcxx-repo --kernel-max-size 255 -o ARM_ALLOW_MULTI_CORE -o KERNEL_STACK_SIZE=0x200000`. The first build is long. newlib and libc++ compile from source, once per board.

After that, name the archive to rebuild one board. `BOARD` selects which, and defaults to `rpi4`.

```sh
make libSDL2-rpi4.a              # the default board
make BOARD=rpi5 libSDL2-rpi5.a   # another board
make all-boards                  # every board
```

Plain `make` with no target prints the list of targets and builds nothing. `make help` prints the same list.

## Building the examples

```sh
make examples                 # every example under examples/, for BOARD
make BOARD=rpi5 examples      # the same, against the Pi 5 archive
```

This rebuilds BOARD's archive from nothing first, so every example links a library this run produced. It then builds each example under `examples/` in turn. Each example deletes its own last image before it builds, so a failed or skipped build cannot leave a stale image behind. A failure does not stop the run. At the end it reports which examples built and which did not.

An individual example also builds from its own directory with `cd examples/gradient && make BOARD=rpi5`. That is what `make examples` runs for each one.

## Choosing single-core or multicore

You choose when you configure the world, and the build fixes the choice. Both work, and the application's source is the same either way.

- A single-core world, configured without `ARM_ALLOW_MULTI_CORE`, builds this library with the core split compiled out. Every call runs on the one core, through the same call sites. Use this for single-core hardware and older boards.
- A multicore world, configured with `ARM_ALLOW_MULTI_CORE`, which is what `make deps` does, builds the split too. Building it turns nothing on. The split stays inert until a host kernel calls `SDL2Circle_SplitInit`, so one image can still run everything on core 0.

The API is the same in both. `SDL2Circle_SplitInit` exists in both builds. In a single-core build it reports that there is no multicore world to split into and changes nothing, and `SDL2Circle_SplitActive` keeps answering no. Every call site already handles that answer.

If you build through Circle's `Rules.mk`, as the examples do, you get `ARM_ALLOW_MULTI_CORE` from the world itself, however it was configured. If you compile any translation unit outside `Rules.mk`, in a foreign build system with its own flag list, that flag must match the world the object links against. Circle's headers change layout on it (spinlocks, atomics, memory layout). An object compiled without it disagrees with the library it links against. It builds and links, and fails at runtime.

A world elsewhere on disk works with `make CIRCLESTDLIBHOME=/path/to/circle-stdlib`.

## `USE_PHYSICAL_COUNTER`

This library requires it, and every world it builds has it. Circle defines it in `sysconfig.h` for `RASPPI >= 2`, which is every board here. Only `NO_PHYSICAL_COUNTER` removes it.

It appears in no world's `Config.mk`, so searching for it there tells you nothing. A correctly configured world returns an empty search. The thing to look for is a world built with `NO_PHYSICAL_COUNTER`. That world breaks this library.

The option decides what `CTimer::GetClockTicks64` compiles to. With it, the call is `mrs CNTPCT_EL0`, a CPU system register private to the core that reads it. It needs no lock, no device and no other core. Without it, the same call reads the system timer's memory-mapped registers. That is a device, and a device belongs to core 0.

The timing this library does is not on core 0. An application's frame pacing, every timed wait in the C++ threading runtime and the presentation core's own accounting all read the counter constantly, from cores that must never touch a device. With the option those are register reads. Without it every one of them is a device access from the wrong core, and nothing reports it. It builds and links, and fails on hardware.

A world without the option does not meet this library's requirements, whatever else it is configured with.

## Stack allocation

Every core gets 2 MB. Four cores, so 8 MB of the board's memory, for every application that uses this library. Circle's own default is 128 KB a core. This library configures its worlds at 2 MB instead.

If your application needs more, ask for it:

```sh
make world CIRCLE_KERNEL_STACK_SIZE=0x400000
```

The size is fixed here rather than left to each application because a stack that is too small does not report itself. Circle lays the four core stacks out one after another with no guard page between them. A core that runs past the bottom of its stack writes into the stack of the core below. For the application core under the split, that is core 0's stack. A Circle kernel object is a local of `main()`, so it sits at the top of core 0's stack and is the first thing an overflow reaches. What you see is a picture that corrupts for a frame or two, then a data abort inside a device interrupt handler, pointing at code that did nothing wrong.

An engine that keeps its per-frame working set on the stack is the case to watch. Most renderers written before memory was cheap do this. TyrQuake `alloca`s its edge and surface arrays on every frame it draws. That is about 198 KB at the engine's own minimum limits on a 64-bit target, and its source says it expects at least a megabyte. On 128 KB the first frame of real geometry ran a core off the bottom of its stack. An `alloca` of an array sized for 32-bit pointers also grows by about 1.7 times when every pointer in it is eight bytes.

A world already configured keeps the stacks it was configured with. The value goes into `Config.mk` at configure time and is compiled into the world's startup code. Changing it means reconfiguring and rebuilding that world, however recently the library was rebuilt against it.

## Choosing the crossing count

`make PRESENT_CMDS=n` (0 by default) sets how much of a frame may travel to the presentation core as a list of drawing commands rather than as a finished picture. See [Choosing what crosses](CORE-SPLIT.md#choosing-what-crosses-the-crossing-count). The value is compiled into the archive. Objects live in per-count trees, so builds never mix. Changing the value deletes the archive rather than return the previous count's build under the same name.

Applications link by including `sdl-app.mk` after Circle's `Rules.mk`. See any Makefile under `examples/`. It links with `sdl-app.ld` and adds the Circle sound library the audio backend needs. The link script is required with binutils 2.44 and later, whose linker refuses non-adjacent TLS sections under the default script ordering, and libc++'s threading carries TLS. `sdl-app.ld` derives from Circle's `circle.ld` and stays GPLv3 (see its header). Everything else here is zlib.

## Catching a stub the library has replaced

An application that defines its own empty versions of SDL calls this library does not implement yet should link the archive in full while developing:

```make
LIBS = --whole-archive $(SHIM)/libSDL2-$(BOARD).a --no-whole-archive \
	$(CIRCLE_STDLIB_LIBS)
```

An object file linked directly into the kernel wins over an archive member that defines the same symbol, and the linker says nothing. So once this library implements a call the application had stubbed, the application keeps calling its own empty version. The real one is never linked in and no warning appears. The library looks as if it does not work.

Linking the archive in full turns that into a duplicate-symbol error naming both definitions. The fix is to delete the stub.

`--no-whole-archive` ends the effect after this archive, so the C library, libc++ and Circle link as before. An application that already uses most of this library grows very little.

This is a development setting, not one for a shipped build. An application that uses a small part of the library will carry the rest of it. Nothing here requires the setting, and no example sets it.

It is also the only test that proves an application carries no SDL of its own. Reading the source for leftover SDL functions proves nothing. A whole-archive link decides it mechanically. Every archive member is pulled in, so any function the application still defines for itself collides with this library's and appears in the error.

A whole-archive link with no duplicate symbols is proof. Run it once after removing an application's private SDL, and before declaring that removal finished.

The library holds itself to the same standard. Every SDL, `IMG_` and `Mix_` symbol the archive references, the archive defines. A symbol declared in a header and defined nowhere is invisible to a selective link and fails only under whole-archive, which would make this check fail for everyone. The sweep that shows it:

```sh
nm --defined-only libSDL2-<board>.a | awk '{print $3}'         | sort -u > defined
nm -u             libSDL2-<board>.a | awk '$1=="U"{print $2}'  | sort -u > undefined
comm -23 undefined defined | grep -E '^(SDL_|IMG_|Mix_)'
```

Run it against a full `make rebuild`, never an incremental build.
