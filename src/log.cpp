//
// log.cpp - output from any core, without touching the hardware.
//
// This file keeps two questions apart: where output goes, and what it looks
// like once it gets there.
//
// Where output goes is a property of the machine: the serial port always,
// and the screen as well until an application takes the display. It is one
// decision for the whole board, held as Circle's logger target device.
// src/console.cpp puts a tee in front of the serial device when the screen
// is attached and puts the serial device back when it is dropped, so the
// target is the destination set at any moment. DestinationWrite below is
// the whole of reaching it.
//
// What output looks like is a property of whoever is printing. A log record
// carries a source, a severity and a timestamp, taken from Circle's logger.
// Raw output - a program's own standard output - carries nothing at all: a
// program printing a number expects that number back, not a decorated
// version of it, and it may print half a line or bytes that are not text.
// So the raw channel adds no source, no severity, no timestamp and no line
// discipline.
//
// Both go to the same destinations by the same rules, and neither is built
// on top of the other.
//
// The serial console is a device, and devices belong to the hardware core.
// Every other core writes its lines into a ring of its own instead, and the
// hardware core's servo drains all the rings into the logger. Nothing
// crosses but memory, and a core that logs is never blocked by a core that
// is printing.
//
// One ring per core, single producer and single consumer, exactly like the
// event and audio rings next door: the owning core is the only writer, the
// servo is the only reader, and the two never need a lock between them.
//
// Log records and raw output share that ring, so a program's printed line
// and the log line it writes next come out in the order it produced them.
// A record says which of the two it is and the drain gives it to the right
// one.
//
// A ring that is full drops the line and counts it, rather than stalling
// the producing core or overwriting a line already in the ring. The console
// is a fixed, slow width, so dropping is the steady state for a chatty
// application, not a fault. The drain reports when dropping starts and when
// it stops, rather than once per dropped line.
//
// The servo's drain is bounded (see LOG_DRAIN_MAX_US below), which is what
// keeps core 0 alive: an unbounded drain paired with a producer that never
// waits would be enough to stop the board.
//
// The hardware core does not use a ring: it owns the console and writes
// straight through. That keeps the boot log immediate, before any servo
// exists to drain anything, and keeps interrupt handlers safe, since an
// interrupt on the hardware core can log without landing in the middle of a
// half-written record.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/device.h>
#include <circle/logger.h>
#include <circle/timer.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Circle's logger takes a line at a time and truncates past its own buffer,
// so there is no value in carrying more than it will print.
static const unsigned LOG_LINE_MAX = 200;

// Per core. Sized to hold a start-up burst, not just a frame's worth: the
// lines a port most needs are the ones it produces while bringing itself up,
// before anything is steady, and those are exactly the ones a small ring
// loses. Memory is not what is scarce on these boards.
//
// Must be a power of two: the head and tail are free-running counters
// reduced modulo this size, so a size that does not divide 2^32 would make
// the two disagree about where a record sits the first time they wrap.
static const unsigned LOG_RING_BYTES = 32768;
static const unsigned LOG_MAX_CORES = 4;

// How much the servo prints in one pass, and why there is a limit at all.
//
// The console is a device on core 0, and core 0's servo is also what pumps
// USB, feeds the sound device and yields to the scheduler. A drain that ran
// until its ring was empty would give that ring priority over every one of
// those, and could fail to terminate: the producer never waits - a full
// ring drops the line and returns - so an application that logs faster than
// the console can carry keeps the ring permanently non-empty, `head` never
// meets `tail`, and the drain never returns. Core 0 then stops servicing
// everything it owns: USB never finishes enumerating, video never comes up,
// and the application's own lines never appear, because they are being
// dropped into a ring nobody is emptying.
//
// So the drain takes a bounded bite and returns: a time slice rather than a
// line count, because what is being rationed is time on the console. At
// 115200 baud in polling mode one 80-character line is around 7 ms, and the
// same budget stays honest at a different rate or on a different device.
// The record cap is a second bound for the case where lines are short
// enough that the clock has barely moved.
static const unsigned LOG_DRAIN_MAX_US = 2000;
static const unsigned LOG_DRAIN_MAX_RECORDS = 16;

namespace
{

// A record is this header followed by its bytes. The header is copied byte
// by byte into the ring, so nothing here needs the ring to be aligned.
//
// `from` is stored as a pointer, not a copy: every subsystem tag in this
// library is a string literal, so it outlives any record that names it and
// every core sees it at the same address. Callers must respect that - a tag
// built on the stack would be gone by the time the servo printed it.
//
// `raw` is which of the two channels produced the record. A raw record's
// bytes go to the destination exactly as they arrived and `from` and
// `severity` mean nothing in it; anything else is a log line and gets its
// label from Circle's logger when the servo prints it.
struct LogRec
{
    const char *from;
    unsigned severity;
    unsigned len;          // bytes following the header
    bool raw;
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
//
// The text is one byte longer than the longest line it may hold. A line that
// reaches LOG_LINE_MAX characters without a newline is flushed by length, and
// terminating it writes at index LOG_LINE_MAX - which without the extra byte
// is one past the end, landing in the next core's buffer.
struct LineBuffer
{
    unsigned len;
    char text[LOG_LINE_MAX + 1];
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

// Reaches every destination at once, adding nothing on the way.
//
// Circle's logger holds the target device, and src/console.cpp is what makes
// that device reach more than one place: the tee it installs at attach
// writes the serial port and then the screen, and the drop puts the plain
// serial device back. So reading the target back is reading the destination
// set as it stands, with no second copy of the rule to keep in step.
//
// There is no target before the host kernel has initialised its logger, and
// bytes handed over then have nowhere to go.
//
// CLogger writes its own text to the target outside its lock
// (lib/logger.cpp, CLogger::Write(const char *): the target write happens
// before m_SpinLock.Acquire), so this takes nothing the logger does not.
void DestinationWrite(const char *bytes, unsigned len)
{
    CDevice *pTarget = CLogger::Get()->GetTarget();
    if (pTarget != nullptr)
        pTarget->Write(bytes, len);
}

// Whether each core is currently losing output, and how much it has lost
// since it started. Only the servo touches these.
bool g_dropping[LOG_MAX_CORES] = {};
u32  g_droppedTotal[LOG_MAX_CORES] = {};

// Reports when a ring starts losing records and when it stops, rather than
// once per pass. A ring is full precisely when the console cannot keep up,
// so a line per pass about it would spend the scarce console time
// describing its own scarcity, pushing out the very records it is
// reporting the loss of. Two lines per episode say the same thing.
void ReportDrops(unsigned core, LogRing &ring)
{
    const u32 lost = ring.dropped.exchange(0, std::memory_order_relaxed);

    if (lost)
    {
        g_droppedTotal[core] += lost;
        if (!g_dropping[core])
        {
            g_dropping[core] = true;
            CLogger::Get()->Write("sdl2log", LogWarning,
                                  "core %u: output ring full, records are "
                                  "being dropped (the console cannot carry "
                                  "them this fast)", core);
        }
        return;
    }

    if (g_dropping[core])
    {
        g_dropping[core] = false;
        CLogger::Get()->Write("sdl2log", LogWarning,
                              "core %u: output ring keeping up again, %u "
                              "record(s) lost in total", core,
                              g_droppedTotal[core]);
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

// Publish one finished record into the calling core's ring.
void RingPush(const char *from, unsigned severity, const char *text,
              unsigned len, bool raw)
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

    LogRec rec{from, severity, len, raw};
    RingCopyIn(ring, tail, &rec, sizeof(rec));
    RingCopyIn(ring, tail + sizeof(rec), text, len);

    // The record is only visible once its bytes are: the release on tail is
    // what makes a half-written record impossible for the servo to see.
    ring.tail.store(tail + need, std::memory_order_release);
    asm volatile("dsb ish; sev" ::: "memory");
}

// Hand a finished line to wherever it goes, labelled on the way.
void Emit(const char *from, unsigned severity, const char *text, unsigned len)
{
    if (WritesDirectly())
    {
        // %s and not the text itself: a line carrying a percent sign is
        // data, never a format.
        CLogger::Get()->Write(from, ToCircle(severity), "%s", text);
        return;
    }
    RingPush(from, severity, text, len, false);
}

// Hand a piece of raw output to the same destinations, untouched.
void EmitRaw(const char *bytes, unsigned len)
{
    if (WritesDirectly())
    {
        DestinationWrite(bytes, len);
        return;
    }

    // A record holds at most LOG_LINE_MAX bytes, so a longer write becomes
    // several records. Truncating is what the labelled path does with an
    // over-long line, and it is exactly wrong here: a byte stream that loses
    // its middle is worse than one that arrives in pieces, and the pieces
    // rejoin at the destination because nothing is added between them.
    while (len > 0)
    {
        const unsigned take = len > LOG_LINE_MAX ? LOG_LINE_MAX : len;
        RingPush(nullptr, SDL2CIRCLE_LOG_NOTICE, bytes, take, true);
        bytes += take;
        len   -= take;
    }
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
    // Byte-oriented material that is a log - a subsystem that produces its
    // diagnostics as a stream rather than a line at a time - arrives in
    // whatever pieces it was written in, so lines are assembled here and
    // published one at a time. The logger prints lines; it has nowhere to
    // put half of one.
    //
    // A program's ordinary output is not this: it goes to
    // SDL2Circle_WriteBytes below, which labels nothing and waits for
    // nothing.
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

extern "C" void SDL2Circle_WriteBytes(const char *bytes, unsigned len)
{
    // A program's own output. Nothing here may touch it: no tag, no
    // severity, no timestamp, and no waiting for an end of line. Half a
    // line is output, a byte that is not text is output, and a program
    // that prints a number gets that number back and nothing else.
    //
    // The only thing this shares with the log is where the bytes end up.
    if (bytes == nullptr || len == 0)
        return;

    EmitRaw(bytes, len);
}

// ---------------------------------------------------------------------------
// Draining
// ---------------------------------------------------------------------------

void SDL2Circle_LogDrain(void)
{
    if (!SDL2Circle_SplitActive())
        return;                       // nothing rings when nothing is split

    const u64 started = CTimer::GetClockTicks64();   // CLOCKHZ is 1 MHz
    unsigned printed = 0;

    // Where this pass starts, and why it moves. The bounded bite has to be
    // shared out, or the lowest-numbered core that keeps logging would spend
    // the whole budget every pass and the cores above it would never be
    // drained at all - silence that would look exactly like a core that had
    // stopped. Each pass resumes at the core after the one the budget ran
    // out on, so every ring gets its turn.
    static unsigned s_nextCore = 0;

    for (unsigned i = 0; i < LOG_MAX_CORES; i++)
    {
        const unsigned c = (s_nextCore + i) % LOG_MAX_CORES;
        LogRing &ring = g_rings[c];

        // Lines are taken in the order the core produced them, which is the
        // order that means anything.
        for (;;)
        {
            if (printed >= LOG_DRAIN_MAX_RECORDS
                || CTimer::GetClockTicks64() - started >= LOG_DRAIN_MAX_US)
            {
                // Out of budget. Next pass starts at the core after this
                // one: starting at this one again would hand the whole
                // budget back to the ring that just used it, and a core
                // that logs without pause would keep every core above it
                // permanently silent. What is left here waits one turn,
                // which is a delay rather than a loss.
                s_nextCore = (c + 1) % LOG_MAX_CORES;
                return;
            }

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

            // The one place the two channels part company. Raw output goes to
            // the destination by count, because it may hold a byte that is
            // not text and it has no line to end; a log record goes through
            // the logger and comes out labelled.
            if (rec.raw)
                DestinationWrite(line, rec.len);
            else
                CLogger::Get()->Write(rec.from ? rec.from : "sdl2",
                                      ToCircle(rec.severity), "%s", line);
            printed++;
        }

        ReportDrops(c, ring);
    }

    // Every ring came up empty, so the next pass may as well start at the
    // beginning again.
    s_nextCore = 0;
}

// ---------------------------------------------------------------------------
// SDL's own logging API
//
// SDL_Log and its family are an application's diagnostic channel, and for
// some applications the only one they have. Every one of them ends up in
// SDL2Circle_Log, so a line an application writes takes exactly the same
// route as a line the library writes: into the calling core's ring, drained
// by core 0's servo. That matters more here than it looks - the serial
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
