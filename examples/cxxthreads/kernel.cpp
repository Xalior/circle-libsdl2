//
// kernel.cpp — the C++ threading runtime, exercised from the application core.
//
// This is the smallest program that would have caught a fault which halted
// two games on the bench after everything else about them worked, and it has
// since grown to cover the rest of the same runtime.
//
// Circle's scheduler is SPECIFIED as core 0 only — doc/multicore.txt: "It
// cannot be used on more than one core at a time and should always run on
// core 0." circle-stdlib builds libc++'s threading on that scheduler, so the
// C++ standard library's threading is core-0-only by design and by
// documentation.
//
// This library runs applications on a SECOND core anyway. That is our
// decision, so the primitives that decision breaks are our debt — and this is
// the thing that proves the debt is paid. Everything below runs on core 1,
// where a port runs its game, and none of it used to be safe there:
//
//   std::recursive_mutex   took its ownership from "the task the scheduler is
//                          currently running", which off core 0 is whatever
//                          core 0 was doing at that instant and moves
//                          continuously. Lock and unlock microseconds apart
//                          on one core, and the two readings differ and the
//                          board halts.
//   std::mutex             worked while uncontended and blocked on a
//                          scheduler object when it was not, which is the
//                          same fault without the noise.
//   std::condition_variable  did both, on every wait.
//   std::thread            could not be created at all, because constructing
//                          a task registers it with the core-0 scheduler.
//   thread_local           read its storage out of "the current task", which
//                          off core 0 is the wrong task by construction.
//
// It prints its results and stops. There is no picture: a board with no
// display still has a serial console, and a report that needs a screen is no
// report at all.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

static const char From[] = "cxxthreads";

// Make a write visible to the other cores, and stop a core for good. Both are
// one instruction and neither belongs to any library — a core that has
// finished has nothing further it may safely touch.
static inline void PublishToOtherCores(void)
{
    asm volatile("dsb ish; sev" ::: "memory");
}

static void ParkCore(void)
{
    for (;;)
        asm volatile("wfe" ::: "memory");
}

// ---------------------------------------------------------------------------
// The results
// ---------------------------------------------------------------------------
//
// Core 1 records what it found and core 0 prints it. Nothing on core 1 logs:
// the serial console is core 0's device, and a check that halted the board on
// its way to saying something would take the report with it — which is
// exactly the failure being tested for.

static const unsigned MaxChecks = 24;

struct Check
{
    const char *m_pWhat;
    bool        m_bPassed;
};

static Check s_Check[MaxChecks];
static std::atomic<unsigned> s_nChecks{0};
static std::atomic<unsigned> s_bDone{0};
static std::atomic<unsigned> s_nReached{0};   // the line the board used to die on

static void Record(const char *pWhat, bool bPassed)
{
    const unsigned n = s_nChecks.fetch_add(1, std::memory_order_relaxed);
    if (n < MaxChecks)
    {
        s_Check[n].m_pWhat   = pWhat;
        s_Check[n].m_bPassed = bPassed;
    }
}

// ---------------------------------------------------------------------------
// What the tests share
// ---------------------------------------------------------------------------

// A thread_local with a destructor, which is how __cxa_thread_atexit is
// reached. The worker thread ends, so its copy is destroyed; core 1's copy
// never is, because core 1 never ends — a limit this library states rather
// than hides.
struct Marker
{
    int m_nValue = 0;
    ~Marker(void);
};

static std::atomic<unsigned> s_nMarkersDestroyed{0};

Marker::~Marker(void)
{
    s_nMarkersDestroyed.fetch_add(1, std::memory_order_release);
}

static thread_local Marker t_Marker;

// Reading it through a function the compiler cannot see into, so the read is
// a real one through the thread pointer rather than something folded away.
static int __attribute__((noinline)) ReadMarker(void)
{
    return t_Marker.m_nValue;
}

// The condition variable and what it guards.
static std::mutex              s_Mutex;
static std::condition_variable s_Cond;
static bool                    s_bReady   = false;
static int                     s_nWorkerMarker = -1;

CKernel::CKernel(void)
    : m_Serial(0, FALSE, 0),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer)
{
    m_ActLED.Blink(3);
}

boolean CKernel::Initialize(void)
{
    boolean bOK = TRUE;
    if (bOK) bOK = m_Serial.Initialize(115200);
    if (bOK) bOK = m_Logger.Initialize(&m_Serial);
    if (bOK) bOK = m_Interrupt.Initialize();
    if (bOK) bOK = m_Timer.Initialize();

    // Arms core 0's own thread pointer AND starts the threading runtime's
    // core-0 creator task, which is what lets core 1 create a std::thread.
    // Every port already makes this call; that is the whole of what a port
    // has to do to adopt any of this.
    if (bOK) SDL2Circle_ArmCoreRuntime();

    if (bOK) bOK = m_Cores.Initialize();
    return bOK;
}

// ---------------------------------------------------------------------------
// The worker thread
// ---------------------------------------------------------------------------
//
// Created FROM core 1, which is the part that used to be impossible: building
// a scheduler task registers it with the core-0 scheduler, so the request is
// passed to core 0 and the thread runs there, cooperatively. It gets its own
// thread_local storage, which is what the reading it reports back proves.

static void Worker(void)
{
    t_Marker.m_nValue = 4242;
    s_nWorkerMarker = ReadMarker();

    {
        const std::lock_guard<std::mutex> g(s_Mutex);
        s_bReady = true;
    }
    s_Cond.notify_one();
}

void CTestCores::Run(unsigned nCore)
{
    SDL2Circle_ArmCoreRuntime();

    if (nCore != 1)
    {
        ParkCore();
        return;
    }

    // Everything below runs on core 1.

    // ---- std::recursive_mutex ---------------------------------------------
    //
    // Reaching the line after the first unlock is itself the result: that is
    // where the board used to stop.
    {
        std::recursive_mutex m;
        m.lock();
        m.unlock();
        s_nReached.fetch_add(1, std::memory_order_release);
        Record("recursive_mutex: lock then unlock", true);
    }

    {
        // Recursion to depth, which is the whole reason the type exists, and
        // free again afterwards so the depth counting is right rather than
        // merely survivable.
        std::recursive_mutex m;
        m.lock();
        m.lock();
        m.lock();
        m.unlock();
        m.unlock();
        m.unlock();
        Record("recursive_mutex: three deep", true);

        const bool bFree = m.try_lock();
        Record("recursive_mutex: free again afterwards", bFree);
        if (bFree)
            m.unlock();
    }

    {
        // The ordinary way a game meets this type.
        std::recursive_mutex m;
        {
            const std::lock_guard<std::recursive_mutex> a(m);
            const std::lock_guard<std::recursive_mutex> b(m);   // recursive
        }
        const bool bFree = m.try_lock();
        Record("recursive_mutex: nested lock_guard", bFree);
        if (bFree)
            m.unlock();
    }

    {
        // A static, which is how both of the games that fell over actually
        // hold theirs — one in an audio manager, one in a logging library
        // that takes it on every line.
        static std::recursive_mutex s_static;
        for (unsigned i = 0; i < 1000; i++)
        {
            const std::lock_guard<std::recursive_mutex> g(s_static);
        }
        Record("recursive_mutex: a static, taken a thousand times", true);
    }

    // ---- std::mutex -------------------------------------------------------
    //
    // It must not have regressed: it worked here before any of this and every
    // port depends on it.
    {
        std::mutex m;
        m.lock();
        m.unlock();
        const bool bFree = m.try_lock();
        Record("mutex: lock, unlock, try_lock", bFree);
        if (bFree)
            m.unlock();
    }

    // ---- thread_local -----------------------------------------------------
    {
        t_Marker.m_nValue = 1234;
        Record("thread_local: written and read back on the application core",
               ReadMarker() == 1234);
    }

    // ---- std::thread, condition variable, and per-thread storage ----------
    {
        std::thread worker(Worker);
        Record("thread: created from the application core", worker.joinable());

        {
            // The wait itself: core 1 blocks here while core 0 runs the
            // worker task, and is released by its notify.
            std::unique_lock<std::mutex> lock(s_Mutex);
            s_Cond.wait(lock, [] { return s_bReady; });
        }
        Record("condition_variable: waited on, and woken by the thread", true);

        worker.join();
        Record("thread: joined", true);

        Record("thread_local: the thread had its own copy",
               s_nWorkerMarker == 4242);
        Record("thread_local: this core's copy is untouched by it",
               ReadMarker() == 1234);
        Record("thread_local: its destructor ran when the thread ended",
               s_nMarkersDestroyed.load(std::memory_order_acquire) >= 1);
    }

    // ---- a timed wait that runs out ---------------------------------------
    //
    // Nothing will signal this one. It proves the deadline arithmetic, which
    // crosses two clocks: the calendar time libc++ builds an absolute
    // deadline in, and the free-running counter this core measures against.
    {
        const u64 nStart = CTimer::GetClockTicks64();

        std::unique_lock<std::mutex> lock(s_Mutex);
        const std::cv_status status =
            s_Cond.wait_for(lock, std::chrono::milliseconds(100));
        lock.unlock();

        const u64 nElapsed = CTimer::GetClockTicks64() - nStart;

        Record("condition_variable: a timed wait reports its timeout",
               status == std::cv_status::timeout);
        Record("condition_variable: and waited about the right length of time",
               nElapsed >= 90000 && nElapsed <= 400000);
    }

    // ---- std::call_once ---------------------------------------------------
    {
        static std::once_flag flag;
        unsigned nRan = 0;
        for (unsigned i = 0; i < 5; i++)
            std::call_once(flag, [&nRan] { nRan++; });
        Record("call_once: ran exactly once out of five calls", nRan == 1);
    }

    s_bDone.store(1, std::memory_order_release);
    PublishToOtherCores();
    ParkCore();
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice,
                   "the C++ threading runtime, on core 1 (no scheduler there)");

    // Bounded: if core 1 has stopped, this must say so rather than hang and
    // look like the very fault it is testing for. The wait YIELDS rather than
    // delays, because core 0 is where the worker thread runs and it will not
    // run if this core never gives it the chance.
    const unsigned nTimeoutMs = 10000;
    const unsigned nDeadline  = m_Timer.GetTicks() + MSEC2HZ(nTimeoutMs);

    while (!s_bDone.load(std::memory_order_acquire)
           && m_Timer.GetTicks() < nDeadline)
    {
        m_Scheduler.Yield();
    }

    if (!s_bDone.load(std::memory_order_acquire))
    {
        m_Logger.Write(From, LogError,
                       "core 1 did not finish: %u check(s) ran, first unlock %s",
                       s_nChecks.load(),
                       s_nReached.load() ? "was reached" : "WAS NEVER REACHED");
        m_Logger.Write(From, LogError,
                       "that is the fault this example exists to catch");
        return ShutdownHalt;
    }

    unsigned nRan    = s_nChecks.load();
    unsigned nFailed = 0;
    if (nRan > MaxChecks)
        nRan = MaxChecks;

    for (unsigned i = 0; i < nRan; i++)
    {
        const boolean bOK = s_Check[i].m_bPassed ? TRUE : FALSE;
        if (!bOK)
            nFailed++;
        m_Logger.Write(From, bOK ? LogNotice : LogError, "%s  %s",
                       bOK ? "ok  " : "FAIL", s_Check[i].m_pWhat);
    }

    m_Logger.Write(From, nFailed ? LogError : LogNotice,
                   "%u check(s) on core 1, %u failed", nRan, nFailed);
    m_Logger.Write(From, nFailed ? LogError : LogNotice, "%s",
                   nFailed ? "FAILED"
                           : "the C++ threading runtime works off core 0");

    return ShutdownHalt;
}
