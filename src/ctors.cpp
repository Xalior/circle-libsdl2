//
// ctors.cpp - running the application's static constructors once the kernel
// exists.
//
// The other half of this is sdl-app-init.ld, which splits .init_array so that
// the framework's constructors run before main as usual and the
// application's are held back. That file explains why the split is needed and
// where the line falls. This is what walks the held-back half.
//
// Runs from SDL2Circle_ArmCoreRuntime, on core 0, immediately after that
// core's C++ runtime is armed and before any other core is started. An
// application reaches that call from its kernel's Initialize, after the
// timer, the card, the console and stdio are up, so everything a constructor
// might touch exists, and before the core split is armed, so this runs as
// plain single-core code and logging here is legal.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"

// The bounds of the deferred array, defined by sdl-app-init.ld.
//
// Declared weak: an application may link a hand-written script that does
// not include the fragment, in which case these resolve to nothing, the
// range is empty, and the application keeps its original behaviour of every
// constructor running before main.
//
// They are declared as data and used only by address: a linker-defined symbol
// has no contents, only a location.
extern "C" void (*__sdl2_deferred_init_start)(void) __attribute__((weak));
extern "C" void (*__sdl2_deferred_init_end)(void) __attribute__((weak));

void SDL2Circle_RunDeferredConstructors(void)
{
    // Once per boot. The arming call this hangs off is idempotent per core,
    // but nothing else guarantees a second one could not arrive.
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

    void (**pStart)(void) = &__sdl2_deferred_init_start;
    void (**pEnd)(void) = &__sdl2_deferred_init_end;

    if (pStart == nullptr || pEnd == nullptr || pEnd <= pStart)
        return;   // no fragment, or nothing was deferred

    const unsigned nCount = (unsigned)(pEnd - pStart);
    SDL2Circle_Log("sdl2init", SDL2CIRCLE_LOG_NOTICE,
                   "running %u deferred static constructor(s)", nCount);

    for (void (**pFunc)(void) = pStart; pFunc < pEnd; pFunc++)
    {
        // No log line per constructor: a console that carries only a few
        // lines per second would spend hundreds of lines on every boot. The
        // two lines around this loop show whether the set ran, so a boot
        // that stops between them narrows down which half of start-up to
        // look in; per-constructor detail can be added behind a switch if
        // it is needed again.
        (**pFunc)();
    }

    SDL2Circle_Log("sdl2init", SDL2CIRCLE_LOG_NOTICE,
                   "deferred static constructors complete");
}
