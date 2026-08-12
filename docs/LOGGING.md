# Output from any core

Two questions decide what happens to a line of output, and they are not the same question.

**Where output goes is a property of the machine.** The serial port always, and the screen as well until an application takes the display. It is one decision for the whole board, made by the host kernel while it initialises itself, and no caller has a say in it.

**What output looks like is a property of whoever is printing.** A log line carries a source, a severity and a timestamp, because that is what makes a log useful. A program's ordinary output carries nothing at all, because a program that prints a number expects that number and not a decorated version of it.

So there is one set of destinations underneath and two ways of writing into it.

```c
#include <SDL2/SDL_circle.h>

// labelled: goes out as "12:34:56.78 mygame: level 3 loaded"
SDL2Circle_Log("mygame", SDL2CIRCLE_LOG_NOTICE, "level %d loaded", n);

// raw: goes out as "3"
SDL2Circle_WriteBytes("3", 1);
```

Both are safe from any core, on the same terms. The serial console is a device, so only the hardware core may write to it. That would leave every other core unable to print anything — and the cores with the most to report, the application core and the chosen presentation core, are exactly the ones that could not.

So they do not write. **Each core copies into a ring of its own and returns**, and the hardware core's servo drains every ring. Nothing crosses but memory. A core that prints never touches the console and is never delayed by one that is printing. Both channels share the one ring per core, so a program's printed line and the log line it writes next come out in the order it produced them.

## Guarantees

- **It never blocks, and it never hides a loss.** If a ring is full the record is dropped and counted. The drain says when a core STARTS dropping and when it STOPS, with the total — rather than a line per pass, which would spend the scarce console on describing its own scarcity. Waiting would put the calling core to sleep for the sake of a diagnostic, and overwriting would silently corrupt the record.
- **The console is far slower than any core, and dropping is the steady state for an application that talks a lot.** Measure the rate before relying on it: it is much lower than the baud rate alone suggests, and low enough that a game logging once per data file while it scans its content can outrun it without appearing chatty. If the lines matter, raise the baud rate or log less — the ring cannot make the wire wider.
- **The servo's drain is bounded, and that bound is load-bearing.** It prints for a couple of milliseconds and returns, leaving the rest of the servo loop and the scheduler to run, and resumes at the next core so no ring is starved by a noisier one.

  Both halves of that matter. A drain whose only exit is an empty ring, in front of a producer that never waits, does not terminate at all once an application out-produces the console: the ring stays permanently non-empty, `head` never meets `tail`, and core 0 stops pumping USB, feeding audio and yielding. And a drain that resumed on the core that used up the budget would hand it straight back to the ring that just took it, leaving every core above it permanently silent — which on the wire is indistinguishable from a core that has stopped.

  A budget in TIME rather than in lines is what makes this hold: the cost of a line depends on the console, and a line count that is safe on one is not on another.

## Where output lands

**The machine decides, and neither the application nor the host kernel has to ask.** An application writes into a channel and knows of nothing beyond it. The destinations are attached once, while the machine is coming up, and that is the only moment anything is attached.

**The serial port is always a destination.** It is whatever device the kernel gave Circle's logger, and it stays that for the whole run.

**The screen is a second destination from boot, on every board, and this library both attaches and draws it.** It happens inside `SDL2Circle_ArmCoreRuntime`, the call every host kernel already makes on core 0, so a kernel that has never heard of any of this gets both destinations by doing nothing.

That is deliberately not something a kernel opts into. A destination a kernel can forget is a destination that goes missing in silence: there is nothing on the wire, and nothing on the glass, to say that half the output is not arriving.

Every line the logger carries — the kernel's own, this library's, and everything an application writes or prints — appears on the display as well as on the wire. The library reports the console it built on both of them:

```
sdl2console: screen log: 1920x1080 pixels, 4 bytes per pixel (pitch 7680), 240x67 characters of 8x16
```

**A board with no display is not a fault.** It is a machine with one destination instead of two; the library says so once, at notice, and the serial log carries on unaffected.

### Saying no, and asking earlier

**A machine that wants the serial port alone stamps `--rapi-no-screen-log`** into the boot argument block, alongside the library's other switches. It is a switch on the machine rather than a call in a kernel for the same reason the rule itself is the machine's.

It turns off two things. The log is not drawn. And **the display grant is not made at boot** — asking the firmware for the framebuffer is what sets the display mode, so this is how a machine says the mode is to be settled by the application's first window and by nothing before it.

**`SDL2Circle_LogAttachScreen` remains, for a kernel that wants the screen EARLIER than the arming call** — one with bring-up of its own worth watching on the glass, such as mounting a card. Call it on core 0 once the logger is on the serial device. It answers 0 when the screen is a destination, including when it already was, so a kernel that calls it after the library has is not told of a problem it does not have.

**The screen destination is dropped when an application initialises SDL video**, because that is the moment the application takes the display. From then on the log goes to the serial port alone, and the library says so before it goes. Nothing is ever attached after that: `SDL2Circle_LogAttachScreen` refuses once the application has the display, and a removal cannot spoil a picture. That is what makes a console and a game writing the same framebuffer unreachable rather than merely discouraged.

The drop happens after `SDL_Init` has decided whether it will start at all, so a consumer turned away at the door — no virtual display device declared — still gets the line saying why, on the screen as well as on the wire.

### Why this library draws it and not Circle

**Circle's screen device never asks the firmware what it was granted.** Its colour depth is a compile-time value, and nothing corrects it afterwards: the framebuffer object reads the granted *pitch* back out of the firmware's reply and keeps it, but never the granted *depth*, so its `GetDepth()` returns its constructor's argument for the object's whole life. Circle's terminal sizes every row of pixels from that number. On a board whose firmware hands out a surface at a depth nobody asked for — a Pi 5 grants 32 bits per pixel whatever the request was — each character is then drawn at the wrong stride into the right buffer, and the console paints a fraction of each scanline in characters squeezed by the same ratio. There is no depth setting that fixes this, because the number is not stale by configuration, it is stale by construction.

**This library reads its numbers back.** The pitch and the buffer address are the firmware's reply to the allocation; the width and height are the firmware's report of the display it is scanning; and the bytes per pixel are that pitch divided by that width — a granted quantity over a granted quantity, with nothing assumed between them. There is no board test anywhere in it and nothing to configure, so it is right on a board this library has never seen. The same reading is what makes the picture correct for every application drawing through SDL, which is why only a kernel reaching past this library to Circle's own console ever had the fault.

**The console and an application's window share the one framebuffer grant.** There is one allocation on this board; asking for it is what sets the display mode, so a second, different request would be a second mode. Attaching the screen at boot makes that grant early, with the same request an application's first window would have made — including `width=` and `height=` from `cmdline.txt` — and the window then adopts it.

**The text is white on black.** Any other colour needs the channel order the firmware chose, which is a further question this library does not ask. All bits set is white in every pixel format a Pi grants, and all bits clear is black in every one. Circle's logger brackets its lines in colour escape sequences; the console recognises them in order to throw them away, so they are never drawn as text.

The character shapes come from Circle's character generator, which turns a character into a bitmap and knows nothing about the screen, the depth or the pitch — none of what is wrong next door reaches it. The cell size is whatever font that world was configured with.

## Raw output

`SDL2Circle_WriteBytes` is the channel for a program's own output. It reaches the destinations above and **adds nothing**: no source, no severity, no timestamp, and no line discipline. Half a line is output. A byte that is not text is output. Nothing waits for an end of line.

**The C library's standard output and standard error are bound to it**, so an ordinary `printf` in any program built on this library follows the same rule as everything else on the board. The library binds them itself, during `SDL2Circle_ArmCoreRuntime`, and there is nothing for a host kernel to call.

**A host kernel that wants its own console for C output keeps it, and must bind it BEFORE `SDL2Circle_ArmCoreRuntime`.** The C library binds its three standard descriptors together and refuses — inside an assertion, which stops the board — to bind them twice. So the library checks first and leaves them alone when they are already bound, and a kernel that binds after that call is the one arrangement that fails.

**All three descriptors stay held for the life of the program, standard input included**, although nothing on this board ever types at it. The C library hands out the lowest free descriptor, so a released standard descriptor would go to the first file a program opened — and a language runtime that reads descriptor 0, 1 or 2 as the console would then send that file's writes to the console instead of to the card. A C program that reads standard input waits, because no character arrives.

## Format and delivery

- **`from` is stored as a pointer**, so it must outlive the call — a string literal, never a buffer on the stack. It is printed later, on another core.
- **Byte-oriented material that is genuinely a log has its own entry point.** `SDL2Circle_LogBytes` takes it in whatever pieces it was written in and assembles it into lines, publishing each one as it completes under a source and a severity. A log carries lines and has nowhere to put half of one. A program's ordinary output is not this; that is `SDL2Circle_WriteBytes` above.
- **The hardware core writes straight through**, and so does every core when the split is inactive. That keeps the boot log immediate, before any servo exists to drain anything, and it means a single-core build pays nothing at all for this.

Lines from other cores appear when the servo next drains, so they carry the drain's timestamp rather than the moment they were produced, and they are ordered per core rather than against each other. For working out what happened that is enough; for measuring how long something took, use the performance reports in [PERFORMANCE.md](PERFORMANCE.md).
