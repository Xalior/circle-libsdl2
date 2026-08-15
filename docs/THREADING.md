# C++ standard library threading

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`, `std::call_once`, `std::thread` and `thread_local` all work on whichever core your application was placed on, and nothing has to be done to get that.

They need saying about because the ordinary answer does not apply here. Circle's cooperative scheduler is documented as belonging to core 0 alone, and circle-stdlib builds the C++ threading runtime on top of it - correctly, for the core it was written for. This library is the one that runs applications somewhere else, so it supplies that part of the runtime itself: the primitives are built from processor atomics, and every wait in them yields to Circle's scheduler on core 0, hands the core to the next cooperative context on a core that has them, and spins anywhere else.

The build fragment `sdl-app.mk`, which your kernel's Makefile already includes, is what selects this implementation instead of circle-stdlib's. Nothing in circle-stdlib is modified.

## Where a thread runs

By default, on core 0, as a cooperative Circle task - wherever it was created from. That is what a service thread wants: it costs no core, and it may reach Circle. A creation issued from another core is passed to core 0 rather than refused. This is true of `std::thread` and of `SDL_CreateThread` alike; the two share one placement rule throughout.

Two consequences follow, and the second is the reason for the section after this one.

Such a thread runs when core 0 gives up control, which it does constantly - but a thread that computes without ever waiting or sleeping holds core 0, and core 0 is the only core that services the SD card, the USB host and the serial port. Neither API knows, when it makes a thread, that hardware timing is waiting behind it, and an application is quite entitled to hand a thread two seconds of arithmetic. On core 0 those are two seconds of unserviced devices.

And it is on core 0 at all: an application that was moved to another core to get away from those devices creates a worker, and the worker lands back among them.

The library says so once per core, the first time a core off core 0 creates a thread, and names the call below. Nothing else about the default placement is announced, because nothing else about it changed.

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

A wait occupies the core it waits on, off core 0, for the same reason `SDL_Delay` does - so a core with nothing else runnable is a spinning core. A timed wait off core 0 polls the system counter rather than sleeping to the deadline. `thread_local` destructors run when a thread ends, so a `thread_local` belonging to the application core - which never ends - is never destroyed.

## Examples

`examples/cxxthreads` runs on a second core and reports: recursive mutexes, a condition variable woken by a thread the application core created, per-thread `thread_local` storage and its destructors, a timed wait that runs out, and `call_once`. Each result is reported by name on the serial console.
