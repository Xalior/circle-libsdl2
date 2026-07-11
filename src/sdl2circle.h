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
#include <SDL2/SDL_circle.h>   // the public split/I-O surface backing this glue

void SDL2Circle_InputInit(void);   // bring up USB (idempotent)
void SDL2Circle_InputPump(void);   // PnP + translate HID reports to events
void SDL2Circle_AudioPump(void);   // run app audio callback into the queue

// Debug UART key injection: the host kernel hands us its serial (only for
// --rapi-debug-uart); the pump then types serial-RX bytes into the machine as
// SDL key events. Inert until SetInjectSerial gets a non-null device.
class CSerialDevice;
void SDL2Circle_SetInjectSerial(CSerialDevice *pSerial);
void SDL2Circle_InjectPump(void);

// ---- core split internals (src/split.cpp) ----------------------------------
//
// The split's moving parts, shared between the shim's translation units.
// Everything is inert until SDL2Circle_SplitInit (SDL_circle.h) runs; the
// single-core build keeps today's direct paths.

// Run fn(arg) on core 0 and wait for completion. Direct call when the split
// is inactive or the caller already is core 0; otherwise marshaled through
// the call mailbox to the core-0 servo. Rare-call path (init, window/audio
// creation, file service) — per-frame traffic uses the dedicated rings.
void SDL2Circle_CallOn0(void (*fn)(void *), void *arg);

// Calling core (0 when multicore support is compiled out).
unsigned SDL2Circle_ThisCore(void);

// Cross-core event ring (core 0 producer -> app core consumer).
union SDL_Event;
int  SDL2Circle_EventRingPush(const union SDL_Event *ev);   // 0 if full
int  SDL2Circle_EventRingPop(union SDL_Event *ev);          // 0 if empty

// Consumer-side key/modifier state replay (src/input.cpp): the app core
// mirrors keyboard state from the events it drains, so SDL_GetKeyboardState
// answers locally.
void SDL2Circle_ApplyEventState(const union SDL_Event *ev);

// Audio sample ring (app core producer -> core-0 device feeder).
unsigned SDL2Circle_AudioRingSpace(void);
void     SDL2Circle_AudioRingWrite(const unsigned char *data, unsigned bytes);
unsigned SDL2Circle_AudioRingRead(unsigned char *data, unsigned maxbytes);
void     SDL2Circle_AudioDrain(void);   // core-0 servo: ring -> sound device

// App heartbeat: the app core bumps it once per pump; the core-0 watchdog
// dumps state when it stalls (the split's replacement for the in-band pump
// deadman).
void SDL2Circle_HeartbeatBump(void);

// Presentation: SDL_RenderPresent posts a frame (command list + target
// framebuffer half); the presentation worker executes it and flips.
enum
{
    SDL2CIRCLE_PRESENT_MAX_CMDS = 16
};

struct SDL2CirclePresentCmd
{
    enum { FILL, COPY } op;
    int dx, dy, w, h;             // destination rectangle
    u32 color;                    // FILL
    const u8 *src;                // COPY: pixel buffer (frozen for the frame)
    int srcpitch;
    u8 blend;                     // COPY: straight-alpha blend requested
    u8 alphamod;
};

// Post a frame; blocks (WFE) until the worker has accepted the PREVIOUS
// frame, keeping exactly one frame in flight (two texture buffers suffice).
void SDL2Circle_PresentPost(const SDL2CirclePresentCmd *cmds, unsigned ncmds,
                            unsigned half);

// video.cpp services for the worker: execute one command into a framebuffer
// half; page-flip to a half.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half);
void SDL2Circle_VideoFlip(unsigned half);

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
