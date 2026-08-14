# C++ standard library threading

`std::mutex`, `std::recursive_mutex`, `std::condition_variable`, `std::call_once`, `std::thread` and `thread_local` all work on whichever core your application was placed on, and nothing has to be done to get that.

They need saying about because the ordinary answer does not apply here. Circle's cooperative scheduler is documented as belonging to core 0 alone, and circle-stdlib builds the C++ threading runtime on top of it - correctly, for the core it was written for. This library is the one that runs applications somewhere else, so it supplies that part of the runtime itself: the primitives are built from processor atomics, and every wait in them yields to Circle's scheduler on core 0 and spins on any other core.

The build fragment `sdl-app.mk`, which your kernel's Makefile already includes, is what selects this implementation instead of circle-stdlib's. Nothing in circle-stdlib is modified.

## Where a `std::thread` runs

By default, on core 0, as a cooperative Circle task - wherever it was created from. That is what a service thread wants: it costs no core, and it may reach Circle. A creation issued from another core is passed to core 0 rather than refused.

Being cooperative has a consequence worth knowing. Such a thread runs when core 0 gives up control, which it does constantly, but a thread that computes without ever waiting or sleeping holds core 0 - and core 0 is where every device on the board is serviced.

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

## The limits

A wait occupies the core it waits on, off core 0, for the same reason `SDL_Delay` does. A timed wait off core 0 polls the system counter rather than sleeping to the deadline. `thread_local` destructors run when a thread ends, so a `thread_local` belonging to the application core - which never ends - is never destroyed.

## Examples

`examples/cxxthreads` runs on a second core and reports: recursive mutexes, a condition variable woken by a thread the application core created, per-thread `thread_local` storage and its destructors, a timed wait that runs out, and `call_once`. Each result is reported by name on the serial console.
