# circle-libsdl2

**An SDL2-compatible shim for bare-metal Raspberry Pi** — a library that
implements the SDL2 API in place of SDL itself. Write (or port) an
ordinary SDL2 application, link it against this library and the
[Circle](https://github.com/rsta2/circle) framework, and it boots directly
from `kernel8.img` — no operating system underneath at all.

This is **not a port of SDL**. It is a from-scratch implementation of the
SDL2 API surface that real applications call, mapped directly onto Circle's
bare-metal drivers.

It also lets your application **run somewhere other than core 0** and still
call SDL in the ordinary way. Circle's own subsystems — scheduler, interrupts,
USB, FatFs, sound — are core-0-only by construction, so an application on
another core cannot touch any of them. This library removes that restriction:
it marshals every platform call back to core 0 through lock-free rings and
mailboxes, and provides a presentation worker you can run on a core of your
choosing. See [Running off core 0](#running-off-core-0).

Proven in real use by [pi-mame](https://github.com/Xalior/pi-mame), which runs
MAME's emulation core on bare metal through this library — a full application
using the whole API surface at once: fullscreen software rendering, USB HID
keyboards, HDMI audio, files off the SD card, and its emulation running on a
core that never touches a device.

## What works

| SDL2 subsystem | Circle backing |
|---|---|
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending, scaled `SDL_RenderCopy` | `CBcmFrameBuffer` — double-buffered, vsync page flip. Where the firmware grants one screen instead of two, the finished frame is scaled onto it, on a core of the host's choosing |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel, or the virtual device the application declared for itself |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Off core 0: USB stays on core 0, events cross by ring |
| Mouse → SDL events, `SDL_GetMouseState`, relative mode, warping | Circle USB mouse (raw reports — no Circle-drawn cursor). A mouse says how far it moved and never where it is, so the library keeps the position itself and clamps it to the window. Off core 0: USB and event synthesis stay on core 0; the position and buttons are held in memory both cores see |
| Joysticks, gamepads and wheels: enumeration, hot-plug, axes, hats, buttons, GUIDs, coarse rumble | Circle's USB gamepad drivers — the generic HID one and the console-specific ones. Off core 0: USB and event synthesis stay on core 0; the readings are held in memory both cores see |
| Game controllers: `gamecontrollerdb.txt` mappings, `SDL_IsGameController`, mapped axes and buttons, controller events | the mapping text is read the way SDL2 reads it, and found by the same joystick GUID SDL2 builds |
| Files as `SDL_RWops` streams, and streams over memory | the I/O service, so an application off core 0 opens a file with the ordinary SDL call |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue. The device plays 16-bit signed stereo; an application that asks for something else and does not permit a change gets **what it asked for**, converted at the device boundary, and `obtained` reports its own spec. Permit a change and `obtained` reports the device's instead. Off core 0: your callback fills a ring, core 0's servo task feeds the device |
| Events: queue, `SDL_PumpEvents`, window focus | the per-frame service point: USB pump and scheduler yield on core 0, ring drain and liveness signal off it |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs). `SDL_Delay` yields to the scheduler on core 0; off it, spins to a µs-exact deadline — deterministic, but it occupies the core for the duration |
| Files: an I/O service callable from any core | FatFs on core 0, marshalled (`SDL2Circle_IO*`) — for applications whose own file layer must not touch the SD card directly |
| Surfaces and pixel formats: `SDL_CreateRGBSurface*`, `SDL_ConvertSurface`, `SDL_ConvertSurfaceFormat`, `SDL_MapRGB`/`SDL_MapRGBA`, `SDL_GetRGB`/`SDL_GetRGBA`, palettes and `SDL_SetPaletteColors`, `SDL_FillRect`, blitting, `SDL_SetColorKey`, lock/unlock, `SDL_ConvertPixels` | in memory. 8-bit paletted surfaces are first-class: a game that renders through a palette does so here without converting anything itself |
| Threads and synchronisation: `SDL_CreateThread`, mutexes, condition variables, semaphores, atomics, thread-local storage | Circle's scheduler and the AArch64 atomics |
| C++ standard library threading: `std::mutex`, `std::recursive_mutex`, `std::condition_variable`, `std::call_once`, `std::thread`, `thread_local` | this library's own implementation of the interface libc++ asks its platform for, built from processor atomics so it is valid on every core. See [C++ threading](#c-threading) |
| Timers: `SDL_AddTimer`/`SDL_RemoveTimer` | the system timer, serviced from the frame's service point |
| `SDL_image`: `IMG_Load` and its family | PNG decoded here — a DEFLATE decompressor and a PNG reader, because there is no libpng and no zlib. BMP goes to `SDL_LoadBMP_RW` |
| `SDL_mixer`: several sounds at once, channels, volumes, panning, music | above the audio device, mixing into it. Chunks are converted at load, never in the callback |
| Audio conversion: `SDL_BuildAudioCVT`, `SDL_ConvertAudio`, `SDL_LoadWAV_RW`, `SDL_MixAudioFormat` | in memory, through float — every width, either byte order, one or two channels, any rate ratio |
| `SDL_Log` and its family, message boxes | the same ring every other line takes, so an application's own diagnostics are safe to write from its own core |
| `SDL_GetBasePath` / `SDL_GetPrefPath`, clipboard, key names, `SDL_stdinc` strings and maths | in memory, and the card. See "Declaring the base path" |
| Typed text: `SDL_TEXTINPUT` events, `SDL_StartTextInput`/`SDL_StopTextInput`/`SDL_IsTextInputActive` | the same USB keyboard, mapped through the modifier state to the character a US layout would produce. A key press sends the key event first and the text second, as SDL does |
| Init/error/version/hints | — |

**The framebuffer is 32-bit; the formats an application works in are its
own.** The panel is always allocated at 32 bits per pixel and a texture is
always STORED as ARGB8888, because the presentation path reads a texture's
pixels directly and converting during presentation would put that work on
the core that must not be delayed.

The application's format is honoured at the EDGE instead. `SDL_CreateTexture`
accepts any format SDL names; `SDL_QueryTexture` answers with the one that
was asked for; pixels handed in through `SDL_UpdateTexture` are converted on
the way in, and `SDL_LockTexture` hands back a staging buffer in the
application's own format which `SDL_UnlockTexture` converts. So an
application writes the pixels it believes it is writing, and the cost falls
on its own core. Where the format IS ARGB8888 there is no staging buffer and
no conversion at all.

Surfaces carry no such restriction: any depth, any masks, and 8-bit paletted
throughout.

Not yet, and each of these fails with a message saying so rather than
quietly doing something else:

- **No MIDI synthesiser.** `Mix_LoadMUS` reads WAV. A MIDI file is a score
  rather than a recording, and performing one is a sound engine in its own
  right.
- **No fades in the mixer.** `Mix_FadeInMusic` and its relatives start and
  stop at volume.
- **No rotation in `SDL_RenderCopyEx`.** Mirroring works, in all three
  combinations; an angle is refused.
- **No render-to-texture.** `SDL_SetRenderTarget` accepts the default target
  and refuses a texture.
- **No force-feedback device.** The `SDL_Haptic` calls exist and report that
  there is none, so an application's "rumble if it can" path takes its other
  branch.
- **No OpenGL, Vulkan or Metal.** The Pi has no bare-metal GPU driver;
  software rendering is the design, not a temporary measure. The entry
  points all exist: `SDL_GL_CreateContext` returns null with an error, so a
  program with an optional accelerated renderer takes its software path, and
  `SDL_GL_DeleteContext` is a no-op so its shutdown path still runs. They
  are here because that shutdown path is usually written as
  `if (ctx) SDL_GL_DeleteContext(ctx)` — dead code that must still LINK.

  **A window never reports `SDL_WINDOW_OPENGL`, `SDL_WINDOW_VULKAN` or
  `SDL_WINDOW_METAL`, whatever was asked for**, because there is no such
  renderer to report. A program that tests the window flag therefore takes
  its software path straight away, which is the path that works here.
  Upstream SDL reaches the same outcome by refusing to create the window at
  all; the window is worth having, so the bit goes instead of the window.
- Controller motion sensors and touchpads, virtual joysticks.

## Presentation geometry

Resolutions are always involved on a bare-metal Pi, and the library
names each of them rather than treating them as one.

**The scanout is the physical display** — what the hardware actually sends
over the display cable. `width=` and `height=` in `cmdline.txt` ask the
firmware for a display mode, allocating the framebuffer is what sets it, and
**the firmware then reports the mode it actually set. That report is the
scanout.** It is read from the firmware, never calculated: not from the
framebuffer's pitch, not from its size, and not from the width and height
Circle returns, which are only the arguments it was constructed with.

**Set neither and the panel keeps its own mode.** The library then asks the
firmware for no particular size, which is how you say "whatever the display
is already doing", and the firmware allocates the display's own mode. There
is no default resolution anywhere in the library — a default would not be a
preference, it would be an instruction, because asking for a mode is what
sets one. A card whose configuration asks for nothing gets whatever mode the
attached display is already using.

**On a Pi 5, either set no display mode at all or set exactly the one the
screen is already using.** That board's firmware chooses its display mode
before any kernel starts and does not change it afterwards. It still accepts
a `width=`/`height=` request, and then reports the requested mode back as
though it had applied it, while continuing to send the screen's own mode to
the display. This library takes the firmware's report as the truth, so it
would describe a mode that is not being displayed, and every measurement
derived from it would be wrong.

A Pi 3 or a Pi 4 applies the requested mode and reports it correctly, so
neither has this problem.

**The canvas is the virtual display** — the display area the application is
given, and the relation between its shape and the scanout's decides the
letterboxing. **The application declares it**, in its own code, before
`SDL_Init` — see [Declaring the display](#declaring-the-display). It is
required, and there is no fallback: without it the library refuses to start.

**These are settings doing different jobs, and they coexist.** Neither is a
fallback for the other and there is no order of precedence between them. One
is asked of the firmware by the operator; the other is declared by the
application. `width=` and `height=` never set the virtual display, and the
declaration never sets the physical one.

**The application's own resolution** is whatever it renders at. It calls
`SDL_CreateWindow` and `SDL_RenderCopy` as usual, and the rectangles it
passes are canvas coordinates, because the canvas is the window. An
application never learns what the physical display is doing.

A frame therefore travels application → canvas → scanout, and **the library
composes both steps into a single resampling pass** when the frame is
presented — on the presentation core when the core split is active, inline
otherwise. The canvas contributes arithmetic, never an intermediate copy.

- **`SDL_RenderCopy` honours its rectangles.** A destination the same size as
  the source, on a canvas that is the scanout, is still an unscaled blit —
  the same bytes, on the same path. Anything else resamples.
- **Nearest neighbour, and only nearest neighbour.** Per-axis index tables
  are built once per geometry and reused; an exact integer ratio skips the
  tables and replicates. `SDL_HINT_RENDER_SCALE_QUALITY` is stored like any
  other hint and **has no effect** — `"linear"` is a later phase, not a
  silent fallback.
- **Fit is the default placement.** The canvas is scaled up as far as it
  fits, centered, and the remainder of the scanout stays black. Put
  `canvas=stretch` in `cmdline.txt` to fill the scanout instead, without
  preserving the aspect ratio.
- **A frame is composed in ordinary memory, never directly in the
  framebuffer.** The framebuffer is uncached, and a scaler writing it
  directly pays that cost once per pixel: on a Pi 4, 26.1 ms for a 1280x720
  frame against 1.4 ms into ordinary memory. So a present is composed
  off-screen and the finished frame is copied to the framebuffer in whole
  rows — 6.0 ms for the same frame — which is also what keeps the picture
  whole, because the screen is written by that one copy and is never visible
  mid-composition.
- **The copy to the screen runs on the DMA engine where it can.** When the
  firmware grants enough memory for two screens, presenting is a page flip
  and the copy goes to the half being panned to. When it grants only one — a
  Pi 5 does — the finished frame goes to the granted surface itself, which is
  the most expensive thing the presentation core does. So the library gives
  that copy to a DMA channel and returns without waiting for it, scaling the
  next frame into a second buffer while the transfer runs. One frame is in
  flight at a time. If no
  DMA channel is free, the CPU does the copy exactly as before.
- **The present path is built once and reused.** Its buffers and its DMA
  channel are sized by the framebuffer the firmware granted, and that grant
  is made once and kept for as long as the machine runs, so a second window
  adopts the same one. An application may destroy its window, renderer and
  textures and create new ones as often as it likes — which is what a
  settings menu does whenever a video setting changes — and none of it is
  allocated again. There are only a few DMA channels on the board and the
  sound device needs one too.

The library logs the whole chain once at startup and once per distinct
geometry, so a serial console tells you what happened without guessing:

```
sdl2video: scanout 1920x1080 (firmware reported), canvas 720x576 (declared virtual device)
sdl2video: canvas 720x576 on scanout 1920x1080: fit -> 1350x1080+285+0
sdl2video: granted 1080 rows < 2160: shadow-buffered present
sdl2video: present: dma copy, channel 11, 8294400 bytes, double-shadowed
sdl2video: copy src 320x224 -> canvas 720x504+0+36 -> scanout 1350x945+285+67 (nearest)
```

The `present:` line names the path that is actually in use — `dma copy` or
`cpu copy`, and the reason when it is the latter.

### What the window flags say

`SDL_GetWindowFlags` describes **the machine, not the request**. A flag an
application passed to `SDL_CreateWindow` is reported back only where this
window can honour it; reporting it otherwise tells the asker its own question
back, and a game branches on the answer.

- **`SDL_WINDOW_INPUT_FOCUS` is always set.** There is one window and no
  window manager to take focus away from it. A flag that is never set reads
  exactly like a flag that is false, and a game that believes it has lost
  focus pauses, stops drawing or drops input — a black screen with a clean
  log.
- **`SDL_WINDOW_SHOWN` is always set, and `SDL_HideWindow` does not clear
  it.** The surface is on the glass and cannot leave it, so `SHOWN` is the
  truth. `SDL_WINDOW_HIDDEN` and `SDL_WINDOW_MINIMIZED` are never reported.
- **`SDL_WINDOW_OPENGL`, `SDL_WINDOW_VULKAN` and `SDL_WINDOW_METAL` are never
  reported.** See the accelerated-graphics note above.
- **`SDL_WINDOW_MOUSE_FOCUS` is asked, not stored**, because a USB mouse can
  arrive or leave long after the window is made. It answers the same question
  `SDL_GetMouseFocus` answers, so the flag and the function cannot drift
  apart — which is the rule for any flag whose truth can change after the
  window exists.

### Declaring the display

**Every application declares the display it is to be given**, in its own
code, before `SDL_Init`. This is not optional and there is no fallback of any
kind — not the boot command line, not the panel. An application that has not
declared one has not said what display it is to be given, and the library
will not invent one, so `SDL_Init` fails and says why on the console.

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
resolution**, which is the point: it draws in the virtual display it
declared, and the placement rules above put that onto the physical screen.

- **It is fixed.** One declaration is accepted, before anything has asked the
  library about the display. A second one is refused, and so is one made
  after the display size has been settled — the first display query, or the
  first window. The size an application is given cannot change while it is
  running, so every geometry derived from it is worked out once and holds for
  the rest of the run.
- **32 bits per pixel, and nothing else.** The framebuffer is allocated at 32
  bits and streaming ARGB8888 is the only texture format, so another depth is
  refused rather than quietly rounded to this one. Width and height must both
  be above zero.
- **The return value reports the outcome.** Zero means accepted; -1 means
  refused, with `SDL_GetError` saying which of the above was not met. A
  refused declaration changes nothing, and an earlier accepted one still
  stands.
- **It states the virtual display, not the physical one.** The mode the panel
  is driven at remains the operator's decision, asked for in `config.txt` and
  `cmdline.txt` and granted, or not, by the firmware. Declaring a virtual
  device asks the firmware for nothing; it says what the application is to be
  shown, and the library scales.
- **Without it the library does not start.** `SDL_Init` returns failure with
  `SDL_GetError` explaining, and puts a line on the console naming the call
  to make. Nothing is initialised, no device is touched, and no display
  question can be answered.

### Matching the virtual display to the physical one

**Where the numbers come from is for the application to decide, and only the
application.** A build constant, a settings file, an option of its host
kernel's own, a value read from a network port. The library is told; it
discovers nothing and offers no way to ask what the physical display is.

So an application that wants its virtual display to **match** the panel
determines the physical size for itself and passes it in — which takes only
the few lines below, using Circle's public property tags, and needs nothing
from this library at all:

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
complete worked example. `examples/videocycle` shows the variation an application
off core 0 needs: the firmware mailbox belongs to core 0, so its host kernel
asks and declares before the application core is released.

**`examples/virtdev` is the opposite demonstration** — it declares a size
matching nothing on the board, because the virtual display is whatever the
application says it is and need not resemble the hardware.

`examples/virtdev` is a bootable example of all of this — see
[Examples](#examples).

### Declaring the base path

SDL gives an application two directories: the one it was installed in
(`SDL_GetBasePath`) and one it may write settings and saved games into
(`SDL_GetPrefPath`). On a desktop SDL works both out for itself, from where
the running program came from and from the user's home directory.

Neither question has an answer here. The payload was chain-loaded over a
wire or started from a card; there is no program image to locate, no user
and no home directory. Where an application's files were put is a decision
somebody made when they built the card, and the only party that knows it is
the one embedding this library. So it states it, once, before `SDL_Init`:

```c
SDL2Circle_DeclareBasePath("/games/example");
```

**The library learns nothing else from this.** It stores the string, hands
it back from `SDL_GetBasePath`, and composes `SDL_GetPrefPath` below it. It
does not read the path, does not check that it exists, and carries no
default for any particular application — it cannot know one is there.

The declaration is fixed, on the same terms as the virtual display: accepted
once, before anything has asked for a path, and refused afterwards. Both
functions return a string the caller releases with `SDL_free`, and both end
in a separator, because that is SDL's contract and applications append to
the result without checking.

**Not declaring one is not an error.** It answers `/`, with one warning on
the log. This differs deliberately from the virtual display, which stops
`SDL_Init` when it is missing: there is no sane default display size, but a
board has exactly one filesystem and `/` is a real directory an application
can read and write. SDL returns a non-null path on every desktop platform,
so a great many applications dereference the answer without looking —
refusing would turn a missing declaration into a crash inside the
application rather than a message from here.

### The library's own boot switches

A boot argument block sits at a fixed offset inside the kernel image, and a
loader writes a plain argument string into it before pushing the image — so a
setting can ride a boot without anything being rebuilt.

**This library reads that block itself**, and acts on the switches that
describe what IT does. An application does not forward them, is never asked
for them, and cannot fail to pass them on. That last point is the whole
reason: while each application interpreted these in turn, one that had never
heard of a switch silently lost the capability — the switch was stamped, the
loader confirmed it, and nothing happened, with nothing anywhere to say why.

| switch | effect |
|---|---|
| `--rapi-debug-uart` | types bytes arriving on the serial console into the machine as SDL key events. Takes **no value** |
| `--rapi-perf=N` | a performance report every N seconds |

An application still reads the same block for its own arguments, and still
strips every `--rapi-` switch before its program sees them. Those are its
arguments; reading the block twice is harmless, because nothing here writes
to it.

Serial key injection needs two things, and neither turns it on alone: the
switch, and a serial device. **A kernel lends its own device unconditionally**
— it must not construct one, because a second device on the same slot halts
the board inside its constructor:

```c
SDL2Circle_SetInjectSerial (&m_Serial);   // no condition around it
```

Whether anything is injected through it is then the library's decision, taken
from the switch it found for itself.

Anything that decides how the KERNEL starts stays with the kernel, and
`rapi-split` is the example: it chooses whether the core split is set up at
all, which means choosing which cores are started. That happens before this
library exists, and it is read from `cmdline.txt` rather than from the block.

## Joysticks and game controllers

SDL has different ways of reading the same piece of hardware, and this
library offers both.

A **joystick** is the device as it really is: however many axes, hats and
buttons it happens to have, numbered in the order the device reports them.
Every pad Circle can bind appears this way — the generic HID driver accepts
any device that presents as a gamepad, and in addition there are drivers for
the PlayStation, Xbox and Switch pads. Nothing has to be configured; plug it
in and it is there.

A **game controller** is the same device seen through a **mapping**: a line of
text that says which raw axis, button or hat direction corresponds to each
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
lines for the running platform load. No published database contains lines
tagged for Circle, so lines tagged `Linux` are accepted as well — and they are
the right ones, because the joystick GUIDs this library builds have exactly
the shape Linux builds for a USB device, including the CRC of the device
name. An unmodified `gamecontrollerdb.txt` therefore works as it stands.

Both layers deliver the SDL events an application expects —
`SDL_JOYAXISMOTION`, `SDL_JOYBUTTONDOWN`/`UP`, `SDL_JOYHATMOTION`,
`SDL_CONTROLLERAXISMOTION`, `SDL_CONTROLLERBUTTONDOWN`/`UP` — and both handle
devices that arrive and leave while the application is running:
`SDL_JOYDEVICEADDED` carries a device index, `SDL_JOYDEVICEREMOVED` an
instance ID, exactly as SDL defines them.

**Rumble is coarse, and the library does not pretend otherwise.** Circle
offers settings — off, weak, strong — so `SDL_JoystickRumble` and
`SDL_GameControllerRumble` take the stronger of SDL's two magnitudes and
choose the closest of those, honouring the duration. There is no per-motor control and
no envelope underneath to expose.
`SDL_Haptic` — SDL's force-feedback API, with its effect shapes and
directions — is **not implemented at all**, because nothing under it could
carry an effect faithfully, and an effect that silently becomes a buzz is
worse than one that reports it cannot be played.

Also unimplemented, and reporting failure rather than pretending: controller
LEDs, trigger rumble, motion sensors, touchpads, and virtual joysticks.

`examples/padview` puts all of this on screen — see [Examples](#examples).

## Running off core 0

The Circle kernel is core-0-only by design: its scheduler, interrupts, USB,
FatFs and sound may only be touched from there. So an application that wants a
core to itself has a problem — on any other core, it cannot call the platform
at all.

This library solves that. **Your code runs wherever the host kernel puts it
and keeps calling plain `SDL_*`;** the library marshals the calls. Call
`SDL2Circle_SplitInit` once, on core 0, before the application starts, and it
arms tasks **on core 0**:

- a **servo** — the service task. It drains the call mailbox, runs the I/O
  service, pumps USB input into the event ring, feeds the sound device from
  the audio ring, and updates the CPU throttle;
- a **watchdog** — reports a stalled application core instead of leaving the
  board stopped with no explanation.

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

- **The pump no longer does the platform work.** `SDL_PumpEvents` on your core
  only touches shared memory — drain the event ring, mirror key state, update
  the liveness signal. USB plug-and-play and HID translation stay on core 0's
  servo.
- **Audio inverts from pull to push.** Your callback fills the audio ring; the
  servo feeds the sound device at its own cadence.
- **`SDL_Delay` becomes exact, and occupies the core.** On core 0 it is
  `CScheduler::MsSleep` — cooperative, so you become *runnable* at the deadline
  but only resume when the scheduler next gets control, and a peer that is slow
  to yield overshoots your delay by however long it takes. Off core 0 there is
  no scheduler to defer to, so the library spins on the system timer (µs
  resolution, `yield` hint) and returns at the deadline. You trade a scheduler
  yield for a deterministic wait, and the cost is that the core is fully
  occupied for the duration — which is only sound *because* the core is
  dedicated and has nothing else to run. The same spin on core 0 would starve
  the servo, the watchdog, USB and the audio feed, which is why core 0 keeps
  yielding.

**Single-core is the same code in its degenerate case.** Without `SplitInit`
there is no ring and no mailbox: every call executes directly, the pump does
the platform work itself, audio pulls, and the watchdog is an in-band timer
that dumps the scheduler's task list if the main loop stops for 30 seconds.
One codebase, one set of call sites, a branch on `SplitActive()`. That is also
exactly what a single-core Circle build produces — the split is compiled out
and only the direct paths remain. See Building for choosing between the two.

### The roles

- **The hardware core (core 0).** Circle's own subsystems: scheduler,
  interrupts, USB, the SD card, sound. Every device call, from any core, is
  executed here.
- **The main core.** The application, this library, and the C library. **All
  of SDL runs here** — every call, drawing included, and every one of them has
  finished the work its API promises by the time it returns.
- **The hardware output scale core.** It performs the job of display hardware
  this board does not have: a finished frame goes in, a scanout comes out. It
  runs no part of SDL and knows nothing about it. Pixels only.

**By default what crosses to that last core is a finished frame — pixels, and
where on the screen they go, rather than a list of drawing to do.** Drawing
is SDL's, and SDL runs on the main core. A build may raise the crossing count
and send short frames as a command list instead; see
[Choosing what crosses](#choosing-what-crosses-the-crossing-count).

The pixels are **SDL's framebuffer**: one buffer at the canvas size, owned by
this library, that every frame is drawn into on the main core. It is double
buffered, so the buffer handed to the presentation core is never the one the
next frame is being drawn into.

**No memory belonging to the application is ever handed to another core.**
An application texture is the application's: it may be destroyed, locked or
redrawn the moment a call returns, and a frame that is still being read
elsewhere cannot depend on it. So a frame is composed here first, always, and
the composed frame is what travels.

The cost is one canvas-sized copy per frame on the main core. It buys a rule
with no exceptions, which is the only kind that survives contact with an
application nobody here wrote.

### Choosing what crosses: the crossing count

**How much of a frame travels to the presentation core as a list of drawing
commands, rather than as a finished picture, is a build-time choice.** One
number decides it:

```sh
make PRESENT_CMDS=0     # the default
make PRESENT_CMDS=8
```

- A frame whose draw list **fits within the count** crosses as a list, and
  the presentation core composes it command by command. Nothing is drawn on
  this side.
- A frame that **does not fit** is drawn into SDL's framebuffer here, and
  that framebuffer crosses as a finished picture.

**Zero — every frame a picture — is the default.** At zero nothing is held
back: the first draw call of a frame goes straight into SDL's framebuffer,
and every one after it does too. Raising the count moves composition to the
presentation core, frame by frame, up to the mailbox's capacity — every value
in between is a real setting, not a mode.

Both paths reach the same executor, which writes into the shadow buffer or
the staging frame according to what the firmware granted, and the page flip
or the copy to the screen is decided by the grant either way. **What crosses
does not know or care what was granted.**

#### The two numbers

```c
SDL2CIRCLE_RECORD_MAX_CMDS  16      /* src/sdl2circle.h — mailbox capacity */
SDL2CIRCLE_PRESENT_MAX_CMDS  n      /* PRESENT_CMDS — what may cross */
```

The frame mailbox is a fixed structure in coherent memory that both cores
read — one frame in flight, no allocation and no locks on the path a frame
takes. Each of its sixteen slots holds a source and destination rectangle, a
colour, a source pointer and pitch, and two blend flags, and the space is
reserved whether a frame uses it or not. That capacity is the hard ceiling on
the count, and the build refuses a count above it.

Drawing begins the moment a frame grows past the count, so a frame that is
going to be drawn here starts at that point rather than waiting for present —
the work is the same either way, but spread across the application's own draw
calls instead of occurring all at once at the end.

Nothing is ever dropped. A frame that outgrows the count has the commands held
so far replayed into SDL's framebuffer, and every later call is drawn directly
into it, so it renders correctly; it simply crosses as a picture.

The count is compiled into the archive when the library is built. It is not in
any installed header, so an application's own translation units neither see
it nor need to match it; and because every count shares one archive name,
changing it deletes the archive so the previous build cannot be quietly
reused.

### Why the split is shaped this way

The goal is native speed on modest hardware, and the way to get it is to
remove from the application every cost that is not the application's own
work.

So the split is not a way to make use of spare cores. It is a way to **give
the application core one job**: the program's own main loop, and as little
else as possible. Presentation and scaling go to the chosen presentation
core. Devices, files and the sound feed go to core 0. What is left on the
application core is the program itself, which is the only work that cannot be
moved.

The second requirement matters just as much. **The topology is chosen, not
assumed, and the same compiled binary must be able to run every role on a
single core.** A board with cores to spare and a board with none run the same
image; only the host kernel's choice differs. Today the single-core case is
the degenerate path — no ring, no mailbox, every call direct — and the roles
may later become tasks sharing one core on hardware that cannot spare
several. That is why every crossing in this library is a mailbox or a ring
that any core may consume, and why nothing in it is written against a
particular core number. The library does not decide where anything runs, and
it must never start assuming.

### What the host kernel has to do

In this order. It is ordinary Circle start-up until the split is armed; only
steps 3 and 4 are calls into this library.

1. **Initialise the platform first.** Bring up interrupts, the timer, the
   serial console, the SD card, and mount the filesystem — all on core 0, as
   normal. The other cores are about to start asking core 0 for things, so
   those services must already exist.

   **A `CScheduler` is needed to run the split, and the library supplies one
   if you have not.** The servo and the watchdog are Circle tasks, and a Circle
   task registers itself with the scheduler while it is being constructed —
   through `CScheduler::Get()`, which stops the machine rather than reporting
   an absence. So `SDL2Circle_SplitInit` asks `CScheduler::IsActive()` first
   and creates one where there is none.

   **A host that declares its own keeps it**, and nothing about it changes:
   the library only ever asks whether one exists. Declaring one as a kernel
   member is still the clearer thing to do if the host has any use for it of
   its own — cooperative tasks, `CScheduler::Yield` in an idle loop — because
   then the host controls where it sits in the member order. A host with no
   use for one need not have one at all.

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
   leftover value is zero the read goes to mapped low memory and the throw
   appears to work — which is why omitting this call works on one board and
   takes a data abort on the next, on an ordinary throw, looking exactly like
   a hardware fault. It is one call and it is not optional. On core 0 it also
   starts the C++ threading runtime, which is why that needs nothing of its
   own; see [C++ threading](#c-threading).
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

Details that are easy to get wrong:

- **A core that is given no role must be parked** in a wait loop. Returning
  from your dispatch function lets the core continue into whatever code
  follows it.
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
point it at these functions and nothing else is needed.

If it does not, and it simply calls `fopen`, `std::ifstream` or `opendir`
throughout its source, **redirect the C library underneath it** instead of
editing the application. The linker's `--wrap` option does this without
touching a single vendored file:

```
# in your kernel's Makefile
LDFLAGS += --wrap=_open --wrap=_close --wrap=_read --wrap=_write \
           --wrap=_lseek --wrap=_fstat --wrap=_unlink \
           --wrap=opendir --wrap=readdir --wrap=closedir
```

Then write one small file defining `__wrap__open`, `__wrap__read` and so on.
Each has the same form:

```c
long __wrap__read(int fd, void *buf, size_t len)
{
    if (on_core_0())                       // or the split is not active
        return __real__read(fd, buf, len); // the genuine implementation
    ...                                    // otherwise use the I/O service
}
```

What makes this work, and all of it matters:

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
  which `SDL2Circle_IOOpen` returns when you ask for it.

Descriptors 0, 1 and 2 are the console rather than files, so they have no
route through the I/O service. Send those to core 0 with
`SDL2Circle_CallOn0` and let the original implementation run there.

### C++ threading

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`,
`std::call_once`, `std::thread` and `thread_local` all work on whichever core
your application was placed on, and nothing has to be done to get that.

They need saying about because the ordinary answer does not apply here.
Circle's cooperative scheduler is documented as belonging to core 0 alone, and
circle-stdlib builds the C++ threading runtime on top of it — correctly, for
the core it was written for. This library is the one that runs applications
somewhere else, so it supplies that part of the runtime itself: the primitives
are built from processor atomics, and every wait in them yields to Circle's
scheduler on core 0 and spins on any other core.

The build fragment `sdl-app.mk`, which your kernel's Makefile already
includes, is what selects this implementation instead of circle-stdlib's.
Nothing in circle-stdlib is modified.

**Where a `std::thread` runs.** By default, on core 0, as a cooperative Circle
task — wherever it was created from. That is what a service thread wants: it
costs no core, and it may reach Circle. A creation issued from another core is
passed to core 0 rather than refused.

Being cooperative has a consequence worth knowing. Such a thread runs when
core 0 gives up control, which it does constantly, but a thread that computes
without ever waiting or sleeping holds core 0 — and core 0 is where every
device on the board is serviced.

**Giving a thread a core of its own.** A host kernel that has a spare core can
lend it:

```c
void CMyCores::Run(unsigned nCore)
{
    SDL2Circle_ArmCoreRuntime();

    switch (nCore)
    {
    case 1:  RunTheApplication();          break;
    case 2:  SDL2Circle_SplitPresentCore(); break;
    default: SDL2Circle_ThreadCoreOffer();  break;   // never returns
    }
}
```

The application then asks for the next thread it creates to go there:

```c
if (SDL2Circle_ThreadPinNext(3) == 0)
    std::thread worker(...);        // runs alone on core 3
```

`SDL2Circle_ThreadCoresFree` answers with the cores that are available right
now, as a bitmask. A core the application or the presentation worker is using
is never in it, and neither is one already running a thread: this library
tracks which cores it has spoken for, so a request for a busy core is refused
rather than granted on top of what is already there. A lent core runs one
thread at a time.

**The limits.** A wait occupies the core it waits on, off core 0, for the same
reason `SDL_Delay` does. A timed wait off core 0 polls the system counter
rather than sleeping to the deadline. `thread_local` destructors run when a
thread ends, so a `thread_local` belonging to the application core — which
never ends — is never destroyed.

`examples/cxxthreads` runs all of this on a second core and reports what it
found on the serial console.

## Logging from any core

The serial console is a device, so only the hardware core may write to it.
That would leave every other core unable to log anything — and the cores with
the most to report, the application core and the chosen presentation core,
are exactly the ones that could not.

So they do not write. **Each core formats its line into a ring of its own and
returns**, and the hardware core's servo drains every ring into the logger.
Nothing crosses but memory. A core that logs never touches the console and is
never delayed by one that is printing.

```c
#include <SDL2/SDL_circle.h>

SDL2Circle_Log("mygame", SDL2CIRCLE_LOG_NOTICE, "level %d loaded", n);
```

- **It never blocks, and it never hides a loss.** If a ring is full the line
  is dropped and counted. The drain says when a core STARTS dropping and when
  it STOPS, with the total — rather than a line per pass, which would spend
  the scarce console on describing its own scarcity. Waiting would put the
  calling core to sleep for the sake of a diagnostic, and overwriting would
  silently corrupt the record.
- **The console is far slower than any core, and dropping is the steady state
  for an application that talks a lot.** Measure the rate before relying on
  it: it is much lower than the baud rate alone suggests, and low enough that
  a game logging once per data file while it scans its content can outrun it
  without appearing chatty. If the lines matter, raise the baud rate or log
  less — the ring cannot make the wire wider.
- **The servo's drain is bounded, and that bound is load-bearing.** It prints
  for a couple of milliseconds and returns, leaving the rest of the servo
  loop and the scheduler to run, and resumes at the next core so no ring is
  starved by a noisier one.

  Both halves of that matter. A drain whose only exit is an empty ring, in
  front of a producer that never waits, does not terminate at all once an
  application out-produces the console: the ring stays permanently
  non-empty, `head` never meets `tail`, and core 0 stops pumping USB,
  feeding audio and yielding. And a drain that resumed on the core that used
  up the budget would hand it straight back to the ring that just took it,
  leaving every core above it permanently silent — which on the wire is
  indistinguishable from a core that has stopped.

  A budget in TIME rather than in lines is what makes this hold: the cost of
  a line depends on the console, and a line count that is safe on one is not
  on another.
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
performance reports below.

## Performance reports

Call `SDL2Circle_SetPerfInterval(10)` and the library prints, every 10
seconds, one line per core that has run instrumented code. It is silent
until a host asks for it, and how a host decides to ask — a boot switch, a
build option, never — is for the host to decide, not the library:

```
sdl2perf: 60.0 fps c2: awake 41.2% (9885M of 24005M): app 0.0% render 50.5% dma 6.1% vsync 4.1% wait 0.0% serve 0.0% audio 0.1% input 0.0% yield 0.0%
```

The frame rate counts presented frames.

**`awake` is how busy the core was; the percentages after it divide only
that awake time.** The two are separate on purpose, because a processor's
cycle counter stops while the core is asleep. A core parked for most of
every frame and a core running continuously can print identical percentages —
what distinguishes them is `awake`, which is the counted cycles measured
against the wall clock, and the wall clock never stops. Read the line as
"this core was awake 41% of the time, and here is what it did while it was".

The categories: `render` is present-path compute; `serve` is the hardware
core doing another core's work, the call mailbox and the log drain; `audio`
and `input` are the pumps; `yield` is time given to other scheduler tasks;
and `app` is whatever is left, which is the application itself on its own
core.

Blocking is reported in parts, because each has different fixes. `dma` is
waiting for the transfer into the framebuffer to finish. `vsync` is waiting
for the raster to reach the vertical blanking interval. `wait` is one core
waiting on another across the frame mailbox — a wait on software rather
than on the display. Each is kept separate from
`render` on purpose: at a locked frame rate the blocking absorbs every spare
cycle, and combined with `render` it would make the present path look
saturated when it is mostly idle.

The `awake` figure assumes the processor clock reported at the first report
holds for the run. A board that is thermally throttling is changing that
clock during the measurement, so treat `awake` as approximate there.

The categories do not overlap. Sections nest — a wait happens inside the
present that is waiting — and each one is charged only for the cycles its
own body used, with its children's time attributed to them. So the percentages
partition the core's cycles and `app` is genuinely what is left.

A single-core build reports core 0 alone, which is the whole machine
there. Under the core split each active core reports its own line, which
is how a question like "how busy is the presentation core, could it
afford filtering" gets a measured answer.

Unarmed, the instrument costs one branch per section. Arming it is
`SDL2Circle_SetPerfInterval(seconds)` in `SDL2/SDL_circle.h`, and that is
the only way to enable it — the library reads nothing from boot
configuration for this.

## Design

- **The application owns the main loop; this library runs from it.**
  Everything the library does per frame is driven by `SDL_PumpEvents` (called
  by `SDL_PollEvent`) and `SDL_RenderPresent`. An application that polls
  events and presents frames keeps the whole machine alive; one that stops
  doing either stalls a cooperatively scheduled board, which is what the
  watchdog exists to report. Audio callbacks never run in interrupt context.
- **The CPU clock and the case fan belong to this library.** Circle's
  `CCPUThrottle` is both of those in one class: it sets the clock rate, and
  it decides what happens when the processor passes `socmaxtemp`. With no fan
  pin named, it reduces the clock to idle until the chip cools. Where
  `cmdline.txt` names one with `gpiofanpin=`, it leaves the clock alone and
  switches the fan on instead. Circle creates neither for itself — it requires
  that a system
  holds exactly one such object and that something calls it regularly, or
  none of that management ever happens. A host kernel has no per-frame loop
  to call from and this library already runs one, so **the library owns the
  object** and drives it from whichever per-frame service point is active:
  the servo on the hardware core under the split, `SDL_PumpEvents` otherwise.
  There is one update policy and it belongs with the owner, because Circle's
  `Update()` already does its work at most once every four seconds and costs
  a clock read the rest of the time, and a second interval at a call site
  would only conflict with that one.
- **A host kernel must not create a `CCPUThrottle` of its own.** Circle
  allows exactly one, and constructing a second stops the machine. Nor can
  this library compensate for a host that does: Circle offers no safe way to
  ask whether one exists, because `CCPUThrottle::Get()` stops the machine
  rather than reporting an absence. So the library keeps its own record of
  what it made, and a host that also makes one will halt the board.
- **Hardware management starts in `SDL_Init`, unless you ask for it
  sooner.** The clock is taken to maximum there, because Circle boots the
  board at its idle rate. A kernel that brings up I2C, SPI or the mini UART
  in its own `Initialize()` needs the clock settled *before* those: raising
  the CPU clock also moves the core clock, and those peripherals take their
  transfer speed and their baud rate from it. Such a kernel declares a
  `CSDL2CircleHardware` member (`SDL2/SDL_circle.h`) and gets hardware
  management while the kernel object is being constructed, before anything in
  `Initialize()` runs. A kernel that declares nothing need do nothing, and is
  served at `SDL_Init`.
- **Self-contained initialisation.** This library initialises everything it
  needs (USB host controller, framebuffer, sound) inside `SDL_Init`.
  Host-kernel contract: initialize `CInterruptSystem` and `CTimer` before
  `SDL_Init`. To run split, the host kernel also starts the secondary cores
  and hands one to `SDL2Circle_SplitPresentCore` — this library marshals
  calls; it does not own the cores.
- **No `std::thread`, anywhere in the library.** Every concurrent thing the
  library runs is a Circle `CTask` on core 0 or a core the host gave it, and
  everything between cores is a lock-free ring or a mailbox. The library
  never starts a thread of its own, so nothing it does depends on the C++
  threading runtime being live. An application is free to use `std::thread`
  — the per-frame pump yields the scheduler so cooperative threads make
  progress — but that is the application's choice and not something the
  library needs.
- **The scheduler is the one Circle object the split cannot do without,
  and this library will provide it.** A Circle task requests the scheduler
  while it is being constructed, through a call that stops the machine when
  there is nothing to return, so the split's servo and watchdog cannot exist
  without one. `CScheduler::IsActive()` is a safe question — unlike
  `CCPUThrottle`, which cannot be asked at all — so the library asks it,
  makes a scheduler only where the host has not, and leaves a host's own
  scheduler entirely alone.
- **Honest headers.** `include/SDL2/` is the official SDL2 2.32.4 header
  set (zlib license, see `SDL2-LICENSE.txt`) with one substitution: an
  `SDL_config.h` for AArch64/newlib/Circle. Your application compiles against
  the genuine SDL2 API; unimplemented entry points fail at link time instead
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
make deps       # builds every Circle world, then every archive
```

This library **supplies its own runtime world** — the configured
`circle-stdlib` build it compiles and links against. `circle-stdlib` is the
Circle framework plus newlib and libc++, and it is a nested submodule here,
not something you fetch and configure alongside.

**There is one world and one archive per board**, because each is compiled
for its own processor and its own `RASPPI` value, and an object built for one
board is not usable on another:

| Board | World | Archive |
|---|---|---|
| Pi 3 | `circle-stdlib-rpi3` | `libSDL2-rpi3.a` |
| Pi 4 | `circle-stdlib-rpi4` | `libSDL2-rpi4.a` |
| Pi 5 | `circle-stdlib-rpi5` | `libSDL2-rpi5.a` |

`make deps` does all of them. For each board it fetches the world's sources —
including libc++ from an immutable LLVM tag, because Codeberg regenerates its
archives and downloading the tarball fails its hash check on a clean build —
then configures that world (`-r <board> -p aarch64-none-elf- --libcxx-repo
--kernel-max-size 256 -o ARM_ALLOW_MULTI_CORE`) and builds it, and finally
builds this library against each. The first build is long: newlib and libc++
are compiled from source, once per board.

Afterwards, a plain `make` rebuilds one board's archive — `BOARD` selects
which, and defaults to `rpi4`:

```sh
make                  # libSDL2-rpi4.a
make BOARD=rpi5       # libSDL2-rpi5.a
make all-boards       # every board
```

### Choosing single-core or multicore

**You choose when you configure the world, and the choice is fixed when you
build.** Both are supported and the application's source is the same either
way.

- **A single-core world** — configured without `ARM_ALLOW_MULTI_CORE` — builds
  this library with the core split compiled out. Every call runs directly, on
  the one core, through the same call sites. This is the build for single-core
  hardware and for older boards, and it is the library's original behaviour.
- **A multicore world** — configured with `ARM_ALLOW_MULTI_CORE`, which is what
  `make deps` does — builds the split as well. Building it forces nothing on:
  the split stays inert until a host kernel calls `SDL2Circle_SplitInit`, so
  one image can still run everything on core 0.

The API is identical. `SDL2Circle_SplitInit` exists in both builds; in a
single-core one it reports that there is no multicore world to split into and
changes nothing, and `SDL2Circle_SplitActive` continues to answer no, which is
the answer every call site already handles.

Building through Circle's `Rules.mk` — as the examples do — you get
`ARM_ALLOW_MULTI_CORE` from the world itself, whichever way it was
configured, and there is nothing to think about. **If you compile any
translation unit outside `Rules.mk`** — a foreign build system with its own
flag list — that flag must match the world the object will link against.
Circle's headers change shape on it (spinlocks, atomics, memory layout), so
an object compiled without it disagrees with the library it links against,
and nothing tells you: it builds, it links, and it is wrong at runtime.

A world elsewhere on disk works with
`make CIRCLESTDLIBHOME=/path/to/circle-stdlib`.

### The world must set `USE_PHYSICAL_COUNTER`

This library requires it, and `make deps` configures a world that has it.

It decides what `CTimer::GetClockTicks64` compiles to. With the option, it is
`mrs CNTPCT_EL0` — a CPU system register private to the core that reads it,
needing no lock, no device and no other core. Without it, the same call reads
the system timer's memory-mapped registers, which is a device, and a device
belongs to core 0.

That matters because the timing this library does is not on core 0 and cannot
be: an application's frame pacing, every timed wait in the C++ threading
runtime, the presentation core's own accounting. Those read the counter
constantly, from cores that must never touch a device. With the option they
are core-private register reads; without it every one of them is a breach of
the rule that keeps this design standing up, and the breach is silent — it
builds, it links, and it is wrong on hardware.

So it is not a tuning choice. A world without it does not satisfy this
library's requirements, whatever else it is configured with.

### Choosing the crossing count

`make PRESENT_CMDS=n` (0 by default) sets how much of a frame may travel to
the presentation core as a list of drawing commands rather than as a finished
picture — see
[Choosing what crosses](#choosing-what-crosses-the-crossing-count). The value
is compiled into the archive, objects are kept in per-count trees so builds
never mix, and changing it deletes the archive rather than risk returning
the previous count's build under the same name.

Applications link by including `sdl-app.mk` after Circle's `Rules.mk`
(see any Makefile under `examples/`): it links with `sdl-app.ld` — required
with binutils 2.44+, whose linker refuses non-adjacent TLS sections with
the default script ordering (libc++'s threading carries TLS) — and adds the
Circle sound library the audio backend needs. `sdl-app.ld` is derived from
Circle's `circle.ld` and remains GPLv3 (see its header); everything else
here is zlib.

### Catching a stub this library has since replaced

An application that defines its own empty versions of SDL calls this library
does not implement yet should link the archive in full while developing:

```make
LIBS = --whole-archive $(SHIM)/libSDL2-$(BOARD).a --no-whole-archive \
	$(CIRCLE_STDLIB_LIBS)
```

An object file linked directly into the kernel takes precedence over an
archive member defining the same symbol, and the linker reports nothing when
it does. So once this library implements a call an application had stubbed,
the application keeps calling its own empty version: the real one is never
linked in, and no warning is produced. The symptom is that the library
appears not to work.

Linking the archive in full makes the same situation a duplicate-symbol
error, naming both definitions. The correct fix is then to delete the stub.

`--no-whole-archive` ends the effect immediately after this archive, so the
C library, libc++ and Circle are linked as before. An application that
already uses most of this library gains nothing in size from the change.

This is a setting for development rather than for a shipped build. An
application that deliberately uses a small part of the library will carry
the rest of it. Nothing here requires the setting, and no example sets it.

**It is also the only test that proves an application carries no SDL of its
own.** Reading through an application's source for leftover SDL functions
proves nothing — the one that matters is the one nobody thought to look at.
A whole-archive link decides it mechanically: every archive member is pulled
in, so any function the application still defines for itself collides with
this library's and is named in the error.

So a whole-archive link that produces **no duplicate symbols** is positive
proof, rather than an absence of evidence. It is worth running once after
removing an application's private SDL, and it is the check to apply before
declaring that removal finished.

The library holds itself to the same standard: every SDL, `IMG_` and `Mix_`
symbol the archive references, the archive defines. A symbol that is
declared in a header and defined nowhere is invisible to a selective link
and fails only under whole-archive — which would make this check
unsatisfiable for everyone. The sweep that shows it:

```sh
nm --defined-only libSDL2-<board>.a | awk '{print $3}'         | sort -u > defined
nm -u             libSDL2-<board>.a | awk '$1=="U"{print $2}'  | sort -u > undefined
comm -23 undefined defined | grep -E '^(SDL_|IMG_|Mix_)'
```

Run it against a full `make rebuild`, never an incremental build.

## Examples

Each is a complete bootable kernel exercising one subsystem. They are the
library's worked examples and its test harness at the same time — useful as
templates, and the way a change is proven before it ships:

- `examples/gradient` — animated full-screen gradient (video path)
- `examples/keyecho` — scancode display, modifier lights, held-key grid (input)
- `examples/mouseview` — the pointer on screen with the button lights, the
  wheel total and the frame's own relative reading beside it, and a bar per
  event type so a click that produced no event is visible. It also hands its
  serial port to the library's input-injection channel, so the pointer can be
  driven from a terminal — `mouse to 100 100`, `mouse tap left` — when there
  is nobody at the desk. It carries a patchable-defaults block at image
  offset 0x800, so a network loader can stamp a switch into the image before
  it boots, and the build refuses to leave behind an image that lost the
  block (mouse)
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
- `examples/videocycle` — the whole video and audio subsystem destroyed and
  recreated in a loop, at alternating source geometry, while a presentation
  core keeps running: what a settings menu does on every change, with nobody
  at the keyboard (core split, present-path lifetime)
- `examples/dispinfo` — no SDL at all: a serial-only probe that logs what the
  firmware reports about the attached display, and what Circle's framebuffer
  returns for a series of allocation requests. It is where the raw numbers
  behind the presentation geometry come from
- `examples/cxxthreads` — the C++ standard library's threading, run on core 1
  where an application runs and where none of it used to work: recursive
  mutexes, a condition variable woken by a thread the application core
  created, per-thread `thread_local` storage and its destructors, a timed wait
  that runs out, and `call_once`. It reports each result by name on the serial
  console, so a board with no display still says what happened (C++ runtime)

## License

zlib, matching SDL itself — see `LICENSE`.

One file is an exception. `sdl-app.ld`, the linker script an application
links with, is derived from Circle's `circle.ld` and remains GPLv3; its own
header says so.
