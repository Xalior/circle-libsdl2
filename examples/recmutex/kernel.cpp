//
// kernel.cpp — std::recursive_mutex on a core that has no scheduler.
//
// This is the smallest program that would have caught a fault which halted
// two games on the bench after everything else about them worked.
//
// std::recursive_mutex used to be backed by Circle's CMutex, which records
// ownership as "the task the scheduler is currently running". There is one
// scheduler and it belongs to core 0, so on any other core the answer is
// whatever core 0 happened to be doing at that instant — and it moves
// continuously. Lock and unlock microseconds apart on the same core, and the
// two readings differ and Circle asserts. No contention, no second core
// involved, nothing exotic: just the wrong core.
//
// It hid because it is a property of WHICH MUTEX TYPE the code reaches for,
// not of what the code does. std::mutex was always fine, so almost all
// locking worked, and only the code that happened to want recursion fell
// over. That is why this example uses both, and why it runs them on core 1
// rather than on core 0 where everything passes either way.
//
// It prints its results and stops. There is no picture: a board with no
// display still has a serial console, and this is a test of the C++ runtime
// rather than of the video path.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>

#include <atomic>
#include <mutex>

static const char From[] = "recmutex";

// Make a write visible to the other cores, and stop a core for good. Both
// are one instruction and neither belongs to any library — a core that has
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

// Results, published by core 1 and read by core 0. Plain atomics: the point
// of the exercise is that core 1 has nothing else it can safely use.
static std::atomic<unsigned> s_Ran{0};
static std::atomic<unsigned> s_Passed{0};
static std::atomic<unsigned> s_Failed{0};
static std::atomic<unsigned> s_Done{0};
static std::atomic<unsigned> s_Reached{0};

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
    if (bOK) SDL2Circle_ArmCoreRuntime();
    if (bOK) bOK = m_Cores.Initialize();
    return bOK;
}

// Each check records itself and moves on. Nothing here logs: core 1 must not
// touch a device, and a check that halted the board would take the report
// with it — which is exactly the failure being tested for.
static void Check(bool bOK)
{
    s_Ran.fetch_add(1, std::memory_order_relaxed);
    if (bOK)
        s_Passed.fetch_add(1, std::memory_order_relaxed);
    else
        s_Failed.fetch_add(1, std::memory_order_relaxed);
}

void CTestCores::Run(unsigned nCore)
{
    SDL2Circle_ArmCoreRuntime();

    if (nCore != 1)
    {
        ParkCore();
        return;
    }

    // Everything below runs on core 1. Reaching the line after the first
    // unlock is itself the result: that is where the board used to stop.
    {
        std::recursive_mutex m;
        m.lock();
        m.unlock();
        s_Reached.fetch_add(1, std::memory_order_release);
        Check(true);
    }

    {
        // Recursion to depth, which is the whole reason the type exists.
        std::recursive_mutex m;
        m.lock();
        m.lock();
        m.lock();
        m.unlock();
        m.unlock();
        m.unlock();
        Check(true);

        // Free again afterwards, so the depth counting is right and not
        // merely survivable.
        Check(m.try_lock());
        m.unlock();
    }

    {
        // The ordinary way a game meets this type.
        std::recursive_mutex m;
        {
            const std::lock_guard<std::recursive_mutex> a(m);
            const std::lock_guard<std::recursive_mutex> b(m);   // recursive
            Check(true);
        }
        Check(m.try_lock());
        m.unlock();
    }

    {
        // std::mutex on the same core, because it must not have regressed:
        // it worked here before this change and every port depends on it.
        std::mutex m;
        m.lock();
        m.unlock();
        Check(true);
        Check(m.try_lock());
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
        Check(true);
    }

    s_Done.store(1, std::memory_order_release);
    PublishToOtherCores();
    ParkCore();
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice,
                   "std::recursive_mutex on core 1 (no scheduler there)");

    // Bounded: if core 1 has stopped, this must say so rather than hang and
    // look like the very fault it is testing for.
    const unsigned nTimeoutMs = 5000;
    unsigned nWaited = 0;
    while (!s_Done.load(std::memory_order_acquire) && nWaited < nTimeoutMs)
    {
        m_Timer.MsDelay(10);
        nWaited += 10;
    }

    if (!s_Done.load(std::memory_order_acquire))
    {
        m_Logger.Write(From, LogError,
                       "core 1 did not finish: %u check(s) ran, first unlock %s",
                       s_Ran.load(),
                       s_Reached.load() ? "was reached" : "WAS NEVER REACHED");
        m_Logger.Write(From, LogError,
                       "that is the fault this example exists to catch");
        return ShutdownHalt;
    }

    const unsigned nFailed = s_Failed.load();
    m_Logger.Write(From, nFailed ? LogError : LogNotice,
                   "%u check(s) on core 1: %u passed, %u failed",
                   s_Ran.load(), s_Passed.load(), nFailed);
    m_Logger.Write(From, LogNotice, "%s",
                   nFailed ? "FAILED" : "std::recursive_mutex works off core 0");

    return ShutdownHalt;
}
