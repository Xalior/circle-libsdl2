# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi.** Write (or port) an
ordinary SDL2 application, link it against this library and the
[Circle](https://github.com/rsta2/circle) framework, and it boots directly
from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the
SDL2 API surface that real applications call, mapped directly onto Circle's
bare-metal drivers.

It also lets your application **run somewhere other than core 0** and still
just call SDL. Circle's world — scheduler, interrupts, USB, FatFs, sound — is
core-0-only by construction, so an application on another core cannot touch any
of it. This library closes that gap: it marshals every platform call back to
core 0 through lock-free rings and mailboxes, and hands you a presentation
worker to run on a core of your choosing. See
[Running off core 0](#running-off-core-0).

Proven in anger by [pi-mame](https://github.com/Xalior/pi-mame), which runs
MAME's core on bare metal through this library — a real application, leaning on
the whole surface at once: fullscreen software rendering, USB HID keyboards,
HDMI audio, files off the SD card, and its emulation running on a core that
never touches a device.

## What works

| SDL2 subsystem | Circle backing |
|---|---|
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending | `CBcmFrameBuffer` — double-buffered, vsync page flip. Off core 0: draw calls become a command list the presentation core executes |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Off core 0: USB stays on core 0, events cross by ring |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue. Off core 0: your callback fills a ring, core 0's servo feeds the device |
| Events: queue, `SDL_PumpEvents`, window focus | the per-frame heartbeat: USB pump and scheduler yield on core 0, ring drain and liveness beat off it |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs); `SDL_Delay` off core 0 is a plain timed wait — the scheduler is core-0-only |
| Files: an I/O service callable from any core | FatFs on core 0, marshalled (`SDL2Circle_IO*`) — for applications whose own file layer must not touch the card directly |
| Init/error/version/hints | — |

Not yet: mouse, game controllers, haptics, OpenGL (the Pi 4 has no
bare-metal GPU driver — software rendering is the design, not a stopgap).

## Running off core 0

Circle is core-0-only by construction: its scheduler, interrupts, USB, FatFs
and sound may only be touched from there. So an application that wants a core
to itself has a problem — on any other core, it cannot call the platform at
all.

This library is the bridge. **Your code runs wherever the host kernel puts it
and keeps calling plain `SDL_*`;** the library marshals. Call
`SDL2Circle_SplitInit` once, on core 0, before the application starts, and it
arms two tasks **on core 0**:

- a **servo** — drains the call mailbox, runs the I/O service, pumps USB input
  into the event ring, feeds the sound device from the audio ring, ticks the
  CPU throttle;
- a **watchdog** — reports a stalled application core instead of letting the
  board die in silence.

Everything crosses in coherent memory: lock-free single-producer/consumer rings
(events in, audio out), a one-deep frame mailbox, and a one-deep call mailbox
for the rare marshalled call — device bring-up, framebuffer allocation, file
I/O. Never a Circle scheduler primitive, which would be illegal off core 0.

**The library does not own the cores.** It never starts one. The host kernel
starts the secondary cores (`CMultiCoreSupport`), decides where the application
runs, and — if it wants presentation off the application's core — runs
`SDL2Circle_SplitPresentCore` on a core of its choosing, where it blits posted
frames and page-flips. That is the whole of what this library puts on a core
other than 0.

What changes for your code, once it is off core 0:

- **The pump stops doing the work.** `SDL_PumpEvents` on your core only touches
  shared memory — drain the event ring, mirror key state, bump the heartbeat.
  USB plug-and-play and HID translation stay on core 0's servo.
- **Audio inverts from pull to push.** Your callback fills the audio ring; the
  servo feeds the sound device at its own cadence.
- **`SDL_Delay` is a plain timed wait,** not a scheduler yield.

**Single-core is the same code, degenerate.** Without `SplitInit` there is no
ring and no mailbox: every call executes directly, the pump does the platform
work itself, audio pulls, and the watchdog is an in-band timer that dumps the
scheduler's task list if the main loop goes quiet for 30 seconds. One codebase,
one set of call sites, a branch on `SplitActive()`. Running off core 0 needs a
multicore Circle world — see Building.

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
  libc++ threading). To run the application off core 0, the host kernel also
  starts the secondary cores and may hand one to `SDL2Circle_SplitPresentCore`
  — this library marshals; it never starts or owns a core.
- **Honest headers.** `include/SDL2/` is the official SDL2 2.32.4 header
  set (zlib license, see `SDL2-LICENSE.txt`) with one substitution: an
  `SDL_config.h` for AArch64/newlib/Circle. Your app compiles against the
  genuine SDL2 API; unimplemented entry points fail at link time instead
  of surprising you at runtime. The cross-core surface is the one addition, and
  it is deliberately outside the SDL2 namespace: `SDL2/SDL_circle.h`.

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

The world is configured **multicore** (`ARM_ALLOW_MULTI_CORE`) because running
an application off core 0 needs the other cores; a single-core world cannot
serve that. A
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
