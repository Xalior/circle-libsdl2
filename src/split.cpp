//
// split.cpp — the core split: every ring, lock and wake primitive.
//
// Roles (multicore builds, activated by SDL2Circle_SplitInit):
//
//   core 0    the Circle world: scheduler, IRQs, USB, EMMC/FatFs, sound.
//             Gains the SERVO task (drains the call mailbox, executes the
//             I/O service, pumps USB input into the event ring, feeds the
//             sound device from the audio ring, ticks the CPU throttle), the
//             WATCHDOG task (dumps state when the app's heartbeat stalls),
//             and the STDIN task (owns the one call that may block: a read
//             on standard input waiting for a keypress).
//   application core  the application, alone. Calls plain SDL_* functions; the shim
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
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include <new>
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

// ---------------------------------------------------------------------------
// Which cores are spoken for
// ---------------------------------------------------------------------------
//
// The split hands out roles — core 0 the Circle world, one core the
// application, one core presentation — and a host kernel parks whatever is
// left. Something else in this library then wants a core to put a pinned
// thread on (src/libcxxthreading.cpp), and the one thing it must never do is
// take a core that already has a job. So the roles are recorded as they are
// taken, and the answer is asked for rather than assumed.
//
// Core 0 is in the set from the start and never leaves it: it is the
// machine. The other two identify themselves — the presentation worker when
// it starts, the application core the first time it does either of the two
// things only the application does, beat the per-frame heartbeat or ask for
// a thread of its own. Nothing here guesses from a core number: which core
// runs what is a host kernel's decision, not this library's.
static std::atomic<unsigned> g_cores_claimed{1};

void SDL2Circle_ClaimCore(unsigned nCore)
{
    g_cores_claimed.fetch_or(1u << nCore, std::memory_order_release);
}

unsigned SDL2Circle_ClaimedCores(void)
{
    return g_cores_claimed.load(std::memory_order_acquire);
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
// Cross-core spin lock (client side of the shared mailboxes: the application core
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

// Calls the servo has ENTERED, bumped before the handler runs. Against
// g_calls_served it answers the one question a frozen board cannot otherwise
// be asked: started == served means core 0 finished everything it was given
// and stopped somewhere else; started == served + 1 means core 0 is still
// INSIDE a marshalled call and never came out of it. Those are different
// faults with different owners, and from outside they look identical.
static std::atomic<u64> g_calls_started{0};

// Servo passes. Counted separately from the work the servo does, because
// "core 0 is alive and idle" and "core 0 has stopped" are otherwise the same
// picture: every other counter here only moves when there is something to
// do, so a quiet system and a dead one read identically. This one moves on
// every lap regardless, and it is the difference between the two.
static std::atomic<u64> g_servo_beats{0};

// ---------------------------------------------------------------------------
// Reporting a cross-core wait that has gone on too long
//
// Every wait below is unbounded by design: the far side owns something this
// side needs, and giving up would not make the answer arrive, it would carry
// on without it. So none of them is abandoned here.
//
// What they stop doing is waiting SILENTLY. A stall between two cores with no
// output looks exactly like a board that has died, and the two are
// investigated completely differently. One line naming what is being waited
// for, and the counters that say whether the other core is still running,
// turns a silent freeze into a diagnosis.
//
// The line goes through the ordinary log ring, so it costs the waiting core
// nothing and touches no device. It has to: the console belongs to core 0
// because of how the hardware is wired, not because of a policy that could
// be waived, so there is no arrangement under which another core writes it
// instead. If core 0 is alive it appears at once.
//
// If core 0 is the side that has stopped, it never appears, and no amount of
// cleverness elsewhere would change that. CORE 0 IS THE MACHINE: the serial
// port, USB, video, the system timer, the scheduler and the watchdog are all
// its. A core 0 that does not return is not a core that has gone quiet, it
// is a dead board, and there is nothing left running anywhere to notice or
// to say so.
//
// Which is why the answer is not a better report. It is that NOTHING ON THE
// SERVO'S PATH MAY BLOCK — see the servo loop below, where every handler it
// runs has to be bounded and has to return.
// ---------------------------------------------------------------------------

static const u64 STALL_REPORT_US = 5000000;   // 5 s

namespace
{
class StallWatch
{
public:
    // gate_on_servo: this wait has no deadline of its own — the far side is
    // a human, not a peer that owes an answer — so elapsed time alone would
    // report the wait itself as the fault. Gated, the clock instead tracks
    // g_servo_beats, core 0's own per-lap counter: for as long as it keeps
    // advancing, core 0 is alive and simply has nothing yet to hand back,
    // which is not a stall. Only the beats themselves going quiet — core 0
    // stuck somewhere, unable to complete a lap — is.
    explicit StallWatch(const char *what, bool gate_on_servo = false)
    : m_what(what), m_start(CTimer::GetClockTicks64()), m_reported(false),
      m_gate(gate_on_servo),
      m_lastBeats(g_servo_beats.load(std::memory_order_relaxed)),
      m_lastBeatChange(m_start) {}

    void tick(void)
    {
        if (m_reported)
            return;

        u64 now = CTimer::GetClockTicks64();
        if (m_gate)
        {
            u64 beats = g_servo_beats.load(std::memory_order_relaxed);
            if (beats != m_lastBeats)
            {
                m_lastBeats = beats;
                m_lastBeatChange = now;
            }
            if (now - m_lastBeatChange < STALL_REPORT_US)
                return;
        }
        else if (now - m_start < STALL_REPORT_US)
        {
            return;
        }

        m_reported = true;
        SDL2Circle_Log("split", SDL2CIRCLE_LOG_ERROR,
                       "core %u has waited %us for %s "
                       "(servo laps %llu, calls started/served %llu/%llu) — %s",
                       SDL2Circle_ThisCore(),
                       (unsigned)(STALL_REPORT_US / 1000000), m_what,
                       (unsigned long long)g_servo_beats.load(std::memory_order_relaxed),
                       (unsigned long long)g_calls_started.load(std::memory_order_relaxed),
                       (unsigned long long)g_calls_served.load(std::memory_order_relaxed),
                       g_calls_started.load(std::memory_order_relaxed)
                           != g_calls_served.load(std::memory_order_relaxed)
                           ? "core 0 is INSIDE a marshalled call and has not "
                             "returned from it"
                           : m_gate
                             ? "core 0's servo has stopped completing laps"
                             : "core 0 is not inside a marshalled call");
    }

private:
    const char *m_what;
    u64 m_start;
    bool m_reported;
    bool m_gate;
    u64 m_lastBeats;
    u64 m_lastBeatChange;
};
}   // namespace

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
    {
        StallWatch watch("a call it marshalled to core 0");
        while (g_call.ack.load(std::memory_order_acquire) < seq)
        {
            wfe();
            watch.tick();
        }
    }
    g_call_lock.unlock();
}

// ---------------------------------------------------------------------------
// Event ring: core 0 (USB input pump, window events) -> application core.
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
// Audio ring: application core (callback output) -> core 0 (sound device feeder).
// Byte-granular SPSC.
//
// THIS SIZE IS NOT THE BUFFER DEPTH, and the difference is the whole reason
// the ring is this big. How much audio waits here is decided by the producer,
// which stops well short of full (audio.cpp), because everything waiting is
// delay before a sound is heard. The storage is generous so that an
// application whose callback produces a large block at a time still has room
// for one — a ring too small to hold a single block would never be written to
// at all, and the sound would not be late, it would be absent.
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

// How much finished audio is waiting here. Every byte of it is time between a
// sound being started and being heard, which is why the producer reads this
// and stops rather than filling the ring: see the latency budget in audio.cpp.
unsigned SDL2Circle_AudioRingUsed(void)
{
    u32 tail = g_audio.tail.load(std::memory_order_relaxed);
    u32 head = g_audio.head.load(std::memory_order_acquire);
    return tail - head;
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
// Present mailbox: application core -> presentation worker. One frame in flight:
// the poster waits for the worker's previous ACK before publishing, so two
// texture buffers are provably enough.
// ---------------------------------------------------------------------------

struct alignas(64) FrameBox
{
    std::atomic<u64> seq{0};    // poster bumps
    std::atomic<u64> taken{0};  // worker matches once the BOX is copied out
    std::atomic<u64> ack{0};    // worker matches once the frame is consumed
    std::atomic<u64> done{0};   // worker matches once the frame is on the glass
    unsigned half;
    unsigned ncmds;
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_RECORD_MAX_CMDS];
};

static FrameBox g_frame;

void SDL2Circle_PresentPost(const SDL2CirclePresentCmd *cmds, unsigned ncmds,
                            unsigned half)
{
    u64 seq = g_frame.seq.load(std::memory_order_relaxed);
    {
        // WAIT FOR THE BOX, NEVER FOR THE PICTURE.
        //
        // The only thing the poster can damage by writing here is the
        // command list the worker has not copied out yet, and copying it is
        // a memcpy of a handful of commands. Everything after that — the
        // scale, the raster, the transfer — reads the worker's own copy and
        // the texture stores, and the stores protect themselves (see
        // busy_seq in video.cpp). None of it is this core's business.
        //
        // This used to wait on `ack`, which the worker publishes after the
        // SCALE. That made the application core's frame rate the
        // presentation core's frame rate: the game could not begin a frame
        // until the previous picture had been composed, on a machine where
        // the game has a core to itself and should never wait for anything.
        SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT);
        StallWatch watch("the presentation core to copy the previous frame out");
        while (g_frame.taken.load(std::memory_order_acquire) < seq)
        {
            wfe();
            watch.tick();
        }
    }

    if (ncmds > SDL2CIRCLE_RECORD_MAX_CMDS)
        ncmds = SDL2CIRCLE_RECORD_MAX_CMDS;
    memcpy(g_frame.cmds, cmds, ncmds * sizeof(*cmds));
    g_frame.ncmds = ncmds;
    g_frame.half = half;
    g_frame.seq.store(seq + 1, std::memory_order_release);
    publish();
}

// ---------------------------------------------------------------------------
// Which posted frame the worker has finished READING
//
// A texture's pixel store is handed to the worker as a raw pointer inside a
// posted command, so the store cannot be written again until the worker has
// read the last frame that pointed at it. These two numbers are what makes
// that decidable from the application core:
//
//   posted  the sequence of the last frame handed over
//   acked   the sequence of the last frame the worker has finished reading
//
// The worker publishes `ack` immediately after the scale and before any
// output work, so it means exactly "everything the poster owns has been
// read" — which is the question a store needs answered, and no more.
//
// A store recorded into the frame still being BUILT carries the sequence
// that frame will get once it is posted, which is greater than `posted`.
// Nothing is reading that store, and the comparison in texture_write_buffer
// relies on being able to tell those two states apart.
// ---------------------------------------------------------------------------

u64 SDL2Circle_PresentPostedSeq(void)
{
    return g_frame.seq.load(std::memory_order_relaxed);
}

u64 SDL2Circle_PresentAckedSeq(void)
{
    return g_frame.ack.load(std::memory_order_acquire);
}

void SDL2Circle_PresentWaitAck(u64 seq)
{
    if (!g_split.load(std::memory_order_acquire))
        return;
    SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT);
    StallWatch watch("the presentation core to release a texture buffer");
    while (g_frame.ack.load(std::memory_order_acquire) < seq)
    {
        wfe();
        watch.tick();
    }
}

void SDL2Circle_PresentQuiesce(void)
{
    if (!g_split.load(std::memory_order_acquire))
        return;

    // Wait for the worker to be out of the frame entirely — not merely done
    // reading what the poster owns (`ack`, which it publishes early so the
    // application core is never held up by output), but past the flip, where
    // it is still using the window and the present buffers. Only the poster
    // bumps `seq`, and this is the poster, so `seq` is stable here.
    u64 seq = g_frame.seq.load(std::memory_order_relaxed);
    StallWatch watch("the presentation core to finish the frame in flight");
    while (g_frame.done.load(std::memory_order_acquire) < seq)
    {
        idle_wait();
        watch.tick();
    }
}

extern "C" void SDL2Circle_SplitPresentCore(void)
{
    // This core is about to run library code for the rest of its life, so it
    // arms itself rather than trusting that it was armed. A host that already
    // did loses nothing: the second call returns.
    SDL2Circle_ArmCoreRuntime();

    // And it is spoken for from here to the end of the run, which is what
    // stops a pinned thread being put on top of it.
    SDL2Circle_ClaimCore(SDL2Circle_ThisCore());

    u64 done = 0;
    for (;;)
    {
        u64 seq = g_frame.seq.load(std::memory_order_acquire);
        if (seq == done)
        {
            // Idle between frames. Instrumented so this core's report says
            // how much of it was spare: at a locked frame rate that is most
            // of it, and it must not read as work.
            SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT);
            wfe();
            continue;
        }
        // TAKE THE BOX AND RELEASE IT IMMEDIATELY.
        //
        // The command list is copied into this core's own memory before
        // anything is drawn, so the box is free from here on and the
        // application core is never held up by the picture. The copy is a
        // few hundred bytes; the scale that follows is megabytes and
        // milliseconds, and none of it needs the box.
        //
        // What the scale still reads is the TEXTURE STORES the commands
        // point at, and those are held by busy_seq on the other side rather
        // than by making the poster wait — a store is spoken for until
        // `ack`, which is published once the scale below has finished with
        // it.
        static SDL2CirclePresentCmd s_local[SDL2CIRCLE_RECORD_MAX_CMDS];
        unsigned ncmds = g_frame.ncmds;
        const unsigned half = g_frame.half;
        if (ncmds > SDL2CIRCLE_RECORD_MAX_CMDS)
            ncmds = SDL2CIRCLE_RECORD_MAX_CMDS;
        memcpy(s_local, g_frame.cmds, ncmds * sizeof(s_local[0]));

        g_frame.taken.store(seq, std::memory_order_release);
        publish();

        {
            // Consuming the frame: the scale, reading this core's copy of
            // the command list and the texture stores it points at.
            SDL2CirclePerfScope render(SDL2CIRCLE_PERF_RENDER);
            for (unsigned i = 0; i < ncmds; i++)
                SDL2Circle_VideoExecCmd(&s_local[i], half);
        }

        // RELEASE THE TEXTURE STORES HERE, and not one line later.
        //
        // The scale above is the last thing that reads a store the
        // application core owns, so this is the earliest point at which one
        // may be written again. The application core is not waiting on this
        // — it was released at `taken`, above — but a store it wants to
        // reuse is, and holding this back would stall the writer for no
        // reason.
        //
        // What follows is the OUTPUT side — waiting for the previous
        // transfer, waiting for the raster, starting the next transfer —
        // and it touches only the shadow this core owns and the framebuffer.
        // None of it is the application's business, so none of it belongs
        // inside the window the application is waiting on. Holding the
        // acknowledgement until after it cost the application core a whole
        // frame's output latency on top of its own work, which is what kept
        // the split below the frame rate a single core managed.
        //
        // The double-buffered texture is what makes the early release safe:
        // released now, the application core writes the OTHER buffer, never
        // the one just read. The worker stays sequential — the next frame's
        // scale cannot start until this flip returns — so the shadow it is
        // about to hand the engine is never written behind it either.
        done = seq;
        g_frame.ack.store(done, std::memory_order_release);
        publish();

        {
            SDL2CirclePerfScope render(SDL2CIRCLE_PERF_RENDER);
            SDL2Circle_VideoFlip(half);
        }

        // Out of the frame completely: nothing this core holds still points
        // at the window, the present buffers or a texture. A teardown waiting
        // in SDL2Circle_PresentQuiesce is released here and not at the
        // acknowledgement above, which says only that the poster's memory is
        // free to reuse.
        g_frame.done.store(done, std::memory_order_release);
        publish();
    }
}

// ---------------------------------------------------------------------------
// Heartbeat: the application core bumps it once per pump; the watchdog task dumps
// state when it stalls — the split's replacement for the in-band pump
// deadman, and it can see a wedged application core the in-band version couldn't.
//
// One case the application core cannot bump this for itself: parked in
// SDL2Circle_ReadStdin, asleep in wfe(), waiting on a human. The watchdog
// task (below) covers that case on the application's behalf, by reading
// the stdin request/ack pair and core 0's own servo lap counter directly —
// see the comment on CSplitWatchdogTask::Run.
// ---------------------------------------------------------------------------

static std::atomic<u64> g_heartbeat{0};

void SDL2Circle_HeartbeatBump(void)
{
    g_heartbeat.fetch_add(1, std::memory_order_relaxed);

    // Whoever beats this is the application, so whichever core it beats from
    // is the application core. That is the only place in this library that
    // learns it, and it costs one test against a word already in cache.
    const unsigned nCore = SDL2Circle_ThisCore();
    if (!(SDL2Circle_ClaimedCores() & (1u << nCore)))
        SDL2Circle_ClaimCore(nCore);
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
struct IoPath2  { const char *from; const char *to; int r; };
struct IoCwdQ   { char *buf; uint32_t size; int r; };
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
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::ftruncate(a->h, (off_t)a->size) < 0) ? -errno : 0;
}

void io_close(void *p)
{
    auto *a = (IoClose *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::close(a->h) < 0) ? -errno : 0;
}

void io_unlink(void *p)
{
    auto *a = (IoPath *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::unlink(a->path) < 0) ? -errno : 0;
}

void io_mkdir(void *p)
{
    auto *a = (IoPath *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::mkdir(a->path, 0777) < 0) ? -errno : 0;
}

void io_rmdir(void *p)
{
    auto *a = (IoPath *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::rmdir(a->path) < 0) ? -errno : 0;
}

void io_rename(void *p)
{
    auto *a = (IoPath2 *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::rename(a->from, a->to) < 0) ? -errno : 0;
}

void io_chdir(void *p)
{
    auto *a = (IoPath *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::chdir(a->path) < 0) ? -errno : 0;
}

void io_getcwd(void *p)
{
    auto *a = (IoCwdQ *)p;
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
    a->r = (::getcwd(a->buf, (size_t)a->size) == nullptr) ? -errno : 0;
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
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
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
    g_io_ops.fetch_add(1, std::memory_order_relaxed);
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

extern "C" int SDL2Circle_IORmdir(const char *path)
{
    IoPath a{path, 0};
    SDL2Circle_CallOn0(io_rmdir, &a);
    return a.r;
}

extern "C" int SDL2Circle_IORename(const char *oldpath, const char *newpath)
{
    IoPath2 a{oldpath, newpath, 0};
    SDL2Circle_CallOn0(io_rename, &a);
    return a.r;
}

// The working directory is one setting for the whole board, held by the
// filesystem on core 0 rather than by the caller, so a change made through
// this call is a change every core sees — including core 0's own C library
// calls. Two callers that both use relative paths share it and must agree
// about it; a caller that cannot make that agreement names absolute paths,
// which nothing here can disturb.
extern "C" int SDL2Circle_IOChdir(const char *path)
{
    IoPath a{path, 0};
    SDL2Circle_CallOn0(io_chdir, &a);
    return a.r;
}

extern "C" int SDL2Circle_IOGetCwd(char *buf, uint32_t size)
{
    IoCwdQ a{buf, size, 0};
    SDL2Circle_CallOn0(io_getcwd, &a);
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
// Standard input: read on fd 0, the descriptor circle-libsdl2 binds to its
// own console (src/stdio.cpp). A read there can wait indefinitely for a
// keypress, and the call mailbox above is bounded on purpose — the servo
// drains it inline, on the same path that also has to pump USB and drain
// every other core's log ring, so nothing put through it may block.
//
// So this is not a call the servo serves. It is a REQUEST HANDED TO A TASK
// OF ITS OWN (CSplitStdinTask, below), which is free to sit inside a
// blocking read() because nothing else on core 0 waits on that task. The
// mailbox shape is the same one-outstanding-request handoff as the call box,
// just answered by a different task.
// ---------------------------------------------------------------------------

namespace
{

struct alignas(64) StdinBox
{
    std::atomic<u64> req{0};    // caller bumps to ask for a read
    std::atomic<u64> ack{0};    // the stdin task bumps once the result is in
    void            *buf;
    uint32_t         len;
    long             result;    // >= 0 bytes read, < 0 a negated errno
};

StdinBox g_stdin;

long read_stdin_now(void *buf, uint32_t len)
{
    errno = 0;
    long n = (long)::read(0, buf, len);
    return (n < 0) ? -errno : n;
}

} // namespace

extern "C" long SDL2Circle_ReadStdin(void *buf, uint32_t len)
{
    // No split, or already the core that owns the descriptor: nothing to
    // hand off, and core 0 is where CScheduler::Yield() inside the C
    // library's own blocking read is legitimate to run.
    if (!g_split.load(std::memory_order_acquire) || SDL2Circle_ThisCore() == 0)
        return read_stdin_now(buf, len);

    g_stdin.buf = buf;
    g_stdin.len = len;
    u64 seq = g_stdin.req.load(std::memory_order_relaxed) + 1;
    g_stdin.req.store(seq, std::memory_order_release);
    publish();

    // THE HEARTBEAT THE WATCHDOG WATCHES OTHERWISE ONLY BEATS INSIDE
    // SDL_PumpEvents (src/events.cpp) — no help to a program with no pump,
    // reading standard input as its whole main loop instead. That loop's own
    // truthful proof of life is this call: bumped once per request, so a
    // program reading a character at a time beats once per keystroke.
    SDL2Circle_HeartbeatBump();
    {
        // Waiting for a keypress has no deadline: the far side is a human,
        // who may sit at a prompt for as long as they like, and that is not
        // a fault. Gated so the report tracks core 0 itself instead — see
        // StallWatch — which is what a program actually wedged there still
        // trips.
        //
        // Nothing bumps the heartbeat again in here. wfe() below is a true
        // sleep: the only thing that wakes it is the sev() CSplitStdinTask
        // issues when the real read completes, so a wait that runs long has
        // no periodic wakeup to run a timer check on — a loop timing itself
        // against CTimer here would sit in wfe() the whole second and never
        // see it. The watchdog task (below, this file) reads g_stdin
        // directly instead, so this core does not have to do anything for
        // an outstanding request to keep counting as alive.
        StallWatch watch("a keypress on standard input", true);
        while (g_stdin.ack.load(std::memory_order_acquire) < seq)
        {
            wfe();
            watch.tick();
        }
    }
    return g_stdin.result;
}

// ---------------------------------------------------------------------------
// Core-0 tasks: the servo, the watchdog, and standard input.
// ---------------------------------------------------------------------------

class CSplitServoTask : public CTask
{
public:
    CSplitServoTask(void) : CTask(TASK_STACK_SIZE * 4) { SetName("sdl-servo"); }

    void Run(void) override
    {
        // ARM THE THREAD POINTER FIRST, BEFORE ANYTHING ELSE.
        //
        // Circle starts every task with TPIDR_EL0 at zero: InitializeRegs
        // clears the whole register block and never sets it, and the task
        // switch loads it back verbatim. So until this line runs, every
        // thread_local this task can reach — newlib's errno among them, which
        // is on the path of any log line — resolves through a null pointer.
        // Reading it does not fault reliably; it aliases low memory shared
        // with every other unarmed task, so it corrupts quietly and stops the
        // board somewhere else entirely.
        //
        // This task never ends, so the block it takes is never given back.
        SDL2Circle_SetThreadPointer(SDL2Circle_AllocTLSBlock());

        for (;;)
        {
            // One lap. Counted before any of the work, so the count says the
            // servo is running even when there is nothing for it to do.
            g_servo_beats.fetch_add(1, std::memory_order_relaxed);

            // Call mailbox (init, window/audio creation, I/O service).
            // Scoped because on the hardware core this is not housekeeping,
            // it is another core's work being done here — and how much of
            // it there is, is the question a split report exists to answer.
            u64 req = g_call.req.load(std::memory_order_acquire);
            if (req > g_call.ack.load(std::memory_order_relaxed))
            {
                SDL2CirclePerfScope serve(SDL2CIRCLE_PERF_SERVE);
                g_calls_started.fetch_add(1, std::memory_order_relaxed);
                publish();
                g_call.fn(g_call.arg);
                g_calls_served.fetch_add(1, std::memory_order_relaxed);
                g_call.ack.store(req, std::memory_order_release);
                publish();
            }

            // USB plug-and-play + HID -> event ring.
            {
                SDL2CirclePerfScope input(SDL2CIRCLE_PERF_INPUT);
                SDL2Circle_InputPump();
            }

            // Debug-UART robot hands -> event ring. Like InputPump, this reads
            // hardware (the serial RX), so it MUST run on core 0: the application core's
            // pump early-returns past it. Its synthesized key events go through
            // SDL_PushEvent, which on core 0 publishes to the same ring the app
            // core drains. (Inert unless --rapi-debug-uart armed it.)
            {
                SDL2CirclePerfScope input(SDL2CIRCLE_PERF_INPUT);
                SDL2Circle_InjectPump();
            }

            // Audio ring -> sound device.
            {
                SDL2CirclePerfScope audio(SDL2CIRCLE_PERF_AUDIO);
                SDL2Circle_AudioDrain();
            }

            // Every other core's log lines -> the console. This core owns
            // the serial device, so this is the only place they can reach
            // it, and it is why a core that logs never has to block. It is
            // charged as service because that is what it is: printing on
            // another core's behalf.
            {
                SDL2CirclePerfScope serve(SDL2CIRCLE_PERF_SERVE);
                SDL2Circle_LogDrain();
            }

            // Performance reports. The pump's tail call never runs under
            // the split (the application core's pump early-returns after its
            // shared-memory work), so the reporter's home is here on the
            // hardware core: the logger is core 0's device, and the other
            // cores' banks are shared memory read by their stamps.
            SDL2Circle_PerfTick();

            // The CPU clock and the case fan. This library owns them, and
            // this servo is the heartbeat that drives them under the split:
            // the application core's pump returns before its own tail, so
            // this is the only loop left that runs every frame.
            SDL2Circle_HardwareTick();

            {
                SDL2CirclePerfScope yield(SDL2CIRCLE_PERF_YIELD);
                CScheduler::Get()->Yield();
            }
        }
    }
};

class CSplitWatchdogTask : public CTask
{
public:
    CSplitWatchdogTask(void) : CTask(TASK_STACK_SIZE) { SetName("sdl-watchdog"); }

    void Run(void) override
    {
        // Same as the servo: Circle hands a new task a null thread pointer.
        SDL2Circle_SetThreadPointer(SDL2Circle_AllocTLSBlock());

        u64 lastBeat = 0;
        u64 lastServo = g_servo_beats.load(std::memory_order_relaxed);
        u64 lastChange = CTimer::GetClockTicks64();
        bool dumped = false;

        for (;;)
        {
            CScheduler::Get()->MsSleep(5000);

            u64 beat = g_heartbeat.load(std::memory_order_relaxed);
            u64 servo = g_servo_beats.load(std::memory_order_relaxed);
            u64 now = CTimer::GetClockTicks64();

            // A program parked in SDL2Circle_ReadStdin cannot beat this
            // itself while it waits: it is asleep in wfe(), and nothing
            // wakes it early to run a timer check (see the comment there).
            // What this task can check instead, on the application's
            // behalf, is whether a stdin request is still outstanding
            // (req != ack) while core 0's own servo is still lapping —
            // the same evidence StallWatch already trusts for its own
            // report on the waiting side. That is proof the program is
            // waiting on a human, not wedged, without needing the program
            // to do anything: the beat this tick represents is core 0
            // still serving the call, not the application pumping.
            u64 req = g_stdin.req.load(std::memory_order_acquire);
            u64 ack = g_stdin.ack.load(std::memory_order_acquire);
            bool servingStdin = (req != ack) && (servo != lastServo);
            lastServo = servo;

            if (beat != lastBeat || servingStdin)
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
                                      "beats=%llu servo=%llu calls start/served=%llu/%llu io=%llu frames post/ack=%llu/%llu ev push/drop=%llu/%llu",
                                      beat,
                                      g_servo_beats.load(),
                                      g_calls_started.load(),
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

class CSplitStdinTask : public CTask
{
public:
    CSplitStdinTask(void) : CTask(TASK_STACK_SIZE) { SetName("sdl-stdin"); }

    void Run(void) override
    {
        // Same as the servo: Circle hands a new task a null thread pointer.
        SDL2Circle_SetThreadPointer(SDL2Circle_AllocTLSBlock());

        u64 done = 0;
        for (;;)
        {
            u64 req = g_stdin.req.load(std::memory_order_acquire);
            if (req == done)
            {
                CScheduler::Get()->Yield();
                continue;
            }

            // THE ACTUAL, BLOCKING READ ON THE BOUND DESCRIPTOR. It may sit
            // here for as long as no key is pressed, yielding internally
            // (circle-newlib's console glue) the whole time. That yield only
            // cedes to this task's neighbours on core 0 — the servo and the
            // watchdog keep running — because it is this task that is
            // waiting, not the servo's own path.
            g_stdin.result = read_stdin_now(g_stdin.buf, g_stdin.len);
            done = req;
            g_stdin.ack.store(done, std::memory_order_release);
            publish();
        }
    }
};

// The scheduler, where this library had to make one.
//
// Circle's CTask registers itself with the scheduler while it is being
// constructed, and it reaches it through CScheduler::Get(), which stops the
// machine rather than reporting an absence. So the servo and the watchdog
// below cannot be created without one — and a host that had not declared a
// CScheduler took that fault here, in a constructor, with nothing said.
//
// Unlike CCPUThrottle, which this library owns for much the same reason,
// this one can be ASKED: CScheduler::IsActive() is a safe question and
// answers honestly. So the library makes one only where the host has not,
// and a host that has its own keeps it untouched.
//
// It cannot be an ordinary static object. A static is constructed before the
// kernel exists, and this has to happen after — the same reason the CPU
// throttle is placement-new'd into storage of its own.
//
// NOTHING EVER DESTROYS IT. The servo and the watchdog are registered with
// it and run for as long as the machine does, so taking it away would leave
// them registered with nothing; and an application that shuts its video
// world down and builds another one has not stopped those tasks. The flag
// records that this library is the owner, which is what a teardown would
// have to consult, and what keeps a second call from making a second one.
static bool s_bOwnScheduler = false;
alignas(CScheduler) static u8 s_SchedulerStore[sizeof(CScheduler)];

static void ensure_scheduler(void)
{
    if (s_bOwnScheduler || CScheduler::IsActive())
        return;

    new (s_SchedulerStore) CScheduler;
    s_bOwnScheduler = true;

    CLogger::Get()->Write(From, LogNotice,
                          "no scheduler in the system: this library made one");
}

extern "C" void SDL2Circle_SplitInit(void)
{
#ifdef ARM_ALLOW_MULTI_CORE
    if (g_split.load(std::memory_order_relaxed))
        return;

    // Before the tasks, not after: constructing one is what reaches for the
    // scheduler, and reaching for a scheduler that is not there is fatal.
    ensure_scheduler();

    new CSplitServoTask;      // CTask registers itself with the scheduler
    new CSplitWatchdogTask;
    new CSplitStdinTask;

    // The C++ threading runtime's creator task, on the same terms. It is
    // started from SDL2Circle_ArmCoreRuntime too, and does nothing without a
    // scheduler; this is the call that catches the host kernel which armed
    // core 0 before it had one, because the line above guarantees one now.
    SDL2Circle_ThreadRuntimeInit();

    g_split.store(true, std::memory_order_release);
    publish();

    CLogger::Get()->Write(From, LogNotice,
                          "core split active: servo + watchdog + stdin on core 0");
#else
    CLogger::Get()->Write(From, LogError,
                          "core split requires the multicore world; staying single-core");
#endif
}
