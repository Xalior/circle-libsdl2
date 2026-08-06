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

// ---------------------------------------------------------------------------
// SDL's own logging API
//
// SDL_Log and its family are an application's diagnostic channel, and for
// some applications the only one they have. Every one of them ends up in
// SDL2Circle_Log, so a line an application writes takes exactly the same
// route as a line the library writes: into the calling core's ring, drained
// by core 0's servo. That matters more here than it looks — the serial
// console is a device, a device belongs to core 0, and an application runs
// on another core by construction. Writing it directly would be writing a
// device from the wrong core.
//
// A category's priority is honoured before the line is formatted, so a
// suppressed line costs no formatting.
// ---------------------------------------------------------------------------

namespace
{

// SDL's documented defaults: application at INFO, assert at WARN, test at
// VERBOSE, everything else at ERROR. `s_priority` holds a category's
// priority once it has been set, and 0 means "still the default".
SDL_LogPriority s_priority[SDL_LOG_CATEGORY_CUSTOM] = {};
SDL_LogPriority s_all_priority = (SDL_LogPriority)0;

SDL_LogOutputFunction s_output = nullptr;
void *s_output_userdata = nullptr;

SDL_LogPriority DefaultPriority(int category)
{
    switch (category)
    {
    case SDL_LOG_CATEGORY_APPLICATION: return SDL_LOG_PRIORITY_INFO;
    case SDL_LOG_CATEGORY_ASSERT:      return SDL_LOG_PRIORITY_WARN;
    case SDL_LOG_CATEGORY_TEST:        return SDL_LOG_PRIORITY_VERBOSE;
    default:                           return SDL_LOG_PRIORITY_ERROR;
    }
}

SDL_LogPriority PriorityOf(int category)
{
    if (category >= 0 && category < SDL_LOG_CATEGORY_CUSTOM && s_priority[category])
        return s_priority[category];
    if (s_all_priority)
        return s_all_priority;
    return DefaultPriority(category);
}

// SDL's six priorities onto the four severities the serial log carries.
unsigned ToSeverity(SDL_LogPriority priority)
{
    switch (priority)
    {
    case SDL_LOG_PRIORITY_CRITICAL:
    case SDL_LOG_PRIORITY_ERROR:    return SDL2CIRCLE_LOG_ERROR;
    case SDL_LOG_PRIORITY_WARN:     return SDL2CIRCLE_LOG_WARNING;
    case SDL_LOG_PRIORITY_INFO:     return SDL2CIRCLE_LOG_NOTICE;
    default:                        return SDL2CIRCLE_LOG_DEBUG;
    }
}

// The name each category is logged under, so a line says where it came from
// the way every other line on the log does.
const char *CategoryName(int category)
{
    switch (category)
    {
    case SDL_LOG_CATEGORY_APPLICATION: return "app";
    case SDL_LOG_CATEGORY_ERROR:       return "sdlerror";
    case SDL_LOG_CATEGORY_ASSERT:      return "sdlassert";
    case SDL_LOG_CATEGORY_SYSTEM:      return "sdlsystem";
    case SDL_LOG_CATEGORY_AUDIO:       return "sdlaudio";
    case SDL_LOG_CATEGORY_VIDEO:       return "sdlvideo";
    case SDL_LOG_CATEGORY_RENDER:      return "sdlrender";
    case SDL_LOG_CATEGORY_INPUT:       return "sdlinput";
    case SDL_LOG_CATEGORY_TEST:        return "sdltest";
    default:                           return "sdl";
    }
}

} // namespace

extern "C" void SDL_LogMessageV(int category, SDL_LogPriority priority,
                                const char *fmt, va_list ap)
{
    if (!fmt || priority < PriorityOf(category))
        return;

    // An installed output function is given the finished text, as SDL does,
    // and takes the place of the log entirely.
    if (s_output)
    {
        char line[LOG_LINE_MAX + 1];
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);
        if (n < 0)
            return;
        line[(unsigned)n < LOG_LINE_MAX ? (unsigned)n : LOG_LINE_MAX] = '\0';
        s_output(s_output_userdata, category, priority, line);
        return;
    }

    SDL2Circle_LogV(CategoryName(category), ToSeverity(priority), fmt, ap);
}

extern "C" void SDL_LogMessage(int category, SDL_LogPriority priority,
                               const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, priority, fmt, ap);
    va_end(ap);
}

// SDL_Log is the application category at INFO; the rest name their priority
// and share it.
#define SDL2CIRCLE_LOG_AT(name, category, priority)                       \
    extern "C" void name(int cat, const char *fmt, ...)                   \
    {                                                                     \
        va_list ap;                                                       \
        va_start(ap, fmt);                                                \
        SDL_LogMessageV(cat, priority, fmt, ap);                          \
        va_end(ap);                                                       \
        (void)(category);                                                 \
    }

SDL2CIRCLE_LOG_AT(SDL_LogVerbose,  0, SDL_LOG_PRIORITY_VERBOSE)
SDL2CIRCLE_LOG_AT(SDL_LogDebug,    0, SDL_LOG_PRIORITY_DEBUG)
SDL2CIRCLE_LOG_AT(SDL_LogInfo,     0, SDL_LOG_PRIORITY_INFO)
SDL2CIRCLE_LOG_AT(SDL_LogWarn,     0, SDL_LOG_PRIORITY_WARN)
SDL2CIRCLE_LOG_AT(SDL_LogError,    0, SDL_LOG_PRIORITY_ERROR)
SDL2CIRCLE_LOG_AT(SDL_LogCritical, 0, SDL_LOG_PRIORITY_CRITICAL)

#undef SDL2CIRCLE_LOG_AT

extern "C" void SDL_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}

extern "C" void SDL_LogSetPriority(int category, SDL_LogPriority priority)
{
    if (category >= 0 && category < SDL_LOG_CATEGORY_CUSTOM)
        s_priority[category] = priority;
}

extern "C" SDL_LogPriority SDL_LogGetPriority(int category)
{
    return PriorityOf(category);
}

extern "C" void SDL_LogSetAllPriority(SDL_LogPriority priority)
{
    s_all_priority = priority;
    for (int i = 0; i < SDL_LOG_CATEGORY_CUSTOM; i++)
        s_priority[i] = priority;
}

extern "C" void SDL_LogResetPriorities(void)
{
    s_all_priority = (SDL_LogPriority)0;
    for (int i = 0; i < SDL_LOG_CATEGORY_CUSTOM; i++)
        s_priority[i] = (SDL_LogPriority)0;
}

extern "C" void SDL_LogSetOutputFunction(SDL_LogOutputFunction callback,
                                         void *userdata)
{
    s_output = callback;
    s_output_userdata = userdata;
}

extern "C" void SDL_LogGetOutputFunction(SDL_LogOutputFunction *callback,
                                         void **userdata)
{
    if (callback) *callback = s_output;
    if (userdata) *userdata = s_output_userdata;
}
