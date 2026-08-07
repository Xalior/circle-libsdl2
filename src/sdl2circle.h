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

// Whether SDL2Circle_DeclareVirtualDevice (SDL_circle.h) has been called and
// accepted. SDL_Init asks before it brings anything up: the virtual display
// is what the library is built around and there is no fallback for it, so a
// consumer that has not declared one cannot be started.
bool SDL2Circle_VirtualDeviceDeclared(void);

void SDL2Circle_InputInit(void);   // bring up USB (idempotent)
void SDL2Circle_InputPump(void);   // PnP + translate HID reports to events
void SDL2Circle_AudioPump(void);   // run app audio callback into the queue

// Board hardware — the CPU clock and the case fan (src/hardware.cpp).
// SDL2Circle_HardwareInit (SDL_circle.h) creates the one CCPUThrottle this
// library owns; this drives it. Call it from the per-frame heartbeat that is
// live: the hardware core's servo under the core split, SDL_PumpEvents
// otherwise. It is inert until hardware management has been brought up, and
// carries the whole update policy, so no call site holds an interval of its
// own.
void SDL2Circle_HardwareTick(void);

// Debug UART key injection: the host kernel hands us its serial device, and
// the pump then types serial-RX bytes into the machine as SDL key events.
//
// THE KERNEL HANDS THE DEVICE OVER UNCONDITIONALLY. Whether injection is
// wanted is decided by --rapi-debug-uart, which THIS LIBRARY reads for
// itself out of the boot argument block (src/bootargs.cpp) — a kernel that
// has never heard of the switch still gets it working, and cannot gate the
// device handover on a switch it does not know about. Injection needs both
// halves: the device, and the switch.
//
// A kernel must not construct a serial device to hand over. It lends the one
// it already owns; a second device on the same slot halts the board in its
// constructor.
class CSerialDevice;
void SDL2Circle_SetInjectSerial(CSerialDevice *pSerial);
void SDL2Circle_InjectPump(void);

// ---- boot argument block (src/bootargs.cpp) --------------------------------
//
// The library's own switches, found by the library rather than forwarded to
// it. Idempotent; safe to call from any start-up order.
void SDL2Circle_ReadBootArgs(void);

// The application's static constructors, deferred by sdl-app-init.ld until
// the kernel exists. Called once, on core 0, from SDL2Circle_ArmCoreRuntime.
void SDL2Circle_RunDeferredConstructors(void);
bool SDL2Circle_DebugUartArmed(void);

// ---- core split internals (src/split.cpp) ----------------------------------
//
// The split's moving parts, shared between the shim's translation units.
// Everything is inert until SDL2Circle_SplitInit (SDL_circle.h) runs; the
// single-core build keeps today's direct paths.

// SDL2Circle_CallOn0 — run fn(arg) on core 0 and wait for completion —
// is part of the public split surface (SDL2/SDL_circle.h, included above),
// because a host kernel needs the same mailbox for its own devices.

// Drain every core's log ring into the logger. Core-0 servo only; a no-op
// when the split is inactive, because then nothing rings.
void SDL2Circle_LogDrain(void);

// va_list form of SDL2Circle_Log (SDL2/SDL_circle.h).
extern "C" void SDL2Circle_LogV(const char *from, unsigned severity,
                                const char *fmt, __builtin_va_list args);

// Calling core (0 when multicore support is compiled out).
unsigned SDL2Circle_ThisCore(void);

// Cross-core event ring (core 0 producer -> application core consumer).
union SDL_Event;
int  SDL2Circle_EventRingPush(const union SDL_Event *ev);   // 0 if full
int  SDL2Circle_EventRingPop(union SDL_Event *ev);          // 0 if empty

// Consumer-side key/modifier state replay (src/input.cpp): the application core
// mirrors keyboard state from the events it drains, so SDL_GetKeyboardState
// answers locally.
void SDL2Circle_ApplyEventState(const union SDL_Event *ev);

// Audio sample ring (application core producer -> hardware-core device feeder).
unsigned SDL2Circle_AudioRingSpace(void);
void     SDL2Circle_AudioRingWrite(const unsigned char *data, unsigned bytes);
unsigned SDL2Circle_AudioRingRead(unsigned char *data, unsigned maxbytes);
void     SDL2Circle_AudioDrain(void);   // hardware-core servo: ring -> sound device

// App heartbeat: the application core bumps it once per pump; the hardware-core watchdog
// dumps state when it stalls (the split's replacement for the in-band pump
// deadman).
void SDL2Circle_HeartbeatBump(void);

// Presentation: SDL_RenderPresent posts a frame (command list + target
// framebuffer half); the presentation worker executes it and flips.
//
// TWO DIFFERENT LIMITS, and keeping them apart is the whole of how the
// crossing is tuned.
//
//   RECORD_MAX_CMDS   how many draw calls the recorder can hold before it
//                     gives up and paints instead. It is a property of the
//                     recorder and never changes. Recording is also what
//                     lets a finished frame be RECOGNISED — a clear plus one
//                     opaque blit is the whole picture already, sitting in
//                     the application's own texture, and spotting that is
//                     what saves painting it again. Recognising needs a
//                     couple of recorded commands, nothing like this many.
//
//   PRESENT_MAX_CMDS  how many commands may CROSS to the presentation core
//                     as a list. This is the knob. A frame whose draw list
//                     fits within it crosses as a list and is composed on
//                     the far side; a frame that does not fit crosses as a
//                     finished picture instead.
//
// ZERO IS THE INTERESTING END and it is the default: every frame crosses as
// a picture. It costs nothing extra, because recording still happens and the
// simple shape is still recognised — the frame that crosses is the
// application's texture exactly where it already sits, and nothing is
// painted. Raising the knob moves composition across to the presentation
// core, frame by frame, up to the recorder's own capacity.
//
// They were one value, so a low count starved the recogniser as well as the
// crossing: at zero the recorder gave up on the first draw call, the simple
// shape could never be seen, and every frame paid for a full canvas paint it
// did not need. That is why the knob could only usefully be set high.
//
// Set the knob with `make PRESENT_CMDS=n`. It is baked into the archive and
// is not part of any installed header, so a consumer's own translation units
// neither see it nor need to match it.
// A macro and not an enum: the bounds check below is a preprocessor
// comparison, and the preprocessor cannot see an enumerator — it reads the
// name as an undefined identifier worth zero, which quietly turns the check
// into "any count above zero is out of range".
#define SDL2CIRCLE_RECORD_MAX_CMDS 16

#ifndef SDL2CIRCLE_PRESENT_MAX_CMDS
#define SDL2CIRCLE_PRESENT_MAX_CMDS 0
#endif

#if SDL2CIRCLE_PRESENT_MAX_CMDS < 0 \
 || SDL2CIRCLE_PRESENT_MAX_CMDS > SDL2CIRCLE_RECORD_MAX_CMDS
#error SDL2CIRCLE_PRESENT_MAX_CMDS must be between 0 and SDL2CIRCLE_RECORD_MAX_CMDS
#endif

struct SDL2CirclePresentCmd
{
    enum { FILL, COPY } op;
    int dx, dy, w, h;             // destination rectangle, SCANOUT coordinates
    u32 color;                    // FILL
    const u8 *src;                // COPY: pixel buffer (frozen for the frame)
    int srcpitch;
    int sw, sh;                   // COPY: source extent. Equal to w,h means an
                                  // unscaled blit; otherwise the executor
                                  // resamples sw x sh onto w x h, which already
                                  // composes both geometry hops (application
                                  // frame into the canvas, canvas onto the
                                  // scanout) into ONE pass.
    u8 blend;                     // COPY: straight-alpha blend requested
    u8 alphamod;
};

// Post a frame; blocks (WFE) until the worker has accepted the PREVIOUS
// frame, keeping exactly one frame in flight (two texture buffers suffice).
void SDL2Circle_PresentPost(const SDL2CirclePresentCmd *cmds, unsigned ncmds,
                            unsigned half);

// Block until the presentation worker holds nothing of the poster's: every
// posted frame consumed AND flipped. Teardown that frees something the
// worker may still be reading — a texture's pixels, the window — calls this
// first. A no-op without the split, where presentation is in-band anyway.
void SDL2Circle_PresentQuiesce(void);

// Frame sequences, for deciding when a posted texture store may be written
// again. See the comment above their definitions in split.cpp.
u64 SDL2Circle_PresentPostedSeq(void);
u64 SDL2Circle_PresentAckedSeq(void);
void SDL2Circle_PresentWaitAck(u64 seq);

// video.cpp services for the worker: execute one command into a framebuffer
// half; page-flip to a half.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half);
void SDL2Circle_VideoFlip(unsigned half);

// Perf accounting (src/perf.cpp): PMCCNTR-based category split, reported
// through the logger by the pump. Everything not inside an instrumented
// section is attributed to the application ("app" = the application itself).
enum
{
    SDL2CIRCLE_PERF_RENDER = 0,   // texture upload + blit + present compute
    SDL2CIRCLE_PERF_WAIT_DMA,     // blocking on the outstanding transfer to
                                  // the framebuffer finishing
    SDL2CIRCLE_PERF_WAIT_VSYNC,   // blocking on the raster reaching the
                                  // vertical blanking interval
    SDL2CIRCLE_PERF_WAIT,         // blocking on another core: the frame
                                  // mailbox in both directions. Kept apart
                                  // from the two above because it is a wait
                                  // on software, not on the display.
                                  // Historically: blocking inside present,
                                  // outstanding-DMA wait. Split out because
                                  // at a locked frame rate these absorb all
                                  // slack and would otherwise make render
                                  // impersonate saturation.
    SDL2CIRCLE_PERF_AUDIO,        // audio callback pump, and the feed to
                                  // the sound device on the hardware core
    SDL2CIRCLE_PERF_INPUT,        // USB PnP + HID translation
    SDL2CIRCLE_PERF_SERVE,        // hardware core answering another core:
                                  // the call mailbox and the log drain
    SDL2CIRCLE_PERF_YIELD,        // scheduler yield (other tasks' time)
    SDL2CIRCLE_PERF_NCATS
};

u64 SDL2Circle_PerfCycles(void);
bool SDL2Circle_PerfEnabled(void);
void SDL2Circle_PerfAccumulate(unsigned cat, u64 cycles);
void SDL2Circle_PerfTick(void);

// Nested-section bookkeeping for the calling core. A scope hands its own
// elapsed cycles up when it ends and takes back what its children spent, so
// an inner section's time belongs to the inner category ALONE. Without
// this, a wait inside a render would be counted in both and the categories
// would sum past the core's real cycles.
u64  SDL2Circle_PerfChildTake(void);            // reset and return the tally
void SDL2Circle_PerfChildAdd(u64 cycles);       // add to the enclosing tally

// Dev instrumentation switch (off by default; internal — the shipped SDL
// surface carries no host-side knobs): nSeconds > 0 enables the PMU cycle
// counter and periodic split reports from the pump heartbeat.
extern "C" void SDL2Circle_SetPerfInterval(unsigned nSeconds);

// Scoped section timer: no-op (one branch) while perf is disabled.
//
// Scopes nest — a wait sits inside the present that is waiting — and each
// one keeps only the cycles its own body spent: it starts its children's
// tally fresh, subtracts whatever they report, and hands its whole span up
// to its own parent. So the categories partition a core's cycles instead of
// overlapping, and what is left over is genuinely uninstrumented.
class SDL2CirclePerfScope
{
public:
    SDL2CirclePerfScope(unsigned cat)
        : m_cat(cat), m_active(SDL2Circle_PerfEnabled()),
          m_t0(m_active ? SDL2Circle_PerfCycles() : 0),
          m_outerChildren(m_active ? SDL2Circle_PerfChildTake() : 0)
    {
    }
    ~SDL2CirclePerfScope(void)
    {
        if (!m_active)
            return;
        u64 span = SDL2Circle_PerfCycles() - m_t0;
        u64 children = SDL2Circle_PerfChildTake();
        SDL2Circle_PerfAccumulate(m_cat, span > children ? span - children : 0);
        SDL2Circle_PerfChildAdd(m_outerChildren + span);
    }

private:
    unsigned m_cat;
    bool m_active;
    u64 m_t0;
    u64 m_outerChildren;
};

#endif
