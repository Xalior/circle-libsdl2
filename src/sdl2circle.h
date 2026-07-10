//
// sdl2circle.h — internal glue between the SDL surface and Circle backends.
// Not installed; consumers see only <SDL2/*.h>.
//
// Contract with the host kernel: CInterruptSystem and CTimer are initialized
// before SDL_Init. Everything else (USB, framebuffer, ...) is owned by the
// shim, so an SDL app is self-contained whether chainloaded or SD-booted.
//
#ifndef _sdl2circle_h
#define _sdl2circle_h

#include <circle/types.h>

void SDL2Circle_InputInit(void);   // bring up USB (idempotent)
void SDL2Circle_InputPump(void);   // PnP + translate HID reports to events
void SDL2Circle_AudioPump(void);   // run app audio callback into the queue

// Perf accounting (src/perf.cpp): PMCCNTR-based category split, reported
// through the logger by the pump. Everything not inside an instrumented
// section is attributed to the application ("app" = MAME's emulation).
enum
{
    SDL2CIRCLE_PERF_RENDER = 0,   // texture upload + blit + present
    SDL2CIRCLE_PERF_AUDIO,        // audio callback pump
    SDL2CIRCLE_PERF_INPUT,        // USB PnP + HID translation
    SDL2CIRCLE_PERF_YIELD,        // scheduler yield (other tasks' time)
    SDL2CIRCLE_PERF_NCATS
};

u64 SDL2Circle_PerfCycles(void);
bool SDL2Circle_PerfEnabled(void);
void SDL2Circle_PerfAccumulate(unsigned cat, u64 cycles);
void SDL2Circle_PerfTick(void);

// Dev instrumentation switch (off by default; internal — the shipped SDL
// surface carries no host-side knobs): nSeconds > 0 enables the PMU cycle
// counter and periodic split reports from the pump heartbeat.
extern "C" void SDL2Circle_SetPerfInterval(unsigned nSeconds);

// Scoped section timer: no-op (one branch) while perf is disabled.
class SDL2CirclePerfScope
{
public:
    SDL2CirclePerfScope(unsigned cat)
        : m_cat(cat), m_active(SDL2Circle_PerfEnabled()),
          m_t0(m_active ? SDL2Circle_PerfCycles() : 0)
    {
    }
    ~SDL2CirclePerfScope(void)
    {
        if (m_active)
            SDL2Circle_PerfAccumulate(m_cat, SDL2Circle_PerfCycles() - m_t0);
    }

private:
    unsigned m_cat;
    bool m_active;
    u64 m_t0;
};

#endif
