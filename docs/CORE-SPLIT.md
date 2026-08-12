# Running off core 0: the core split

The Circle kernel is core-0-only by design: its scheduler, interrupts, USB, FatFs and sound may only be touched from there. So an application that wants a core to itself has a problem — on any other core, it cannot call the platform at all.

This library solves that. **Your code runs wherever the host kernel puts it and keeps calling plain `SDL_*`;** the library marshals the calls. Call `SDL2Circle_SplitInit` once, on core 0, before the application starts, and it arms tasks **on core 0**:

- a **servo** — the service task. It drains the call mailbox, runs the I/O service, pumps USB input into the event ring, feeds the sound device from the audio ring, and updates the CPU throttle;
- a **watchdog** — reports a stalled application core instead of leaving the board stopped with no explanation.

Everything crosses in coherent memory: lock-free single-producer/consumer rings (events in, audio out), a one-deep frame mailbox, and a one-deep call mailbox for the rare marshalled call — device bring-up, framebuffer allocation, file I/O. Never a Circle scheduler primitive, which would be illegal off core 0.

**The library does not own the cores.** It never starts one. The host kernel starts the secondary cores (`CMultiCoreSupport`), decides where the application runs, and — if it wants presentation off the application's core — runs `SDL2Circle_SplitPresentCore` on a core of its choosing, where it blits posted frames and page-flips. That is the whole of what this library puts on a core other than 0.

## What changes for application code

Once your code is off core 0:

- **The pump no longer does the platform work.** `SDL_PumpEvents` on your core only touches shared memory — drain the event ring, mirror key state, update the liveness signal. USB plug-and-play and HID translation stay on core 0's servo.
- **Audio inverts from pull to push.** Your callback fills the audio ring; the servo feeds the sound device at its own cadence.
- **`SDL_Delay` becomes exact, and occupies the core.** On core 0 it sleeps through `CScheduler::MsSleep` — cooperative, so you become *runnable* at the deadline but only resume when the scheduler next gets control, and a peer that is slow to yield overshoots your delay by however long it takes. Off core 0 there is no scheduler to defer to, so the library spins on the system timer (µs resolution, `yield` hint) and returns at the deadline. You trade a scheduler yield for a deterministic wait, and the cost is that the core is fully occupied for the duration — which is only sound *because* the core is dedicated and has nothing else to run. The same spin on core 0 would starve the servo, the watchdog, USB and the audio feed, which is why core 0 keeps yielding.

  **A delay runs your audio callback, on either core.** The callback has no thread of its own here, so it runs from whichever context calls into the library, and a delay that only counted time would be a delay with the sound device unfed. That matters beyond the sound: a game that produces a frame of audio into a buffer of its own, finds the buffer full and waits in millisecond steps for the callback to take some out is waiting for something only the wait itself can do. The core 0 sleep is taken in one-millisecond slices for the same reason, so a long delay there does not park the callback either. Keep the callback out of a section with `SDL_LockAudioDevice`, which the pump obeys.

**Single-core is the same code in its degenerate case.** Without `SplitInit` there is no ring and no mailbox: every call executes directly, the pump does the platform work itself, audio pulls, and the watchdog is an in-band timer that dumps the scheduler's task list if the main loop stops for 30 seconds. One codebase, one set of call sites, a branch on `SplitActive()`. That is also exactly what a single-core Circle build produces — the split is compiled out and only the direct paths remain. See [Building](BUILDING.md) for choosing between the two.

## The roles

- **The hardware core (core 0).** Circle's own subsystems: scheduler, interrupts, USB, the SD card, sound. Every device call, from any core, is executed here.
- **The main core.** The application, this library, and the C library. **All of SDL runs here** — every call, drawing included, and every one of them has finished the work its API promises by the time it returns.
- **The hardware output scale core.** It performs the job of display hardware this board does not have: a finished frame goes in, a scanout comes out. It runs no part of SDL and knows nothing about it. Pixels only.

**By default what crosses to that last core is a finished frame — pixels, and where on the screen they go, rather than a list of drawing to do.** Drawing is SDL's, and SDL runs on the main core. A build may raise the crossing count and send short frames as a command list instead; see [Choosing what crosses](#choosing-what-crosses-the-crossing-count).

The pixels are **SDL's framebuffer**: one buffer at the canvas size, owned by this library, that every frame is drawn into on the main core. It is double buffered, so the buffer handed to the presentation core is never the one the next frame is being drawn into.

**No memory belonging to the application is ever handed to another core.** An application texture is the application's: it may be destroyed, locked or redrawn the moment a call returns, and a frame that is still being read elsewhere cannot depend on it. So a frame is composed here first, always, and the composed frame is what travels.

The cost is one canvas-sized copy per frame on the main core. It buys a rule with no exceptions, which is the only kind that survives contact with an application nobody here wrote.

## Choosing what crosses: the crossing count

**How much of a frame travels to the presentation core as a list of drawing commands, rather than as a finished picture, is a build-time choice.** One number decides it:

```sh
make PRESENT_CMDS=0     # the default
make PRESENT_CMDS=8
```

- A frame whose draw list **fits within the count** crosses as a list, and the presentation core composes it command by command. Nothing is drawn on this side.
- A frame that **does not fit** is drawn into SDL's framebuffer here, and that framebuffer crosses as a finished picture.

**Zero — every frame a picture — is the default.** At zero nothing is held back: the first draw call of a frame goes straight into SDL's framebuffer, and every one after it does too. Raising the count moves composition to the presentation core, frame by frame, up to the mailbox's capacity — every value in between is a real setting, not a mode.

Both paths reach the same executor, which writes into the shadow buffer or the staging frame according to what the firmware granted, and the page flip or the copy to the screen is decided by the grant either way. **What crosses does not know or care what was granted.**

### The two numbers

```c
SDL2CIRCLE_RECORD_MAX_CMDS  16      /* src/sdl2circle.h — mailbox capacity */
SDL2CIRCLE_PRESENT_MAX_CMDS  n      /* PRESENT_CMDS — what may cross */
```

The frame mailbox is a fixed structure in coherent memory that both cores read — one frame in flight, no allocation and no locks on the path a frame takes. Each of its sixteen slots holds a source and destination rectangle, a colour, a source pointer and pitch, and two blend flags, and the space is reserved whether a frame uses it or not. That capacity is the hard ceiling on the count, and the build refuses a count above it.

Drawing begins the moment a frame grows past the count, so a frame that is going to be drawn here starts at that point rather than waiting for present — the work is the same either way, but spread across the application's own draw calls instead of occurring all at once at the end.

Nothing is ever dropped. A frame that outgrows the count has the commands held so far replayed into SDL's framebuffer, and every later call is drawn directly into it, so it renders correctly; it simply crosses as a picture.

The count is compiled into the archive when the library is built. It is not in any installed header, so an application's own translation units neither see it nor need to match it; and because every count shares one archive name, changing it deletes the archive so the previous build cannot be quietly reused.

## Why the split is shaped this way

The goal is native speed on modest hardware, and the way to get it is to remove from the application every cost that is not the application's own work.

So the split is not a way to make use of spare cores. It is a way to **give the application core one job**: the program's own main loop, and as little else as possible. Presentation and scaling go to the chosen presentation core. Devices, files and the sound feed go to core 0. What is left on the application core is the program itself, which is the only work that cannot be moved.

The second requirement matters just as much. **The topology is chosen, not assumed, and the same compiled binary must be able to run every role on a single core.** A board with cores to spare and a board with none run the same image; only the host kernel's choice differs. Today the single-core case is the degenerate path — no ring, no mailbox, every call direct — and the roles may later become tasks sharing one core on hardware that cannot spare several. That is why every crossing in this library is a mailbox or a ring that any core may consume, and why nothing in it is written against a particular core number. The library does not decide where anything runs, and it must never start assuming.

## What the host kernel has to do

In this order. It is ordinary Circle start-up until the split is armed; only steps 3 and 4 are calls into this library.

1. **Initialise the platform first.** Bring up interrupts, the timer, the serial console, the SD card, and mount the filesystem — all on core 0, as normal. The other cores are about to start asking core 0 for things, so those services must already exist.

   **A `CScheduler` is needed to run the split, and the library supplies one if you have not.** The servo and the watchdog are Circle tasks, and a Circle task registers itself with the scheduler while it is being constructed — through `CScheduler::Get()`, which stops the machine rather than reporting an absence. So `SDL2Circle_SplitInit` asks `CScheduler::IsActive()` first and creates one where there is none.

   **A host that declares its own keeps it**, and nothing about it changes: the library only ever asks whether one exists. Declaring one as a kernel member is still the clearer thing to do if the host has any use for it of its own — cooperative tasks, `CScheduler::Yield` in an idle loop — because then the host controls where it sits in the member order. A host with no use for one need not have one at all.

   A scheduler the library made is never destroyed. The servo and the watchdog are registered with it and run for as long as the machine does.

   If this kernel initializes I2C, SPI or the mini UART, declare a `CSDL2CircleHardware` member as well, so the CPU clock is settled before those peripherals are configured. See [Design](DESIGN.md) for why.

2. **Start the secondary cores** (`CMultiCoreSupport::Initialize`). Your subclass decides what each one does. This library never starts a core and never chooses one; the host owns that decision entirely.

3. **Call `SDL2Circle_ArmCoreRuntime` as the first statement on every core**, core 0 included. This step is not part of the split and is not optional for a single-core kernel either: every host calls it, whether or not it ever calls `SplitInit` below. A core that has just started has no thread pointer, and C++ exception state is reached through it, so the first thrown exception dereferences whatever the firmware left in that register. Where the leftover value is zero the read goes to mapped low memory and the throw appears to work — which is why omitting this call works on one board and takes a data abort on the next, on an ordinary throw, looking exactly like a hardware fault. On core 0 it also starts the C++ threading runtime, which is why that needs nothing of its own (see [C++ threading](THREADING.md)), and brings up board hardware — the CPU clock and case fan — which is why a kernel declares no `CCPUThrottle` of its own (see [Design](DESIGN.md)).

4. **Call `SDL2Circle_SplitInit` once, on core 0.** It creates the servo and the watchdog. Until it has returned, no other core may call `SDL_*`: the mailboxes are not armed and the call would run on the wrong core.

5. **Start the application** on the core you chose for it, after step 4. Because steps 2 and 4 happen in that order, the application core has to wait for a signal — a plain shared flag is enough — rather than beginning the moment it starts.

Meanwhile one of your secondary cores runs `SDL2Circle_SplitPresentCore`, which never returns. That is the core that scales each finished frame onto the screen and makes it visible. It runs no SDL and holds no application state; it is display hardware, written in software.

Details that are easy to get wrong:

- **A core that is given no role must be parked** in a wait loop. Returning from your dispatch function lets the core continue into whatever code follows it.
- **Core 0 must keep yielding for as long as the application runs.** The servo is a scheduler task, so it only runs when something gives up the core. A host that waits for the application by spinning without yielding will deadlock: the application core waits for answers that core 0 is never free to give. Wait in a loop that calls `CScheduler::Yield`.

A host kernel is also the one piece of software that may still need a device this library does not own — its own serial port, a GPIO line. For those, `SDL2Circle_CallOn0` runs a function on core 0 and waits for it. It is the same mailbox the library marshals through, and it is a direct call, costing nothing, when the split is inactive or the caller is already core 0.

## What the application needs

Almost nothing. Call plain `SDL_*` and it works on whatever core you were placed on. There is exactly one thing the library cannot marshal for you.

**Files.** The C library on this platform drives the SD card directly from whichever core calls it, and off core 0 that is not allowed. So an application that reads files must reach them through the I/O service in `SDL2/SDL_circle.h` — `SDL2Circle_IOOpen`, `IORead`, `IOWrite`, `IOOpenDir` and the rest. Every one of them is safe from any core, and every one of them becomes an ordinary direct call in a single-core build, so there is no second code path to maintain.

**The working directory is one setting for the whole board.** `SDL2Circle_IOChdir` and `SDL2Circle_IOGetCwd` reach the filesystem's own current directory, which lives on core 0 and is shared by every core, this library's file calls and the host kernel's alike. A change made from anywhere is a change everywhere. Two parts of a program that both use relative paths therefore have to agree about it; a part that cannot make that agreement uses absolute paths, which the setting does not affect.

If the application has its own file layer — many ports and emulators do — point it at these functions. Two things are then still the host kernel's:

- **Initialise the C library's standard streams before the application opens anything.** A Circle kernel that calls `CGlueStdioInit` takes descriptors 0, 1 and 2 for the console. Without it the first file the application opens is handed descriptor 0, because the C library gives out the lowest free slot — and a runtime that reads 0, 1 and 2 as the console then sends every write meant for that file to the console instead, with nothing reporting a fault.
- **Bring the card up before any other core starts asking for it**, which is step 1 above.

If it does not, and it simply calls `fopen`, `std::ifstream` or `opendir` throughout its source, **redirect the C library underneath it** instead of editing the application. The linker's `--wrap` option does this without touching a single vendored file:

```
# in your kernel's Makefile
LDFLAGS += --wrap=_open --wrap=_close --wrap=_read --wrap=_write \
           --wrap=_lseek --wrap=_fstat --wrap=_unlink \
           --wrap=opendir --wrap=readdir --wrap=closedir
```

Then write one small file defining `__wrap__open`, `__wrap__read` and so on. Each has the same form:

```c
long __wrap__read(int fd, void *buf, size_t len)
{
    if (on_core_0())                       // or the split is not active
        return __real__read(fd, buf, len); // the genuine implementation
    ...                                    // otherwise use the I/O service
}
```

What makes this work, and all of it matters:

- **`--wrap` rather than redefining the symbols.** The C library's file syscalls are defined together in one object file. Redefining some of them either collides at link time or, worse, leaves the originals linked in beside your versions with no warning at all.
- **`__real_*` is the mechanism, not a convenience.** The I/O service does its work by making these same calls once it has reached core 0. Without a route back to the original, the wrapper would call itself forever.
- **The service names an offset on every read and write; the C library expects a file to remember where it is.** So your wrapper has to keep the position itself, one value per open descriptor, advancing it on each read and write and setting it on each seek. Seeking from the end needs the file's length, which `SDL2Circle_IOOpen` returns when you ask for it.

Descriptors 0, 1 and 2 are the console rather than files, so they have no route through the I/O service. Send those to core 0 with `SDL2Circle_CallOn0` and let the original implementation run there.

## Examples

`examples/videocycle` destroys and recreates the whole video and audio subsystem in a loop at alternating source geometry, while a presentation core keeps running — what a settings menu does on every change, with nobody at the keyboard.
