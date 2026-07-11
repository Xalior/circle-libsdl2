//
// split.cpp — the core split: every ring, lock and wake primitive.
//
// Roles (multicore builds, activated by SDL2Circle_SplitInit):
//
//   core 0    the Circle world: scheduler, IRQs, USB, EMMC/FatFs, sound.
//             Gains the SERVO task (drains the call mailbox, executes the
//             I/O service, pumps USB input into the event ring, feeds the
//             sound device from the audio ring, ticks the CPU throttle) and
//             the WATCHDOG task (dumps state when the app's heartbeat
//             stalls).
//   app core  the application, alone. Calls plain SDL_* functions; the shim
//             marshals here. Its per-frame pump touches nothing but shared
//             memory (rings, atomics) — no Circle service is ever called
//             off core 0 except the documented multicore-safe mailbox.
//   present   a dedicated worker core: executes posted frame command lists
//             (blit + fill) into the framebuffer and page-flips.
//
// Communication is single-producer/single-consumer rings and 1-deep
// request/response mailboxes in coherent memory — atomics + WFE/SEV, never
// Circle scheduler primitives (which are core-0-only by construction).
// Rare calls (init, window/audio creation, file service) go through the
// call mailbox; per-frame traffic (events, audio samples, frames) has a
// dedicated ring each. Calls that can be answered locally never cross.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"

#include <circle/sysconfig.h>
#include <circle/atomic.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/cputhrottle.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef ARM_ALLOW_MULTI_CORE
#include <circle/multicore.h>
#endif

static const char From[] = "sdl2split";

static inline void wfe(void) { asm volatile("wfe" ::: "memory"); }
static inline void publish(void) { asm volatile("dsb ish; sev" ::: "memory"); }

static std::atomic<bool> g_split{false};

extern "C" int SDL2Circle_SplitActive(void)
{
    return g_split.load(std::memory_order_acquire) ? 1 : 0;
}

unsigned SDL2Circle_ThisCore(void)
{
#ifdef ARM_ALLOW_MULTI_CORE
    return CMultiCoreSupport::ThisCore();
#else
    return 0;
#endif
}

// Idle appropriate to the calling core: core 0 must keep its cooperative
// world alive; other cores nap in WFE.
static inline void idle_wait(void)
{
    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
        CScheduler::Get()->Yield();
    else
        wfe();
}

// ---------------------------------------------------------------------------
// Cross-core spin lock (client side of the shared mailboxes: the app core
// and any worker thread may issue calls concurrently).
// ---------------------------------------------------------------------------

struct SpinLock
{
    std::atomic<u32> state{0};

    void lock(void)
    {
        for (;;)
        {
            u32 expect = 0;
            if (state.compare_exchange_weak(expect, 1, std::memory_order_acquire,
                                            std::memory_order_relaxed))
                return;
            idle_wait();
        }
    }
    void unlock(void)
    {
        state.store(0, std::memory_order_release);
        publish();
    }
};

// ---------------------------------------------------------------------------
// Call mailbox: run fn(arg) on core 0. One outstanding call; the servo
// answers between scheduler yields (measured RTT ~0.3 µs plus the work).
// ---------------------------------------------------------------------------

struct alignas(64) CallBox
{
    std::atomic<u64> req{0};
    std::atomic<u64> ack{0};
    void (*fn)(void *);
    void *arg;
};

static CallBox g_call;
static SpinLock g_call_lock;
static std::atomic<u64> g_calls_served{0};

void SDL2Circle_CallOn0(void (*fn)(void *), void *arg)
{
    if (!g_split.load(std::memory_order_acquire) || SDL2Circle_ThisCore() == 0)
    {
        fn(arg);
        return;
    }

    g_call_lock.lock();
    g_call.fn = fn;
    g_call.arg = arg;
    u64 seq = g_call.req.load(std::memory_order_relaxed) + 1;
    g_call.req.store(seq, std::memory_order_release);
    publish();
    while (g_call.ack.load(std::memory_order_acquire) < seq)
        wfe();
    g_call_lock.unlock();
}

// ---------------------------------------------------------------------------
// Event ring: core 0 (USB input pump, window events) -> app core.
// ---------------------------------------------------------------------------

static const unsigned EVENT_RING_SIZE = 256;   // power of two

struct alignas(64) EventRing
{
    alignas(64) std::atomic<u32> tail{0};   // producer
    alignas(64) std::atomic<u32> head{0};   // consumer
    alignas(64) SDL_Event slot[EVENT_RING_SIZE];
};

static EventRing g_events;
static std::atomic<u64> g_events_pushed{0};
static std::atomic<u64> g_events_dropped{0};

int SDL2Circle_EventRingPush(const SDL_Event *ev)
{
    u32 tail = g_events.tail.load(std::memory_order_relaxed);
    u32 head = g_events.head.load(std::memory_order_acquire);
    if (tail - head >= EVENT_RING_SIZE)
    {
        g_events_dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g_events.slot[tail % EVENT_RING_SIZE] = *ev;
    g_events.tail.store(tail + 1, std::memory_order_release);
    g_events_pushed.fetch_add(1, std::memory_order_relaxed);
    publish();
    return 1;
}

int SDL2Circle_EventRingPop(SDL_Event *ev)
{
    u32 head = g_events.head.load(std::memory_order_relaxed);
    u32 tail = g_events.tail.load(std::memory_order_acquire);
    if (tail == head)
        return 0;
    *ev = g_events.slot[head % EVENT_RING_SIZE];
    g_events.head.store(head + 1, std::memory_order_release);
    return 1;
}

// ---------------------------------------------------------------------------
// Audio ring: app core (callback output) -> core 0 (sound device feeder).
// Byte-granular SPSC; sized to carry the underrun budget between servo
// visits on top of the device's own queue.
// ---------------------------------------------------------------------------

static const unsigned AUDIO_RING_SIZE = 64 * 1024;   // power of two

struct alignas(64) AudioRing
{
    alignas(64) std::atomic<u32> tail{0};
    alignas(64) std::atomic<u32> head{0};
    alignas(64) u8 data[AUDIO_RING_SIZE];
};

static AudioRing g_audio;

unsigned SDL2Circle_AudioRingSpace(void)
{
    u32 tail = g_audio.tail.load(std::memory_order_relaxed);
    u32 head = g_audio.head.load(std::memory_order_acquire);
    return AUDIO_RING_SIZE - (tail - head);
}

void SDL2Circle_AudioRingWrite(const unsigned char *data, unsigned bytes)
{
    u32 tail = g_audio.tail.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < bytes; i++)
        g_audio.data[(tail + i) % AUDIO_RING_SIZE] = data[i];
    g_audio.tail.store(tail + bytes, std::memory_order_release);
}

unsigned SDL2Circle_AudioRingRead(unsigned char *data, unsigned maxbytes)
{
    u32 head = g_audio.head.load(std::memory_order_relaxed);
    u32 tail = g_audio.tail.load(std::memory_order_acquire);
    unsigned avail = tail - head;
    if (avail > maxbytes)
        avail = maxbytes;
    for (unsigned i = 0; i < avail; i++)
        data[i] = g_audio.data[(head + i) % AUDIO_RING_SIZE];
    g_audio.head.store(head + avail, std::memory_order_release);
    return avail;
}

// ---------------------------------------------------------------------------
// Present mailbox: app core -> presentation worker. One frame in flight:
// the poster waits for the worker's previous ACK before publishing, so two
// texture buffers are provably enough.
// ---------------------------------------------------------------------------

struct alignas(64) FrameBox
{
    std::atomic<u64> seq{0};    // poster bumps
    std::atomic<u64> ack{0};    // worker matches after flip
    unsigned half;
    unsigned ncmds;
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_PRESENT_MAX_CMDS];
};

static FrameBox g_frame;

void SDL2Circle_PresentPost(const SDL2CirclePresentCmd *cmds, unsigned ncmds,
                            unsigned half)
{
    u64 seq = g_frame.seq.load(std::memory_order_relaxed);
    while (g_frame.ack.load(std::memory_order_acquire) < seq)
        wfe();

    if (ncmds > SDL2CIRCLE_PRESENT_MAX_CMDS)
        ncmds = SDL2CIRCLE_PRESENT_MAX_CMDS;
    memcpy(g_frame.cmds, cmds, ncmds * sizeof(*cmds));
    g_frame.ncmds = ncmds;
    g_frame.half = half;
    g_frame.seq.store(seq + 1, std::memory_order_release);
    publish();
}

extern "C" void SDL2Circle_SplitPresentCore(void)
{
    u64 done = 0;
    for (;;)
    {
        u64 seq = g_frame.seq.load(std::memory_order_acquire);
        if (seq == done)
        {
            wfe();
            continue;
        }
        for (unsigned i = 0; i < g_frame.ncmds; i++)
            SDL2Circle_VideoExecCmd(&g_frame.cmds[i], g_frame.half);
        SDL2Circle_VideoFlip(g_frame.half);
        done = seq;
        g_frame.ack.store(done, std::memory_order_release);
        publish();
    }
}

// ---------------------------------------------------------------------------
// Heartbeat: the app core bumps it once per pump; the watchdog task dumps
// state when it stalls — the split's replacement for the in-band pump
// deadman, and it can see a wedged app core the in-band version couldn't.
// ---------------------------------------------------------------------------

static std::atomic<u64> g_heartbeat{0};

void SDL2Circle_HeartbeatBump(void)
{
    g_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// I/O service: blocking file/directory API valid from any core. Off core 0
// each operation travels the call mailbox and executes as plain POSIX on
// the servo — the only context that touches FatFs/EMMC. Results are values
// or negated errno; the caller's errno is never used (not core-safe).
// ---------------------------------------------------------------------------

static std::atomic<u64> g_io_ops{0};

namespace
{

struct IoOpen   { const char *path; unsigned flags; uint64_t *size_out; int r; };
struct IoRw     { int h; void *buf; const void *cbuf; uint64_t off; uint32_t len; long r; };
struct IoTrunc  { int h; uint64_t size; int r; };
struct IoClose  { int h; int r; };
struct IoPath   { const char *path; int r; };
struct IoStatQ  { const char *path; SDL2Circle_IOStat *st; int r; };
struct IoDirO   { const char *path; intptr_t r; };
struct IoDirR   { intptr_t dir; SDL2Circle_IODirEntry *e; int r; };
struct IoDirC   { intptr_t dir; };

void io_open(void *p)
{
    auto *a = (IoOpen *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);

    int access;
    if (a->flags & SDL2CIRCLE_IO_WRITE)
    {
        access = (a->flags & SDL2CIRCLE_IO_READ) ? O_RDWR : O_WRONLY;
        if (a->flags & SDL2CIRCLE_IO_CREATE)
            access |= O_CREAT | O_TRUNC;
    }
    else if (a->flags & SDL2CIRCLE_IO_READ)
        access = O_RDONLY;
    else
    {
        a->r = -EINVAL;
        return;
    }

    int fd = ::open(a->path, access, 0666);
    if (fd < 0)
    {
        a->r = -errno;
        return;
    }
    if (a->size_out)
    {
        struct stat st;
        if (::fstat(fd, &st) < 0)
        {
            a->r = -errno;
            ::close(fd);
            return;
        }
        *a->size_out = (uint64_t)st.st_size;
    }
    a->r = fd;
}

void io_read(void *p)
{
    auto *a = (IoRw *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    if (::lseek(a->h, (off_t)a->off, SEEK_SET) < 0)
    {
        a->r = -errno;
        return;
    }
    ssize_t n = ::read(a->h, a->buf, a->len);
    a->r = (n < 0) ? -errno : (long)n;
}

void io_write(void *p)
{
    auto *a = (IoRw *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    if (::lseek(a->h, (off_t)a->off, SEEK_SET) < 0)
    {
        a->r = -errno;
        return;
    }
    ssize_t n = ::write(a->h, a->cbuf, a->len);
    a->r = (n < 0) ? -errno : (long)n;
}

void io_trunc(void *p)
{
    auto *a = (IoTrunc *)p;
    a->r = (::ftruncate(a->h, (off_t)a->size) < 0) ? -errno : 0;
}

void io_close(void *p)
{
    auto *a = (IoClose *)p;
    a->r = (::close(a->h) < 0) ? -errno : 0;
}

void io_unlink(void *p)
{
    auto *a = (IoPath *)p;
    a->r = (::unlink(a->path) < 0) ? -errno : 0;
}

void io_mkdir(void *p)
{
    auto *a = (IoPath *)p;
    a->r = (::mkdir(a->path, 0777) < 0) ? -errno : 0;
}

void io_stat(void *p)
{
    auto *a = (IoStatQ *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    struct stat st;
    if (::stat(a->path, &st) < 0)
    {
        a->r = -errno;
        return;
    }
    a->st->isdir = S_ISDIR(st.st_mode) ? 1 : 0;
    a->st->size = (uint64_t)st.st_size;
    a->st->mtime = (int64_t)st.st_mtime;
    a->r = 0;
}

void io_opendir(void *p)
{
    auto *a = (IoDirO *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (intptr_t)::opendir(a->path);
}

void io_readdir(void *p)
{
    auto *a = (IoDirR *)p;
    errno = 0;
    struct dirent *d = ::readdir((DIR *)a->dir);
    if (!d)
    {
        a->r = errno ? -errno : 0;
        return;
    }
    strncpy(a->e->name, d->d_name, sizeof(a->e->name) - 1);
    a->e->name[sizeof(a->e->name) - 1] = '\0';

    // FatFs dirents carry no type; stat is the servo's to make anyway, but
    // path assembly belongs to the caller — report type by stat only when
    // the entry's own metadata is absent.
    a->e->isdir = 0;
    a->e->size = 0;
    a->e->mtime = 0;
#ifdef DT_DIR
    a->e->isdir = (d->d_type == DT_DIR) ? 1 : 0;
#endif
    a->r = 1;
}

void io_closedir(void *p)
{
    auto *a = (IoDirC *)p;
    ::closedir((DIR *)a->dir);
}

} // namespace

extern "C" int SDL2Circle_IOOpen(const char *path, unsigned flags, uint64_t *size_out)
{
    IoOpen a{path, flags, size_out, 0};
    SDL2Circle_CallOn0(io_open, &a);
    return a.r;
}

extern "C" long SDL2Circle_IORead(int handle, void *buf, uint64_t offset, uint32_t length)
{
    IoRw a{handle, buf, nullptr, offset, length, 0};
    SDL2Circle_CallOn0(io_read, &a);
    return a.r;
}

extern "C" long SDL2Circle_IOWrite(int handle, const void *buf, uint64_t offset, uint32_t length)
{
    IoRw a{handle, nullptr, buf, offset, length, 0};
    SDL2Circle_CallOn0(io_write, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOTruncate(int handle, uint64_t size)
{
    IoTrunc a{handle, size, 0};
    SDL2Circle_CallOn0(io_trunc, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOClose(int handle)
{
    IoClose a{handle, 0};
    SDL2Circle_CallOn0(io_close, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOUnlink(const char *path)
{
    IoPath a{path, 0};
    SDL2Circle_CallOn0(io_unlink, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOMkdir(const char *path)
{
    IoPath a{path, 0};
    SDL2Circle_CallOn0(io_mkdir, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOStatPath(const char *path, SDL2Circle_IOStat *st)
{
    IoStatQ a{path, st, 0};
    SDL2Circle_CallOn0(io_stat, &a);
    return a.r;
}

extern "C" intptr_t SDL2Circle_IOOpenDir(const char *path)
{
    IoDirO a{path, 0};
    SDL2Circle_CallOn0(io_opendir, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOReadDir(intptr_t dir, SDL2Circle_IODirEntry *e)
{
    IoDirR a{dir, e, 0};
    SDL2Circle_CallOn0(io_readdir, &a);
    return a.r;
}

extern "C" void SDL2Circle_IOCloseDir(intptr_t dir)
{
    IoDirC a{dir};
    SDL2Circle_CallOn0(io_closedir, &a);
}

// ---------------------------------------------------------------------------
// Core-0 tasks: the servo and the watchdog.
// ---------------------------------------------------------------------------

class CSplitServoTask : public CTask
{
public:
    CSplitServoTask(void) : CTask(TASK_STACK_SIZE * 4) {}

    void Run(void) override
    {
        u64 lastThrottle = 0;
        for (;;)
        {
            // Call mailbox (init, window/audio creation, I/O service).
            u64 req = g_call.req.load(std::memory_order_acquire);
            if (req > g_call.ack.load(std::memory_order_relaxed))
            {
                g_call.fn(g_call.arg);
                g_calls_served.fetch_add(1, std::memory_order_relaxed);
                g_call.ack.store(req, std::memory_order_release);
                publish();
            }

            // USB plug-and-play + HID -> event ring.
            SDL2Circle_InputPump();

            // Audio ring -> sound device.
            SDL2Circle_AudioDrain();

            // Thermal management (Circle requires periodic Update()).
            CCPUThrottle *throttle = CCPUThrottle::Get();
            if (throttle)
            {
                u64 now = CTimer::GetClockTicks64();
                if (now - lastThrottle > 2000000)
                {
                    lastThrottle = now;
                    throttle->Update();
                }
            }

            CScheduler::Get()->Yield();
        }
    }
};

class CSplitWatchdogTask : public CTask
{
public:
    CSplitWatchdogTask(void) : CTask(TASK_STACK_SIZE) {}

    void Run(void) override
    {
        u64 lastBeat = 0;
        u64 lastChange = CTimer::GetClockTicks64();
        bool dumped = false;

        for (;;)
        {
            CScheduler::Get()->MsSleep(5000);

            u64 beat = g_heartbeat.load(std::memory_order_relaxed);
            u64 now = CTimer::GetClockTicks64();
            if (beat != lastBeat)
            {
                lastBeat = beat;
                lastChange = now;
                dumped = false;
                continue;
            }

            // Silence before the first beat is startup, not a stall — the
            // app may load for a long time before it pumps. Report late
            // starts once, gently.
            u64 quiet = (now - lastChange) / 1000000;   // seconds
            if (beat == 0)
            {
                if (quiet >= 120 && !dumped)
                {
                    dumped = true;
                    CLogger::Get()->Write(From, LogWarning,
                                          "no first heartbeat after %us", (unsigned)quiet);
                }
                continue;
            }

            if (quiet >= 30 && !dumped)
            {
                dumped = true;
                CLogger::Get()->Write(From, LogError,
                                      "HEARTBEAT STALLED %us -- state dump:", (unsigned)quiet);
                CLogger::Get()->Write(From, LogError,
                                      "beats=%llu calls=%llu io=%llu frames post/ack=%llu/%llu ev push/drop=%llu/%llu",
                                      beat,
                                      g_calls_served.load(),
                                      g_io_ops.load(),
                                      g_frame.seq.load(), g_frame.ack.load(),
                                      g_events_pushed.load(), g_events_dropped.load());
                if (CScheduler::IsActive())
                    CScheduler::Get()->ListTasks(CLogger::Get()->GetTarget());
            }
        }
    }
};

extern "C" void SDL2Circle_SplitInit(void)
{
#ifdef ARM_ALLOW_MULTI_CORE
    if (g_split.load(std::memory_order_relaxed))
        return;

    new CSplitServoTask;      // CTask registers itself with the scheduler
    new CSplitWatchdogTask;

    g_split.store(true, std::memory_order_release);
    publish();

    CLogger::Get()->Write(From, LogNotice,
                          "core split active: servo + watchdog on core 0");
#else
    CLogger::Get()->Write(From, LogError,
                          "core split requires the multicore world; staying single-core");
#endif
}
