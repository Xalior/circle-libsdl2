//
// events.cpp — event queue skeleton.
//
// Event queue plus SDL_PumpEvents, the shim's per-frame heartbeat.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include <circle/sched/scheduler.h>
#include <circle/cputhrottle.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <cstring>

namespace
{
constexpr unsigned QUEUE_SIZE = 256;

SDL_Event s_queue[QUEUE_SIZE];
unsigned s_head = 0;   // next slot to read
unsigned s_count = 0;
}

extern "C" int SDL_PushEvent(SDL_Event *event)
{
    if (s_count >= QUEUE_SIZE)
        return SDL_SetError("event queue full");
    s_queue[(s_head + s_count) % QUEUE_SIZE] = *event;
    s_count++;
    return 1;
}

extern "C" void SDL_PumpEvents(void)
{
    // The shim's cooperative heartbeat: called every frame by any SDL app
    // (via SDL_PollEvent), it services USB plug-and-play, translates HID
    // reports, and yields so cooperative std::threads make progress.
    if (CScheduler::IsActive())
        CScheduler::Get()->Yield();

    // Liveness beacon + deadman: a debug line every 10 s proves the app's
    // main loop is still pumping. A kernel timer re-armed on every beat
    // fires from IRQ context if the pump goes silent for 30 s and dumps
    // the scheduler's task list — the wedged system's own post-mortem.
    {
        static u64 lastBeat = 0;
        static TKernelTimerHandle deadman = 0;
        u64 now = CTimer::GetClockTicks64();
        if (now - lastBeat > 10000000)
        {
            lastBeat = now;
            CLogger::Get()->Write("sdl2", LogDebug, "pump alive t=%us",
                                  (unsigned)(now / 1000000));

            if (deadman != 0)
                CTimer::Get()->CancelKernelTimer(deadman);
            deadman = CTimer::Get()->StartKernelTimer(
                30 * HZ,
                [](TKernelTimerHandle, void *, void *) {
                    CLogger::Get()->Write("sdl2", LogError,
                                          "PUMP STALLED 30s -- task dump:");
                    if (CScheduler::IsActive())
                        CScheduler::Get()->ListTasks(CLogger::Get()->GetTarget());
                });
        }
    }

    // Tick the host kernel's CPU throttle (if it created one) so thermal
    // management actually runs — Circle requires periodic Update() calls.
    CCPUThrottle *throttle = CCPUThrottle::Get();
    if (throttle)
    {
        static u64 lastUpdate = 0;
        u64 now = CTimer::GetClockTicks64();
        if (now - lastUpdate > 2000000)   // every 2 s
        {
            lastUpdate = now;
            throttle->Update();
        }
    }

    SDL2Circle_InputPump();
    SDL2Circle_AudioPump();
}

extern "C" int SDL_PollEvent(SDL_Event *event)
{
    SDL_PumpEvents();
    if (s_count == 0)
        return 0;
    if (event)
    {
        *event = s_queue[s_head];
        s_head = (s_head + 1) % QUEUE_SIZE;
        s_count--;
    }
    return 1;
}

extern "C" Uint8 SDL_EventState(Uint32, int state)
{
    return (state == SDL_QUERY) ? SDL_ENABLE : (Uint8)state;
}

extern "C" void SDL_FlushEvent(Uint32) {}
extern "C" void SDL_FlushEvents(Uint32, Uint32) {}
