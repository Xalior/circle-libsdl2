# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi.** Write (or port) an
ordinary SDL2 application, link it against this library and the
[Circle](https://github.com/rsta2/circle) framework, and it boots directly
from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the
SDL2 API surface that real applications call, mapped directly onto Circle's
bare-metal drivers.

And it will give your application **a CPU core of its own** — no interrupts, no
device work, no scheduler on it — with presentation on a second core and every
platform call marshalled back to core 0. That is the *core split*, and it is
the heart of this library; a single-core build is the same code with the
marshalling switched off. See [The core split](#the-core-split).

## What it is proven on

Its founding use case is [**pi-mame**](https://github.com/Xalior/pi-mame):
MAME's emulation core, running bare-metal on a Raspberry Pi 4 with no
operating system underneath it at all. Not a demo of one machine — pi-mame
ships whole vendor platforms, home computers and consoles alike, and every
machine on every one of them reaches the hardware through this shim. Which
machines those are is pi-mame's business, and its documentation is where they
are listed.

A demanding application asks for all of it, which is what makes it a good
proof: one fullscreen framebuffer at a resolution fixed by boot config, a
software renderer, USB HID keyboards, HDMI audio, and files read off the SD
card — with the application itself running on a CPU core of its own and
touching none of that hardware directly. Nothing about the shim is specific to
MAME, or to emulators: any SDL2 application gets the same surface.

## What works

| SDL2 subsystem | Circle backing |
|---|---|
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending | `CBcmFrameBuffer` — double-buffered, vsync page flip. Split: draw calls become a command list the presentation core executes |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Split: USB lives on core 0, events cross by ring |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue. Split: your callback fills a ring, core 0's servo feeds the device |
| Events: queue, `SDL_PumpEvents`, window focus | the per-frame heartbeat: USB pump and scheduler yield single-core, ring drain and liveness beat under the split |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs); `SDL_Delay` off core 0 is a plain timed wait — the scheduler is core-0-only |
| Files: an I/O service callable from any core | FatFs on core 0, marshalled (`SDL2Circle_IO*`) — for applications whose own file layer must not touch the card directly |
| Init/error/version/hints | — |

Not yet: mouse, game controllers, haptics, OpenGL (the Pi 4 has no
bare-metal GPU driver — software rendering is the design, not a stopgap).

## The core split

**Your application gets a CPU core to itself.** Call `SDL2Circle_SplitInit`
(`SDL2/SDL_circle.h`) and the shim takes the platform off it:

| core | role |
|---|---|
| 0 | the Circle world — every device, the scheduler, a servo task that feeds the sound device, and a watchdog |
| app | your code, alone: no interrupts, no device work, no scheduler |
| presentation | blit and page flip, nothing else |

Your core never touches hardware. It reaches it through lock-free single
producer/consumer rings — events in, audio out — a one-deep frame mailbox
(post a frame, the presentation core executes it while you build the next),
and a call mailbox for the rare marshalled call: device bring-up, framebuffer
allocation, file I/O. The consequences are worth stating plainly, because they
invert what a single-core SDL app assumes:

- **The pump stops doing the work.** On your core, `SDL_PumpEvents` only
  touches shared memory — drain the event ring, mirror key state, bump a
  heartbeat. USB plug-and-play and HID translation stay on core 0.
- **Audio inverts from pull to push.** Your callback runs on your core into
  the audio ring; core 0's servo feeds the sound device at its own cadence.
- **The watchdog goes cross-core.** Core 0 watches your heartbeat and reports
  a stalled core instead of the whole board dying in silence.

**Single-core is the same code, degenerate.** Without `SplitInit` there is no
ring and no mailbox: every call executes directly, the pump does the work
itself, audio pulls, and the watchdog is an in-band timer that dumps the
scheduler's task list if the main loop goes quiet for 30 seconds. One codebase,
one set of call sites, a branch on `SplitActive()`. The split needs a multicore
Circle world — see Building.

## Design

- **The app owns the main loop; the shim rides it.** Everything the shim does
  per frame hangs off `SDL_PumpEvents` (called by `SDL_PollEvent`) and
  `SDL_RenderPresent`. An app that polls events and presents frames keeps the
  whole machine alive; one that stops doing either wedges a cooperatively
  scheduled board, which is what the watchdog exists to report. Audio
  callbacks never run in interrupt context.
- **The CPU throttle gets ticked.** Circle needs periodic `CCPUThrottle`
  updates or thermal management never runs, and the host kernel has no loop of
  its own to do it in — so the shim does it, from the pump.
- **Self-contained payloads.** The shim brings up everything it needs
  (USB host controller, framebuffer, sound) inside `SDL_Init`. Host-kernel
  contract: initialize `CInterruptSystem` and `CTimer` before `SDL_Init`;
  run a `CScheduler` if your app uses `std::thread`
  (via [circle-stdlib](https://codeberg.org/larchcone/circle-stdlib)'s
  libc++ threading). To run split, the host kernel also starts the secondary
  cores and hands one to `SDL2Circle_SplitPresentCore` — the shim marshals,
  it does not own the cores.
- **Honest headers.** `include/SDL2/` is the official SDL2 2.32.4 header
  set (zlib license, see `SDL2-LICENSE.txt`) with one substitution: an
  `SDL_config.h` for AArch64/newlib/Circle. Your app compiles against the
  genuine SDL2 API; unimplemented entry points fail at link time instead
  of surprising you at runtime. The split's own surface is the one addition,
  and it is deliberately outside the SDL2 namespace: `SDL2/SDL_circle.h`.

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
provided it was configured the same way.

Building through Circle's `Rules.mk` — as the test apps do — you get the
world's own `DEFINE`, `-DARM_ALLOW_MULTI_CORE` included, and there is nothing
to think about. **If you compile any translation unit outside it** — a foreign
build system with its own flag list — it must carry that define too. Circle's
headers change shape on it (spinlocks, atomics, memory layout), so an object
compiled without it disagrees with the library it links against, and nothing
tells you: it builds, it links, and it is wrong at runtime.

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
