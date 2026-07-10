# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi.** Write (or port) an
ordinary SDL2 application, link it against this library and the
[Circle](https://github.com/rsta2/circle) framework, and it boots directly
from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the
SDL2 API surface that real applications call, mapped directly onto Circle's
bare-metal drivers. Proven by its founding use case: **MAME running
bare-metal on a Raspberry Pi 4**, booting ZX Spectrum 48K BASIC and full
NextZXOS (ZX Spectrum Next) with video, USB keyboard, and HDMI audio — all
through this shim.

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

You need two things on your machine first:

1. The **Arm GNU toolchain** for `aarch64-none-elf` (bare-metal AArch64),
   on your `PATH` — from the
   [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
2. A checkout of
   [circle-stdlib](https://codeberg.org/larchcone/circle-stdlib) — the
   Circle framework plus newlib and libc++ (that specific fork carries the
   libc++ build with `std::thread` support this project uses).

Clone the two projects side by side, then configure and build
circle-stdlib for the Pi 4 (this also builds Circle itself):

```sh
git clone --recursive https://codeberg.org/larchcone/circle-stdlib.git
git clone https://github.com/Xalior/circle-libsdl2.git

cd circle-stdlib
./configure -r 4 -p aarch64-none-elf- --libcxx
make
cd ..
```

Now build the shim:

```sh
cd circle-libsdl2
make            # produces libSDL2.a
```

The Makefiles default to the sibling layout above; a circle-stdlib tree
anywhere else works with `make CIRCLESTDLIBHOME=/path/to/circle-stdlib`.

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
