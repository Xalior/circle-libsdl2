# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi** — a library that implements the SDL2 API in place of SDL itself. Write (or port) an ordinary SDL2 application, link it against this library and the [Circle](https://github.com/rsta2/circle) framework, and it boots directly from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the SDL2 API surface that real applications call, mapped directly onto Circle's bare-metal drivers.

It also lets your application **run somewhere other than core 0** and still call SDL in the ordinary way. Circle's own subsystems — scheduler, interrupts, USB, FatFs, sound — are core-0-only by construction, so an application on another core cannot touch any of them. This library removes that restriction: it marshals every platform call back to core 0 through lock-free rings and mailboxes, and provides a presentation worker you can run on a core of your choosing. See [Running off core 0](docs/CORE-SPLIT.md).

## In this repository

[`circle-diskcache/`](circle-diskcache/) is a read cache for a Circle block device. It uses Circle and nothing else — not this library, not SDL — and any Circle project can copy its two files and use it. It is kept here because that is a convenient place for it today, and it may move.

Proven in real use by [pi-mame](https://github.com/Xalior/pi-mame), which runs MAME's emulation core on bare metal through this library — a full application using the whole API surface at once: fullscreen software rendering, USB HID keyboards, HDMI audio, files off the SD card, and its emulation running on a core that never touches a device.

## Quick start

### Building

```sh
git clone --recursive https://github.com/Xalior/circle-libsdl2.git
cd circle-libsdl2
make deps       # builds every Circle world, then every archive
```

See [Building](docs/BUILDING.md) for choosing boards, configuring stack size, and other options.

### Using the library

Every application declares the display it will receive before `SDL_Init`:

```c
#include <SDL2/SDL_circle.h>

if (SDL2Circle_DeclareVirtualDevice(32, 800, 450) != 0)
    fprintf(stderr, "%s\n", SDL_GetError());
```

Then use SDL normally. See the [Examples](docs/EXAMPLES.md) for complete bootable kernels — each exercises one subsystem and stands alone as a worked example.

If your application runs off core 0, see [Running off core 0](docs/CORE-SPLIT.md) for what changes. It's not much: the same SDL calls work, file I/O goes through a marshalled service, and that's it.

## Key topics

- **[What works](docs/FEATURES.md)** — the SDL2 subsystems this library implements, and what's not there yet
- **[Display and video](docs/DISPLAY.md)** — presentation geometry, virtual vs physical display, declaring your screen
- **[Input](docs/INPUT.md)** — keyboard (USB HID, multiple layouts), joysticks, game controllers, mouse
- **[Audio](docs/AUDIO.md)** — callback API, mixing, conversion, timers
- **[Running off core 0](docs/CORE-SPLIT.md)** — core split architecture, marshalling, the servo and watchdog, file I/O
- **[C++ threading](docs/THREADING.md)** — std::mutex, std::thread, thread_local — all work on any core
- **[Logging](docs/LOGGING.md)** — SDL2Circle_Log from any core without blocking
- **[Performance](docs/PERFORMANCE.md)** — profiling and instrumentation
- **[Design](docs/DESIGN.md)** — architecture decisions, ownership rules, what the host kernel does
- **[Building](docs/BUILDING.md)** — prerequisites, per-board archives, single-core vs multicore
- **[Examples](docs/EXAMPLES.md)** — what each example does

## License

zlib, matching SDL itself — see `LICENSE`.

One file is an exception. `sdl-app.ld`, the linker script an application links with, is derived from Circle's `circle.ld` and remains GPLv3; its own header says so.
