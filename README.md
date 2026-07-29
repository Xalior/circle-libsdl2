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
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending, scaled `SDL_RenderCopy` | `CBcmFrameBuffer` — double-buffered, vsync page flip. Off core 0: draw calls become a command list the presentation core executes |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Off core 0: USB stays on core 0, events cross by ring |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue. Off core 0: your callback fills a ring, core 0's servo feeds the device |
| Events: queue, `SDL_PumpEvents`, window focus | the per-frame heartbeat: USB pump and scheduler yield on core 0, ring drain and liveness beat off it |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs). `SDL_Delay` yields to the scheduler on core 0; off it, spins to a µs-exact deadline — deterministic, but it burns the core |
| Files: an I/O service callable from any core | FatFs on core 0, marshalled (`SDL2Circle_IO*`) — for applications whose own file layer must not touch the card directly |
| Init/error/version/hints | — |

**All rendering is 32-bit, and only 32-bit is proven.** The framebuffer is
always allocated at 32 bits per pixel, and the only texture format is
streaming ARGB8888 — a request for any other depth or format fails with an
error rather than falling back. Every consumer proven so far (the test
applications, pi-mame) renders 32-bit ARGB8888 end to end; no other pixel
depth has been exercised on real hardware.

Not yet: mouse, game controllers, haptics, OpenGL (the Pi 4 has no
bare-metal GPU driver — software rendering is the design, not a stopgap).

## Presentation geometry

Three resolutions are always in play on a bare-metal Pi, and the library
names all three rather than blurring them together.

**The scanout** is what the display hardware really puts on the wire. The
library derives it from the framebuffer the firmware actually granted — a
grant's pitch and size, never the width and height the firmware echoes back,
which are not reliable. On a Pi 3 or Pi 4 the firmware honors the boot
request, so the scanout is whatever `config.txt` asked for. A Pi 5 ignores
mode requests while still acknowledging them, and scans out the mode its
display reports; the grant is the only place that shows up.

**The canvas** is the resolution the operator asked for, from `cmdline.txt`
`width=` and `height=`. It is the world the application is given, and its
shape against the scanout's decides the letterboxing. **Leave it unset and
the canvas becomes the scanout** — the canvas step disappears, nothing needs
configuring, and this is the default on every board.

**The application's own resolution** is whatever it renders at. It calls
`SDL_CreateWindow` and `SDL_RenderCopy` as usual, and the rectangles it
passes are canvas coordinates, because the canvas is the window. An
application never learns what the glass is doing.

A frame therefore travels application → canvas → scanout, and **the library
composes both steps into a single resampling pass** at present time — on the
presentation core when the core split is active, inline otherwise. The canvas
contributes arithmetic, never an intermediate copy.

- **`SDL_RenderCopy` honors its rectangles.** A destination the same size as
  the source, on a canvas that is the scanout, is the same unscaled blit as
  ever — the same bytes, on the same path. Anything else resamples.
- **Nearest neighbour, and only nearest neighbour.** Per-axis index tables
  are built once per geometry and reused; an exact integer ratio skips the
  tables and replicates. `SDL_HINT_RENDER_SCALE_QUALITY` is stored like any
  other hint and **has no effect** — `"linear"` is a later phase, not a
  silent fallback.
- **Fit is the default placement.** The canvas is scaled up as far as it
  fits, centered, and the remainder of the scanout stays black. Put
  `canvas=stretch` in `cmdline.txt` to fill the scanout instead and let the
  aspect ratio go.
- **The copy to the screen runs on the DMA engine where it can.** When the
  firmware grants enough memory for two screens, presenting is a page flip
  and copies nothing. When it grants only one — a Pi 5 does — the finished
  frame has to be copied into the granted surface, and that surface is
  uncached, which makes it far and away the most expensive thing the
  presentation core does. So the library hands that copy to a DMA channel
  and returns without waiting for it, scaling the next frame into a second
  buffer while the transfer runs. One frame is in flight at a time. If no
  DMA channel is free, the CPU does the copy exactly as before.

The library logs the whole chain once at startup and once per distinct
geometry, so a serial console tells you what happened without guessing:

```
sdl2video: scanout 1920x1080 (grant, native surface), canvas 720x576 (cmdline width=/height=)
sdl2video: canvas 720x576 on scanout 1920x1080: fit -> 1350x1080+285+0
sdl2video: granted 1080 rows < 2160: shadow-buffered present
sdl2video: present: dma copy, channel 11, 8294400 bytes, double-shadowed
sdl2video: copy src 320x224 -> canvas 720x504+0+36 -> scanout 1350x945+285+67 (nearest)
```

The `present:` line names the path that is actually live — `dma copy` or
`cpu copy`, and the reason when it is the latter.

## Running off core 0

Circle kernel is core-0-only by design: its scheduler, interrupts, USB, FatFs
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
- **`SDL_Delay` becomes exact, and costs the core.** On core 0 it is
  `CScheduler::MsSleep` — cooperative, so you become *runnable* at the deadline
  but only resume when the scheduler next gets control, and a peer that is slow
  to yield overshoots your delay by however long it takes. Off core 0 there is
  no scheduler to defer to, so the shim spins on the system timer (µs
  resolution, `yield` hint) and returns at the deadline. You trade a scheduler
  yield for a deterministic wait, and you pay for it by burning that core for
  the duration — which is only sound *because* the core is dedicated and has
  nothing else to run. The same spin on core 0 would starve the servo, the
  watchdog, USB and the audio feed, which is why core 0 keeps yielding.

**Single-core is the same code, degenerate.** Without `SplitInit` there is no
ring and no mailbox: every call executes directly, the pump does the platform
work itself, audio pulls, and the watchdog is an in-band timer that dumps the
scheduler's task list if the main loop goes quiet for 30 seconds. One codebase,
one set of call sites, a branch on `SplitActive()`. Running off core 0 needs a
multicore Circle world — see Building.

## Performance receipts

Add `rapi-perf=10` to `cmdline.txt` and the library prints, every 10
seconds, one line per core that has run instrumented code:

```
sdl2perf: 60.0 fps c0: cycles 24005M: app 38.9% render 50.5% wait 10.2% audio 0.1% input 0.0% yield 0.0%
```

The frame rate counts presented frames. The percentages split that core's
cycles (each core's own PMU cycle counter) between the library's
instrumented sections — `render` is present-path compute, `wait` is
blocking time (vertical sync, an outstanding DMA transfer), `audio` and
`input` are the pumps, `yield` is time given to other scheduler tasks —
and `app` is the core's uninstrumented remainder: the application on its
own core, kernel and servo housekeeping on core 0. `wait` is reported
apart from `render` on purpose: at a locked frame rate the blocking waits
absorb all spare time, and folded together they would make the present
path look saturated when it is mostly idle.

A single-core build reports core 0 alone, which is the whole machine
there. Under the core split each active core reports its own line, which
is how a question like "how busy is the presentation core, could it
afford filtering" gets a measured answer.

The option absent, the instrument costs one branch per section. Hosts can
also arm it in code: `SDL2Circle_SetPerfInterval(seconds)` in
`SDL2/SDL_circle.h`.

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
