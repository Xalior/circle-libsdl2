//
// perf.cpp — per-category cycle accounting over the PMU cycle counter.
//
// Answers "where does core-0 time go" by measurement: instrumented
// sections (render, audio pump, input pump, scheduler yield) accumulate
// PMCCNTR_EL0 deltas; everything unaccounted between reports is the
// application's own compute (for MAME: the emulation loop). IRQ time is
// not separable on this stack — interrupts land inside whichever
// section they preempt; splitting them out needs an IRQ-entry hook
// Circle does not expose.
//
// Off by default: SDL2Circle_SetPerfInterval(seconds) enables the PMU
// cycle counter (EL1) and periodic reports through the logger from the
// pump's heartbeat.
//
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"
#include <circle/logger.h>

static u64 s_acc[SDL2CIRCLE_PERF_NCATS];
static unsigned s_interval;       // seconds; 0 = disabled
static u64 s_lastReportTicks;     // CNTVCT at last report
static u64 s_lastReportCycles;    // PMCCNTR at last report

static inline u64 cntvct(void)
{
    u64 v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline u64 cntfrq(void)
{
    u64 v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

u64 SDL2Circle_PerfCycles(void)
{
    u64 v;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

bool SDL2Circle_PerfEnabled(void)
{
    return s_interval != 0;
}

void SDL2Circle_PerfAccumulate(unsigned cat, u64 cycles)
{
    if (cat < SDL2CIRCLE_PERF_NCATS)
        s_acc[cat] += cycles;
}

extern "C" void SDL2Circle_SetPerfInterval(unsigned nSeconds)
{
    s_interval = nSeconds;
    if (!nSeconds)
        return;

    // Enable the PMU cycle counter at EL1 (long counter, no filtering:
    // every core-0 cycle counts, IRQs included).
    u64 pmcr;
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr |= 1 /*E*/ | (1 << 2) /*C reset*/ | (1 << 6) /*LC*/;
    asm volatile("msr pmcr_el0, %0" ::"r"(pmcr));
    asm volatile("msr pmccfiltr_el0, %0" ::"r"(0ull));
    asm volatile("msr pmcntenset_el0, %0" ::"r"(1ull << 31));
    asm volatile("isb");

    s_lastReportTicks = cntvct();
    s_lastReportCycles = SDL2Circle_PerfCycles();
    for (unsigned i = 0; i < SDL2CIRCLE_PERF_NCATS; i++)
        s_acc[i] = 0;
}

// Called from the pump heartbeat; prints one split line per interval.
void SDL2Circle_PerfTick(void)
{
    if (!s_interval)
        return;

    u64 now = cntvct();
    if (now - s_lastReportTicks < (u64)s_interval * cntfrq())
        return;

    u64 cycles = SDL2Circle_PerfCycles();
    u64 total = cycles - s_lastReportCycles;
    if (!total)
        return;

    u64 render = s_acc[SDL2CIRCLE_PERF_RENDER];
    u64 audio = s_acc[SDL2CIRCLE_PERF_AUDIO];
    u64 input = s_acc[SDL2CIRCLE_PERF_INPUT];
    u64 yield = s_acc[SDL2CIRCLE_PERF_YIELD];
    u64 accounted = render + audio + input + yield;
    u64 app = total > accounted ? total - accounted : 0;

    // Per-mille for one decimal of percent without floats.
    auto pm = [total](u64 v) { return (unsigned)(v * 1000 / total); };
    CLogger::Get()->Write("sdl2perf", LogNotice,
                          "cycles %lluM: app %u.%u%% render %u.%u%% audio %u.%u%% input %u.%u%% yield %u.%u%%",
                          total / 1000000,
                          pm(app) / 10, pm(app) % 10,
                          pm(render) / 10, pm(render) % 10,
                          pm(audio) / 10, pm(audio) % 10,
                          pm(input) / 10, pm(input) % 10,
                          pm(yield) / 10, pm(yield) % 10);

    for (unsigned i = 0; i < SDL2CIRCLE_PERF_NCATS; i++)
        s_acc[i] = 0;
    s_lastReportTicks = now;
    s_lastReportCycles = cycles;
}
