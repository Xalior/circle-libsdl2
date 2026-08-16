# Changelog

Versions here mark the points at which this library was proven on real
hardware by an application built on it. There is no release process and no
build artifact: the library is consumed as source, at a commit.

Anything marked **Consumers must act** changes what a host kernel has to do,
and will stop an existing kernel from building or running until it is
followed.

## vPoC3

### A held key repeats

A USB keyboard reports which keys are down and nothing more, so holding a key
produced one character and then silence: the reports stopped changing and
there was nothing left to translate. Every machine that appears to repeat a
held key makes the repeats itself, and the library now does it.

A repeat is an `SDL_KEYDOWN` with `key.repeat` set, carrying the same
scancode, keycode and modifiers as the press it repeats, followed by the same
`SDL_TEXTINPUT` that press produced. An application that wants only real
presses filters on the flag, which is what the flag has always been for and
which now sometimes reports true.

One key repeats at a time - the one pressed most recently - after half a
second, then about thirty characters a second. Pressing a second key while
the first is held moves the repeat to it, and releasing the second does not
bring the first back. Modifiers and lock keys never repeat and never take the
repeat from a key that has it, so holding a letter and then pressing shift
goes on repeating the letter in upper case.

The repeats are generated where the keyboard is already read, on core 0's
input pump, so nothing new runs asynchronously and no timer was added. A key
the debug-UART robot hands leave down repeats as well, since it is a held key
like any other; a `key tap` is far shorter than the delay and never does.

There is nothing to configure and no call to make: the delay and the rate are
the library's, and an application that already reads key events receives the
repeats.

### A cross-core wait gives its core to the threads on it

Waiting for the presentation core, for a keypress on standard input, or for a
call marshalled to core 0 used to put the application core to sleep. On a core
that schedules its own threads that is the wrong idle: the core has runnable
work of its own and was sleeping through it.

All of those waits now hand the core to the next runnable context first, and
sleep only when there is nothing to hand it to. This is where most of a
core's spare time actually is - a `PRESENTVSYNC` caller waits most of every
frame for the presentation core, and a program at a prompt waits on a human
indefinitely.

A core that has not been asked to schedule its own threads is unaffected and
sleeps exactly as before.

Three of those waits write a one-deep mailbox, and a wait that hands the core
on can be re-entered by the context that gets it. Posting a frame, reading
standard input and marshalling a call to core 0 now each hold that mailbox's
lock across both the wait and the write, so a second caller blocks instead of
overwriting a request in flight. One uncontended compare-and-swap per frame.
The waits that only read a counter take no lock, because they write nothing.

SDL's own rule is unchanged: use a renderer, its textures and its window from
one thread only.

### A thread that never gets a turn says so

Nothing in this design is preemptive, so a main loop that never waits, yields
or sleeps keeps its core and the threads on it never start. From outside that
is a board doing nothing with no fault reported anywhere - the window draws,
the pointer moves, and every part is working exactly as told.

A core that has a runnable thread it has not given a turn in five seconds now
says so on the log, once, naming the thread and whether it has ever run at
all. The report is driven from the application's own per-frame beat rather
than from the scheduler, because the fault is that the scheduler is never
entered. It costs two loads and a compare per frame, and nothing on a core
that schedules no threads.

**Consumers should act** on this line if it appears: it means the loop needs a
`std::this_thread::yield()` where it polls.

### An application core can run its own threads

`SDL2Circle_ThreadsStayOnThisCore`, called on a core, makes every thread
created on that core afterwards a cooperative context on that core -
`std::thread` and `SDL_CreateThread` alike. The library schedules it itself:
a stack out of the heap, a thread-local block of its own, an identity of its
own, no Circle task, no call into Circle's scheduler and nothing posted to
core 0.

This matters because the default placement is core 0, and core 0 is the only
core that services the SD card, the USB host and the serial port. An
application that was moved off core 0 on purpose, creating a worker, put that
worker back among them - and nothing said so. Worse, a thread that computes
without yielding costs the board something only on core 0: neither API knows,
when it makes a thread, that hardware timing is waiting behind it, and an
application is entitled to hand a thread two seconds of arithmetic. On core 0
those are two seconds of unserviced devices. An application core has nothing
on it that hardware waits for, which is exactly why work belongs there.

It changes nothing about threading semantics. A thread on core 0 is a
cooperative Circle task and a context on an application core is cooperative
too: each runs until it waits, sleeps, yields or ends, and neither was ever
preemptive. What changes is which core the cooperative context runs on.

**This is behaviour a host asks for.** A host kernel that does not make the
call gets exactly what it got before, with none of the new code reached: the
run list of a core nobody asked about is never created, every placement is
the one it was, and `SDL_ThreadID` gives the answers it always gave. What
such a host does now get is one line on the log, once per core, the first
time a core off core 0 creates a thread - saying where the thread went and
naming the call that changes it.

A consumer that does make the call should check three things. Its threads now
contend with the application rather than with core 0, so a worker that
computes without ever waiting holds the application core instead of holding
core 0 - which is the point, since holding the application core costs the
board nothing. Its threads can no longer touch Circle directly, for the same
reason the application core cannot: they are not on core 0, and the
marshalling calls (`SDL2Circle_CallOn0`, the I/O service) are how they reach
it. And each thread gets a fixed stack whose low end is checked on every
switch, so an overrun that used to corrupt something silently is now a line
on the log naming the thread. `SDL_CreateThreadWithStackSize` chooses that
size, `SDL_CreateThread` gets SDL's usual megabyte, and a `std::thread` gets
the size it would have had on core 0.

`SDL2Circle_ThreadPinNext` is unchanged and still applies to `std::thread`
alone; a pin still wins over everything else. The two threading APIs continue
to share one identity and one wait, so a lock held through one and inspected
through the other still agrees about who holds it.

### Standard input echoes what its reader chooses, not what Circle chose

The console no longer echoes a typed character itself. `CConsole::SetOptions`
is called with neither `CONSOLE_OPTION_ICANON` nor `CONSOLE_OPTION_ECHO` set,
so a read still hands back a character the instant its key is pressed - that
has not changed - but nothing draws it. Every reader of standard input draws
its own bytes now.

The reason is backspace. A reader building an edited line needs to draw
something different for the character behind a backspace than for every
other character it reads: a space over the old glyph and the cursor stepped
back, not the backspace byte itself. Circle's own echo cannot make that
distinction - it draws every byte it sees the same way, backspace included,
which is why the console never erases in place - so a reader that wants
in-place editing has to own its echo entirely, and a console that echoed
some of the time and left the rest to the reader would draw the wrong thing
for the one byte that matters.

**Consumers must act** if anything reads standard input through this
library and expected to see it echoed for free: it will not be, and must
draw what it reads itself. Free Pascal's target for this board already does
(`fpc/rtl/circlesdl2/sysfile.inc`, `Do_Read`); nothing else here reads
standard input.

### A typed line can be corrected before it is read

`Do_Read`, in `fpc/rtl/circlesdl2/sysfile.inc`, reads and edits a whole line
from standard input before it hands anything back: backspace removes the
character behind the cursor, from the line and from the screen, and what
comes back once Enter is pressed is the corrected text, never the keys that
typed it. Backspace at the start of an empty line does nothing, rather than
erasing whatever this console printed before the line began.

It is built entirely on the single-character read this library has always
offered (`SDL2Circle_ReadStdin`) and the raw write this library has always
offered (`SDL2Circle_WriteBytes`) - nothing new crosses from Pascal into this
library for it. Free Pascal cannot tell a single-character Read from a
ReadLn apart at this level, since both reach `Do_Read` the same way, so
`Do_Read` does not try: every read from standard input assembles a line and
returns it whole, and Read(Char) simply takes the first character of it -
the same as a single-character read behaves on any other platform's console.

### A texture can be drawn into

Render targets work. `SDL_CreateTexture` accepts `SDL_TEXTUREACCESS_TARGET`,
`SDL_SetRenderTarget` aims the renderer at such a texture, every drawing call
then lands in that texture instead of in the frame, and passing null aims the
renderer back at the frame. The texture is a source for `SDL_RenderCopy` like
any other. `SDL_GetRenderTarget` answers with the texture the renderer is
aimed at, `SDL_RenderTargetSupported` answers true, and `SDL_GetRendererInfo`
reports `SDL_RENDERER_TARGETTEXTURE`.

This is what a game does when it composes its whole picture at one fixed low
resolution and magnifies the finished image in a single step. Such a game
used to stop at start-up here, because the texture it wanted could not be
created.

Nothing new composes the picture: a render target is the executor that
composes the frame, writing into the texture instead. What is different about
a target is written down in [Display and video](DISPLAY.md#render-targets).

Aiming at a texture that was not created as a target is refused, as upstream
SDL refuses it. So is copying a target into itself, which upstream SDL leaves
undefined and which here would be one buffer read as it is written.

`examples/rendertarget` is the bootable demonstration, and checks each of
those answers on the serial console before it starts drawing.

### SDL_SetWindowHitTest is accepted rather than left undefined

`SDL_SetWindowHitTest` now exists: the callback and its data are recorded,
and the call always succeeds. A hit test tells a window manager which part
of a window drags it or resizes it, standing in for a title bar. There is no
window manager here - one window, filling the one display - so the
condition the callback exists to answer never arises, and it is never
called. A NULL callback still means what it means upstream: hit-testing is
disabled, which was already true of every window.

There is no `SDL_GetWindowHitTest` to accept alongside it - that entry point
does not exist in upstream SDL2 either.

### A program's own output is no longer treated as a log line

Where output goes and what output looks like are now two separate things.

Where it goes is unchanged and is still the machine's decision: the serial
port always, and the screen as well until an application takes the display.
What is new is a second way of writing into those destinations.
`SDL2Circle_WriteBytes` takes bytes and adds nothing to them - no source, no
severity, no timestamp, and no waiting for an end of line. A program that
prints a number gets that number. `SDL2Circle_Log` and `SDL2Circle_LogBytes`
are unchanged and still label everything they carry.

**Consumers will see this**: the C library's standard output and standard
error are now bound to that raw channel by the library itself, during
`SDL2Circle_ArmCoreRuntime`. An ordinary `printf` used to reach a descriptor
nobody had bound, so the C library answered "bad file descriptor" and the
bytes were lost; it now appears on the serial port, and on the screen while
the screen is still a destination, exactly as it was written. Nothing in
circle-newlib is changed to do this - the library gives the C library's own
console glue a device of its own.

**Consumers must act** if the host kernel binds its own console with
`CGlueStdioInit`. This library binds the three standard descriptors itself,
in `SDL2Circle_ArmCoreRuntime`, to a console with a keyboard on it. The C
library binds those descriptors together and stops the board inside an
assertion if they are bound twice, so a kernel that also binds its own halts
before the application starts. Remove the `CConsole` member and the
`CGlueStdioInit` call from the kernel: it needs neither.

All three standard descriptors are now held for the life of the program.
They used to be free,
and the C library hands out the lowest free descriptor, so the first file such
a program opened was given descriptor 0 - which a language runtime reads as
the console, quietly sending that file's writes to the console instead of to
the card.

Standard input is bound but no character ever arrives on it, because this
board has no console input. A C program that reads standard input waits.

### Output goes to serial and to the screen, and there is nothing to arrange

Every board now shows its output on the display as well as on the serial port,
until an application takes the display. That is the whole rule. Nothing has to
be called and nothing can be configured.

It is one device, built during `SDL2Circle_ArmCoreRuntime` - the call every
host kernel already makes on core 0 - holding the serial device the kernel gave
Circle's logger, the screen, and a flag saying whether the screen is still the
library's to draw on. Its write puts the bytes on serial always and draws them
while the flag is set. **Circle's logger is pointed at that device once and
never pointed anywhere else again.** So a kernel that has never heard of any of
this gets both destinations by doing nothing.

The display hand-off is no longer an event that rearranges anything. When an
application initialises SDL video the flag is cleared, and that is all: the
logger's destination is the same device before and after, and the serial half
is untouched. There is only ever one destination object and what it will do was
settled before anything ran, so the console and the application writing the same
framebuffer has no mechanism rather than being forbidden by a rule.

A board with no display is not a fault - it is a machine with one destination
instead of two, said once at notice.

`SDL2Circle_LogAttachScreen` is no longer something a consumer needs to call at
all. It remains for one case: a host kernel with bring-up of its own worth
watching on the glass, such as mounting a card, which happens before the arming
call. It builds the same one device at that earlier moment, and calling it
twice - or calling it after the library already has - does nothing and answers
0, so a kernel that made this call before is unaffected and can drop it.

Kernels used to reach both places by building Circle's own screen device and
teeing it with the serial device themselves. The shape is the same one; what
changes is whose screen it is. Circle's console is wrong on some boards and
cannot be made right: its colour depth is a compile-time value, and the
framebuffer object reads the granted pitch back out of the firmware's reply but
never the granted depth, so it goes on reporting the depth it was constructed
with. Circle's terminal sizes every row of pixels from that. Where the
firmware grants a depth nobody asked for - a Pi 5 grants 32 bits per pixel
whatever the request was - each character is drawn at the wrong stride and the
console paints a fraction of each scanline in squeezed characters.

This library already reads those numbers back for its own drawing, which is
why every application drawing through SDL has a correct picture on every
board. The console is now drawn from the same readings: the pitch and the
buffer address are the firmware's reply to the allocation, the width and
height are the firmware's report of the display, and the bytes per pixel are
that pitch divided by that width. There is no board test in it and nothing to
configure.

A kernel that keeps its own screen device keeps working, and is the one
arrangement this replaces - see [LOGGING.md](LOGGING.md).

### The I/O service gained rename, directory removal, and the working directory

The service carried open, read, write, seek, truncate, unlink, directory
creation and directory listing. It did not carry rename, directory removal,
change of directory, or the query that reports the working directory. An
application whose files go through the service could not rename a file, and
could not remove a directory it had made.

Nothing underneath was missing. The C library on this board has all four, and
the filesystem is configured for the relative-path calls the last two need, so
the gap was in the marshalling layer alone. Each new operation has the same
shape as the ones already there: one argument structure, one handler, and one
call performed on the core that owns the card.

Nothing already exported changed, so an existing kernel needs no action.

### The file-operation counter counts every operation

It was raised by open, read, write, stat and directory open, and not by
truncate, close, unlink, directory create, directory read or directory close.
That number appears in the watchdog's stall report, which therefore
under-reported how much file work an application was doing - the case where the
number matters most.

### Two documents corrected

`CORE-SPLIT.md` told an application with its own file layer to point that layer
at the service, and said nothing else was needed. A host kernel must also
initialise the C library's standard streams. Without that, the first file
opened takes the lowest free descriptor, which is the one a language runtime
reads as its console, and everything written to that file goes to the log with
nothing reporting it. The document did say which descriptors are the console,
but only in the section it tells this kind of application to skip.

`AUDIO.md` said timer callbacks run on core 0. They run in the caller's own
line of execution, on the caller's core, at the event pump and at `SDL_Delay`.
This library's other documents and its source have always said so.

### Every core gets a 2 MB stack

Circle's own default is 128 KB a core. This library configures its worlds at
2 MB - four cores, 8 MB of the board's memory - and the same for every
application that uses it. `make world CIRCLE_KERNEL_STACK_SIZE=<bytes>` still
sets it, for an application that needs more.

It is standardised rather than left to each application to ask for because a
stack that is too small does not report itself, and nothing about how it
fails points at a stack. Circle's four core stacks sit next to each other with
no guard page, so a core that runs off the bottom of its own writes into the
next one down - under the split, the application core's neighbour is core 0,
where the host kernel object lives as a local of `main()`. The picture
corrupts for a frame or two, then a device interrupt handler on core 0
dereferences a pointer the application overwrote, and the board takes a data
abort in Circle code that did nothing wrong.

An engine that `alloca`s its per-frame working set is the case to watch, and
it is a common one. TyrQuake's renderer does it on every frame - about 198 KB
at the engine's own minimum limits on a 64-bit target, and its source says it
expects at least a megabyte - so on 128 KB the first frame of real geometry
ran a core off the bottom of its stack. An array sized for 32-bit pointers is
also around 1.7 times larger when every pointer in it is eight bytes.

The value is fixed into the world at configure time, so changing it means
reconfiguring and rebuilding that world.

### SDL has one framebuffer, and every frame is drawn into it

A frame is no longer sent to the presentation core as a pointer into the
application's own texture. Every frame is composed into SDL's framebuffer - a
buffer this library owns, at the size the application was told the display is
- and that buffer is what crosses between the cores.

Two faults go with it.

The presentation core used to keep reading an application texture after
`SDL_RenderPresent` had returned. At that moment the application is entitled
to destroy the texture, or to lock it and draw the next frame into it, and
nothing stopped it. Whether that showed as a torn picture, a wrong one or a
halted board depended on timing alone.

`SDL_RenderReadPixels` used to read the display panel rather than SDL's
framebuffer. Where the picture is fitted and centred on a larger panel, it
returned the top-left corner of the panel - part black margin, part picture,
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
quieter faults that reported nothing at all - a contended `std::mutex`, every
condition variable wait, and every `thread_local` read from the application
core, which was answering with another thread's storage.

`std::thread` can now be created from any core. A thread runs on core 0 as a
cooperative task, which is what a service thread wants: it costs no core, and
it may call Circle. A host kernel that would rather have a thread on a core of
its own can lend one - see `SDL2Circle_ThreadCoreOffer` and
`SDL2Circle_ThreadPinNext` in `SDL2/SDL_circle.h` - and this library will never
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
own size in the top-left corner with the rest of the screen black - with the
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
refuses every other clock - and its refusal returns without writing the
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

Rumble is offered where the device supports it - off, weak and strong, which
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
is created, and it offers no safe way to ask whether one already exists -
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

## vPoC1 - 23a36e5

The first version carried all the way through on hardware - an application
built on it running on a Pi 3, a Pi 4 and a Pi 5.

### The part of SDL2 that works

A fullscreen window with a software `SDL_Renderer` over Circle's
`CBcmFrameBuffer`, drawing through streaming ARGB8888 textures. That is the
only texture format and 32 bits is the only depth: any other request fails
with an error rather than quietly falling back.

`SDL_RenderCopy` honours its source and destination rectangles - a same-size
copy is still a byte-for-byte unscaled blit, anything else resamples - with
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
never from the size the firmware echoes back - a Pi 5 acknowledges a mode
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
presentation - it turns a finished frame into a scanout, runs no SDL and
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
everything it needs from a frame, not after the output waits - the DMA
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
apart from waiting on DMA or on the raster - at a locked frame rate the
blocking waits absorb all the spare time, which previously made a fully idle
core look saturated. Each line leads with how much of the wall clock the core
was actually awake, measured locally, because a parked core's cycle counter
stops. The counter backend is AArch64 only; elsewhere the instrument compiles
to an inert form and says so once when armed.

### What the host kernel must do

Finish Circle's own world - interrupts, timer, serial, SD card, filesystem -
on core 0 before starting any other core. Start the secondary cores yourself
with `CMultiCoreSupport`. Call `SDL2Circle_ArmCoreRuntime()` as the first
statement on every core including core 0: a core that has just started has no
thread pointer, C++ exception state is reached through it, and without this
the first throw reads whatever the firmware left there - which passes on one
board and takes a data abort on the next. Call `SDL2Circle_SplitInit()` once
on core 0 before the application starts. Keep core 0 yielding for as long as
the application runs, because the servo only runs when something yields. Park
any core you give no role.

### Building

A separate static library per board - `libSDL2-rpi3.a`, `libSDL2-rpi4.a`,
`libSDL2-rpi5.a` - because Circle bakes the board in at configure time, each
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
builds one. Without it, `srand(time(NULL))` at global scope - ordinary,
idiomatic C - silently killed a Pi 5 boot.
