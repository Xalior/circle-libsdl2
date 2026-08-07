# Changelog

Versions here mark the points at which this library was proven on real
hardware by an application built on it. There is no release process and no
build artifact: the library is consumed as source, at a commit.

Anything marked **Consumers must act** changes what a host kernel has to do,
and will stop an existing kernel from building or running until it is
followed.

## vPoC3

### Every core gets a 2 MB stack

**Consumers must act.** `make deps` now configures a world with
`KERNEL_STACK_SIZE=0x200000` instead of leaving Circle's 128 KB default in
place. The value is fixed into the world at configure time, so a world
configured before this change keeps the old stacks no matter how recently the
library was rebuilt against it: reconfigure the world and rebuild it.

128 KB is not enough to run a game. A software-rendering engine of the era
this library targets keeps its per-frame working set on the stack — TyrQuake
allocates its edge and surface arrays with `alloca` on every frame it draws,
and needs more than 128 KB to do it before any other consideration.

The failure it produced is worth knowing, because nothing about it points at
a stack. Circle's four core stacks sit next to each other with no guard page,
so the application core ran off the bottom of its own and into core 0's,
where the host kernel object lives as a local of `main()`. The picture
corrupted for a frame or two, then a DMA interrupt handler on core 0
dereferenced a pointer the game had written over, and the board took a data
abort in Circle code that had done nothing wrong.

### SDL has one framebuffer, and every frame is drawn into it

A frame is no longer sent to the presentation core as a pointer into the
application's own texture. Every frame is composed into SDL's framebuffer — a
buffer this library owns, at the size the application was told the display is
— and that buffer is what crosses between the cores.

Two faults go with it.

The presentation core used to keep reading an application texture after
`SDL_RenderPresent` had returned. At that moment the application is entitled
to destroy the texture, or to lock it and draw the next frame into it, and
nothing stopped it. Whether that showed as a torn picture, a wrong one or a
halted board depended on timing alone.

`SDL_RenderReadPixels` used to read the display panel rather than SDL's
framebuffer. Where the picture is fitted and centred on a larger panel, it
returned the top-left corner of the panel — part black margin, part picture,
scaled to the panel and not to the size the caller drew in. It now reads
SDL's framebuffer, in the coordinates the caller drew in, and returns the
frame as it stands: everything drawn since the frame began. A screenshot
taken before the present is the picture the application just drew.

The cost is one full-size copy per frame on the application's core, and for
the frame shape that used to be recognised, one further resampling pass. It
is paid so that no buffer is ever read by one core while another may be
writing it.

### The C++ standard library's threading works on the core your application runs on

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`,
`std::call_once`, `std::thread` and `thread_local` are all usable from the
core this library puts your application on. They were not before.

Circle's cooperative scheduler is documented as core 0's alone, and
circle-stdlib builds the C++ threading runtime on it. Neither project is at
fault: this library is the one that chose to run applications somewhere else,
so it now supplies that part of the runtime itself. The primitives are built
from processor atomics, and every wait in them yields to Circle's scheduler on
core 0 and spins anywhere else.

`std::recursive_mutex` was the visible failure. Locked and unlocked a few
microseconds apart on the application core, with no second core involved and
nothing contending, it stopped the board. Two games hit it after everything
else about them already worked: one through its audio manager, one through a
logging library that takes such a lock for every line it writes. Behind it were
quieter faults that reported nothing at all — a contended `std::mutex`, every
condition variable wait, and every `thread_local` read from the application
core, which was answering with another thread's storage.

`std::thread` can now be created from any core. A thread runs on core 0 as a
cooperative task, which is what a service thread wants: it costs no core, and
it may call Circle. A host kernel that would rather have a thread on a core of
its own can lend one — see `SDL2Circle_ThreadCoreOffer` and
`SDL2Circle_ThreadPinNext` in `SDL2/SDL_circle.h` — and this library will never
place a thread on a core it is already using for the application or for
presentation.

**Nothing has to be done to get this.** A host kernel already calls
`SDL2Circle_ArmCoreRuntime` on core 0, and that call now also starts the one
task the threading runtime needs.

There are limits, and they are stated rather than hidden. A thread on core 0
is cooperative, so a thread that computes without ever waiting keeps core 0 to
itself. A lent core runs one thread at a time. A wait occupies the core it
waits on, off core 0. `thread_local` destructors run when a thread ends, and
the application core never ends.

`examples/cxxthreads` exercises all of it on a second core and reports what it
found on the serial console.

### Applications choose their own display size

An application states the display size it wants and gets exactly that,
whatever screen is attached. Every SDL question about the display answers
with the stated size: the current, desktop and enumerated modes, the display
bounds, the window size and the renderer's output size. The library fits each
finished frame onto the real screen.

This suits an application whose size is a property of the program rather than
a preference, such as an emulated machine or a fixed layout. It also removes
the problem of a Pi 5, which cannot be told what resolution to run at, so
anything sizing itself from the screen was at the mercy of the monitor.

Only 32 bits per pixel is supported, and a request for anything else is
refused rather than rounded to it. So is a second statement, or one made
after the library has already been asked about the display.

There is no default and no fallback. An application that states no size has
not said what it wants, and the library stops rather than choose for it, so
every existing host kernel needs one new call before it will run.

Five of the examples show an application asking the firmware for the screen
size and stating that, which is how you make the two match.

### Putting a frame on screen costs about half of a core

On a Pi 5, filling a 1920x1080 screen from a 398x224 picture at a steady 59.9
frames per second now takes 41% of the core that does it. It took 76%.

The scaler no longer copies a finished output row to produce the next one. An
output row is several times wider than the source row it comes from, so the
copy moved more memory than recalculating it does, and it filled the cache
with output data while evicting the source row still being read.

The saving grows with how much the picture is enlarged, and applies to every
application on every board.

### A picture shaped like the screen fills it exactly

Placement is calculated in exact integers, so a picture whose shape matches
the screen leaves nothing over. Previously it fell one pixel short on each
axis and left a thin black line down the right edge and along the bottom.

### The screen size is read from the firmware

The library asks the firmware what resolution it settled on, rather than
working it out from the framebuffer's pitch and size. It also no longer asks
for a resolution of its own accord: it used to request 640x480 whenever
`cmdline.txt` named no size, which imposed a mode on a card that had asked
for nothing.

**Do not name a display mode on a Pi 5.** That board fixes its mode before
any kernel starts and will not change it. It accepts a `width=`/`height=`
line, reports it back as applied, and carries on sending its own mode to the
screen, so everything downstream then works from a size that is not real. A
Pi 3 and a Pi 4 apply the mode properly and report it correctly.

### The library creates a scheduler if the host has none

Running the core split requires a `CScheduler`, because the servo and
watchdog are Circle tasks and a task cannot be constructed without one. A
host that did not create one got a board that stopped during start-up with
nothing said.

The library now creates one where there is none. A host that creates its own
keeps it, and the library never replaces or reconfigures a scheduler it did
not make.

### A build setting chooses what crosses between the cores

`make PRESENT_CMDS=n` sets how many drawing commands may travel to the
presentation core as a list. A frame that does not fit within the count is
composed into SDL's own framebuffer instead, and that framebuffer crosses.

The default of zero composes every frame here and sends a picture. Which
framebuffer the firmware granted no longer influences the choice.

Only the default has been measured against a real workload.

### A game that draws without a renderer now lands in the same rectangle as one that does

SDL offers two ways to put a frame on screen: ask the window for a surface,
draw into it and say when to show it, or draw through a renderer. Only the
renderer was mapped onto the screen properly.

The window-surface path built its copy in canvas coordinates and then wrote it
to the screen unmapped, so a canvas smaller than the display appeared at its
own size in the top-left corner with the rest of the screen black — with the
fitted rectangle correctly calculated and logged, and then ignored. It also
did that work on whichever core called it, rather than handing the frame to
the presentation core the way every other frame is handed over.

Both are fixed, and the two paths now reach the glass the same way. A window
surface is copied into the double-buffered canvas surface, which is what makes
it safe to hand to another core, and crosses as one frame; the `copy src ... ->
canvas ... -> scanout ...` line that describes every other present now
describes this one too.

EDuke32 draws this way, because the Build engine's classic renderer has its own
software rasteriser and wants nothing but somewhere to put the result.

### The monotonic clock answers every clock a program can ask for

`clock_gettime` is now served by this library, alongside the wall clock it
already served.

The C library underneath answers `CLOCK_REALTIME` and `CLOCK_MONOTONIC` and
refuses every other clock — and its refusal returns without writing the
timespec it was given. Most callers do not check the return, because a
monotonic clock is not expected to fail, so they read whatever their own
stack held; asked twice from the same place, the clock gives the same answer
both times and appears to have stopped. A loop that waits for the clock to
advance then never leaves, on whichever core it is on, printing nothing.

The header this library is built against defines `CLOCK_MONOTONIC_RAW`, so
portable code selects the refused clock in preference to the working one.
EDuke32's timer calibration does exactly that, and hung its application core
one line after it announced SDL.

Every clock this board can answer meaningfully now gets an answer: the
monotonic family and the two CPU-time clocks all read the free-running system
counter, which nothing here adjusts, slews or suspends. A clock outside that
set is still refused, but the timespec is zeroed first, so a caller that
ignores the return reads a defined value instead of its own stack.

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
