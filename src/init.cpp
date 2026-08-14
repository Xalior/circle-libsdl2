//
// init.cpp - subsystem bookkeeping, version, platform - and the two clocks
// this library serves for code that runs before the host kernel exists.
//
// The two feature macros come before every include, and they have to: newlib
// hides clockid_t and clock_gettime behind them, and a definition written
// without them either does not compile or compiles into a second, differently
// typed function that the C library's own header would then clash with. The
// C library this project builds against is configured the same way, in its
// own clock_gettime.cpp, for the same reason.
#define _POSIX_TIMERS 1
#define _POSIX_MONOTONIC_CLOCK 200112L

#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include <circle/koptions.h>
#include <circle/timer.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

static Uint32 s_initialized = 0;

// ---- Wall clock before the kernel exists -----------------------------------
//
// newlib's time()/gettimeofday() reach the hardware through circle-newlib's
// _gettimeofday, whose first act is CTimer::Get()->GetUniversalTime() - and
// both Get() and that call die when no CTimer exists yet. Static constructors
// run exactly that early, and real applications keep a clock call there:
// srand(time(NULL)) at global scope is idiomatic C. With no exception handler
// installed that early either, the result is a board that dies silently
// before its first line of output.
//
// This override, linked from the shim's always-used init object so it wins
// over the libgloss version, makes every clock call safe:
//
//  - Before SDL_Init (the host contract: CInterruptSystem and CTimer exist
//    before SDL_Init - see sdl2circle.h), time is served from the free-
//    running hardware counter, which ticks from power-on and needs neither
//    an object nor an interrupt, seeded with this library's build time. A
//    Pi has no battery RTC, so this is the clock "set once at the factory":
//    wrong by the shelf age of the kernel image, ticking truly, and never
//    1970.
//
//  - From SDL_Init on, calls delegate to the kernel's CTimer - unless its
//    clock is obviously unset (before this library was even built), in
//    which case the factory clock keeps serving. A host kernel that sets
//    real time (CTimer::SetTime, an RTC, NTP) always wins.
//
// The Pi 5 makes the pre-main window especially unforgiving: the header
// UART sits behind RP1, so even touching the serial console from a static
// constructor is fatal. The clock is the one service the hardware gives us
// for free that early - so the shim serves it.

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

// True once SDL_Init has run - the host contract guarantees CTimer::Get()
// is safe from then on. Before that, Get() itself may assert.
static bool s_bKernelTimerUp = false;

namespace
{

struct KernelTimeArgs
{
    unsigned seconds;
    unsigned micros;
    bool     ok;
};

// Core 0's half of the calendar read: the object, its lock and the interrupt
// that advances it all belong here.
void read_kernel_time_on0(void *p)
{
    auto *a = (KernelTimeArgs *)p;
    a->ok = CTimer::Get()->GetUniversalTime(&a->seconds, &a->micros) != FALSE;
}

}   // namespace

bool SDL2Circle_KernelTimeUTC(unsigned *pSeconds, unsigned *pMicroSeconds)
{
    if (!s_bKernelTimerUp)
        return false;

    KernelTimeArgs a{0, 0, false};
    SDL2Circle_CallOn0(read_kernel_time_on0, &a);
    if (!a.ok)
        return false;

    if (pSeconds)
        *pSeconds = a.seconds;
    if (pMicroSeconds)
        *pMicroSeconds = a.micros;
    return true;
}

extern "C" int _gettimeofday(struct timeval *ptimeval, void *ptimezone)
{
    (void) ptimezone;

    // Delegate to the kernel's clock once it exists and has a plausible
    // time. A day of slack covers a host kernel image built shortly before
    // this library. The read itself happens on core 0 - see
    // SDL2Circle_KernelTimeUTC - because the clock is a device.
    {
        unsigned nSeconds = 0, nMicroSeconds = 0;
        if (SDL2Circle_KernelTimeUTC(&nSeconds, &nMicroSeconds)
            && nSeconds + 86400 >= BuildEpoch())
        {
            ptimeval->tv_sec  = (time_t) nSeconds;
            ptimeval->tv_usec = (suseconds_t) nMicroSeconds;
            return 0;
        }
    }

    // The factory clock: hardware counter elapsed since first use, on top
    // of the build time. CTimer::GetClockTicks64 is static and reads the
    // free-running counter directly - valid from the first instruction.
    static u64 s_nFirstTicks = 0;
    if (s_nFirstTicks == 0)
        s_nFirstTicks = CTimer::GetClockTicks64();
    u64 nElapsed = CTimer::GetClockTicks64() - s_nFirstTicks;

    ptimeval->tv_sec  = (time_t)(BuildEpoch() + nElapsed / CLOCKHZ);
    ptimeval->tv_usec = (suseconds_t)(nElapsed % CLOCKHZ);
    return 0;
}

// ---- The monotonic clock ---------------------------------------------------
//
// The same override, for the same reason, from the same always-linked object:
// this is the clock_gettime every consumer of this library reaches.
//
// The C library's own clock_gettime answers CLOCK_REALTIME and
// CLOCK_MONOTONIC and refuses every other clock id, and the refusal returns
// -1 without writing the timespec. A caller that does not check the return -
// most of them, because a monotonic clock is not expected to fail - then
// reads whatever its own stack held. Read the clock twice from the same
// stack frame and the same bytes come back both times, so the clock appears
// to have stopped: any "spin until the clock has advanced" loop runs
// forever, on whichever core it is on, printing nothing. That is a silent
// hang with no evidence at all, and it is not an exotic way to reach one -
// the header this project builds against defines CLOCK_MONOTONIC_RAW, so an
// ordinary portable program asks for the refused id by preference.
//
// So every id this board can answer meaningfully gets an answer:
//
//   REALTIME, REALTIME_COARSE     the wall clock above, whatever is currently
//                                 serving it.
//   MONOTONIC, and its RAW and    time since power-on, read straight from the
//   COARSE spellings, BOOTTIME    free-running system counter. Nothing here
//                                 adjusts, slews or suspends that counter, so
//                                 the distinctions those ids draw on a desktop
//                                 do not exist on this board and one honest
//                                 reading serves all of them.
//   PROCESS_CPUTIME_ID,           the same counter: this machine runs one
//   THREAD_CPUTIME_ID             program from power-on and nothing else, so
//                                 the program's CPU time IS the uptime.
//
// An id outside that set is still refused, because inventing an answer for a
// clock nobody can name is worse than saying no. But the timespec is zeroed
// before the refusal, so a caller that ignores the return reads a defined
// value instead of its own stack.
extern "C" int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    if (tp == nullptr)
    {
        errno = EINVAL;
        return -1;
    }

    switch (clock_id)
    {
    case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_COARSE
    case CLOCK_REALTIME_COARSE:
#endif
    {
        struct timeval tv;
        if (_gettimeofday(&tv, nullptr) != 0)
            break;
        tp->tv_sec  = tv.tv_sec;
        tp->tv_nsec = (long)tv.tv_usec * 1000L;
        return 0;
    }

    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    case CLOCK_MONOTONIC_COARSE:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
#ifdef CLOCK_PROCESS_CPUTIME_ID
    case CLOCK_PROCESS_CPUTIME_ID:
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    case CLOCK_THREAD_CPUTIME_ID:
#endif
    {
        // Static, and a direct read of the free-running counter: valid on
        // every core and from the first instruction, before any CTimer
        // object exists to be asked.
        const u64 nTicks = CTimer::GetClockTicks64();
        tp->tv_sec  = (time_t)(nTicks / CLOCKHZ);
        tp->tv_nsec = (long)((nTicks % CLOCKHZ) * (1000000000ULL / CLOCKHZ));
        return 0;
    }

    default:
        break;
    }

    tp->tv_sec  = 0;
    tp->tv_nsec = 0;
    errno = EINVAL;
    return -1;
}

static void init_input_on0(void *)
{
    SDL2Circle_InputInit();
}

extern "C" int SDL_InitSubSystem(Uint32 flags)
{
    // SDL.h says twice that video brings the event subsystem up with it, and
    // it does. Recording it is what makes SDL_WasInit answer honestly, and a
    // great many applications ask that question before deciding whether to
    // bring events up themselves.
    if (flags & SDL_INIT_VIDEO)
        flags |= SDL_INIT_EVENTS;

    // The library's own boot switches, found by the library rather than
    // forwarded to it, so an application that has never heard of one still
    // gets it. Idempotent, and done before anything is brought up: a switch
    // that changes how a subsystem starts has to be known first.
    SDL2Circle_ReadBootArgs();

    // No virtual display size is needed here: initialising video is not
    // taking the display - creating a window is (src/video.cpp), and that is
    // where the virtual framebuffer's size is settled and where the console
    // hands the screen over. An application may bring video up and never
    // create a window at all; nothing below depends on a size existing yet.

    // Board hardware - the CPU clock and the case fan - is brought up from
    // SDL2Circle_ArmCoreRuntime, not here. Every host kernel already makes
    // that call on core 0 before running anything else (see coreruntime.cpp),
    // which is earlier than any SDL_Init an application can issue, so by the
    // time this function runs the clock is already at the rate the
    // application is going to have; a host kernel that needed it earlier
    // still (CSDL2CircleHardware, or a direct SDL2Circle_HardwareInit call)
    // has already brought it up, and both routes are idempotent.

    // Video/window devices come up lazily in SDL_CreateWindow. USB is not
    // brought up here at all: the host kernel owns the controller and has
    // already initialised it, and this only finds it - see
    // SDL2Circle_InputInit. Still marshalled, because what it finds is core
    // 0's, but it is a lookup rather than a device bring-up, so the servo's
    // first lap has nothing in it that can block.
    if (flags & (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK
                 | SDL_INIT_GAMECONTROLLER))
    {
        SDL2Circle_CallOn0(init_input_on0, nullptr);

        // A board that asked for robot hands and has no controller to give
        // them to does not start the application. Checked here, back on the
        // calling core, rather than inside the call above: that runs on core
        // 0, and a stop there would take the console and the servo with it
        // and leave the board silent. The halt says why it is stopping,
        // repeats it for as long as the board is powered, and never returns.
        if (SDL2Circle_NoInputFatal())
            SDL2Circle_NoInputHalt();
    }

    // The host contract (sdl2circle.h) makes this the earliest moment the
    // kernel's CTimer is guaranteed to exist: the wall clock may delegate
    // from here on.
    s_bKernelTimerUp = true;

    // Performance reports stay silent unless the host arms them through
    // SDL2Circle_SetPerfInterval. The library reads no boot configuration
    // for this: cmdline.txt describes the machine, and how an instrument
    // is switched on - a stamped defaults block, a host option, nothing at
    // all - is the host's design, not the library's.

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
