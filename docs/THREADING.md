# C++ standard library threading

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`, `std::call_once`, `std::thread` and `thread_local` all work on whichever core your application was placed on, and nothing has to be done to get that.

They need saying about because the ordinary answer does not apply here. Circle's cooperative scheduler is documented as belonging to core 0 alone, and circle-stdlib builds the C++ threading runtime on top of it - correctly, for the core it was written for. This library is the one that runs applications somewhere else, so it supplies that part of the runtime itself: the primitives are built from processor atomics, and every wait in them yields to Circle's scheduler on core 0, hands the core to the next cooperative context on a core that has them, and spins anywhere else.

The build fragment `sdl-app.mk`, which your kernel's Makefile already includes, is what selects this implementation instead of circle-stdlib's. Nothing in circle-stdlib is modified.

## Where a thread runs

By default, on core 0, as a cooperative Circle task - wherever it was created from. That is what a service thread wants: it costs no core, and it may reach Circle. A creation issued from another core is passed to core 0 rather than refused. This is true of `std::thread` and of `SDL_CreateThread` alike; the two share one placement rule throughout.

Two consequences follow, and the second is the reason for the section after this one.

Such a thread runs when core 0 gives up control, which it does constantly - but a thread that computes without ever waiting or sleeping holds core 0, and core 0 is the only core that services the SD card, the USB host and the serial port. Neither API knows, when it makes a thread, that hardware timing is waiting behind it, and an application is quite entitled to hand a thread two seconds of arithmetic. On core 0 those are two seconds of unserviced devices.

And it is on core 0 at all: an application that was moved to another core to get away from those devices creates a worker, and the worker lands back among them.

The library says so once per core, the first time a core off core 0 creates a thread:

```
sdl2cxx: core 1: threads run on core 0 as cooperative Circle tasks
```

That is the placement and nothing more. What core 0 is carrying beside those threads is the paragraph above, and the call that keeps them where they were made is the section below. Nothing else about the default placement is announced, because nothing else about it changed.

## Keeping a thread on the core that made it

A host kernel that wants the other answer asks for it, on the core it wants it on:

```c
void CMyCores::Run(unsigned nCore)
{
    SDL2Circle_ArmCoreRuntime();

    switch (nCore)
    {
    case 1:
        SDL2Circle_ThreadsStayOnThisCore();
        RunTheApplication();
        break;
    ...
    }
}
```

Every thread created on that core afterwards is a **cooperative context on that core** - `std::thread` and `SDL_CreateThread` alike. It gets its own stack out of the heap, its own thread-local block and its own identity, and this library schedules it: no Circle task is built for it, Circle's scheduler is never called, and nothing is posted to core 0.

Both APIs start such a thread through one internal call, because what a thread on this core *is* does not differ between them. What does differ - a handle, a reference count, a status word, when a joiner may free what - each API supplies as a pair of function pointers, and the scheduler knows none of it.

Nothing about this is a change of threading semantics, and neither placement was ever preemptive. A thread on core 0 is a cooperative Circle task; a context on an application core is cooperative too. Each runs until it waits, sleeps, yields or ends. What changes is which core the cooperative context runs on, and therefore what its not yielding costs.

The context switch is AArch64 assembly in `src/libcxxthreading.cpp`. It carries `x19`-`x28`, the frame pointer, the link register, the stack pointer, `d8`-`d15` and `TPIDR_EL0` - the callee-saved set under AAPCS64, plus the thread pointer, which is both this library's thread identity and the base every `thread_local` is an offset from.

**A host that does not make the call is not affected by it.** The default placement above is what it gets, the run list of a core nobody asked about is never created, and `SDL_ThreadID` answers exactly as it did before. This is something to turn on, not something to opt out of.

Three things are worth knowing before turning it on:

- A worker now contends with the application rather than with core 0. One that computes without ever waiting holds the application core - but nothing on that core is servicing hardware, so holding it costs the board nothing.
- Such a thread cannot touch Circle directly, for the same reason the application core cannot. `SDL2Circle_CallOn0` and the I/O service are how it reaches core 0's world.
- Its stack is fixed when the thread is made, out of the heap. `SDL_CreateThreadWithStackSize` chooses it; `SDL_CreateThread` gets SDL's usual megabyte; a `std::thread` gets the same size it would have had on core 0, since the type has nowhere to ask for one. The low bytes carry a pattern that is read back on every switch away, so a thread that overruns its stack is named on the log rather than quietly corrupting whatever the allocator put next to it.

The two APIs continue to share one identity and one wait, so a lock held through one and inspected through the other agrees about who holds it.

## What gives a thread its turn

Nothing here is preemptive. A context runs when something on its core gives the core up, and these are the places that do:

- every wait in the C++ primitives - a contended `std::mutex`, a condition variable, `std::call_once`, `join`;
- every wait in SDL's own locks - `SDL_LockMutex`, `SDL_CondWait`, `SDL_SemWait`, `SDL_WaitThread` - and `SDL_AtomicLock` when it is contended;
- `SDL_Delay`, and `std::this_thread::sleep_for`;
- `std::this_thread::yield()`;
- **every wait that crosses to another core**: `SDL_RenderPresent` and `SDL_UpdateWindowSurface` waiting on the presentation core, a read on standard input waiting on a human, and any call the library marshals to core 0;
- a thread ending.

The cross-core waits are the ones worth knowing about, because they are where most of the time goes. A `PRESENTVSYNC` caller spends most of every frame waiting for the presentation core, and a program parked at a prompt waits on a human indefinitely. All of that time now belongs to the threads on that core.

**A main loop that does none of these keeps its core, and its own threads never start.** That is not a defect in the loop - it is what cooperative means - but from outside it looks like a working board doing nothing: the window draws, the pointer moves, and no fault is reported anywhere. So the core says so. Once, after five seconds, naming the thread:

```
sdl2cxx: core 1 gave no turn in 5s: thread 2148340448 never ran
```

The thread is reported as `never ran` when it has not had a single turn since it was created, and `waiting since its last turn` when it has run before and has not been back.

**That line means one thing: nothing running on that core has waited, yielded or slept for five seconds.** The core schedules its own threads, so a loop that polls without ever reaching one of the places above starves every thread on it, however many there are and whatever they were made for. A single `std::this_thread::yield()` inside the loop is the whole of the remedy - it is a place that gives the core up, and that is all a context needs.

The check is driven from the application's own per-frame beat, not from the scheduler, because the fault is precisely that the scheduler is never entered. It costs two loads and a compare per frame, and nothing at all on a core that schedules no threads.

### One writer per mailbox

A cross-core wait that hands the core on can be re-entered by the context that gets it. The library's own mailboxes are held against that: posting a frame, reading standard input and marshalling a call to core 0 each hold that mailbox's lock across both the wait and the write, so a second context blocks rather than overwriting a request in flight. The waits that only read a counter - waiting for a texture buffer to be released, or for a frame to reach the glass - take nothing, because they write nothing.

SDL's own rule is unchanged and still yours to keep: **use a renderer, its textures and its window from one thread only.** That was already true across cores here, and a thread on the application core is no different.

## Giving a thread a core of its own

A host kernel that has a spare core can lend it:

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

`SDL2Circle_ThreadCoresFree` answers with the cores that are available right now, as a bitmask. A core the application or the presentation worker is using is never in it, and neither is one already running a thread: this library tracks which cores it has spoken for, so a request for a busy core is refused rather than granted on top of what is already there. A lent core runs one thread at a time.

A pin wins over everything else. It is asked for immediately before the thread is constructed and applies to that one creation, so it decides where that thread goes whether or not the calling core schedules its own. It applies to `std::thread` only, which is the API it was written for. A lent core cannot itself be asked to schedule its own threads: it is reclaimed when its thread ends, and contexts made on top of it would outlive it.

## The limits

A wait occupies the core it waits on, off core 0, for the same reason `SDL_Delay` does - so a core with nothing else runnable is a spinning core. A core that has been asked to schedule its own threads and has none runnable still sleeps in `wfe` on a cross-core wait, exactly as it did before. A timed wait off core 0 polls the system counter rather than sleeping to the deadline. `thread_local` destructors run when a thread ends, so a `thread_local` belonging to the application core - which never ends - is never destroyed.

The per-core performance report (`SDL2Circle_SetPerfInterval`) accounts a core's cycles, not a context's. Work another context does inside a cross-core wait is therefore counted against the waiting category. That is an instrument a host has to ask for, and it is off in a shipped image.

## Examples

`examples/cxxthreads` runs on a second core and reports: recursive mutexes, a condition variable woken by a thread the application core created, per-thread `thread_local` storage and its destructors, a timed wait that runs out, and `call_once`. Each result is reported by name on the serial console.
