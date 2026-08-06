//
// timer.cpp — ticks, delays and callback timers over Circle's system timer
// (µs, 64-bit)
//
// WHERE A CALLBACK TIMER RUNS. On a desktop SDL serves SDL_AddTimer from a
// thread of its own, so a callback interrupts whatever the program was doing.
// This platform has no such thread to give it: the application core has one
// line of execution, and the hardware core's is spoken for by the servo that
// keeps the devices running. A kernel timer would fire, but it fires in
// interrupt context, and application callbacks that allocate, log or push
// events must never run there.
//
// So the callbacks run in the application's own line of execution, at the two
// points it is certain to reach and safe to be called at: the per-frame pump
// (SDL_PumpEvents, which SDL_PollEvent calls) and SDL_Delay, which is where a
// program spends the time it is not drawing.
//
// WHAT THAT COSTS. A callback runs late whenever the application is inside a
// single long stretch of work with neither a delay nor an event poll in it —
// loading a level, decompressing a sound. It is never dropped, only deferred,
// and the next deadline is measured from when the callback actually ran, so a
// late tick does not make the following one early. An application that needs a
// timer to interrupt its own computation will not get that here.
//
// Re-entry is refused: a callback may call SDL_Delay or poll for events
// without being called again from inside itself.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "threads.h"
#include <circle/timer.h>
#include <circle/sched/scheduler.h>

extern "C" Uint64 SDL_GetTicks64(void)
{
    return CTimer::GetClockTicks64() / 1000ULL;
}

extern "C" Uint32 SDL_GetTicks(void)
{
    return (Uint32)SDL_GetTicks64();
}

extern "C" Uint64 SDL_GetPerformanceCounter(void)
{
    return CTimer::GetClockTicks64();
}

extern "C" Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000ULL;   // CLOCKHZ — the system timer counts microseconds
}

extern "C" void SDL_Delay(Uint32 ms)
{
    // The scheduler is core-0-only by construction: off core 0 a delay is
    // a plain timed wait on the system timer (a dedicated core has nothing
    // else to run).
    if (SDL2Circle_ThisCore() != 0)
    {
        u64 deadline = CTimer::GetClockTicks64() + (u64)ms * 1000;
        while (CTimer::GetClockTicks64() < deadline)
            asm volatile("yield" ::: "memory");
    }
    else if (CScheduler::IsActive())
    {
        // With the scheduler active, sleeping yields to cooperative peers
        // (audio task, IO thread); without it, plain busy delay.
        CScheduler::Get()->MsSleep(ms);
    }
    else
    {
        CTimer::SimpleMsDelay(ms);
    }

    // One of the two points a callback timer is serviced at, and it comes
    // after the wait so that the callback sees the time that has really
    // passed. See the file header.
    SDL2Circle_TimerService();
}

// ---------------------------------------------------------------------------
// Callback timers
// ---------------------------------------------------------------------------

namespace
{

// A fixed table rather than a list, because a timer may be added or removed
// from inside a callback and a fixed table has nothing to invalidate. It is
// far more than any application has been seen to want; SDL itself has no
// limit, and one that is reached says so rather than losing the timer.
constexpr int MAX_TIMERS = 32;

struct CircleTimer
{
    SDL_TimerID       id;           // 0 when the slot is free
    SDL_TimerCallback callback;
    void             *param;
    Uint32            interval;     // what the callback last asked for
    Uint64            due;          // milliseconds on SDL's own clock
};

SDL_SpinLock s_lock;
CircleTimer  s_timers[MAX_TIMERS];
SDL_TimerID  s_nextID = 1;
bool         s_inService = false;

}   // namespace

extern "C" SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback,
                                    void *param)
{
    if (callback == nullptr)
    {
        SDL_SetError("Passed a NULL timer callback");
        return 0;
    }

    SDL_TimerID id = 0;

    SDL_AtomicLock(&s_lock);
    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (s_timers[i].id != 0)
            continue;

        if (s_nextID == 0)          // an id of zero means "no timer"
            s_nextID = 1;
        id = s_nextID++;

        s_timers[i].id       = id;
        s_timers[i].callback = callback;
        s_timers[i].param    = param;
        s_timers[i].interval = interval;
        s_timers[i].due      = SDL_GetTicks64() + interval;
        break;
    }
    SDL_AtomicUnlock(&s_lock);

    if (id == 0)
        SDL_SetError("SDL_AddTimer: all %d timer slots are in use", MAX_TIMERS);
    return id;
}

extern "C" SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    SDL_bool removed = SDL_FALSE;

    if (id != 0)
    {
        SDL_AtomicLock(&s_lock);
        for (int i = 0; i < MAX_TIMERS; i++)
        {
            if (s_timers[i].id != id)
                continue;
            s_timers[i].id = 0;
            removed = SDL_TRUE;
            break;
        }
        SDL_AtomicUnlock(&s_lock);
    }

    return removed;
}

void SDL2Circle_TimerService(void)
{
    if (s_inService)
        return;
    s_inService = true;

    const Uint64 now = SDL_GetTicks64();

    for (int i = 0; i < MAX_TIMERS; i++)
    {
        // The slot is read under the lock and the callback runs outside it,
        // because a callback is free to add or remove timers of its own.
        SDL_AtomicLock(&s_lock);
        const SDL_TimerID       id       = s_timers[i].id;
        const SDL_TimerCallback callback = s_timers[i].callback;
        void             *const param    = s_timers[i].param;
        const Uint32            interval = s_timers[i].interval;
        const bool              due      = id != 0 && now >= s_timers[i].due;
        SDL_AtomicUnlock(&s_lock);

        if (!due)
            continue;

        // SDL hands the callback the interval it is running at, and takes the
        // return value as the next one. Zero cancels the timer.
        const Uint32 next = callback(interval, param);

        SDL_AtomicLock(&s_lock);
        if (s_timers[i].id == id)       // still the same timer, not replaced
        {
            if (next == 0)
            {
                s_timers[i].id = 0;
            }
            else
            {
                s_timers[i].interval = next;
                s_timers[i].due      = SDL_GetTicks64() + next;
            }
        }
        SDL_AtomicUnlock(&s_lock);
    }

    s_inService = false;
}
