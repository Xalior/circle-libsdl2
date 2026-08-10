# Logging from any core

The serial console is a device, so only the hardware core may write to it. That would leave every other core unable to log anything — and the cores with the most to report, the application core and the chosen presentation core, are exactly the ones that could not.

So they do not write. **Each core formats its line into a ring of its own and returns**, and the hardware core's servo drains every ring into the logger. Nothing crosses but memory. A core that logs never touches the console and is never delayed by one that is printing.

```c
#include <SDL2/SDL_circle.h>

SDL2Circle_Log("mygame", SDL2CIRCLE_LOG_NOTICE, "level %d loaded", n);
```

## Guarantees

- **It never blocks, and it never hides a loss.** If a ring is full the line is dropped and counted. The drain says when a core STARTS dropping and when it STOPS, with the total — rather than a line per pass, which would spend the scarce console on describing its own scarcity. Waiting would put the calling core to sleep for the sake of a diagnostic, and overwriting would silently corrupt the record.
- **The console is far slower than any core, and dropping is the steady state for an application that talks a lot.** Measure the rate before relying on it: it is much lower than the baud rate alone suggests, and low enough that a game logging once per data file while it scans its content can outrun it without appearing chatty. If the lines matter, raise the baud rate or log less — the ring cannot make the wire wider.
- **The servo's drain is bounded, and that bound is load-bearing.** It prints for a couple of milliseconds and returns, leaving the rest of the servo loop and the scheduler to run, and resumes at the next core so no ring is starved by a noisier one.

  Both halves of that matter. A drain whose only exit is an empty ring, in front of a producer that never waits, does not terminate at all once an application out-produces the console: the ring stays permanently non-empty, `head` never meets `tail`, and core 0 stops pumping USB, feeding audio and yielding. And a drain that resumed on the core that used up the budget would hand it straight back to the ring that just took it, leaving every core above it permanently silent — which on the wire is indistinguishable from a core that has stopped.

  A budget in TIME rather than in lines is what makes this hold: the cost of a line depends on the console, and a line count that is safe on one is not on another.

## Format and delivery

- **`from` is stored as a pointer**, so it must outlive the call — a string literal, never a buffer on the stack. It is printed later, on another core.
- **Byte output has its own entry point.** `SDL2Circle_LogBytes` takes output in whatever pieces it was written in — an application's `stdout` — and assembles it into lines, publishing each one as it completes. A log carries lines and has nowhere to put half of one.
- **The hardware core writes straight through**, and so does every core when the split is inactive. That keeps the boot log immediate, before any servo exists to drain anything, and it means a single-core build pays nothing at all for this.

Lines from other cores appear when the servo next drains, so they carry the drain's timestamp rather than the moment they were produced, and they are ordered per core rather than against each other. For working out what happened that is enough; for measuring how long something took, use the performance reports in [PERFORMANCE.md](PERFORMANCE.md).
