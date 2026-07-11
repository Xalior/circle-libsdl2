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
//   marshaled to the core-0 servo, which is the only context that ever
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

// Activate the core split. Call ONCE on core 0, after the filesystem is
// mounted and the scheduler exists, BEFORE the application starts. Creates
// the core-0 servo task (call proxy, I/O service, input pump, audio feed,
// CPU-throttle tick) and the watchdog task (dumps state when the
// application's per-frame heartbeat stalls).
void SDL2Circle_SplitInit(void);

// Presentation-worker entry: run this on the secondary core that owns
// presentation (CMultiCoreSupport::Run calls it). Blits posted frames into
// the framebuffer and page-flips. Never returns.
void SDL2Circle_SplitPresentCore(void);

// Non-zero once SDL2Circle_SplitInit has run.
int SDL2Circle_SplitActive(void);

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

// All calls block until the core-0 servo answers; results are plain values
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

#endif /* SDL_circle_h_ */
