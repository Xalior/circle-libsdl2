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
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending, scaled `SDL_RenderCopy` | `CBcmFrameBuffer` — double-buffered, vsync page flip. Where the firmware grants one screen instead of two, the finished frame is scaled onto it, on a core of the host's choosing |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel, or the virtual device the application declared for itself |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Off core 0: USB stays on core 0, events cross by ring |
| Joysticks, gamepads and wheels: enumeration, hot-plug, axes, hats, buttons, GUIDs, coarse rumble | Circle's USB gamepad drivers — the generic HID one and the five console ones. Off core 0: USB and event synthesis stay on core 0; the readings live in memory both cores see |
| Game controllers: `gamecontrollerdb.txt` mappings, `SDL_IsGameController`, mapped axes and buttons, controller events | the mapping text is read the way SDL2 reads it, and found by the same joystick GUID SDL2 builds |
| Files as `SDL_RWops` streams, and streams over memory | the I/O service, so an application off core 0 opens a file with the ordinary SDL call |
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

Not yet: mouse, `SDL_Haptic` force feedback, controller motion sensors and
touchpads, virtual joysticks, OpenGL (the Pi 4 has no bare-metal GPU driver —
software rendering is the design, not a stopgap).

## Presentation geometry

Three resolutions are always in play on a bare-metal Pi, and the library
names all three rather than blurring them together.

**The scanout is the physical display** — what the hardware really puts on
the wire. `width=` and `height=` in `cmdline.txt` ask the firmware for a
display mode, allocating the framebuffer is what sets it, and **the firmware
then reports the mode it actually set. That report is the scanout.** It is
read from the firmware, never calculated: not from the framebuffer's pitch,
not from its size, and not from the width and height Circle hands back,
which are only the arguments it was constructed with.

**Set neither and the panel keeps its own mode.** The library then asks the
firmware for no particular size, which is how you say "whatever the display
is already doing", and the firmware allocates the display's own mode. There
is no default resolution anywhere in the library — a default would not be a
preference, it would be an instruction, because asking for a mode is what
sets one. A card that says nothing gets the panel it is plugged into.

**The canvas is the virtual display** — the world the application is given,
and its shape against the scanout's decides the letterboxing. **The
application declares it**, in its own code, before `SDL_Init` — see
[Declaring the display](#declaring-the-display). It is required, and there
is no fallback: without it the library refuses to start.

**These are two numbers doing two jobs, and they coexist.** Neither is a
fallback for the other and there is no order of precedence between them. One
is asked of the firmware by the operator; the other is declared by the
application. `width=` and `height=` never set the virtual display, and the
declaration never sets the physical one.

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
- **A frame is composed in ordinary memory, never on the glass.** The
  framebuffer is uncached, and a scaler writing it directly pays that cost
  once per pixel: on a Pi 4, 26.1 ms for a 1280x720 frame against 1.4 ms
  into ordinary memory. So a present is composed off-screen and the finished
  frame is moved out in whole rows — 6.0 ms for the same frame — which is
  also what keeps the picture whole, because the screen is written by that
  one move and can never be caught mid-composition.
- **The copy to the screen runs on the DMA engine where it can.** When the
  firmware grants enough memory for two screens, presenting is a page flip
  and the move goes to the half being panned to. When it grants only one — a
  Pi 5 does — the finished frame goes to the granted surface itself, which is
  the most expensive thing the presentation core does. So the library hands
  that copy to a DMA channel
  and returns without waiting for it, scaling the next frame into a second
  buffer while the transfer runs. One frame is in flight at a time. If no
  DMA channel is free, the CPU does the copy exactly as before.
- **The present path is built once and reused.** Its buffers and its DMA
  channel are sized by the framebuffer the firmware granted, and that grant
  is made once and kept for as long as the machine runs, so a second window
  adopts the same one. An application may destroy its window, renderer and
  textures and create new ones as often as it likes — which is what a
  settings menu does whenever a video setting changes — and none of it is
  taken again. There are only a few DMA channels on the board and the sound
  device needs one too.

The library logs the whole chain once at startup and once per distinct
geometry, so a serial console tells you what happened without guessing:

```
sdl2video: scanout 1920x1080 (firmware reported), canvas 720x576 (declared virtual device)
sdl2video: canvas 720x576 on scanout 1920x1080: fit -> 1350x1080+285+0
sdl2video: granted 1080 rows < 2160: shadow-buffered present
sdl2video: present: dma copy, channel 11, 8294400 bytes, double-shadowed
sdl2video: copy src 320x224 -> canvas 720x504+0+36 -> scanout 1350x945+285+67 (nearest)
```

The `present:` line names the path that is actually live — `dma copy` or
`cpu copy`, and the reason when it is the latter.

### Declaring the display

**Every application declares the display it is to be given**, in its own
code, before `SDL_Init`. This is not optional and there is no fallback of any
kind — not the boot command line, not the panel. A consumer that has not
declared one has not said what display its application is to be given, and
the library will not invent one, so `SDL_Init` fails and says why on the
console.

```c
#include <SDL2/SDL_circle.h>

if (SDL2Circle_DeclareVirtualDevice(32, 800, 450) != 0)
    fprintf(stderr, "%s\n", SDL_GetError());
```

That becomes the canvas. `SDL_GetCurrentDisplayMode`,
`SDL_GetDesktopDisplayMode`, `SDL_GetDisplayMode` and `SDL_GetDisplayBounds`
all answer with it, `SDL_CreateWindow` returns a window of that size whatever
it was asked for, and the library carries each frame from there to whatever
the panel is really doing. **The application never learns the real output
resolution**, which is the point: it draws in the world it declared, and the
placement rules above put that world on the glass.

- **It is fixed.** One declaration is accepted, before anything has asked the
  library about the display. A second one is refused, and so is one made
  after the display size has been settled — the first display query, or the
  first window. The size an application is given cannot change under it, so
  every geometry derived from it is worked out once and holds for the run.
- **32 bits per pixel, and nothing else.** The framebuffer is allocated at 32
  bits and streaming ARGB8888 is the only texture format, so another depth is
  refused rather than quietly rounded to this one. Width and height must both
  be above zero.
- **The return value is the report.** Zero means accepted; -1 means refused,
  with `SDL_GetError` saying which of the above was not met. A refused
  declaration changes nothing, and an earlier accepted one still stands.
- **It states the virtual display, not the physical one.** The mode the panel
  is driven at remains the operator's business, asked for in `config.txt` and
  `cmdline.txt` and granted, or not, by the firmware. Declaring a virtual
  device asks the firmware for nothing; it says what the application is to be
  shown, and the library scales.
- **Without it the library does not start.** `SDL_Init` returns failure with
  `SDL_GetError` explaining, and puts a line on the console naming the call
  to make. Nothing is brought up, no device is touched, and no display
  question is answerable.

### Matching the virtual display to the physical one

**Where the numbers come from is the application's business, and only the
application's.** A build constant, a settings file, an option of its host
kernel's own, a value off a network port. The library is told; it discovers
nothing and offers no way to ask what the panel is.

So an application that wants its virtual display to **match** the panel works
the physical size out for itself and passes it in — which takes a handful of
lines against Circle's public property tags, and needs nothing from this
library at all:

```c
#include <circle/bcmpropertytags.h>

CBcmPropertyTags Tags;
TPropertyTagDisplayDimensions Dim;
memset(&Dim, 0, sizeof Dim);
if (Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
    && Dim.nWidth != 0 && Dim.nHeight != 0)
{
    SDL2Circle_DeclareVirtualDevice(32, (int) Dim.nWidth, (int) Dim.nHeight);
}
```

**`examples/gradient`, `examples/keyecho`, `examples/tone`, `examples/padview` and
`examples/videocycle` each do exactly this** — every one carries the query in its
own kernel source rather than sharing a helper, so each stands alone as a
complete worked answer. `examples/videocycle` shows the variation an application
off core 0 needs: the firmware mailbox belongs to core 0, so its host kernel
asks and declares before the application core is released.

**`examples/virtdev` is the opposite demonstration** — it declares a size
matching nothing on the board, because the virtual display is whatever the
application says it is and need not resemble the hardware.

`examples/virtdev` is a bootable example of all of this — see
[Examples](#examples).

## Joysticks and game controllers

SDL has two ways of reading the same piece of hardware, and this library
offers both.

A **joystick** is the device as it really is: however many axes, hats and
buttons it happens to have, numbered in the order the device reports them.
Every pad Circle can bind appears this way — the generic HID driver takes
anything that looks like a gamepad, and there are drivers for the PlayStation,
Xbox and Switch pads on top of that. Nothing has to be configured; plug it in
and it is there.

A **game controller** is the same device seen through a MAPPING: a line of
text that says which raw axis, button or hat direction plays the part of each
control on a standard modern pad, so an application can ask for "the A button"
and get an answer. Mappings come from a database file, the community-maintained
`gamecontrollerdb.txt`, loaded with `SDL_GameControllerAddMappingsFromFile`.

**A device with no line in that database is not a game controller**, and
`SDL_IsGameController` says so. It is still a fully working joystick, and an
application that reads raw axes and buttons works with it perfectly. This is
the normal answer for anything that is not shaped like a console pad — a
steering wheel, a flight stick, an arcade panel — and the library gives it
rather than inventing a layout that would put the accelerator somewhere
surprising.

Database lines are tagged with the platform they were recorded on, and only
lines for the running platform load. No published database has ever heard of
Circle, so lines tagged `Linux` are accepted as well — and they are the right
ones, because the joystick GUIDs this library builds have exactly the shape
Linux builds for a USB device, down to the CRC of the device name. An
unmodified `gamecontrollerdb.txt` therefore works as it stands.

Both layers deliver the SDL events an application expects —
`SDL_JOYAXISMOTION`, `SDL_JOYBUTTONDOWN`/`UP`, `SDL_JOYHATMOTION`,
`SDL_CONTROLLERAXISMOTION`, `SDL_CONTROLLERBUTTONDOWN`/`UP` — and both handle
devices that arrive and leave while the application is running:
`SDL_JOYDEVICEADDED` carries a device index, `SDL_JOYDEVICEREMOVED` an
instance ID, exactly as SDL defines them.

**Rumble is coarse, and says so.** Circle offers three settings — off, weak,
strong — so `SDL_JoystickRumble` and `SDL_GameControllerRumble` take the
stronger of SDL's two magnitudes and pick one of those three, honouring the
duration. There is no per-motor control and no envelope underneath to expose.
`SDL_Haptic` — SDL's force-feedback API, with its effect shapes and
directions — is **not implemented at all**, because nothing under it could
carry an effect faithfully, and an effect that silently becomes a buzz is
worse than one that reports it cannot be played.

Also unimplemented, and reporting failure rather than pretending: controller
LEDs, trigger rumble, motion sensors, touchpads, and virtual joysticks.

`examples/padview` puts all of this on screen — see [Examples](#examples).

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
one set of call sites, a branch on `SplitActive()`. That is also exactly what
a single-core world builds — the split is compiled out and only the direct
paths remain. See Building for choosing between the two.

### The three roles

- **The hardware core (core 0).** Circle's world: scheduler, interrupts, USB,
  the SD card, sound. Every device call from anywhere ends up here.
- **The main core.** The application, this library, and the C library. **All
  of SDL runs here** — every call, drawing included, and every one of them has
  finished the work its API promises by the time it returns.
- **The hardware output scale core.** It stands in for display hardware this
  board does not have: a finished frame goes in, a scanout comes out. It runs
  no part of SDL and knows nothing about it. Pixels only.

**What crosses to that last core is a finished frame — pixels, and where on
the screen they go. Never a list of drawing to do.** Drawing is SDL's, and
SDL lives on the main core.

Almost every frame an application draws is the same shape: clear the target,
then blit one opaque texture over it. This library recognises that shape as it
records the frame, and reduces it to the texture itself, exactly where it
already sits in memory — for a game's own raster that is a few hundred
kilobytes rather than a screenful. The clear reduces to nothing: the only part
of it anything can see is the border around the placed frame, and a border
changes when the geometry changes, not sixty times a second.

A frame of any other shape — several draws, a blend over what was already
there, more draws than the recorder holds — is drawn on the main core into a
canvas-sized surface, which is then the finished frame. That surface is
allocated the first time such a frame occurs and never if none ever does. So
the quick path is something this library notices about a frame, never
something an application has to know or do: **every drawing sequence is
correct, and the common one is also cheap.**

The result on a single-screen grant is one pass over the screen instead of
three: resample the finished frame to scanout size, make it visible. What used
to be a screen-sized clear, a screen-sized compose and a screen-sized copy is
now the one resample.

### Choosing what crosses: the crossing count

**How much of a frame travels to the presentation core as a list of drawing
commands, rather than as a finished picture, is a build-time choice.** One
number decides it:

```sh
make PRESENT_CMDS=0     # the default
make PRESENT_CMDS=8
```

- A frame whose draw list **fits within the count** crosses as a list, and
  the presentation core composes it command by command. Nothing is reduced
  and no intermediate surface is written on this side.
- A frame that **does not fit** crosses as a finished picture instead.

**Zero — every frame a picture — is the default, and it costs nothing
extra.** The usual frame is a clear plus one opaque blit, and that shape *is*
the finished picture already: it is the application's own texture, exactly
where it already sits in memory, a few hundred kilobytes for a game raster.
The library recognises the shape and sends the texture. Nothing is painted
and nothing is copied. The clear reduces to nothing but a border repaint when
the geometry moves.

Raising the count moves composition across to the presentation core, frame by
frame, up to the recorder's own capacity — every value in between is a real
setting, not a mode.

Both endings reach the same executor, which writes into the shadow buffer or
the staging frame according to what the firmware granted, and the page flip
or the copy to the screen is the grant's business either way. **What crosses
does not know or care what was granted.**

#### Recognising a picture is not the same as sending one

Two limits are at work and they are deliberately separate.

```c
SDL2CIRCLE_RECORD_MAX_CMDS  16      /* src/sdl2circle.h — the recorder */
SDL2CIRCLE_PRESENT_MAX_CMDS  n      /* PRESENT_CMDS — what may cross */
```

The **recorder** holds draw calls so that a finished frame can be recognised
before anything is painted. Recognising a clear plus one blit takes a couple
of recorded commands, nothing like sixteen, and it has to keep working
however few commands the build lets cross — otherwise a low count would
starve the recogniser as well as the crossing, and every frame would pay for
a full canvas paint it did not need. That is what a single shared limit used
to do, and it is why the count could only usefully be set high.

The **count** decides only how much may travel as a list.

Painting begins the moment a frame can be neither of the two things worth
holding it back for: no longer the simple shape, and already too long to
send. Both possibilities are checked as each draw call arrives, so a frame
that is going to need painting starts there and then rather than waiting for
present — the work is the same either way, but spread across the
application's own draw calls instead of landing in one lump at the end.

Nothing is ever dropped. A frame the recorder gives up on is replayed into
the canvas-sized surface and every later call is painted straight in, so it
renders correctly; it simply crosses as a picture.

The recorder is a fixed array because the frame mailbox is a fixed structure
in coherent memory that both cores read — one frame in flight, no allocation
and no locks on the path a frame takes. Each slot holds a source and
destination rectangle, a colour, a source pointer and pitch, and two blend
flags, and the space is reserved whether a frame uses it or not.

The count is baked into the archive when the library is built. It is not in
any installed header, so an application's own translation units neither see
it nor need to match it; and because every count shares one archive name,
changing it deletes the archive so the previous build cannot be quietly
reused.

**No count has been measured against a real workload.** The recorder has held
sixteen since the core split was written, and the default of zero is what the
library has effectively always done on the board it ships on.

### Why the split is shaped this way

The goal is native speed on modest hardware, and the way to get it is to
stop making the application pay for anything that is not the application.

So the split is not a way to use up spare cores. It is a way to **give the
application core one job**: the program's own main loop, and as near to
nothing else as can be arranged. Presentation and scaling go to the elected
presentation core. Devices, files and the sound feed go to core 0. What is
left on the application core is the program itself, which is the only work
that cannot be moved.

The other half matters just as much. **The topology is elected, not assumed,
and the same compiled binary must be able to run every role on a single
core.** A board with cores to spare and a board with none run the same
image; only the host kernel's choice differs. Today the single-core case is
the degenerate path — no ring, no mailbox, every call direct — and the roles
may later become tasks sharing one core on hardware that cannot spare
several. That is why every crossing in this library is a mailbox or a ring
that any core may consume, and why nothing in it is written against a
particular core number. The library does not decide where anything runs, and
it must never start assuming.

### What the host kernel has to do

In this order. It is ordinary Circle start-up until the split is armed; only
the last two steps are about this library.

1. **Finish the world first.** Bring up interrupts, the timer, the serial
   console, the SD card, and mount the filesystem — all on core 0, as normal.
   The other cores are about to start asking core 0 for things, so there must
   be something there to answer with.

   **A `CScheduler` is needed to run split, and the library supplies one if
   you have not.** The servo and the watchdog are Circle tasks, and a Circle
   task registers itself with the scheduler while it is being constructed —
   through `CScheduler::Get()`, which stops the machine rather than reporting
   an absence. So `SDL2Circle_SplitInit` asks `CScheduler::IsActive()` first
   and creates one where there is none.

   **A host that declares its own keeps it**, exactly as before, and nothing
   about it changes: the library only ever asks whether one exists. Declaring
   one as a kernel member is still the clearer thing to do if the host has
   any use for it of its own — cooperative tasks, `CScheduler::Yield` in an
   idle loop — because then the host controls where it sits in the member
   order. A host with no use for one can now simply not have one.

   A scheduler the library made is never destroyed. The servo and the
   watchdog are registered with it and run for as long as the machine does.

   If this kernel initializes I2C, SPI or the mini UART, declare a
   `CSDL2CircleHardware` member as well, so the CPU clock is settled before
   those peripherals are configured. See the Design section for why.
2. **Start the secondary cores** (`CMultiCoreSupport::Initialize`). Your
   subclass decides what each one does. This library never starts a core and
   never chooses one; the host owns that decision entirely.
3. **Call `SDL2Circle_ArmCoreRuntime` as the first statement on every core**,
   core 0 included. A core that has just started has no thread pointer, and
   C++ exception state is reached through it, so the first thrown exception
   dereferences whatever the firmware left in that register. Where the
   leftover value is zero the read lands in mapped low memory and the throw
   appears to work — which is why skipping this passes on one board and takes
   a data abort on the next, on an ordinary throw, looking exactly like a
   hardware fault. It is one call and it is not optional.
4. **Call `SDL2Circle_SplitInit` once, on core 0.** It creates the servo and
   the watchdog. Until it has returned, no other core may call `SDL_*`: the
   mailboxes are not armed and the call would run on the wrong core.
5. **Start the application** on the core you chose for it, after step 4.
   Because steps 2 and 4 happen in that order, the application core has to
   wait for a signal — a plain shared flag is enough — rather than beginning
   the moment it starts.

Meanwhile one of your secondary cores runs `SDL2Circle_SplitPresentCore`,
which never returns. That is the core that scales each finished frame onto the
screen and makes it visible. It runs no SDL and holds no application state; it
is display hardware, written in software.

Two details that are easy to get wrong:

- **A core that is given no role must be parked** in a wait loop. Returning
  from your dispatch function lets a core run off into whatever follows it.
- **Core 0 must keep yielding for as long as the application runs.** The servo
  is a scheduler task, so it only runs when something gives up the core. A
  host that waits for the application by spinning without yielding will
  deadlock: the application core waits for answers that core 0 is never free
  to give. Wait in a loop that calls `CScheduler::Yield`.

A host kernel is also the one piece of software that may still need a device
this library does not own — its own serial port, a GPIO line. For those,
`SDL2Circle_CallOn0` runs a function on core 0 and waits for it. It is the
same mailbox the library marshals through, and it is a direct call, costing
nothing, when the split is inactive or the caller is already core 0.

### What the application needs

Almost nothing. Call plain `SDL_*` and it works on whatever core you were
placed on. There is exactly one thing the library cannot marshal for you.

**Files.** The C library on this platform drives the SD card directly from
whichever core calls it, and off core 0 that is not allowed. So an
application that reads files must reach them through the I/O service in
`SDL2/SDL_circle.h` — `SDL2Circle_IOOpen`, `IORead`, `IOWrite`, `IOOpenDir`
and the rest. Every one of them is safe from any core, and every one of them
becomes an ordinary direct call in a single-core build, so there is no second
code path to maintain.

If the application has its own file layer — many ports and emulators do —
point it at these functions and the job is done.

If it does not, and it simply calls `fopen`, `std::ifstream` or `opendir` all
over its source, **redirect the C library underneath it** instead of editing
the application. The linker's `--wrap` option does this without touching a
single vendored file:

```
# in your kernel's Makefile
LDFLAGS += --wrap=_open --wrap=_close --wrap=_read --wrap=_write \
           --wrap=_lseek --wrap=_fstat --wrap=_unlink \
           --wrap=opendir --wrap=readdir --wrap=closedir
```

Then write one small file defining `__wrap__open`, `__wrap__read` and so on.
Each reads the same way:

```c
long __wrap__read(int fd, void *buf, size_t len)
{
    if (on_core_0())                       // or the split is not active
        return __real__read(fd, buf, len); // the genuine implementation
    ...                                    // otherwise use the I/O service
}
```

Three things make this work, and all three matter:

- **`--wrap` rather than redefining the symbols.** The C library's file
  syscalls are defined together in one object file. Redefining some of them
  either collides at link time or, worse, leaves the originals linked in
  beside your versions with no warning at all.
- **`__real_*` is the mechanism, not a convenience.** The I/O service does its
  work by making these same calls once it has reached core 0. Without a route
  back to the original, the wrapper would call itself forever.
- **The service names an offset on every read and write; the C library expects
  a file to remember where it is.** So your wrapper has to keep the position
  itself, one value per open descriptor, advancing it on each read and write
  and setting it on each seek. Seeking from the end needs the file's length,
  which `SDL2Circle_IOOpen` hands back when you ask for it.

Descriptors 0, 1 and 2 are the console rather than files, so they have no
route through the I/O service. Send those to core 0 with
`SDL2Circle_CallOn0` and let the original implementation run there.

## Logging from any core

The serial console is a device, so only the hardware core may write to it.
That would leave every other core unable to say anything — and the cores that
have the most to report, the application core and the elected presentation
core, are exactly the ones that may not speak.

So they do not write. **Each core formats its line into a ring of its own and
returns**, and the hardware core's servo drains every ring into the logger.
Nothing crosses but memory. A core that logs never touches the console and is
never held up by one that is printing.

```c
#include <SDL2/SDL_circle.h>

SDL2Circle_Log("mygame", SDL2CIRCLE_LOG_NOTICE, "level %d loaded", n);
```

- **It never blocks, and it never lies.** If a ring is full the line is
  dropped and counted, and the next drain prints how many were lost from
  which core. Waiting would put the calling core to sleep for the sake of a
  diagnostic, and overwriting would silently corrupt the record.
- **`from` is stored as a pointer**, so it must outlive the call — a string
  literal, never a buffer on the stack. It is printed later, on another core.
- **Byte output has its own entry point.** `SDL2Circle_LogBytes` takes output
  in whatever pieces it was written in — an application's `stdout` — and
  assembles it into lines, publishing each one as it completes. A log carries
  lines and has nowhere to put half of one.
- **The hardware core writes straight through**, and so does every core when
  the split is inactive. That keeps the boot log immediate, before any servo
  exists to drain anything, and it means a single-core build pays nothing at
  all for this.

Lines from other cores appear when the servo next drains, so they carry the
drain's timestamp rather than the moment they were produced, and they are
ordered per core rather than against each other. For working out what
happened that is enough; for measuring how long something took, use the
performance receipts below.

## Performance receipts

Call `SDL2Circle_SetPerfInterval(10)` and the library prints, every 10
seconds, one line per core that has run instrumented code. It is silent
until a host asks for it, and how a host decides to ask — a boot switch, a
build option, never — is the host's business, not the library's:

```
sdl2perf: 60.0 fps c2: awake 41.2% (9885M of 24005M): app 0.0% render 50.5% dma 6.1% vsync 4.1% wait 0.0% serve 0.0% audio 0.1% input 0.0% yield 0.0%
```

The frame rate counts presented frames.

**`awake` is how busy the core was; the percentages after it divide up only
that awake time.** The two are separate on purpose, because a processor's
cycle counter stops while the core is asleep. A core parked for most of
every frame and a core running flat out can print identical percentages —
what tells them apart is `awake`, which is the counted cycles measured
against the wall clock, and the wall clock never stops. Read the line as
"this core was awake 41% of the time, and here is what it did while it was".

The categories: `render` is present-path compute; `serve` is the hardware
core doing another core's work, the call mailbox and the log drain; `audio`
and `input` are the pumps; `yield` is time given to other scheduler tasks;
and `app` is whatever is left, which is the application itself on its own
core.

Blocking is reported in three parts, because they have three different
cures. `dma` is waiting for the transfer into the framebuffer to finish.
`vsync` is waiting for the raster to reach the vertical blanking interval.
`wait` is one core waiting on another across the frame mailbox — a wait on
software rather than on the display. All three are kept apart from `render`
on purpose: at a locked frame rate the blocking absorbs every spare cycle,
and folded into `render` it would make the present path look saturated when
it is mostly idle.

The wall figure assumes the processor clock reported at the first report
holds for the run. A board that is thermally throttling is changing that
clock underneath the measurement, so treat `awake` as approximate there.

The categories do not overlap. Sections nest — a wait happens inside the
present that is waiting — and each one is charged only for the cycles its
own body used, with its children's time handed to them. So the percentages
partition the core's cycles and `app` is genuinely what is left.

A single-core build reports core 0 alone, which is the whole machine
there. Under the core split each active core reports its own line, which
is how a question like "how busy is the presentation core, could it
afford filtering" gets a measured answer.

Unarmed, the instrument costs one branch per section. Arming it is
`SDL2Circle_SetPerfInterval(seconds)` in `SDL2/SDL_circle.h`, and that is
the only way in — the library reads nothing from boot configuration for
this.

## Design

- **The app owns the main loop; the shim rides it.** Everything the shim does
  per frame hangs off `SDL_PumpEvents` (called by `SDL_PollEvent`) and
  `SDL_RenderPresent`. An app that polls events and presents frames keeps the
  whole machine alive; one that stops doing either wedges a cooperatively
  scheduled board, which is what the watchdog exists to report. Audio
  callbacks never run in interrupt context.
- **The CPU clock and the case fan belong to the shim.** Circle's
  `CCPUThrottle` is both of those in one class: it sets the clock rate, and
  where `cmdline.txt` names a fan pin with `gpiofanpin=` it switches a case
  fan instead. Circle creates neither for itself — it requires that a system
  holds exactly one such object and that something calls it regularly, or
  none of that management ever happens. A host kernel has no per-frame loop
  to call from and the shim already runs one, so **the shim owns the object**
  and drives it from whichever heartbeat is live: the servo on the hardware
  core under the split, `SDL_PumpEvents` otherwise. There is one update
  policy and it lives with the owner, because Circle's `Update()` already
  limits itself to one measurement every few seconds, and a second interval
  at a call site would only beat against that one.
- **A host kernel must not create a `CCPUThrottle` of its own.** Circle
  allows exactly one, and constructing a second stops the machine. Nor can
  the shim work around a host that does: Circle offers no safe way to ask
  whether one exists, because `CCPUThrottle::Get()` stops the machine rather
  than reporting an absence. So the shim keeps its own record of what it
  made, and a host that also makes one gets a dead board.
- **Hardware management comes up in `SDL_Init`, unless you ask for it
  sooner.** The clock is taken to maximum there, because Circle boots the
  board at its idle rate. A kernel that brings up I2C, SPI or the mini UART
  in its own `Initialize()` needs the clock settled *before* those: raising
  the CPU clock also moves the core clock, and those peripherals take their
  transfer speed and their baud rate from it. Such a kernel declares a
  `CSDL2CircleHardware` member (`SDL2/SDL_circle.h`) and gets hardware
  management while the kernel object is being constructed, before anything in
  `Initialize()` runs. A kernel that declares nothing does nothing and is
  served at `SDL_Init`.
- **Self-contained payloads.** The shim brings up everything it needs
  (USB host controller, framebuffer, sound) inside `SDL_Init`. Host-kernel
  contract: initialize `CInterruptSystem` and `CTimer` before `SDL_Init`.
  To run split, the host kernel also starts the secondary cores and hands
  one to `SDL2Circle_SplitPresentCore` — the shim marshals, it does not own
  the cores.
- **No `std::thread`, anywhere in the library.** Every concurrent thing the
  shim runs is a Circle `CTask` on core 0 or a core the host handed it, and
  everything between cores is a lock-free ring or a mailbox. The library
  never starts a thread of its own, so nothing it does depends on the C++
  threading runtime being live. An application is free to use `std::thread`
  — the per-frame pump yields the scheduler so cooperative threads make
  progress — but that is the application's choice and not something the
  library needs.
- **The scheduler is the one Circle object the split cannot do without,
  and the shim will provide it.** A Circle task reaches for the scheduler
  while it is being constructed, through a call that stops the machine when
  there is nothing to return, so the split's servo and watchdog cannot exist
  without one. `CScheduler::IsActive()` is a safe question — unlike
  `CCPUThrottle`, which cannot be asked at all — so the shim asks it, makes a
  scheduler only where the host has not, and leaves a host's own scheduler
  entirely alone.
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

### Choosing single-core or multicore

**You choose when you configure the world, and the choice is fixed when you
build.** Both are supported and the application's source is the same either
way.

- **A single-core world** — configured without `ARM_ALLOW_MULTI_CORE` — builds
  this library with the core split compiled out. Every call runs directly, on
  the one core, through the same call sites. This is the build for single-core
  hardware and for older boards, and it is what the library did for its whole
  first life.
- **A multicore world** — configured with `ARM_ALLOW_MULTI_CORE`, which is what
  `make deps` does — builds the split as well. Nothing is forced on by
  building it: the split stays inert until a host kernel calls
  `SDL2Circle_SplitInit`, so one image can still run everything on core 0.

The API is identical. `SDL2Circle_SplitInit` exists in both builds; in a
single-core one it reports that there is no multicore world to split into and
changes nothing, and `SDL2Circle_SplitActive` keeps answering no, which is the
answer every call site already handles.

A world elsewhere on disk works with
`make CIRCLESTDLIBHOME=/path/to/circle-stdlib`.

### Choosing the crossing count

`make PRESENT_CMDS=n` (0 by default) sets how much of a frame may travel to
the presentation core as a list of drawing commands rather than as a finished
picture — see
[Choosing what crosses](#choosing-what-crosses-the-crossing-count). The value
is compiled into the archive, objects are kept in per-count trees so builds
never mix, and changing it deletes the archive rather than risk handing back
the previous count's build under the same name.

Building through Circle's `Rules.mk` — as the test apps do — you get the
world's own `DEFINE`, whichever way it was configured, and there is nothing to
think about. **If you compile any translation unit outside it** — a foreign
build system with its own flag list — it must match the world it will link
against. Circle's
headers change shape on it (spinlocks, atomics, memory layout), so an object
compiled without it disagrees with the library it links against, and nothing
tells you: it builds, it links, and it is wrong at runtime.

Applications link by including `sdl-app.mk` after Circle's `Rules.mk`
(see any Makefile under `examples/`): it links with `sdl-app.ld` — required
with binutils 2.44+, whose linker refuses non-adjacent TLS sections with
the stock script ordering (libc++'s threading carries TLS) — and adds the
Circle sound library the audio backend needs. `sdl-app.ld` is derived from
Circle's `circle.ld` and remains GPLv3 (see its header); everything else
here is zlib.

### Catching a stub that outlived its purpose

An application filling gaps in this library — a file of its own standing in
for calls the library does not implement yet — should link the archive whole
while developing:

```make
LIBS = --whole-archive $(SHIM)/libSDL2-$(BOARD).a --no-whole-archive \
	$(CIRCLE_STDLIB_LIBS)
```

An object file linked straight into the kernel beats an archive member of
the same name, and does it in silence. So when this library implements
something an application had stubbed, the stub goes on winning: the
application keeps calling its own do-nothing version, the real one is never
linked, and nothing anywhere says so. That failure looks like the library
not working.

Linking the archive whole makes the same situation a duplicate-symbol error
at link time, naming both definitions. The fix is then to delete the stub,
which was the intention all along.

`--no-whole-archive` closes the scope immediately, so only this archive is
forced and the C library, libc++ and Circle link as they always did. **There
is no size penalty for an application that already uses most of the
library** — measured on a consumer with a stub file of its own, both the
member count and the image were identical to the byte.

It is a development setting rather than a shipping one: an application that
uses only a corner of this library, and knows it, pays for the rest. Nothing
requires it either way, and no example here sets it.

## Examples

Each is a complete bootable kernel exercising one subsystem. They are the
library's worked examples and its test harness at the same time — useful as
templates, and the way a change is proven before it ships:

- `examples/gradient` — animated full-screen gradient (video path)
- `examples/keyecho` — scancode display, modifier lights, held-key grid (input)
- `examples/tone` — 1 kHz sine over HDMI via the callback API (audio)
- `examples/padview` — every attached joystick, gamepad and wheel on screen at
  once: name, GUID, USB IDs, whether the mapping database recognised it, live
  axis bars, hat and button lights, the mapped controller view where there is
  one, and a running log of devices arriving and leaving (joystick and game
  controller)
- `examples/virtdev` — an application declaring the display it is to be given,
  then checking every SDL answer about the display against what it declared,
  and making each declaration the library refuses so the reason for it is on
  the log (virtual device)
- `examples/videocycle` — the whole video and audio world torn down and rebuilt in
  a loop, at alternating source geometry, while a presentation core keeps
  running: what a settings menu does on every change, with nobody at the
  keyboard (core split, present-path lifetime)
- `examples/dispinfo` — no SDL at all: a serial-only probe that logs what the
  firmware reports about the attached display, and what Circle's framebuffer
  returns for a series of allocation requests. It is where the raw numbers
  behind the presentation geometry come from

## License

zlib, matching SDL itself — see `LICENSE`.
