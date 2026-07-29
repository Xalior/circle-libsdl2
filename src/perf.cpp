//
// perf.cpp — per-core, per-category cycle accounting over the PMU.
//
// Answers "where does each core's time go" by measurement: instrumented
// sections (render, wait, audio pump, input pump, scheduler yield)
// accumulate PMCCNTR_EL0 deltas into the bank of the core they ran on;
// everything unaccounted on a core between reports is that core's own
// uninstrumented compute — the application on its core, kernel and servo
// housekeeping on core 0. IRQ time is not separable on this stack —
// interrupts land inside whichever section they preempt; splitting them
// out needs an IRQ-entry hook Circle does not expose.
//
// The PMU cycle counter is per-core hardware, so each core enables its
// own, lazily, the first time instrumented code runs there. A core that
// never runs shim code never reports — under the single-core build the
// report is core 0 alone, and further cores appear the moment a split
// host puts work on them.
//
// Off by default: SDL2Circle_SetPerfInterval(seconds) — or the
// `rapi-perf=N` boot option — arms it, and reports print through the
// logger from the pump's heartbeat.
//
#include "sdl2circle.h"
#include <circle/logger.h>

#define PERF_MAX_CORES 4

struct SPerfBank
{
    u64 acc[SDL2CIRCLE_PERF_NCATS];
    u64 stampCycles;    // owning core's latest PMCCNTR observation
    u64 lastCycles;     // ... at the last report
    u64 children;       // cycles the scopes nested in the current one used
    bool pmuEnabled;    // this core's cycle counter is running
};

// Each bank is written only by its owning core; the reporter reads the
// others' banks as plain aligned loads (single-copy atomic on AArch64,
// and diagnostics tolerate a frame of staleness).
static SPerfBank s_banks[PERF_MAX_CORES];
static unsigned s_interval;       // seconds; 0 = disabled
static u64 s_lastReportTicks;     // CNTVCT at last report

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

// Enable the calling core's PMU cycle counter at EL1 (long counter, no
// filtering: every cycle counts, IRQs included). Must execute ON the core
// it enables, which lazy-enabling from the instrumented path guarantees.
static void enable_pmu_here(void)
{
    u64 pmcr;
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr |= 1 /*E*/ | (1 << 2) /*C reset*/ | (1 << 6) /*LC*/;
    asm volatile("msr pmcr_el0, %0" ::"r"(pmcr));
    asm volatile("msr pmccfiltr_el0, %0" ::"r"(0ull));
    asm volatile("msr pmcntenset_el0, %0" ::"r"(1ull << 31));
    asm volatile("isb");
}

u64 SDL2Circle_PerfCycles(void)
{
    SPerfBank &bank = s_banks[SDL2Circle_ThisCore() % PERF_MAX_CORES];
    if (!bank.pmuEnabled)
    {
        enable_pmu_here();
        bank.pmuEnabled = true;
    }
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
    SPerfBank &bank = s_banks[SDL2Circle_ThisCore() % PERF_MAX_CORES];
    if (cat < SDL2CIRCLE_PERF_NCATS)
        bank.acc[cat] += cycles;
    // The stamp is how the reporter sees this core's clock without being
    // able to read it: PMCCNTR is core-private, so the owning core
    // publishes its latest reading every time it accounts a section.
    u64 v;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    bank.stampCycles = v;
}

// Nesting tally, per core and touched only by its owner: a scope takes the
// running total when it starts (leaving zero for its own children) and adds
// its whole span back when it ends.
u64 SDL2Circle_PerfChildTake(void)
{
    SPerfBank &bank = s_banks[SDL2Circle_ThisCore() % PERF_MAX_CORES];
    u64 v = bank.children;
    bank.children = 0;
    return v;
}

void SDL2Circle_PerfChildAdd(u64 cycles)
{
    s_banks[SDL2Circle_ThisCore() % PERF_MAX_CORES].children += cycles;
}

extern "C" void SDL2Circle_SetPerfInterval(unsigned nSeconds)
{
    s_interval = nSeconds;
    if (!nSeconds)
        return;

    for (unsigned c = 0; c < PERF_MAX_CORES; c++)
    {
        for (unsigned i = 0; i < SDL2CIRCLE_PERF_NCATS; i++)
            s_banks[c].acc[i] = 0;
        s_banks[c].lastCycles = s_banks[c].stampCycles;
        s_banks[c].children = 0;
    }
    s_lastReportTicks = cntvct();

    // Arm the calling core now; other cores arm themselves on first use.
    u64 base = SDL2Circle_PerfCycles();
    unsigned self = SDL2Circle_ThisCore() % PERF_MAX_CORES;
    s_banks[self].stampCycles = base;
    s_banks[self].lastCycles = base;
}

// Called from the pump heartbeat; prints the frame rate, then one split
// line per core that has run instrumented code since arming.
void SDL2Circle_PerfTick(void)
{
    if (!s_interval)
        return;

    u64 now = cntvct();
    if (now - s_lastReportTicks < (u64)s_interval * cntfrq())
        return;

    // Presented frames since the last report, for the frame rate: the
    // counter lives beside SDL_RenderPresent and costs one increment.
    extern unsigned g_SDL2CirclePresents;
    static unsigned s_lastPresents;
    unsigned frames = g_SDL2CirclePresents - s_lastPresents;
    s_lastPresents = g_SDL2CirclePresents;
    u64 elapsedTicks = now - s_lastReportTicks;
    unsigned fps10 = (unsigned)((u64)frames * 10 * cntfrq() / elapsedTicks);
    s_lastReportTicks = now;

    unsigned self = SDL2Circle_ThisCore() % PERF_MAX_CORES;

    for (unsigned c = 0; c < PERF_MAX_CORES; c++)
    {
        SPerfBank &bank = s_banks[c];
        if (!bank.pmuEnabled)
            continue;

        // The reporter's own clock is read live; every other core's is
        // its published stamp — at most one instrumented section stale.
        u64 cycles = (c == self) ? SDL2Circle_PerfCycles() : bank.stampCycles;
        u64 total = cycles - bank.lastCycles;
        if (!total)
            continue;

        u64 render = bank.acc[SDL2CIRCLE_PERF_RENDER];
        u64 wait = bank.acc[SDL2CIRCLE_PERF_WAIT];
        u64 audio = bank.acc[SDL2CIRCLE_PERF_AUDIO];
        u64 input = bank.acc[SDL2CIRCLE_PERF_INPUT];
        u64 yield = bank.acc[SDL2CIRCLE_PERF_YIELD];
        u64 accounted = render + wait + audio + input + yield;
        u64 app = total > accounted ? total - accounted : 0;

        // Per-mille for one decimal of percent without floats. Wait is
        // printed apart from render on purpose: at a locked frame rate
        // the blocking waits absorb every spare cycle, and folded
        // together they would make the present path impersonate
        // saturation.
        auto pm = [total](u64 v) { return (unsigned)(v * 1000 / total); };
        CLogger::Get()->Write("sdl2perf", LogNotice,
                              "%u.%u fps c%u: cycles %lluM: app %u.%u%% render %u.%u%% wait %u.%u%% audio %u.%u%% input %u.%u%% yield %u.%u%%",
                              fps10 / 10, fps10 % 10, c,
                              total / 1000000,
                              pm(app) / 10, pm(app) % 10,
                              pm(render) / 10, pm(render) % 10,
                              pm(wait) / 10, pm(wait) % 10,
                              pm(audio) / 10, pm(audio) % 10,
                              pm(input) / 10, pm(input) % 10,
                              pm(yield) / 10, pm(yield) % 10);

        for (unsigned i = 0; i < SDL2CIRCLE_PERF_NCATS; i++)
            bank.acc[i] = 0;
        bank.lastCycles = cycles;
    }
}
