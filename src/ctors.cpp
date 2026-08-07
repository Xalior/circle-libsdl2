//
// ctors.cpp — running the application's static constructors once the kernel
// exists.
//
// The other half of this is sdl-app-init.ld, which splits .init_array so that
// the framework's constructors run before main as usual and the
// application's are held back. That file explains why the split is needed and
// where the line falls. This is what walks the held-back half.
//
// WHEN IT RUNS. From SDL2Circle_ArmCoreRuntime, on core 0, immediately after
// that core's C++ runtime is armed and before any other core is started. An
// application reaches that call from its kernel's Initialize, after the
// timer, the card, the console and stdio are up — so everything a constructor
// might touch exists — and before the core split is armed, so this is plain
// single-core code and logging here is legal.
//
// It costs an application nothing to adopt: the call it already makes to arm
// the runtime is the call that does this. There is no second entry point to
// remember and no file to copy.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"

// The bounds of the deferred array, defined by sdl-app-init.ld.
//
// WEAK, because an application may link a hand-written script that does not
// include the fragment. Then these resolve to nothing, the range is empty,
// and the application simply keeps the behaviour it had — every constructor
// running before main. Declaring them strongly would turn "did not adopt the
// fragment" into a link error, which is a worse answer than "carried on as
// before".
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
        // NO LINE PER CONSTRUCTOR. One was invaluable while the mechanism was
        // being found — a constructor that stops the board leaves its own
        // line as the last thing on the wire, and that is the only way to
        // name it — but it is hundreds of lines through a console that
        // carries very few per second, on every boot of every application.
        // The two lines around this loop say whether the set ran; a boot that
        // stops between them says which half of start-up to look in, and the
        // per-constructor detail can come back behind a switch if it is ever
        // needed again.
        (**pFunc)();
    }

    SDL2Circle_Log("sdl2init", SDL2CIRCLE_LOG_NOTICE,
                   "deferred static constructors complete");
}
