# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi.** Write (or port) an
ordinary SDL2 application, link it against this library and the
[Circle](https://github.com/rsta2/circle) framework, and it boots directly
from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the
SDL2 API surface that real applications call, mapped directly onto Circle's
bare-metal drivers.

## What it is proven on

Its founding use case is [**pi-mame**](https://github.com/Xalior/pi-mame):
MAME's emulation core, running bare-metal on a Raspberry Pi 4 with no
operating system underneath it at all. Not a demo of one machine — pi-mame
ships whole vendor platforms, home computers and consoles alike, and every
machine on every one of them reaches the hardware through this shim. Which
machines those are is pi-mame's business, and its documentation is where they
are listed.

What that port leans on is the interesting part, because a demanding
application asks for all of it: one fullscreen framebuffer at a resolution
fixed by boot config, a software renderer, USB HID keyboards, HDMI audio, disk
images and cartridges read off the SD card, and a per-frame heartbeat that
keeps a cooperatively scheduled machine alive. None of it is MAME-specific —
an ordinary SDL2 application gets the same surface.

## What works

| SDL2 subsystem | Circle backing |
|---|---|
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending | `CBcmFrameBuffer` — double-buffered, vsync page flip |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes) |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue |
| Events: queue, `SDL_PumpEvents`, window focus | also the USB plug-and-play pump and cooperative-scheduler yield point |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs) |
| Init/error/version/hints | — |

Not yet: mouse, game controllers, haptics, OpenGL (the Pi 4 has no
bare-metal GPU driver — software rendering is the design, not a stopgap).

## Design

- **The app owns the main loop; the shim rides it.** `SDL_PumpEvents`
  (called by `SDL_PollEvent`) services USB plug-and-play, translates HID
  reports into events, tops up the audio queue by running your audio
  callback, and yields to Circle's cooperative scheduler. Any app that
  polls events or presents frames keeps the whole machine alive — audio
  callbacks never run in interrupt context.
- **The heartbeat is also the watchdog.** Nothing preempts under a
  cooperative scheduler, so a main loop that stops pumping takes the whole
  board down silently. The pump ticks the host kernel's CPU throttle (Circle
  requires periodic `Update()` calls, or thermal management never runs), logs
  a liveness beat, and re-arms a kernel timer that fires from IRQ context if
  the pump goes quiet for 30 seconds — dumping the scheduler's task list, so
  a wedged system writes its own post-mortem instead of just stopping.
- **The core split: your application gets a core to itself.** Call
  `SDL2Circle_SplitInit` (`SDL2/SDL_circle.h`) and the shim moves the platform
  off your back: core 0 keeps the Circle world — devices, scheduler, a servo
  task and a cross-core watchdog — a presentation core does the blit and the
  page flip, and your application runs alone on a core of its own, reaching
  the hardware through lock-free rings (events in, audio out), a one-deep
  frame mailbox, and a call mailbox for the rare marshalled call (device
  bring-up, file I/O). Single-core is the same code path, degenerate: without
  `SplitInit` every call executes directly, exactly as before. Multicore
  Circle is required — see Building.
- **Self-contained payloads.** The shim brings up everything it needs
  (USB host controller, framebuffer, sound) inside `SDL_Init`. Host-kernel
  contract: initialize `CInterruptSystem` and `CTimer` before `SDL_Init`;
  run a `CScheduler` if your app uses `std::thread`
  (via [circle-stdlib](https://codeberg.org/larchcone/circle-stdlib)'s
  libc++ threading).
- **Honest headers.** `include/SDL2/` is the official SDL2 2.32.4 header
  set (zlib license, see `SDL2-LICENSE.txt`) with one substitution: an
  `SDL_config.h` for AArch64/newlib/Circle. Your app compiles against the
  genuine SDL2 API; unimplemented entry points fail at link time instead
  of surprising you at runtime.

## Building

**Prerequisites**

- The **Arm GNU toolchain** for `aarch64-none-elf` (bare-metal AArch64) on
  your `PATH` — from the
  [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
- A modern `bash` (5+) and GNU `getopt` on your `PATH` — circle-stdlib's
  `configure` needs `mapfile` and GNU-style option parsing (macOS ships bash
  3.2 and BSD getopt; `brew install bash gnu-getopt` provides both).

**Steps**

```sh
git clone --recursive https://github.com/Xalior/circle-libsdl2.git
cd circle-libsdl2
make deps       # builds the Circle world, then libSDL2.a
```

The shim **owns its runtime world**: `circle-stdlib` — the Circle framework
plus newlib and libc++ — is a nested submodule here, not something you fetch
and configure alongside. `make deps` fetches libc++ from an immutable LLVM tag
(Codeberg regenerates its archives, so the tarball route fails its hash check
on a clean build), configures that world for the Pi 4
(`-r 4 -p aarch64-none-elf- --libcxx-repo --kernel-max-size 256 -o
ARM_ALLOW_MULTI_CORE`) and builds it, then builds the shim against it. Cold,
that is a long build — newlib and libc++ from source. Afterwards, a plain
`make` rebuilds just `libSDL2.a`.

The world is configured **multicore** (`ARM_ALLOW_MULTI_CORE`) because the core
split needs the other cores; a single-core world cannot serve this shim. A
world elsewhere on disk works with `make CIRCLESTDLIBHOME=/path/to/circle-stdlib`,
provided it was configured the same way. **Compile your application with
`-DARM_ALLOW_MULTI_CORE` too** — Circle's headers change shape on that macro
(spinlocks, atomics, memory), so an application compiled without it disagrees,
silently, with the library it links against.

Applications link by including `sdl-app.mk` after Circle's `Rules.mk`
(see any Makefile under `test/`): it links with `sdl-app.ld` — required
with binutils 2.44+, whose linker refuses non-adjacent TLS sections with
the stock script ordering (libc++'s threading carries TLS) — and adds the
Circle sound library the audio backend needs. `sdl-app.ld` is derived from
Circle's `circle.ld` and remains GPLv3 (see its header); everything else
here is zlib.

## Test apps

Each is a complete bootable kernel exercising one subsystem — useful as
templates:

- `test/gradient` — animated full-screen gradient (video path)
- `test/keyecho` — scancode display, modifier lights, held-key grid (input)
- `test/tone` — 1 kHz sine over HDMI via the callback API (audio)

## License

zlib, matching SDL itself — see `LICENSE`.
