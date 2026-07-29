//
// init.cpp — subsystem bookkeeping, version, platform — and the wall clock
// for code that runs before the host kernel exists.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include <circle/koptions.h>
#include <circle/timer.h>
#include <sys/time.h>

static Uint32 s_initialized = 0;

// ---- Wall clock before the kernel exists -----------------------------------
//
// newlib's time()/gettimeofday() reach the hardware through circle-newlib's
// _gettimeofday, whose first act is CTimer::Get()->GetUniversalTime() — and
// both Get() and that call die when no CTimer exists yet. Static constructors
// run exactly that early, and real applications keep a clock call there:
// srand(time(NULL)) at global scope is idiomatic C. With no exception handler
// installed that early either, the result is a board that dies silently
// before its first line of output.
//
// This override, linked from the shim's always-used init object so it wins
// over the libgloss version, makes every clock call safe and USEFUL:
//
//  - Before SDL_Init (the host contract: CInterruptSystem and CTimer exist
//    before SDL_Init — see sdl2circle.h), time is served from the free-
//    running hardware counter, which ticks from power-on and needs neither
//    an object nor an interrupt, seeded with this library's build time. A
//    Pi has no battery RTC, so this is the clock "set once at the factory":
//    wrong by the shelf age of the kernel image, ticking truly, and never
//    1970.
//
//  - From SDL_Init on, calls delegate to the kernel's CTimer — unless its
//    clock is obviously unset (before this library was even built), in
//    which case the factory clock keeps serving. A host kernel that sets
//    real time (CTimer::SetTime, an RTC, NTP) always wins.
//
// The Pi 5 makes the pre-main window especially unforgiving: the header
// UART sits behind RP1, so even touching the serial console from a static
// constructor is fatal. The clock is the one service the hardware gives us
// for free that early — so the shim serves it.

// Build-timestamp epoch (seconds since 1970-01-01 UTC) from __DATE__/__TIME__.
static unsigned BuildEpoch(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;   // "Mmm dd yyyy"
    const char *t = __TIME__;   // "hh:mm:ss"

    int mon = 1;
    for (int i = 0; i < 12; i++)
        if (d[0] == months[i*3] && d[1] == months[i*3+1] && d[2] == months[i*3+2])
            { mon = i + 1; break; }
    int day  = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    int year = (d[7]-'0')*1000 + (d[8]-'0')*100 + (d[9]-'0')*10 + (d[10]-'0');
    int hh = (t[0]-'0')*10 + (t[1]-'0');
    int mm = (t[3]-'0')*10 + (t[4]-'0');
    int ss = (t[6]-'0')*10 + (t[7]-'0');

    // days since 1970-01-01 (civil-to-days, treated as UTC)
    int y = year - (mon <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    long days = (long)era*146097 + (long)doe - 719468;
    return (unsigned)(days * 86400L + hh*3600 + mm*60 + ss);
}

// True once SDL_Init has run — the host contract guarantees CTimer::Get()
// is safe from then on. Before that, Get() itself may assert.
static bool s_bKernelTimerUp = false;

extern "C" int _gettimeofday(struct timeval *ptimeval, void *ptimezone)
{
    (void) ptimezone;

    // Delegate to the kernel's clock once it exists and has a plausible
    // time. A day of slack covers a host kernel image built shortly before
    // this library.
    if (s_bKernelTimerUp)
    {
        unsigned nSeconds, nMicroSeconds;
        if (CTimer::Get()->GetUniversalTime(&nSeconds, &nMicroSeconds)
            && nSeconds + 86400 >= BuildEpoch())
        {
            ptimeval->tv_sec  = (time_t) nSeconds;
            ptimeval->tv_usec = (suseconds_t) nMicroSeconds;
            return 0;
        }
    }

    // The factory clock: hardware counter elapsed since first use, on top
    // of the build time. CTimer::GetClockTicks64 is static and reads the
    // free-running counter directly — valid from the first instruction.
    static u64 s_nFirstTicks = 0;
    if (s_nFirstTicks == 0)
        s_nFirstTicks = CTimer::GetClockTicks64();
    u64 nElapsed = CTimer::GetClockTicks64() - s_nFirstTicks;

    ptimeval->tv_sec  = (time_t)(BuildEpoch() + nElapsed / CLOCKHZ);
    ptimeval->tv_usec = (suseconds_t)(nElapsed % CLOCKHZ);
    return 0;
}

static void init_input_on0(void *)
{
    SDL2Circle_InputInit();
}

extern "C" int SDL_InitSubSystem(Uint32 flags)
{
    // Video/window devices come up lazily in SDL_CreateWindow; USB comes up
    // here so keyboards enumerate while the app is still initializing.
    // USB (xHCI, interrupts) belongs to core 0: under the core split this
    // marshals to the servo, otherwise it is a direct call.
    if (flags & (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK
                 | SDL_INIT_GAMECONTROLLER))
        SDL2Circle_CallOn0(init_input_on0, nullptr);

    // The host contract (sdl2circle.h) makes this the earliest moment the
    // kernel's CTimer is guaranteed to exist: the wall clock may delegate
    // from here on.
    s_bKernelTimerUp = true;

    // Performance receipts stay silent unless the HOST arms them through
    // SDL2Circle_SetPerfInterval. The library reads no boot configuration
    // for this: cmdline.txt describes the machine, and how an instrument
    // is switched on — a stamped defaults block, a host option, nothing at
    // all — is the host's design, not the library's.

    s_initialized |= flags;
    return 0;
}

extern "C" int SDL_Init(Uint32 flags)
{
    return SDL_InitSubSystem(flags);
}

extern "C" void SDL_QuitSubSystem(Uint32 flags)
{
    s_initialized &= ~flags;
}

extern "C" Uint32 SDL_WasInit(Uint32 flags)
{
    return s_initialized & (flags ? flags : ~0u);
}

extern "C" void SDL_Quit(void)
{
    s_initialized = 0;
}

extern "C" void SDL_GetVersion(SDL_version *ver)
{
    ver->major = SDL_MAJOR_VERSION;
    ver->minor = SDL_MINOR_VERSION;
    ver->patch = SDL_PATCHLEVEL;
}

extern "C" const char *SDL_GetRevision(void)
{
    return "circle-libsdl2";
}

extern "C" const char *SDL_GetPlatform(void)
{
    return "Circle";
}
