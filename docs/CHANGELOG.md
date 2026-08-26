# Changelog

Versions here mark the points at which this library was proven on real
hardware by an application built on it. There is no release process and no
build artifact. The library is consumed as source, at a commit.

**Consumers must act** marks a change to what a host kernel has to do. An
existing kernel will not build or run until it is followed.

## vPoC3

### The usable desktop area is answered

`SDL_GetDisplayUsableBounds` was declared in `SDL_video.h` and implemented
nowhere, so a program that called it did not fail at run time - it failed to
link, with an undefined symbol naming neither the library nor what to do
about it. Any program written for a desktop may call it: it is the ordinary
way to ask how much room a non-fullscreen window has.

It now answers the display's own bounds. The usable area is the display minus
whatever the system reserves - a menu bar, a dock, a taskbar - and there is no
window manager here, so nothing reserves any part of the panel. That is the
same answer SDL gives on a desktop with nothing docked, rather than a refusal
every caller would have to work around.

`rect` may be NULL, which SDL documents for this call and not for
`SDL_GetDisplayBounds`.

### A refused framebuffer says which request was refused

The allocation used to fail in silence. `acquire_fb_on0` deleted the object
and returned, and every caller above it reported "no display yet" in its own
words - none of which is the reason, and one of which is
`SDL_CreateWindow` failing with "the display size cannot be determined".
A board with no picture and nothing on the wire is the same board whether the
firmware declined the grant, the panel is asleep, or the cable is out.

Three lines now say which:

```
sdl2video: framebuffer granted: asked 0x0, got 1920x1080
sdl2video: the firmware refused two 0x0 32bpp screens; asking for one
sdl2video: the firmware refused a 0x0 32bpp framebuffer (0x0 asks for the panel's own mode)
```

`read_physical_display` says when the firmware declines to report the display
at all, for the same reason: it is the other half of what an absent scanout
means.

**The refusal is retried with a single buffer.** Double buffering is what
makes a present a page flip, so it is worth asking for, but a firmware that
will not allocate two screens refuses the whole allocation rather than
granting one. Everything downstream already reads the grant rather than the
request - the present path counts its rows from `GetSize()` - so one screen
needs nothing else told to it, and the frame is copied into the granted
surface instead of panned to.

Nothing a consumer calls changed.

### The desktop stops shrinking under the application

`SDL_GetDesktopDisplayMode` and `SDL_GetDisplayBounds` answered with the
canvas. Since the canvas started following the application, that made the
desktop follow it too, so a program that set a small mode was afterwards told
its screen was that small and could never ask for anything larger again.

SDL keeps the two apart on every platform. The desktop is the display an
application was given, and a game going fullscreen at 640x400 does not shrink
the monitor. The current mode is the mode in effect, and that does move.

The size `settle_canvas` decides, from the `--rapi-vfb` switch, from
`SDL2Circle_DeclareVirtualDevice`, from the first window or from the panel, is
now recorded once as the vFB and never written again. The desktop and the
display bounds answer with it, and `SDL_GetCurrentDisplayMode` answers with
the canvas as before. **Neither is the panel**, so an application still never
learns the real output resolution, the canvas stays the size the application
actually draws, and the presentation core still does all the upscaling.

ScummVM found it. Leaving Myst, whose canvas is 544x332, the launcher asked
for its own 640x400, had the request clipped to a screen that had shrunk
underneath it, and came back at Myst's resolution. It now returns to 640x400.

### The display answers are metered

`--rapi-debug-uart` already logged what an application asks the display to
become. It now also logs what the application is told, which is what it sizes
itself from: `SDL_GetDesktopDisplayMode`, `SDL_GetCurrentDisplayMode`,
`SDL_GetDisplayBounds`, `SDL_GetNumDisplayModes`, `SDL_GetDisplayMode`,
`SDL_GetClosestDisplayMode`, `SDL_GetWindowSize` and
`SDL_GetRendererOutputSize`. A query is asked many times a frame, so each site
remembers its last answer and writes only when the answer changes, which is
the rule the present path's geometry line already follows.

`SDL_SetWindowMouseRect` and `SDL_SetWindowGrab` are metered with them.
Neither is a query. The first is accepted and not acted on, because the
pointer is already confined to the one screen, and the line is what makes
visible that a program believes the pointer is confined to something smaller
than the canvas.

Nothing an application is told changed for this. It is logging only.

### The display size follows the application

An application can change its display size while it runs, as often as it
likes, and destroy and rebuild its window around the change. Four calls move
it and all four take one path: `SDL_SetWindowSize`,
`SDL_SetWindowDisplayMode`, `SDL_SetWindowFullscreen` for
`SDL_WINDOW_FULLSCREEN`, and a later `SDL_CreateWindow`. The canvas is
reallocated, the placement on the panel is worked out again, the letterbox the
previous placement left behind is cleared, and an
`SDL_WINDOWEVENT_SIZE_CHANGED` follows.

**There is a list to choose from.** `SDL_GetNumDisplayModes` and
`SDL_GetDisplayMode` enumerate the standard sizes that fit inside the panel,
largest first, from 1920x1080 down to 160x120. `SDL_GetClosestDisplayMode`
answers a request that fits with itself, because the library would allocate
exactly that canvas if the application went on to set it. The library used to
report one mode, the canvas, so every consumer had to name a size in its own
kernel just to put that size in the list.

**A surface handed out by `SDL_GetWindowSurface` survives the change.** Its
pixels are replaced and its dimensions rewritten in place, so a program that
keeps that pointer in a global, and several do, still holds a valid surface
afterwards. Freeing and replacing the object instead turned a resize into a
fault elsewhere in the program, in code with nothing to do with the display.

The framebuffer grant, the present path, its buffers and its DMA channel
belong to the grant rather than to the window, so a rebuild allocates none of
them again and strands nothing.

This supersedes **Applications choose their own display size**, below, on two
points. A size is no longer required: an application that states none gets its
window's own size, and one that never states any gets the physical panel, read
from the firmware. And the size an application states is no longer the only
one it can have.

### An undeclared base path answers with the working directory

`SDL_GetBasePath` used to answer `/` when nothing declared a path, and
`SDL_GetPrefPath` composes `<base><app>/`, so every program that wrote a
setting or a saved game made a directory of its own at the root of the card.

It now answers with the directory the program is running in, which a host
kernel has already entered before starting the application. That is where the
card keeps that program's files, and it is the one answer this library can
establish for itself. `SDL2Circle_DeclareBasePath` is unchanged and still
wins. It remains the only way to name somewhere other than where the program
was started from.

### The log reads as a log

Every line the library writes is a source, a state and its values, and nothing
else. There is no prose on the wire: no second person, no suggested remedy, no
multi-line explanation of what somebody should check next. The console is a
fixed, slow width shared by every core, so a line costs time that the next
line then waits for, and a line that explains itself in three sentences spends
that budget on the explanation.

Nothing was dropped and nothing new is said. The same events are reported, at
the same severities, at the same moments. What the lines used to explain now
lives beside the call that makes them, or in the document that owns the
subject. `docs/INPUT.md` gained the section on a board with no USB host
controller, including why `--rapi-debug-uart` on such a board stops rather
than runs, and `docs/THREADING.md` carries what a starved cooperative thread
means and what ends it.

**A consumer that greps the serial log for a phrase must update the phrase.**
Nothing else changes for a host kernel.

### The library's debug lines wait to be asked for

`--rapi-debug-uart` now turns on the library's own debug output as well as
serial key injection, and nothing at debug severity prints without it. The
event pump's liveness beacon is the line this is really about: it says the
application's main loop is still running, once every ten seconds, for as long
as the board is up. Without the switch a working board is silent between one
event and the next, which is worth knowing before a quiet console is read as a
library that has stopped.

What reports a main loop that has genuinely stopped is not on the switch and
never was. The pump arms a kernel timer on every beat, and thirty seconds of
silence lets it fire and dump the scheduler's task list.

### The locks reach the keyboard, and the keypad gets its other half

See [INPUT.md](INPUT.md) for all of it.

**The lamps on the keyboard light.** A lock change sends the keyboard the
report that lights its caps, num and scroll lamps. The report is submitted and
not waited for, because Circle's own `SetLEDs` holds core 0 until the transfer
completes, or for three seconds if the keyboard has gone. Not on the Pi 3,
where Circle drives USB through the DWHCI controller, which refuses an
asynchronous control transfer. The lamps stay dark there and nothing an
application reads differs.

**The keypad has two faces.** Num lock chooses between them, as on a real
keyboard. `7` `8` `9` are digits with the lock on and Home, Up and Page Up
with it off, and so on down the keypad, with `5` meaning nothing and the four
operators unaffected. The scancode never moves, keypad 8 is
`SDL_SCANCODE_KP_8` either way, and the meaning is carried in the keycode, so
an application that handles the arrow keys handles the keypad's arrows without
knowing the keypad exists. Shift inverts the lock for one keystroke, and a key
that is navigating produces no `SDL_TEXTINPUT`. Desktop SDL reports
`SDLK_KP_8` whichever way the lock is set, so this is a deliberate difference:
Circle's layout tables return nothing for these keys while the lock is off,
and supplying the meaning is this library's half of that arrangement.

**`SDL_SetModState` reaches the layout.** Setting `KMOD_CAPS` turns caps lock
on for real: the layout's own lock, the case of the letters that follow, and
the lamp. Upstream SDL cannot do this because the layout there belongs to the
host operating system. Both sides are ours, so they are kept in step.

### Caps lock and num lock reach the application

`SDL_GetModState` and the `keysym.mod` of every key event now carry
`KMOD_CAPS`, `KMOD_NUM` and `KMOD_SCROLL`, which the modifier word never used
to hold, so an application asking whether caps lock was on always got no.

A lock is a state rather than a held key. It changes on the press and stands
until the next press, so both the `SDL_KEYDOWN` and the `SDL_KEYUP` of a lock
keystroke carry the new state, and modifiers held with a lock key make no
difference to it. The state read is the keyboard layout's own, the one that
decides the case of the letters `SDL_TEXTINPUT` carries, so the modifier bit
and the typed text cannot disagree. See [Lock keys](INPUT.md#lock-keys).
`examples/keyecho` shows the locks as their own row of lights.

### A held key repeats

A USB keyboard reports which keys are down and nothing more, so a held key
used to produce one character and then silence. The library now generates the
repeats: an `SDL_KEYDOWN` with `key.repeat` set, carrying the same scancode,
keycode and modifiers as the press it repeats, followed by the same
`SDL_TEXTINPUT` that press produced.

One key repeats at a time, the one pressed most recently, after half a second
and then ten times a second. Pressing a second key while the first is held
moves the repeat to it, and releasing the second does not bring the first
back. Modifiers and lock keys never repeat and never take the repeat from a
key that has it, so holding a letter and then pressing shift goes on repeating
the letter in upper case. The repeats are generated on core 0's input pump,
where the keyboard is already read, so no timer was added and nothing new runs
asynchronously. There is nothing to configure. See
[Key repeat](INPUT.md#key-repeat).

### A cross-core wait gives its core to the threads on it

Waiting for the presentation core, for a keypress on standard input, or for a
call marshalled to core 0 used to put the application core to sleep through
runnable work of its own. All three now hand the core to the next runnable
context first, and sleep only when there is nothing to hand it to. A core that
has not been asked to schedule its own threads is unaffected.

Each of those three waits writes a one-deep mailbox, and a wait that hands the
core on can be re-entered by the context that gets it, so all three now hold
that mailbox's lock across both the wait and the write. A second caller blocks
instead of overwriting a request in flight, at one uncontended
compare-and-swap per frame. The waits that only read a counter take no lock.
SDL's own rule is unchanged: use a renderer, its textures and its window from
one thread only.

### A thread that never gets a turn says so

Nothing here is preemptive, so a main loop that never waits, yields or sleeps
keeps its core and the threads on it never start, which from outside is a
board doing nothing with no fault reported anywhere. A core that has a
runnable thread it has not given a turn in five seconds now says so on the
log, once, naming the thread and whether it has ever run. The report is driven
from the application's own per-frame beat rather than from the scheduler,
because the fault is that the scheduler is never entered. It costs two loads
and a compare per frame, and nothing on a core that schedules no threads.

**Consumers should act** on this line if it appears. The loop needs a
`std::this_thread::yield()` where it polls.

### An application core can run its own threads

`SDL2Circle_ThreadsStayOnThisCore`, called on a core, makes every thread
created on that core afterwards a cooperative context on that core, for
`std::thread` and `SDL_CreateThread` alike. The library schedules it itself: a
stack out of the heap, a thread-local block, an identity of its own, no Circle
task and nothing posted to core 0. The default placement is core 0, and core 0
is the only core that services the SD card, the USB host and the serial port,
so a worker that computes for two seconds without yielding is two seconds of
unserviced devices. Threading semantics do not change, only which core a
thread runs on. See
[Keeping a thread on the core that made it](THREADING.md#keeping-a-thread-on-the-core-that-made-it).

A host kernel that does not make the call gets what it got before, with none
of the new code reached. It does now get one line on the log, once per core,
the first time a core off core 0 creates a thread, naming the call that
changes it. A consumer that does make the call should check three things:

- Its threads contend with the application rather than with core 0.
- Its threads cannot touch Circle directly, for the same reason the
  application core cannot. `SDL2Circle_CallOn0` and the I/O service are how
  they reach it.
- Each thread gets a fixed stack whose low end is checked on every switch, so
  an overrun is a line on the log naming the thread rather than silent
  corruption. `SDL_CreateThreadWithStackSize` chooses that size,
  `SDL_CreateThread` gets SDL's usual megabyte, and a `std::thread` gets the
  size it would have had on core 0.

`SDL2Circle_ThreadPinNext` is unchanged and still applies to `std::thread`
alone. A pin still wins over everything else.

### Standard input is not echoed by the console

`CConsole::SetOptions` is called with neither `CONSOLE_OPTION_ICANON` nor
`CONSOLE_OPTION_ECHO`. A read still hands back a character the instant its key
is pressed, but nothing draws it. The reason is backspace: a reader building
an edited line needs to draw a space over the old glyph and step the cursor
back, not the backspace byte itself, and Circle's echo draws every byte the
same way.

**Consumers must act** if anything reads standard input through this library
and expected free echo. It must now draw what it reads. Free Pascal's target
for this board already does.

### A typed line can be corrected before it is read

`Do_Read`, in `fpc/rtl/circlesdl2/sysfile.inc`, reads and edits a whole line
from standard input before it hands anything back. Backspace removes the
character behind the cursor, from the line and from the screen, and what comes
back once Enter is pressed is the corrected text. Backspace at the start of an
empty line does nothing, rather than erasing whatever the console printed
before the line began. It is built on `SDL2Circle_ReadStdin` and
`SDL2Circle_WriteBytes`, both of which this library already offered. Free
Pascal cannot tell a single-character Read from a ReadLn at this level, so
every read assembles a line and returns it whole, and Read(Char) takes the
first character of it.

### A texture can be drawn into

Render targets work. `SDL_CreateTexture` accepts `SDL_TEXTUREACCESS_TARGET`,
`SDL_SetRenderTarget` aims the renderer at such a texture, every drawing call
then lands in that texture, and passing null aims the renderer back at the
frame. The texture is a source for `SDL_RenderCopy` like any other, and
`SDL_GetRenderTarget`, `SDL_RenderTargetSupported` and the
`SDL_RENDERER_TARGETTEXTURE` flag in `SDL_GetRendererInfo` answer accordingly.
This is what a game does when it composes its whole picture at one fixed low
resolution and magnifies the finished image in one step, and such a game used
to stop at start-up here. Aiming at a texture that was not created as a target
is refused, as upstream SDL refuses it, and so is copying a target into
itself. See [Render targets](DISPLAY.md#render-targets).
`examples/rendertarget` is the bootable demonstration.

### `SDL_SetWindowHitTest` is accepted

The callback and its data are recorded and the call always succeeds. A hit
test tells a window manager which part of a window drags or resizes it. There
is no window manager here, one window filling one display, so the condition
the callback exists to answer never arises and it is never called. A NULL
callback means what it means upstream. There is no `SDL_GetWindowHitTest` to
accept alongside it, because upstream SDL2 has no such entry point either.

### A program's own output is not a log line

`SDL2Circle_WriteBytes` takes bytes and adds nothing to them, with no source,
no severity, no timestamp and no waiting for an end of line. `SDL2Circle_Log`
and `SDL2Circle_LogBytes` still label what they carry, and where output goes
is unchanged. See [Raw output](LOGGING.md#raw-output).

**Consumers will see this.** The C library's standard output and standard
error are now bound to that raw channel during `SDL2Circle_ArmCoreRuntime`. An
ordinary `printf` used to reach a descriptor nobody had bound, so the C
library answered "bad file descriptor" and the bytes were lost. Nothing in
circle-newlib is changed to do this. The library gives the C library's own
console glue a device of its own.

**Consumers must act** if the host kernel binds its own console with
`CGlueStdioInit`. The C library binds the three standard descriptors together
and stops the board inside an assertion if they are bound twice. Remove the
`CConsole` member and the `CGlueStdioInit` call from the kernel.

All three descriptors are now held for the life of the program. They used to
be free, and the C library hands out the lowest free descriptor, so the first
file such a program opened was given descriptor 0, which a language runtime
reads as its console. Standard input is bound but no character arrives on it,
because this board has no console input, so a C program that reads standard
input waits.

### Output goes to serial and to the screen, with nothing to arrange

Every board shows its output on the display as well as on the serial port,
until an application takes the display. Nothing has to be called and nothing
can be configured. It is one device, built during `SDL2Circle_ArmCoreRuntime`,
holding the serial device the kernel gave Circle's logger, the screen, and a
flag saying whether the screen is still the library's to draw on. Circle's
logger is pointed at that device once and never pointed anywhere else. When an
application initialises SDL video the flag is cleared, and that is the whole
hand-off. A board with no display is a machine with one destination instead of
two, said once at notice. See [LOGGING.md](LOGGING.md).

Kernels used to reach both places by building Circle's own screen device and
teeing it with the serial device. Circle's console cannot be made right on
some boards: its colour depth is a compile-time value, and its framebuffer
object reads the granted pitch back out of the firmware's reply but never the
granted depth, so Circle's terminal sizes every row of pixels from a depth
that may be wrong. A Pi 5 grants 32 bits per pixel whatever the request was,
and each character is then drawn at the wrong stride. This library already
reads those numbers back for its own drawing, and the console is now drawn
from the same readings, with no board test in it. A kernel that keeps its own
screen device keeps working.

`SDL2Circle_LogAttachScreen` is no longer something a consumer needs to call.
It remains for a host kernel with bring-up of its own worth watching on the
glass, such as mounting a card, which happens before the arming call. Calling
it twice, or after the library already has, does nothing and answers 0.

### The I/O service gained rename, directory removal, and the working directory

The service carried open, read, write, seek, truncate, unlink, directory
creation and directory listing. It now also carries rename, directory removal,
change of directory, and the query that reports the working directory. The C
library on this board had all four already and the filesystem is configured
for the relative-path calls the last two need, so the gap was in the
marshalling layer alone. Nothing already exported changed.

### The file-operation counter counts every operation

It was raised by open, read, write, stat and directory open, and not by
truncate, close, unlink, directory create, directory read or directory close.
That number appears in the watchdog's stall report, which therefore
under-reported how much file work an application was doing.

### Two documents corrected

`CORE-SPLIT.md` told an application with its own file layer to point that
layer at the service and said nothing else was needed. A host kernel must also
initialise the C library's standard streams, or the first file opened takes
descriptor 0.

`AUDIO.md` said timer callbacks run on core 0. They run in the caller's own
line of execution, on the caller's core, at the event pump and at `SDL_Delay`.

### Every core gets a 2 MB stack

Circle's default is 128 KB a core. This library configures its worlds at 2 MB,
8 MB of the board's memory across four cores, and the same for every
application that uses it. `make world CIRCLE_KERNEL_STACK_SIZE=<bytes>` still
sets it, and the value is fixed into the world at configure time. See
[Stack allocation](BUILDING.md#stack-allocation).

A stack that is too small does not report itself. Circle's four core stacks
sit next to each other with no guard page, so a core that runs off the bottom
of its own writes into the next one down, which under the split is core 0,
where the host kernel object lives as a local of `main()`. What follows is a
data abort in Circle code that did nothing wrong. An engine that `alloca`s its
per-frame working set is the case to watch: TyrQuake's renderer takes about
198 KB on a 64-bit target at the engine's own minimum limits, and its source
says it expects at least a megabyte.

### SDL has one framebuffer, and every frame is drawn into it

A frame is no longer sent to the presentation core as a pointer into the
application's own texture. Every frame is composed into SDL's framebuffer, a
buffer this library owns at the size the application was told the display is,
and that buffer is what crosses between the cores. The cost is one full-size
copy per frame on the application's core, and one further resampling pass for
the frame shape that used to be recognised. It is paid so that no buffer is
ever read by one core while another may be writing it.

Two faults go with it. The presentation core used to keep reading an
application texture after `SDL_RenderPresent` had returned, at which point the
application is entitled to destroy that texture or draw the next frame into
it. And `SDL_RenderReadPixels` used to read the display panel rather than
SDL's framebuffer, so where the picture is fitted and centred on a larger
panel it returned the top-left corner of the panel, part black margin and part
picture, scaled to the panel. It now reads SDL's framebuffer in the
coordinates the caller drew in, so a screenshot taken before the present is
the picture the application just drew.

### The C++ standard library's threading works on the application core

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`,
`std::call_once`, `std::thread` and `thread_local` are all usable from the
core this library puts your application on. They were not before. Circle's
cooperative scheduler is documented as core 0's alone and circle-stdlib builds
the C++ threading runtime on it, so this library, which chose to run
applications somewhere else, supplies that part of the runtime itself. The
primitives are built from processor atomics, and every wait in them yields to
Circle's scheduler on core 0 and spins anywhere else.

`std::recursive_mutex` was the visible failure, stopping the board when locked
and unlocked microseconds apart on the application core with nothing
contending. Behind it were faults that reported nothing at all: a contended
`std::mutex`, every condition variable wait, and every `thread_local` read
from the application core, which was answering with another thread's storage.

`std::thread` can now be created from any core. A thread runs on core 0 as a
cooperative task, which costs no core and may call Circle. A host kernel that
would rather have a thread on a core of its own can lend one with
`SDL2Circle_ThreadCoreOffer` and `SDL2Circle_ThreadPinNext` in
`SDL2/SDL_circle.h`, and this library will never place a thread on a core it
is already using for the application or for presentation.

Nothing has to be done to get this. `SDL2Circle_ArmCoreRuntime`, which every
host kernel already calls on core 0, now also starts the one task the
threading runtime needs. The limits are in
[THREADING.md](THREADING.md#the-limits), and `examples/cxxthreads` exercises
all of it on a second core.

### Applications choose their own display size

Superseded in part by **The display size follows the application**, above.

An application states the display size it wants and gets exactly that,
whatever screen is attached. Every SDL question about the display answers with
the stated size: the current, desktop and enumerated modes, the display
bounds, the window size and the renderer's output size. The library fits each
finished frame onto the real screen. This suits an application whose size is a
property of the program rather than a preference, such as an emulated machine
or a fixed layout, and it covers the Pi 5, which cannot be told what
resolution to run at. Only 32 bits per pixel is supported, and a request for
anything else is refused rather than rounded. So is a statement made after the
library has already been asked about the display. See
[Declaring the display](DISPLAY.md#declaring-the-display).

Five of the examples ask the firmware for the screen size and state that,
which is how you make the two match.

### Putting a frame on screen costs about half of a core

On a Pi 5, filling a 1920x1080 screen from a 398x224 picture at a steady 59.9
frames per second takes 41% of the core that does it. It took 76%. The scaler
no longer copies a finished output row to produce the next one: an output row
is several times wider than the source row it comes from, so the copy moved
more memory than recalculating it does, and it filled the cache with output
data while evicting the source row still being read. The saving grows with how
much the picture is enlarged, and applies to every application on every board.

### A picture shaped like the screen fills it exactly

Placement is calculated in exact integers, so a picture whose shape matches
the screen leaves nothing over. It used to fall one pixel short on each axis
and leave a thin black line down the right edge and along the bottom.

### The screen size is read from the firmware

The library asks the firmware what resolution it settled on, rather than
working it out from the framebuffer's pitch and size. It also no longer
requests 640x480 when `cmdline.txt` names no size, which used to impose a mode
on a card that had asked for nothing.

**Do not name a display mode on a Pi 5.** That board fixes its mode before any
kernel starts and will not change it. It accepts a `width=`/`height=` line,
reports it back as applied, and carries on sending its own mode to the screen,
so everything downstream then works from a size that is not real. A Pi 3 and a
Pi 4 apply the mode properly and report it correctly.

### The library creates a scheduler if the host has none

Running the core split requires a `CScheduler`, because the servo and watchdog
are Circle tasks and a task cannot be constructed without one. A host that did
not create one got a board that stopped during start-up with nothing said. The
library now creates one where there is none, and never replaces or
reconfigures a scheduler it did not make.

### A build setting chooses what crosses between the cores

`make PRESENT_CMDS=n` sets how many drawing commands may travel to the
presentation core as a list. A frame that does not fit within the count is
composed into SDL's own framebuffer instead, and that framebuffer crosses. The
default of zero composes every frame here and sends a picture, and which
framebuffer the firmware granted no longer influences the choice. Only the
default has been measured against a real workload. See
[Choosing the crossing count](BUILDING.md#choosing-the-crossing-count).

### A game that draws without a renderer lands in the same rectangle as one that does

SDL offers two ways to put a frame on screen: ask the window for a surface,
draw into it and say when to show it, or draw through a renderer. Only the
renderer was mapped onto the screen properly. The window-surface path built
its copy in canvas coordinates and then wrote it to the screen unmapped, so a
canvas smaller than the display appeared at its own size in the top-left
corner with the rest of the screen black, and it did that work on whichever
core called it rather than handing the frame to the presentation core.

A window surface is now copied into the double-buffered canvas surface, which
is what makes it safe to hand to another core, and crosses as one frame. The
`copy src ... -> canvas ... -> scanout ...` line that describes every other
present now describes this one too. EDuke32 draws this way, because the Build
engine's classic renderer has its own software rasteriser and wants nothing
but somewhere to put the result.

### The monotonic clock answers every clock a program can ask for

`clock_gettime` is now served by this library, alongside the wall clock it
already served. The monotonic family and the two CPU-time clocks all read the
free-running system counter, which nothing here adjusts, slews or suspends. A
clock outside that set is still refused, but the timespec is zeroed first, so
a caller that ignores the return reads a defined value instead of its own
stack.

The C library underneath answers `CLOCK_REALTIME` and `CLOCK_MONOTONIC` and
refuses every other clock, and its refusal returns without writing the
timespec it was given. Most callers do not check the return, because a
monotonic clock is not expected to fail, so they read whatever their own stack
held: asked twice from the same place the clock gives the same answer both
times and appears to have stopped, and a loop waiting for it to advance never
leaves. The header this library is built against defines
`CLOCK_MONOTONIC_RAW`, so portable code selects the refused clock in
preference to the working one. EDuke32's timer calibration does exactly that.
## vPoC2

### Joysticks and game controllers

SDL's joystick and game-controller calls are implemented. Circle publishes
every pad it binds, from the generic HID driver and the console-specific ones
alike, and the library turns that into SDL's two-level identity: a device
index that renumbers as devices come and go, and an instance ID that is never
reused. Devices that arrive and leave while the program is running are
reported as SDL events. See
[Joysticks and game controllers](INPUT.md#joysticks-and-game-controllers).

A joystick GUID is built byte for byte the way SDL2 builds one, including the
CRC-16 of the device name, so an unmodified `gamecontrollerdb.txt` matches.
Mapping lines tagged for Linux are loaded alongside any tagged for this
platform, because a Linux GUID has exactly the shape this builds and no
published database has heard of Circle. A device with no mapping line is
reported as not a game controller and remains a fully working joystick. Rumble
is offered where the device supports it, at off, weak and strong, which is
what Circle exposes; `SDL_Haptic` is deliberately left unimplemented rather
than turning a force-feedback effect into a rumble.

Attach, detach and report translation happen on core 0, where USB lives, in
the same pump that already services the keyboard. What an application asks for
afterwards is answered out of shared memory by whichever core asks, so reading
a device costs nothing on core 0. Rumble is the exception, being a USB
transfer, and is marshalled. `SDL_RWops` arrived with this, over memory and
over the library's own I/O service, because the mapping database is loaded
through one.

### The library owns the CPU clock and the case fan

**Consumers must act.** Remove any `CCPUThrottle` your kernel declares. This
library now creates it. Circle permits exactly one in a system and halts if a
second is created, and it offers no safe way to ask whether one already
exists, because `CCPUThrottle::Get()` halts rather than reporting an absence.
The library therefore owns the object and drives it from whichever per-frame
heartbeat is live, the servo when the core split is running and the event pump
otherwise.

The clock is taken to its maximum, stated rather than left to default, because
Circle starts the board at its idle rate. This happens inside `SDL_Init`. A
host kernel that configures I2C, SPI or the mini UART itself should declare a
`CSDL2CircleHardware` member instead, so the clock is settled while the kernel
is being constructed and before those peripherals take their speed from the
core clock. Where `cmdline.txt` names a fan pin with `gpiofanpin=`, a board
over `socmaxtemp=` switches its fan on and holds its clock rather than slowing
down.

### A video mode change no longer halts the machine

The buffers and DMA channel that carry finished frames to the screen are sized
by the framebuffer grant, which is made once at startup and never given back.
Rebuilding them for each new window leaked a DMA channel and two full-screen
buffers every time, until none were left and the sound device could not open
one; any application offering a video settings menu could reach this. They now
belong to the grant rather than to a window. The sound device is also closed
on the core that owns it, since destroying it returns its own DMA channel, an
interrupt registration and a queue. A test that restarts the whole video world
in a loop is what proved it.

### An example that identifies attached input devices

`examples/padview` is a bootable kernel that shows, for every attached device,
the SDL device index, the Circle device name, the instance ID, the joystick
GUID and the USB identifiers behind it, whether a mapping was found for it,
one live bar per axis, one lit cell per hat direction and one lit square per
button. Where the database recognised the device, the mapped controller axes
and buttons appear underneath the raw ones, and below that is a log of every
attach and detach with the time it happened. It answers "what is this device
and what are its button numbers" before you write them into an application's
configuration.

### The framebuffer log line distinguishes the request from the grant

Only the pitch and the size come back from the firmware. The width, height,
virtual size and depth are the values Circle was constructed with, echoed
unchanged by getters that never learn what the firmware decided. Printed
together and unlabelled they read as one measured geometry, which is how a Pi
5 came to report a 640x480 framebuffer beside a pitch describing a 1920-wide
surface. The two halves are labelled now.

## vPoC1 (23a36e5)

The first version carried all the way through on hardware, with an application
built on it running on a Pi 3, a Pi 4 and a Pi 5.

### The part of SDL2 that works

A fullscreen window with a software `SDL_Renderer` over Circle's
`CBcmFrameBuffer`, drawing through streaming ARGB8888 textures. That is the
only texture format and 32 bits is the only depth; any other request fails
with an error rather than quietly falling back. `SDL_RenderCopy` honours its
source and destination rectangles, with a same-size copy staying a
byte-for-byte unscaled blit and anything else resampling, and with
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
started, and a failure to start sets an SDL error instead of claiming success.
This was verified by recording a 1 kHz tone from an HDMI capture and measuring
it.

Input is USB HID keyboard reports as SDL key events, with
`SDL_GetKeyboardState` and modifiers. Timers are microsecond-resolution over
Circle's `CTimer`, including `SDL_GetTicks64` and the performance counter.

### The picture is scaled once, at the output

Three geometries are named and kept apart. The **scanout** is what the display
hardware puts on the wire. The **canvas** is the size the operator asks for
with `width=` and `height=` in `cmdline.txt`, defaulting to the scanout. The
application's own render resolution is always in canvas coordinates. Both
steps are composed into a single nearest-neighbour resampling pass when the
frame is presented, with index tables built once per geometry and an
integer-ratio path that replicates pixels without a table.

The default placement scales the picture up as far as it fits, centres it and
leaves the remainder black. `canvas=stretch` in `cmdline.txt` fills the
scanout instead and abandons the aspect ratio. The scanout is derived from
what the framebuffer allocation actually granted, never from the size the
firmware echoes back, because a Pi 5 acknowledges a mode request without
honouring it.

A frame is composed in ordinary cached memory and then moved to the screen in
one block, never scaled directly onto the uncached framebuffer. Measured on a
Pi 4 at 1280x720: 26.1 ms to write the screen directly, against 1.4 ms into
memory plus 6.0 ms to move it out. It also makes a frame atomic on the wire,
because the screen is touched only by that one move. The move uses the DMA
engine when a channel is free and falls back to a logged CPU copy when none
is. Where the firmware's grant cannot hold two screen heights, presentation
falls back to a shadow buffer rather than writing past the grant: frames
render into a tightly-pitched shadow and the flip is one row-wise copy behind
a vsync wait, so a partly-drawn frame is never scanned out.

### Running the work on more than one core

The library does not start a core and does not choose one. The host kernel
owns that decision entirely. See [CORE-SPLIT.md](CORE-SPLIT.md).

`SDL2Circle_SplitInit()` runs once on core 0 and arms two Circle tasks: a
servo that drains the call mailbox, runs the file I/O service, pumps USB input
into the event ring and feeds the audio ring to the sound device, and a
watchdog that reports an application core that has stalled.
`SDL2Circle_SplitPresentCore()` is what a host runs on the core it elects for
presentation; it turns a finished frame into a scanout, runs no SDL and holds
no application state. `SDL2Circle_CallOn0()` runs the host's own function on
core 0 through the same mailbox, for hardware the library does not own. Every
path between cores is a lock-free single-producer ring or a one-deep mailbox,
never a Circle scheduler primitive, which would be illegal off core 0.

A draw sequence shaped "clear, then one opaque blit" is recognised while it is
being recorded and reduces to the texture itself crossing the mailbox rather
than a list of commands. Anything else is rasterised into a canvas-sized
surface on the application core first, and that surface crosses instead. This
is something the library notices, never something an application has to
arrange. The application core is released as soon as the presentation core has
read everything it needs from a frame, not after the output waits, because the
DMA transfer and the vsync wait touch only the presentation core's own buffer.
Before that, the split ran slower than a single core.

Audio inverts off core 0. The application's callback fills a ring, and core
0's servo feeds the hardware from it at its own cadence. `SDL_Delay` off
core 0 spins to an exact microsecond deadline, which is deterministic but
burns the core; on core 0 it sleeps cooperatively.

Files are the one thing that cannot be marshalled invisibly, because the C
library drives the SD card directly. An application must reach files through
the `SDL2Circle_IO*` service from any core other than 0. In a single-core
build every one of these degrades to a direct call, so call sites do not
change.

### Logging from any core

Any core can log. Each formats its line into a ring of its own and returns,
and core 0's servo drains every ring to the serial console. A full ring drops
the line and counts it, rather than blocking the core that logged or
corrupting the record.

### Performance reports

`SDL2Circle_SetPerfInterval(N)` prints one serial line every N seconds: frames
presented per second, and a cycle-count split per core across render, audio,
input and waiting, with the remainder attributed to the application's own
work. See [PERFORMANCE.md](PERFORMANCE.md).

Waiting is separated from working, and blocking on another core is counted
apart from waiting on DMA or on the raster, because at a locked frame rate the
blocking waits absorb all the spare time and previously made a fully idle core
look saturated. Each line leads with how much of the wall clock the core was
actually awake, measured locally, because a parked core's cycle counter stops.
The counter backend is AArch64 only; elsewhere the instrument compiles to an
inert form and says so once when armed.

### What the host kernel must do

Finish Circle's own world, meaning interrupts, timer, serial, SD card and
filesystem, on core 0 before starting any other core. Start the secondary
cores yourself with `CMultiCoreSupport`. Call `SDL2Circle_ArmCoreRuntime()` as
the first statement on every core including core 0: a core that has just
started has no thread pointer, C++ exception state is reached through it, and
without this the first throw reads whatever the firmware left there, which
passes on one board and takes a data abort on the next. Call
`SDL2Circle_SplitInit()` once on core 0 before the application starts. Keep
core 0 yielding for as long as the application runs, because the servo only
runs when something yields. Park any core you give no role.

### Building

A separate static library per board, `libSDL2-rpi3.a`, `libSDL2-rpi4.a` and
`libSDL2-rpi5.a`, because Circle bakes the board in at configure time, each
built against its own circle-stdlib world. circle-stdlib is vendored as a
pinned submodule of this library rather than expected as a sibling checkout,
so `make deps` is self-contained. See [BUILDING.md](BUILDING.md).

`sdl-app.mk` and `sdl-app.ld` are a shared link fragment an application's
Makefile includes. The linker script is TLS-safe, because binutils 2.44 and
later refuse a `PT_TLS` segment unless `.tbss` sits next to `.tdata`, which
Circle's stock script does not guarantee.

The split needs a multicore circle-stdlib world. Every source file also
compiles clean without it, and a single-core world builds only the direct call
paths behind the same public API.

### Limitations

No mouse, no game controllers, no haptics. No OpenGL, and that one is a design
position rather than an unfinished job, because there is no bare-metal GPU
driver to put behind it.

Scaling is nearest-neighbour only. `SDL_HINT_RENDER_SCALE_QUALITY` is stored
like any other hint and has no effect, and `"linear"` is not implemented.

Rectangle and fill draws are opaque and ignore the blend mode, so a blended
fill comes out solid.

A build-time-seeded wall clock serves `time()` and `gettimeofday` before
Circle's timer exists, because static constructors run before the host kernel
builds one. Without it, `srand(time(NULL))` at global scope, which is
ordinary, idiomatic C, silently killed a Pi 5 boot.
