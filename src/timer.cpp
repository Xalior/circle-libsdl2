//
// timer.cpp — ticks and delays over Circle's system timer (µs, 64-bit)
//
#include <SDL2/SDL.h>
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
    // With the scheduler active, sleeping yields to cooperative peers
    // (audio task, IO thread); without it, plain busy delay.
    if (CScheduler::IsActive())
        CScheduler::Get()->MsSleep(ms);
    else
        CTimer::SimpleMsDelay(ms);
}
