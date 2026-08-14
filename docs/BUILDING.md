# Building circle-libsdl2

## Prerequisites

- The **Arm GNU toolchain** for `aarch64-none-elf` (bare-metal AArch64) on your `PATH` - from the [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
- A modern `bash` (5+) and GNU `getopt` on your `PATH` - circle-stdlib's `configure` needs `mapfile` and GNU-style option parsing (macOS ships bash 3.2 and BSD getopt; `brew install bash gnu-getopt` provides both).

## Building the library

```sh
git clone --recursive https://github.com/Xalior/circle-libsdl2.git
cd circle-libsdl2
make deps       # builds every Circle world, then every archive
```

This library **supplies its own runtime world** - the configured `circle-stdlib` build it compiles and links against. `circle-stdlib` is the Circle framework plus newlib and libc++, and it is a nested submodule here, not something you fetch and configure alongside.

**There is one world and one archive per board**, because each is compiled for its own processor and its own `RASPPI` value, and an object built for one board is not usable on another:

| Board | World | Archive |
|---|---|---|
| Pi 3 | `circle-stdlib-rpi3` | `libSDL2-rpi3.a` |
| Pi 4 | `circle-stdlib-rpi4` | `libSDL2-rpi4.a` |
| Pi 5 | `circle-stdlib-rpi5` | `libSDL2-rpi5.a` |

`make deps` does all of them. For each board it fetches the world's sources - including libc++ from an immutable LLVM tag, because Codeberg regenerates its archives and downloading the tarball fails its hash check on a clean build - then configures that world (`-r <board> -p aarch64-none-elf- --libcxx-repo --kernel-max-size 256 -o ARM_ALLOW_MULTI_CORE -o KERNEL_STACK_SIZE=0x200000`) and builds it, and finally builds this library against each. The first build is long: newlib and libc++ are compiled from source, once per board.

Afterwards, name the archive to rebuild one board - `BOARD` selects which, and defaults to `rpi4`:

```sh
make libSDL2-rpi4.a              # the default board
make BOARD=rpi5 libSDL2-rpi5.a   # another board
make all-boards                  # every board
```

Plain `make`, with no target named, prints the list of targets instead of building anything - run `make help` for the same list on demand.

## Building the examples

```sh
make examples                 # every example under examples/, for BOARD
make BOARD=rpi5 examples      # the same, against the Pi 5 archive
```

This rebuilds BOARD's archive from nothing first, so every example links a library this run actually produced, then builds each example under `examples/` in turn, having deleted its own last image first so a failed or skipped build cannot leave a stale one behind to be mistaken for a fresh one. It keeps going past a failure and reports, at the end, which examples built and which did not - a single broken example does not stop the rest from being tried.

An individual example still builds standalone from its own directory (`cd examples/gradient && make BOARD=rpi5`), which is what `make examples` does for each of them in turn.

## Choosing single-core or multicore

**You choose when you configure the world, and the choice is fixed when you build.** Both are supported and the application's source is the same either way.

- **A single-core world** - configured without `ARM_ALLOW_MULTI_CORE` - builds this library with the core split compiled out. Every call runs directly, on the one core, through the same call sites. This is the build for single-core hardware and for older boards.
- **A multicore world** - configured with `ARM_ALLOW_MULTI_CORE`, which is what `make deps` does - builds the split as well. Building it forces nothing on: the split stays inert until a host kernel calls `SDL2Circle_SplitInit`, so one image can still run everything on core 0.

The API is identical. `SDL2Circle_SplitInit` exists in both builds; in a single-core one it reports that there is no multicore world to split into and changes nothing, and `SDL2Circle_SplitActive` continues to answer no, which is the answer every call site already handles.

Building through Circle's `Rules.mk` - as the examples do - you get `ARM_ALLOW_MULTI_CORE` from the world itself, whichever way it was configured, and there is nothing to think about. **If you compile any translation unit outside `Rules.mk`** - a foreign build system with its own flag list - that flag must match the world the object will link against. Circle's headers change shape on it (spinlocks, atomics, memory layout), so an object compiled without it disagrees with the library it links against, and nothing tells you: it builds, it links, and it is wrong at runtime.

A world elsewhere on disk works with `make CIRCLESTDLIBHOME=/path/to/circle-stdlib`.

## Required configuration: `USE_PHYSICAL_COUNTER`

This library requires it, and `make deps` configures a world that has it.

It decides what `CTimer::GetClockTicks64` compiles to. With the option, it is `mrs CNTPCT_EL0` - a CPU system register private to the core that reads it, needing no lock, no device and no other core. Without it, the same call reads the system timer's memory-mapped registers, which is a device, and a device belongs to core 0.

That matters because the timing this library does is not on core 0 and cannot be: an application's frame pacing, every timed wait in the C++ threading runtime, the presentation core's own accounting. Those read the counter constantly, from cores that must never touch a device. With the option they are core-private register reads; without it every one of them is a breach of the rule that keeps this design standing up, and the breach is silent - it builds, it links, and it is wrong on hardware.

So it is not a tuning choice. A world without it does not satisfy this library's requirements, whatever else it is configured with.

## Stack allocation

**Every core gets 2 MB.** Four cores, so 8 MB of the board's memory, and the same for every application that uses this library. Circle's own default is 128 KB a core; this library configures its worlds at 2 MB instead.

If your application needs more than that, ask for it:

```sh
make world CIRCLE_KERNEL_STACK_SIZE=0x400000
```

The reason it is standardised rather than left to each application to discover is that **a stack that is too small does not report itself.** Circle lays the four core stacks out one after another with no guard page between them, so a core that runs past the bottom of its stack writes into the stack of the core below - which, for the application core under the split, is core 0's. A Circle kernel object is a local of `main()`, so it sits at the very top of core 0's stack and is the first thing an overflow reaches. What you see is a picture that corrupts for a frame or two and then a data abort inside a device interrupt handler, pointing at code that did nothing wrong.

An engine that keeps its per-frame working set on the stack is the case to watch, and it is not an exotic one - most renderers written before memory was cheap do it. TyrQuake's `alloca`s its edge and surface arrays on every frame it draws: about 198 KB at the engine's own minimum limits on a 64-bit target, and its source says it expects at least a megabyte. On 128 KB the first frame of real geometry ran a core off the bottom of its stack. `alloca` of an array sized for 32-bit pointers also grows by about 1.7 times when every pointer in it is eight bytes.

**A world already configured keeps the stacks it was configured with.** The value is fixed into `Config.mk` at configure time and compiled into the world's startup code, so changing it means reconfiguring and rebuilding that world, however recently the library was rebuilt against it.

## Choosing the crossing count

`make PRESENT_CMDS=n` (0 by default) sets how much of a frame may travel to the presentation core as a list of drawing commands rather than as a finished picture - see [Choosing what crosses](CORE-SPLIT.md#choosing-what-crosses-the-crossing-count). The value is compiled into the archive, objects are kept in per-count trees so builds never mix, and changing it deletes the archive rather than risk returning the previous count's build under the same name.

Applications link by including `sdl-app.mk` after Circle's `Rules.mk` (see any Makefile under `examples/`): it links with `sdl-app.ld` - required with binutils 2.44+, whose linker refuses non-adjacent TLS sections with the default script ordering (libc++'s threading carries TLS) - and adds the Circle sound library the audio backend needs. `sdl-app.ld` is derived from Circle's `circle.ld` and remains GPLv3 (see its header); everything else here is zlib.

## Catching a stub the library has replaced

An application that defines its own empty versions of SDL calls this library does not implement yet should link the archive in full while developing:

```make
LIBS = --whole-archive $(SHIM)/libSDL2-$(BOARD).a --no-whole-archive \
	$(CIRCLE_STDLIB_LIBS)
```

An object file linked directly into the kernel takes precedence over an archive member defining the same symbol, and the linker reports nothing when it does. So once this library implements a call an application had stubbed, the application keeps calling its own empty version: the real one is never linked in, and no warning is produced. The symptom is that the library appears not to work.

Linking the archive in full makes the same situation a duplicate-symbol error, naming both definitions. The correct fix is then to delete the stub.

`--no-whole-archive` ends the effect immediately after this archive, so the C library, libc++ and Circle are linked as before. An application that already uses most of this library gains nothing in size from the change.

This is a setting for development rather than for a shipped build. An application that deliberately uses a small part of the library will carry the rest of it. Nothing here requires the setting, and no example sets it.

**It is also the only test that proves an application carries no SDL of its own.** Reading through an application's source for leftover SDL functions proves nothing - the one that matters is the one nobody thought to look at. A whole-archive link decides it mechanically: every archive member is pulled in, so any function the application still defines for itself collides with this library's and is named in the error.

So a whole-archive link that produces **no duplicate symbols** is positive proof, rather than an absence of evidence. It is worth running once after removing an application's private SDL, and it is the check to apply before declaring that removal finished.

The library holds itself to the same standard: every SDL, `IMG_` and `Mix_` symbol the archive references, the archive defines. A symbol that is declared in a header and defined nowhere is invisible to a selective link and fails only under whole-archive - which would make this check unsatisfiable for everyone. The sweep that shows it:

```sh
nm --defined-only libSDL2-<board>.a | awk '{print $3}'         | sort -u > defined
nm -u             libSDL2-<board>.a | awk '$1=="U"{print $2}'  | sort -u > undefined
comm -23 undefined defined | grep -E '^(SDL_|IMG_|Mix_)'
```

Run it against a full `make rebuild`, never an incremental build.
