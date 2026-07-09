//
// events.cpp — event queue skeleton.
//
// Event queue plus SDL_PumpEvents, the shim's per-frame heartbeat.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include <circle/sched/scheduler.h>
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

    SDL2Circle_InputPump();
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
