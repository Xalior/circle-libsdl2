# Changelog

Versions here mark the points at which this library was proven on real
hardware by an application built on it. There is no release process and no
build artifact: the library is consumed as source, at a commit.

Anything marked **Consumers must act** changes what a host kernel has to do,
and will stop an existing kernel from building or running until it is
followed.

## vPoC3

**Applications choose their own display size.** An application states the
size it wants and gets it, whatever screen is attached, and the library fits
each frame onto the real screen. There is no default: an application that
states nothing does not start.

**Putting a frame on screen costs about half what it did.** On a Pi 5,
filling a 1920x1080 screen from a 398x224 picture at 59.9 frames per second
uses 41% of one core, down from 76%.

**A picture shaped like the screen fills it exactly.** No thin black line
down the right edge and along the bottom.

**The library creates a scheduler if the host has none.** A host that creates
its own keeps it.

**New build setting `PRESENT_CMDS`** chooses what crosses between the cores.
The default suits a normal game and is the only value measured.

**Do not set a display mode on a Pi 5.** That board fixes its mode before any
kernel starts, accepts a `width=`/`height=` line, says it applied it, and
carries on sending its own mode. A Pi 3 and a Pi 4 handle the setting
correctly.

See the README for how any of this works.

## vPoC2

### Joysticks and game controllers

SDL's joystick and game-controller calls are implemented. Circle publishes
every pad it binds, from the generic HID driver and the console-specific ones
alike, and the library turns that into SDL's two-level identity: a device
index that renumbers as devices come and go, and an instance ID that is never
reused. Devices that arrive and leave while the program is running are
reported as SDL events.

A joystick GUID is built byte for byte the way SDL2 builds one, including the
CRC-16 of the device name, so an unmodified `gamecontrollerdb.txt` matches.
Mapping lines tagged for Linux are loaded alongside any tagged for this
platform, because a Linux GUID has exactly the shape this builds and no
published database has heard of Circle. A device with no mapping line is
reported as not a game controller and remains a fully working joystick.

Rumble is offered where the device supports it — off, weak and strong, which
is what Circle exposes. `SDL_Haptic` is deliberately left unimplemented
rather than turning a force-feedback effect into a rumble.

Attach, detach and report translation happen on core 0, where USB lives, in
the same pump that already services the keyboard. What an application asks
for afterwards is answered out of shared memory by whichever core asks, so
reading a device costs nothing on core 0. Rumble is the exception, being a
USB transfer, and is marshalled.

`SDL_RWops` arrived with this, over memory and over the library's own I/O
service, because the mapping database is loaded through one.

### The library owns the CPU clock and the case fan

**Consumers must act:** remove any `CCPUThrottle` your kernel declares. This
library now creates it.

Circle permits exactly one `CCPUThrottle` in a system and halts if a second
is created, and it offers no safe way to ask whether one already exists —
`CCPUThrottle::Get()` halts rather than reporting an absence, so a null check
after it can never run. The library therefore owns the object and drives it
from whichever per-frame heartbeat is live: the servo when the core split is
running, the event pump otherwise.

The clock is taken to its maximum, stated rather than left to default,
because Circle starts the board at its idle rate. This happens inside
`SDL_Init`. A host kernel that configures I2C, SPI or the mini UART itself
should declare a `CSDL2CircleHardware` member instead, so the clock is
settled while the kernel is being constructed and before those peripherals
take their speed from the core clock.

Where `cmdline.txt` names a fan pin with `gpiofanpin=`, a board over
`socmaxtemp=` switches its fan on and holds its clock, rather than slowing
down.

### A video mode change no longer halts the machine

The buffers and DMA channel that carry finished frames to the screen are
sized by the framebuffer grant, which is made once at startup and never
given back. Rebuilding them for each new window leaked a DMA channel and two
full-screen buffers every time, until none were left and the sound device
could not open one. Any application offering a video settings menu could
reach this.

They now belong to the grant rather than to a window. The sound device is
also closed on the core that owns it, since destroying it returns its own
DMA channel, an interrupt registration and a queue.

A test that restarts the whole video world in a loop is what proved it.

### An example that identifies attached input devices

`examples/padview` is a bootable kernel that shows, for every attached device,
the SDL device index, the Circle device name, the instance ID, the joystick
GUID and the USB identifiers behind it, whether a mapping was found for it,
one live bar per axis, one lit cell per hat direction and one lit square per
button. Where the database recognised the device, the mapped controller axes
and buttons appear underneath the raw ones. Below that is a log of every
attach and detach with the time it happened.

It exists to answer "what is this device and what are its button numbers"
before you write them into an application's configuration.

### The framebuffer log line distinguishes the request from the grant

Only the pitch and the size come back from the firmware. The width, height,
virtual size and depth are the values Circle was constructed with, echoed
unchanged by getters that never learn what the firmware decided. Printed
together and unlabelled they read as one measured geometry, which is how a
Pi 5 came to report a 640x480 framebuffer beside a pitch describing a
1920-wide surface. The two halves are labelled now.

## vPoC1 — 23a36e5

The first version carried all the way through on hardware — an application
built on it running on a Pi 3, a Pi 4 and a Pi 5.

### The part of SDL2 that works

A fullscreen window with a software `SDL_Renderer` over Circle's
`CBcmFrameBuffer`, drawing through streaming ARGB8888 textures. That is the
only texture format and 32 bits is the only depth: any other request fails
with an error rather than quietly falling back.

`SDL_RenderCopy` honours its source and destination rectangles — a same-size
copy is still a byte-for-byte unscaled blit, anything else resamples — with
straight-alpha blending and alpha modulation. Also implemented:
`SDL_UpdateTexture`, `SDL_QueryTexture`, `SDL_RenderFillRect`,
`SDL_RenderDrawRect`, axis-aligned `SDL_RenderDrawLine`,
`SDL_CreateRGBSurface` as a memory-backed staging surface, and
`SDL_PixelFormatEnumToMasks`. Windows are created already shown and focused,
and every key event carries its window ID, for consumers that route input by
focused window.

Audio is the `SDL_OpenAudioDevice` callback API over Circle's HDMI sound
device, 16-bit signed stereo, behind roughly 100 milliseconds of hardware
queue. A device reports itself playing only once the hardware has really
started; a failure to start sets an SDL error instead of claiming success.
Verified by recording a 1 kHz tone from an HDMI capture and measuring it.

Input is USB HID keyboard reports as SDL key events, with
`SDL_GetKeyboardState` and modifiers. Timers are microsecond-resolution over
Circle's `CTimer`, including `SDL_GetTicks64` and the performance counter.

### The picture is scaled once, at the output

Three geometries are named and kept apart: the **scanout**, which is what the
display hardware puts on the wire; the **canvas**, which is the size the
operator asks for with `width=` and `height=` in `cmdline.txt`, defaulting to
the scanout; and the application's own render resolution, which is always in
canvas coordinates. Both steps are composed into a single nearest-neighbour
resampling pass when the frame is presented, with index tables built once per
geometry and an integer-ratio path that replicates pixels without a table.

The default placement scales the picture up as far as it fits, centres it and
leaves the remainder black. `canvas=stretch` in `cmdline.txt` fills the
scanout instead and abandons the aspect ratio.

The scanout is derived from what the framebuffer allocation actually granted,
never from the size the firmware echoes back — a Pi 5 acknowledges a mode
request without honouring it, and trusting that echo skews every scaling
calculation after it.

A frame is composed in ordinary cached memory and then moved to the screen in
one block, never scaled directly onto the uncached framebuffer. Measured on a
Pi 4 at 1280x720: 26.1 ms to write the screen directly, against 1.4 ms into
memory plus 6.0 ms to move it out. It also makes a frame atomic on the wire,
because the screen is touched only by that one move. The move uses the DMA
engine when a channel is free and falls back to a logged CPU copy when none
is.

Where the firmware's grant cannot hold two screen heights, presentation falls
back to a shadow buffer rather than writing past the grant: frames render
into a tightly-pitched shadow and the flip is one row-wise copy behind a
vsync wait, so a partly-drawn frame is never scanned out.

### Running the work on more than one core

The library does not start a core and does not choose one. The host kernel
owns that decision entirely; this provides the pieces that make it safe.

`SDL2Circle_SplitInit()` runs once on core 0 and arms two Circle tasks: a
servo that drains the call mailbox, runs the file I/O service, pumps USB
input into the event ring and feeds the audio ring to the sound device; and a
watchdog that reports an application core that has stalled.
`SDL2Circle_SplitPresentCore()` is what a host runs on the core it elects for
presentation — it turns a finished frame into a scanout, runs no SDL and
holds no application state. `SDL2Circle_CallOn0()` runs the host's own
function on core 0 through the same mailbox, for hardware the library does
not own.

Every path between cores is a lock-free single-producer ring or a one-deep
mailbox, never a Circle scheduler primitive, which would be illegal off
core 0.

A draw sequence shaped "clear, then one opaque blit" is recognised while it
is being recorded and reduces to the texture itself crossing the mailbox
rather than a list of commands. Anything else is rasterised into a
canvas-sized surface on the application core first, and that surface crosses
instead. This is something the library notices, never something an
application has to arrange.

The application core is released as soon as the presentation core has read
everything it needs from a frame, not after the output waits — the DMA
transfer and the vsync wait touch only the presentation core's own buffer.
Before that, the split ran slower than a single core.

Audio inverts off core 0: the application's callback fills a ring, and core
0's servo feeds the hardware from it at its own cadence. `SDL_Delay` off
core 0 spins to an exact microsecond deadline, which is deterministic but
burns the core; on core 0 it sleeps cooperatively.

Files are the one thing that cannot be marshalled invisibly, because the C
library drives the SD card directly. An application must reach files through
the `SDL2Circle_IO*` service from any core other than 0. In a single-core
build every one of these degrades to a direct call, so call sites do not
change.

### Logging from any core

Any core can log. Each formats its line into a ring of its own and returns;
core 0's servo drains every ring to the serial console. A full ring drops the
line and counts it, rather than blocking the core that logged or corrupting
the record.

### Performance reports

`SDL2Circle_SetPerfInterval(N)` prints one
serial line every N seconds: frames presented per second, and a cycle-count
split per core across render, audio, input and waiting, with the remainder
attributed to the application's own work.

Waiting is separated from working, and blocking on another core is counted
apart from waiting on DMA or on the raster — at a locked frame rate the
blocking waits absorb all the spare time, which previously made a fully idle
core look saturated. Each line leads with how much of the wall clock the core
was actually awake, measured locally, because a parked core's cycle counter
stops. The counter backend is AArch64 only; elsewhere the instrument compiles
to an inert form and says so once when armed.

### What the host kernel must do

Finish Circle's own world — interrupts, timer, serial, SD card, filesystem —
on core 0 before starting any other core. Start the secondary cores yourself
with `CMultiCoreSupport`. Call `SDL2Circle_ArmCoreRuntime()` as the first
statement on every core including core 0: a core that has just started has no
thread pointer, C++ exception state is reached through it, and without this
the first throw reads whatever the firmware left there — which passes on one
board and takes a data abort on the next. Call `SDL2Circle_SplitInit()` once
on core 0 before the application starts. Keep core 0 yielding for as long as
the application runs, because the servo only runs when something yields. Park
any core you give no role.

### Building

A separate static library per board — `libSDL2-rpi3.a`, `libSDL2-rpi4.a`,
`libSDL2-rpi5.a` — because Circle bakes the board in at configure time, each
built against its own circle-stdlib world. circle-stdlib is vendored as a
pinned submodule of this library rather than expected as a sibling checkout,
so `make deps` is self-contained.

`sdl-app.mk` and `sdl-app.ld` are a shared link fragment an application's
Makefile includes. The linker script is TLS-safe: binutils 2.44 and later
refuse a `PT_TLS` segment unless `.tbss` sits next to `.tdata`, which
Circle's stock script does not guarantee.

The split needs a multicore circle-stdlib world. Every source file also
compiles clean without it, and a single-core world builds only the direct
call paths behind the same public API.

### Limitations

No mouse, no game controllers, no haptics. No OpenGL, and that one is a
design position rather than an unfinished job: there is no bare-metal GPU
driver to put behind it.

Scaling is nearest-neighbour only. `SDL_HINT_RENDER_SCALE_QUALITY` is stored
like any other hint and has no effect; `"linear"` is not implemented.

Rectangle and fill draws are opaque and ignore the blend mode, so a blended
fill comes out solid. This was found by an application integrating against
this library rather than in its own history, and nothing in the library
changes it at this point.

A build-time-seeded wall clock serves `time()` and `gettimeofday` before
Circle's timer exists, because static constructors run before the host kernel
builds one. Without it, `srand(time(NULL))` at global scope — ordinary,
idiomatic C — silently killed a Pi 5 boot.
