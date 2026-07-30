//
// SDL_circle.h — Circle-platform extensions to the SDL surface.
//
// Two consumer groups:
//
//   Host kernels (multicore builds) drive the CORE SPLIT: the application
//   runs on a dedicated secondary core while core 0 keeps the Circle world
//   (scheduler, IRQs, USB, EMMC/FatFs, sound). The shim owns every ring,
//   lock and wake primitive; the application calls plain SDL_* functions
//   and never knows a second core exists.
//
//   Applications (or their platform adapters) get the I/O SERVICE: a small
//   blocking file/directory API valid from ANY core. Off core 0 the call is
//   marshaled to the hardware-core servo, which is the only context that ever
//   touches the filesystem stack (FatFs/EMMC interrupts live on core 0).
//
// Everything here is a no-op / direct call in single-core builds: the split
// never activates, and the I/O service degrades to plain POSIX calls.
//
#ifndef SDL_circle_h_
#define SDL_circle_h_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- core split (host kernel side) -----------------------------------------

// Prepare the CALLING core to run C and C++ code. Call it as the first
// statement on every core a host hands to an application or to this library,
// core 0 included, before that core runs anything else.
//
// What it settles is the thread pointer, which C++ exception state is
// reached through. Nothing else in a Circle world writes that register, so a
// freshly started core holds whatever the firmware left in it — and a throw
// dereferences that. Where the leftover value happens to be zero the read
// lands in mapped low memory and the throw appears to work, so a host that
// skips this can pass on one board and take a data abort on the next, on an
// ordinary thrown exception, looking for all the world like a hardware fault.
//
// Calling it twice on a core does nothing the second time.
void SDL2Circle_ArmCoreRuntime(void);

// Activate the core split. Call ONCE on core 0, after the filesystem is
// mounted and the scheduler exists, BEFORE the application starts. Creates
// the hardware-core servo task (call proxy, I/O service, input pump, audio feed,
// CPU-throttle tick) and the watchdog task (dumps state when the
// application's per-frame heartbeat stalls).
void SDL2Circle_SplitInit(void);

// Presentation-worker entry: run this on the secondary core that owns
// presentation (CMultiCoreSupport::Run calls it). It stands in for display
// hardware the board does not have — a finished frame goes in, a scanout
// comes out — scaling each posted frame onto the screen and making it
// visible. No part of SDL runs here. Never returns.
void SDL2Circle_SplitPresentCore(void);

// Non-zero once SDL2Circle_SplitInit has run.
int SDL2Circle_SplitActive(void);

// Run fn(arg) on core 0 and block until it has finished. This is the same
// one-deep mailbox the library marshals its own platform calls through, and
// it is offered here for the one thing the library cannot do for a host
// kernel: reach a device the library does not own. A host that has to touch
// its own hardware from another core wraps the call in this.
//
// Direct call — no mailbox, no cost — when the split is inactive or the
// caller is already core 0, so the same code serves both builds.
//
// Do NOT use it for files: the I/O service below is the file and directory
// route, and it is valid from any core already.
void SDL2Circle_CallOn0(void (*fn)(void *), void *arg);

// Performance receipts on the serial log: every nSeconds, one line with the
// presented frame rate and a PMU cycle split across the shim's instrumented
// sections (render, audio, input, yield) with the remainder attributed to
// the application's own compute. Costs one branch per section while off.
// Zero disables. Host kernels typically arm it from a boot option
// (cmdline.txt `perfstats=10`) so one binary serves bench and product.
void SDL2Circle_SetPerfInterval(unsigned nSeconds);

// ---- the virtual display device ---------------------------------------------

// Declare the display device the application is to be given: its bit depth,
// and its width and height in pixels. Every SDL answer about the display —
// SDL_GetCurrentDisplayMode, SDL_GetDisplayMode, SDL_GetDesktopDisplayMode,
// SDL_GetDisplayBounds — and the size of the window SDL_CreateWindow returns
// come from these numbers, whatever resolution the panel is really being
// scanned at. The library carries the frame from the one to the other.
//
// THIS IS REQUIRED AND THERE IS NO FALLBACK. Not the boot command line, not
// the physical display, nothing: a consumer that has not declared a virtual
// device has not said what display its application is to be given, and
// SDL_Init refuses to start the library rather than invent one. It fails
// with an SDL error and a line on the console.
//
// The declaration is FIXED. It is accepted once, before anything has asked
// the library about the display, and after that the answer cannot change:
// a second declaration is refused, and so is one made after the display size
// has been settled (the first display query, or the first window). Every
// geometry the library derives from it is therefore computed once and holds
// for the run.
//
// So call it before SDL_Init, from the application or from its host kernel,
// and after the host kernel's logger exists.
//
// WHERE THE NUMBERS COME FROM IS THE CALLER'S BUSINESS ENTIRELY: a build
// constant, a settings file, an option of the host kernel's own, a firmware
// query the caller makes for itself, a value off a network port. This
// library is TOLD what the virtual display is. It discovers nothing, offers
// no way to ask what the panel is, and holds no opinion about what the
// numbers ought to be.
//
// A consumer that wants its virtual display to MATCH the physical one works
// the physical one out itself and passes it in here. That takes a handful of
// lines against Circle's public property tags —
// CBcmPropertyTags::GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, ...) — and the
// test/ examples each carry the block, written out in full.
//
// 32 is the only depth the library can serve: the framebuffer is allocated
// at 32 bits per pixel and streaming ARGB8888 is the only texture format, so
// any other depth is refused rather than silently rounded to this one. Width
// and height must both be greater than zero.
//
// Returns 0 when the declaration is accepted, and -1 when it is refused,
// with SDL_GetError describing which of the above was not met. A refused
// declaration changes nothing: no partial state is kept, and an earlier
// accepted declaration still stands.
//
// The boot options `width=` and `height=` play no part in any of this: they
// ask the firmware for a physical display mode and do nothing else. They
// never set the virtual display, and this call never sets the physical one.
int SDL2Circle_DeclareVirtualDevice(unsigned depth, int width, int height);

// ---- board hardware: CPU clock and case fan ---------------------------------
//
// Circle's CCPUThrottle sets the CPU clock rate and, where cmdline.txt names
// a fan pin, switches a case fan on and off. Circle creates neither for
// itself: it requires that a system holds exactly one such object and that
// something calls it regularly, or none of that management happens. This
// library owns that object and drives it from whichever per-frame heartbeat
// is live — the hardware core's servo when the core split is active, and
// SDL_PumpEvents otherwise.
//
// A HOST KERNEL MUST NOT CREATE A CCPUThrottle OF ITS OWN. Circle allows
// only one, and making a second stops the machine in its constructor. This
// library cannot detect one that a host made either, because Circle offers
// no safe presence test — CCPUThrottle::Get() stops the machine when there
// is nothing for it to return, so it can never be used to ask the question.
//
// Nothing has to be done for the common case: bring-up happens inside
// SDL_Init, on core 0, and an application that does nothing gets it. The
// clock is raised to maximum, because Circle boots the board at its idle
// rate; the cmdline.txt options `socmaxtemp=` and `gpiofanpin=` then govern
// what happens from there (see Circle's doc/cmdline.txt).
//
// Bring it up EARLIER if the host kernel initializes I2C, SPI or the mini
// UART itself. Raising the CPU clock also moves the core clock, and those
// peripherals take their transfer speed and their baud rate from it, so the
// clock must be settled before they are configured. To do that, declare a
// CSDL2CircleHardware (below) as a member of the kernel class, or call this
// from the kernel's own constructor. Calling it again does nothing.
void SDL2Circle_HardwareInit(void);

// Current SoC temperature in degrees Celsius, and current CPU clock rate in
// Hz. A host kernel reports these on its boot log rather than reaching for
// Circle's class. Both answer zero before hardware management is up, and
// zero where the board cannot report the value.
unsigned SDL2Circle_SoCTemperature(void);
unsigned SDL2Circle_CPUClockRate(void);

// ---- logging (any core) -----------------------------------------------------
//
// The serial console is a device, so only core 0 may write to it. These put
// a line on it from ANY core: the calling core formats into a ring of its
// own and returns, and core 0's servo drains every ring into the logger.
// The caller never touches the hardware and is never blocked by it.
//
// When a ring is full the line is DROPPED and counted, and the count is
// printed with the next drain — logging never stalls the core that logs,
// and never quietly loses anything without saying so.
//
// Without the split active these write straight to the logger, so the same
// call sites serve a single-core build at no extra cost.
//
// `from` is the subsystem tag Circle's logger prints. It is stored by
// POINTER and printed later, so it must outlive the call: a string literal,
// never a buffer on the stack.

#define SDL2CIRCLE_LOG_ERROR    1
#define SDL2CIRCLE_LOG_WARNING  2
#define SDL2CIRCLE_LOG_NOTICE   3
#define SDL2CIRCLE_LOG_DEBUG    4

void SDL2Circle_Log(const char *from, unsigned severity, const char *fmt, ...);

// Byte-oriented output — an application's stdout, arriving in whatever
// pieces it was written in. Lines are assembled and published one at a
// time, because a log carries lines and has nowhere to put half of one.
void SDL2Circle_LogBytes(const char *from, const char *bytes, unsigned len);

// ---- I/O service (any core) -------------------------------------------------

// Open flags.
#define SDL2CIRCLE_IO_READ    0x1
#define SDL2CIRCLE_IO_WRITE   0x2
#define SDL2CIRCLE_IO_CREATE  0x4   /* create or truncate (with WRITE) */

typedef struct SDL2Circle_IOStat
{
    uint8_t  isdir;
    uint64_t size;
    int64_t  mtime;    /* seconds since epoch */
} SDL2Circle_IOStat;

typedef struct SDL2Circle_IODirEntry
{
    char     name[256];
    uint8_t  isdir;
    uint64_t size;
    int64_t  mtime;
} SDL2Circle_IODirEntry;

// All calls block until the hardware-core servo answers; results are plain values
// (>= 0) or a negated errno (< 0) — never the caller's errno, which is not
// core-safe here.
int      SDL2Circle_IOOpen(const char *path, unsigned flags, uint64_t *size_out);
long     SDL2Circle_IORead(int handle, void *buf, uint64_t offset, uint32_t length);
long     SDL2Circle_IOWrite(int handle, const void *buf, uint64_t offset, uint32_t length);
int      SDL2Circle_IOTruncate(int handle, uint64_t size);
int      SDL2Circle_IOClose(int handle);
int      SDL2Circle_IOUnlink(const char *path);
int      SDL2Circle_IOMkdir(const char *path);
int      SDL2Circle_IOStatPath(const char *path, SDL2Circle_IOStat *st);
intptr_t SDL2Circle_IOOpenDir(const char *path);                      /* 0 on failure */
int      SDL2Circle_IOReadDir(intptr_t dir, SDL2Circle_IODirEntry *e); /* 1 entry, 0 end, <0 error */
void     SDL2Circle_IOCloseDir(intptr_t dir);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

// The early-bring-up hatch for board hardware, described above. Declare one
// as a member of the Circle kernel class and the library's hardware
// management comes up while the kernel is being constructed, before anything
// in Initialize() runs, instead of waiting for SDL_Init. It holds no state
// of its own: constructing it calls SDL2Circle_HardwareInit, and where it
// sits in the member list decides how early that happens.
class CSDL2CircleHardware
{
public:
    CSDL2CircleHardware(void) { SDL2Circle_HardwareInit(); }
};

#endif

#endif /* SDL_circle_h_ */
