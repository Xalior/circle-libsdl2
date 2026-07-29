//
// log.cpp — a log line from any core, without touching the hardware.
//
// The serial console is a device, and devices belong to the hardware core.
// So every other core writes its lines into a ring of its own instead, and
// the hardware core's servo drains all the rings into the logger. Nothing
// crosses but memory, and a core that logs is never blocked by a core that
// is printing.
//
// One ring per core, single producer and single consumer, exactly like the
// event and audio rings next door: the owning core is the only writer, the
// servo is the only reader, and the two never need a lock between them.
//
// A ring that is full DROPS the line and counts it. Losing a line is bad;
// stalling the core that produced it, or overwriting a line already in the
// ring, is worse. The count is printed with the next drain, so the record
// is honest about its own gaps.
//
// The hardware core does not use a ring. It owns the console, so it writes
// straight through — which keeps the boot log immediate, before any servo
// exists to drain anything, and keeps interrupt handlers safe: an interrupt
// on the hardware core can log without ever landing in the middle of a
// half-written record.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/logger.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Circle's logger takes a line at a time and truncates past its own buffer,
// so there is no value in carrying more than it will print.
static const unsigned LOG_LINE_MAX = 200;

// Per core. Sized to hold a burst of a whole frame's worth of lines while
// the servo is busy elsewhere, which is the case that would otherwise drop.
static const unsigned LOG_RING_BYTES = 8192;
static const unsigned LOG_MAX_CORES = 4;

namespace
{

// A record is this header followed by its text. The header is copied byte
// by byte into the ring, so nothing here needs the ring to be aligned.
//
// `from` is stored as a POINTER, not a copy: every subsystem tag in this
// library is a string literal, so it outlives any record that names it and
// every core sees it at the same address. Callers must respect that — a tag
// built on the stack would be gone by the time the servo printed it.
struct LogRec
{
    const char *from;
    unsigned severity;
    unsigned len;          // bytes of text following the header
};

struct alignas(64) LogRing
{
    alignas(64) std::atomic<u32> tail{0};      // producer: the owning core
    alignas(64) std::atomic<u32> head{0};      // consumer: the servo
    alignas(64) std::atomic<u32> dropped{0};   // lines that did not fit
    alignas(64) char data[LOG_RING_BYTES];
};

LogRing g_rings[LOG_MAX_CORES];

// Partial output waiting for its end of line, one per core. Only ever
// touched by the core it belongs to.
struct LineBuffer
{
    unsigned len;
    char text[LOG_LINE_MAX];
};

LineBuffer g_lines[LOG_MAX_CORES];

inline TLogSeverity ToCircle(unsigned severity)
{
    switch (severity)
    {
    case SDL2CIRCLE_LOG_ERROR:   return LogError;
    case SDL2CIRCLE_LOG_WARNING: return LogWarning;
    case SDL2CIRCLE_LOG_DEBUG:   return LogDebug;
    default:                     return LogNotice;
    }
}

// The hardware core owns the console and writes through it directly. So
// does every core when the split is not active, because then there is only
// this one core and no servo to drain anything.
inline bool WritesDirectly(void)
{
    return !SDL2Circle_SplitActive() || SDL2Circle_ThisCore() == 0;
}

void RingCopyIn(LogRing &ring, u32 at, const void *src, unsigned len)
{
    const char *p = (const char *)src;
    for (unsigned i = 0; i < len; i++)
        ring.data[(at + i) % LOG_RING_BYTES] = p[i];
}

void RingCopyOut(LogRing &ring, u32 at, void *dst, unsigned len)
{
    char *p = (char *)dst;
    for (unsigned i = 0; i < len; i++)
        p[i] = ring.data[(at + i) % LOG_RING_BYTES];
}

// Publish one finished line into the calling core's ring.
void RingPush(const char *from, unsigned severity, const char *text, unsigned len)
{
    if (len > LOG_LINE_MAX)
        len = LOG_LINE_MAX;

    LogRing &ring = g_rings[SDL2Circle_ThisCore() % LOG_MAX_CORES];

    u32 tail = ring.tail.load(std::memory_order_relaxed);
    u32 head = ring.head.load(std::memory_order_acquire);
    unsigned need = sizeof(LogRec) + len;
    if (LOG_RING_BYTES - (tail - head) < need)
    {
        ring.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogRec rec{from, severity, len};
    RingCopyIn(ring, tail, &rec, sizeof(rec));
    RingCopyIn(ring, tail + sizeof(rec), text, len);

    // The record is only visible once its bytes are: the release on tail is
    // what makes a half-written record impossible for the servo to see.
    ring.tail.store(tail + need, std::memory_order_release);
    asm volatile("dsb ish; sev" ::: "memory");
}

// Hand a finished line to wherever it goes.
void Emit(const char *from, unsigned severity, const char *text, unsigned len)
{
    if (WritesDirectly())
    {
        // %s and not the text itself: a line carrying a percent sign is
        // data, never a format.
        CLogger::Get()->Write(from, ToCircle(severity), "%s", text);
        return;
    }
    RingPush(from, severity, text, len);
}

} // namespace

// ---------------------------------------------------------------------------
// Producing
// ---------------------------------------------------------------------------

extern "C" void SDL2Circle_LogV(const char *from, unsigned severity,
                                const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX + 1];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0)
        return;
    unsigned len = (unsigned)n < LOG_LINE_MAX ? (unsigned)n : LOG_LINE_MAX;
    line[len] = '\0';
    Emit(from ? from : "sdl2", severity, line, len);
}

extern "C" void SDL2Circle_Log(const char *from, unsigned severity,
                               const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    SDL2Circle_LogV(from, severity, fmt, args);
    va_end(args);
}

extern "C" void SDL2Circle_LogBytes(const char *from, const char *bytes,
                                    unsigned len)
{
    // Byte-oriented output — an application's stdout — arrives in whatever
    // pieces it was written in, so lines are assembled here and published
    // one at a time. The logger prints lines; it has nowhere to put half of
    // one.
    LineBuffer &buf = g_lines[SDL2Circle_ThisCore() % LOG_MAX_CORES];

    for (unsigned i = 0; i < len; i++)
    {
        char c = bytes[i];
        if (c == '\n' || buf.len == LOG_LINE_MAX)
        {
            buf.text[buf.len] = '\0';
            // A bare newline is a blank line, which is worth keeping: it is
            // how output is spaced.
            Emit(from, SDL2CIRCLE_LOG_NOTICE, buf.text, buf.len);
            buf.len = 0;
            if (c == '\n')
                continue;
        }
        if (c == '\r')
            continue;
        buf.text[buf.len++] = c;
    }
}

// ---------------------------------------------------------------------------
// Draining
// ---------------------------------------------------------------------------

void SDL2Circle_LogDrain(void)
{
    if (!SDL2Circle_SplitActive())
        return;                       // nothing rings when nothing is split

    // Core order, so a reader sees each core's lines in the order that core
    // produced them, which is the order that means anything.
    for (unsigned c = 0; c < LOG_MAX_CORES; c++)
    {
        LogRing &ring = g_rings[c];

        for (;;)
        {
            u32 head = ring.head.load(std::memory_order_relaxed);
            u32 tail = ring.tail.load(std::memory_order_acquire);
            if (head == tail)
                break;

            LogRec rec;
            RingCopyOut(ring, head, &rec, sizeof(rec));
            if (rec.len > LOG_LINE_MAX)
                rec.len = LOG_LINE_MAX;

            char line[LOG_LINE_MAX + 1];
            RingCopyOut(ring, head + sizeof(rec), line, rec.len);
            line[rec.len] = '\0';

            ring.head.store(head + sizeof(rec) + rec.len,
                            std::memory_order_release);

            CLogger::Get()->Write(rec.from ? rec.from : "sdl2",
                                  ToCircle(rec.severity), "%s", line);
        }

        // Report the gaps rather than hide them.
        u32 lost = ring.dropped.exchange(0, std::memory_order_relaxed);
        if (lost)
            CLogger::Get()->Write("sdl2log", LogWarning,
                                  "core %u: %u log lines dropped, ring full", c, lost);
    }
}
