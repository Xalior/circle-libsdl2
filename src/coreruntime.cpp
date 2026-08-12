//
// coreruntime.cpp — the per-core C runtime setup a host owes every core it
// hands to an application.
//
// A core started by a host runs with a processor state nobody prepared. Most
// of it does not matter; one part does. C++ exception state is THREAD-LOCAL,
// and the first thing a throw does is read the thread pointer and dereference
// what it finds. Nothing else in a Circle world writes that register: Circle
// sets it only when switching between its own tasks, and the C library's
// thread-local setup is not linked into a bare-metal image. So a core running
// application code holds whatever the firmware left in it at reset.
//
// The reason this is not obvious, and the reason it costs a day when it
// finally bites: a register that reads zero SURVIVES. The dereference lands
// in low memory, which is mapped, and the throw proceeds over bytes that mean
// nothing and that nothing checks. Two boards can pass every test while a
// third — same binary, same library, firmware that left something else in the
// register — takes a data abort on the first thrown exception. It reads as a
// board fault. It is not one.
//
// So the host arms each core it elects, once, on the core itself, before that
// core runs anything that can throw. This is deliberately a call and not
// something done behind a host's back: the library never starts a core, and
// it has no business writing a system register on a core nobody told it
// about.
//
// The thread pointer needs somewhere real to point. AArch64 puts a thread
// control block at the pointer and the thread-local variables after it, so
// each core gets a block of that shape: the image's initialised thread-local
// data copied in, the rest zeroed. The bounds come from the linker script
// (sdl-app.ld and anything derived from it), which is where a bare-metal
// image's thread-local sections are placed.
//
#include "sdl2circle.h"
#include <SDL2/SDL_circle.h>
#include <circle/types.h>
#include <string.h>

#if AARCH == 64

// Placed by the linker script: the thread-local image this core needs a
// private copy of. .tbss follows .tdata, so these three bound both.
extern "C" u8 __tdata_start, __tdata_end, __tbss_end;

// AArch64's thread pointer addresses a control block; the variables live
// after it.
#define TLS_TCB_BYTES 16

#define CORERUNTIME_MAX_CORES 4

// One block per core, kept so a second call on a core is a no-op rather than
// a leak — and so the block outlives the call, which is the whole point.
static u8 *s_block[CORERUNTIME_MAX_CORES];

void *SDL2Circle_AllocTLSBlock(void)
{
    size_t nData  = (size_t)(&__tdata_end - &__tdata_start);
    size_t nTotal = (size_t)(&__tbss_end  - &__tdata_start);

    u8 *p = new u8[TLS_TCB_BYTES + nTotal];
    if (!p)
        return nullptr;
    memset(p, 0, TLS_TCB_BYTES + nTotal);
    if (nData)
        memcpy(p + TLS_TCB_BYTES, &__tdata_start, nData);

    return p;
}

void SDL2Circle_FreeTLSBlock(void *pBlock)
{
    delete[] (u8 *)pBlock;
}

void SDL2Circle_SetThreadPointer(void *pBlock)
{
    // The register is per core, which is why this must execute ON the core
    // — or, for a scheduler task, inside the task — whose pointer it sets.
    asm volatile("msr tpidr_el0, %0" ::"r"(pBlock) : "memory");
}

void *SDL2Circle_GetThreadPointer(void)
{
    void *p;
    asm volatile("mrs %0, tpidr_el0" : "=r"(p));
    return p;
}

extern "C" void SDL2Circle_ArmCoreRuntime(void)
{
    unsigned core = SDL2Circle_ThisCore() % CORERUNTIME_MAX_CORES;
    if (s_block[core])
        return;

    u8 *p = (u8 *)SDL2Circle_AllocTLSBlock();
    if (!p)
        return;

    s_block[core] = p;
    SDL2Circle_SetThreadPointer(p);

    if (core == 0)
    {
        // Board hardware — the CPU clock and the case fan — before anything
        // else on core 0, so the rest of bring-up runs at the clock the
        // application is going to have. SDL2Circle_HardwareInit is
        // idempotent (see hardware.cpp), so a host kernel that brought this
        // up earlier still — a CSDL2CircleHardware member, or a direct call
        // from its own constructor, needed when it drives I2C, SPI or the
        // mini UART itself — costs nothing extra here.
        SDL2Circle_HardwareInit();

        // The screen, as a second destination for everything printed from
        // here on. Where output goes is a property of the machine, so the
        // library makes this itself rather than leaving each host kernel to
        // remember it — a forgotten destination is silent, and silence is
        // indistinguishable from a board with no display. Before the two
        // calls below, so that what they say appears on the glass as well.
        SDL2Circle_LogAttachScreenAtBoot();

        // The C library's standard output and standard error, bound to this
        // board's output router before anything can print. Here for the same
        // reason as the calls below: this is the one point every host kernel
        // already makes on core 0, early, with its world up — and "early" is
        // load-bearing, because the C library takes the lowest free
        // descriptors and a file opened first would take one of them.
        SDL2Circle_StdioInit();

        // The application's own static constructors, held back by
        // sdl-app-init.ld until the kernel exists. Core 0 only, and after
        // the runtime above rather than before it: a deferred constructor
        // may use thread_local storage, and that is what was just armed.
        //
        // Hung off this call deliberately. An application already makes it,
        // on core 0, at the point in its start-up where everything a
        // constructor could reach is up — so adopting the deferral costs it
        // nothing and there is no second call to forget.
        SDL2Circle_RunDeferredConstructors();

        // The C++ threading runtime's creator task, for the same reason and
        // on the same terms: this call is the one point every host kernel
        // already makes on core 0 with its world up, so a port adopts
        // std::thread by doing nothing. It needs a live scheduler and does
        // nothing without one; SDL2Circle_SplitInit, which guarantees one,
        // calls it again.
        SDL2Circle_ThreadRuntimeInit();
    }
}

#else

#error "circle-libsdl2: no 32-bit backend for the per-core C runtime. \
A core handed to an application needs its thread pointer set before the \
application throws, or the first exception dereferences whatever the \
firmware left in the register. AArch64 writes TPIDR_EL0; a 32-bit build \
needs the ARM equivalent (TPIDRURO through CP15, and exception globals \
reached via __aeabi_read_tp rather than a direct read), which is different \
code and not a recompile of the lines above. Write that backend, or build \
64-bit. Do NOT make this a silent stub: unarmed, the failure is a data \
abort on an ordinary throw, on some boards only, and it looks like a \
hardware fault."

#endif
