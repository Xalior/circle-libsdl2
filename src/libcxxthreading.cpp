//
// libcxxthreading.cpp - libc++'s threading runtime, for the cores this
// library actually puts applications on.
//
// Circle's scheduler is specified as core 0 only. doc/multicore.txt, lines
// 62-64:
//
//     "The cooperative non-preemptive scheduler is intended to allow multiple
//      threads of operation on a single core. It cannot be used on more than
//      one core at a time and should always run on core 0."
//
// circle-stdlib builds libc++'s threading on that scheduler, and says so, so
// the C++ standard library's threading is core-0-only by design and by
// documentation. Every primitive it ships is right where it was meant to run.
//
// What is out of spec is this library's: it starts the application on a
// second core and then lets it call that runtime. There is one scheduler, it
// belongs to core 0, so on the application core "the task the scheduler is
// currently running" is whatever core 0 happens to be doing at that instant -
// and this library's own servo yields on every pass, so the answer moves
// continuously.
//
// The consequence is worth stating plainly: std::recursive_mutex could not
// be used on the application core at all, not merely across cores - locked
// and unlocked microseconds apart on that one core, with no contention and
// no second core involved, it halted the board. Circle's CMutex stamps the
// current task on Acquire and asserts that the same task still holds it on
// Release, and on the application core that stamped task is whatever core 0
// happens to be running at the time, not the code that took the lock.
//
// Other primitives fail more quietly, which is why this file covers the
// whole runtime rather than one type. A contended std::mutex blocks on a
// Circle semaphore, which is a scheduler object, from a core that has no
// scheduler. Releasing one yields core 0's scheduler from another core.
// Every condition variable does both. std::thread cannot be created off
// core 0 at all, because constructing a task registers it with the same
// scheduler. And __libcpp_tls_get reads its slot out of "the current task",
// which off core 0 is the wrong task by construction, so a thread_local read
// from the application core would answer with something else's storage.
//
// This is a debt this library owes, not a fix to someone else's bug: it
// chose to run applications off core 0, and honouring that choice means
// supplying the parts of the C++ runtime that choice breaks.
//
// How the override works: this file defines every symbol circle-stdlib's
// liblibcxx-threading.a defines, and sdl-app.mk - the build fragment every
// application that links this library includes - takes that archive out of
// the library list. Two complete implementations of one ABI, and the link is
// given exactly one of them. Nothing vendored is edited and no symbol is
// ever defined twice. The types are unchanged: their sizes are baked into
// libc++ and into every object that ever declared a std::mutex, so what sits
// behind the opaque storage is all that differs.
//
// What the primitives are. Futex-shaped, and correct on every core:
//
//   std::mutex             one atomic word. Compare-and-swap to take, store
//                          to release.
//   std::recursive_mutex   that, plus an owner and a recursion depth. The
//                          owner is an identity that does not move under its
//                          holder - which is the whole of the fix.
//   condition variable     a generation counter and a waiter count. A signal
//                          bumps the generation; a waiter returns when the
//                          generation it recorded is no longer current.
//   std::thread            a cooperative Circle task on core 0 by default,
//                          created through a core-0 proxy when the request
//                          comes from elsewhere; a bare thread on a core a
//                          host kernel has lent (SDL_circle.h); or, on a core
//                          whose host has asked for it, a cooperative context
//                          this file schedules on that core itself.
//   thread_local           a block of storage per thread, addressed by
//                          TPIDR_EL0, which Circle's task switch already
//                          preserves per task.
//
// Two surfaces, one primitive underneath: src/threads.cpp implements SDL's
// own locks - SDL_mutex, SDL_cond, SDL_sem - and this file implements
// libc++'s ABI. They are different APIs with different storage and different
// contracts (SDL's mutex is recursive; std::mutex must not be), so neither
// can be written in terms of the other. What they share is one identity,
// SDL_ThreadID, and one wait, SDL2Circle_ThreadWaitSpin, which yields to
// Circle's scheduler on the hardware core, gives the core to the next
// cooperative context on a core that has one, and spins anywhere else. Every
// blocking loop in this file goes through that wait, and no other blocking
// primitive appears in it.
//
// WHY THERE IS A SCHEDULER IN HERE AS WELL.
//
// The default placement is core 0, and it is a real placement rather than a
// concession: a std::thread there is a Circle task - cooperative, able to
// reach Circle, costing no core - and it works. What it is not is a thread on
// the core the application is running on. An application that was moved to
// another core to get away from the SD card, USB, the serial port and the
// scheduler creates a worker, and the worker lands back among all four.
//
// Worse than that: core 0 is the one core where a thread that does not yield
// costs the board something. An application may quite reasonably hand a
// thread two seconds of arithmetic, and neither std::thread nor SDL knows,
// when it makes one, that hardware timing is waiting behind it. On core 0
// those are two seconds in which the SD card, the USB host and the serial
// port go unserviced, because core 0 is the only core that services them. An
// application core has nothing on it that hardware is waiting for, which is
// precisely why work belongs there.
//
// So a host kernel can ask for the other answer, one core at a time, with one
// call on the core it wants it on: SDL2Circle_ThreadsStayOnThisCore
// (SDL_circle.h). From then on a thread created on that core is a cooperative
// context ON that core - its own stack out of the heap, its own thread-local
// block, its own identity - and this file schedules it. No Circle task is
// constructed, Circle's scheduler is never called, and nothing is posted to
// core 0.
//
// BOTH SURFACES FOLLOW THAT ONE CALL. std::thread from this file and
// SDL_CreateThread from src/threads.cpp start a thread through the same
// SDL2Circle_ThreadStartHere, because what a thread on this core is does not
// differ between them. What does differ - a handle, a refcount, a status
// word, when a joiner may free what - each surface hands over as two function
// pointers, and the scheduler knows none of it.
//
// NOTHING ABOUT IT IS A CHANGE OF THREADING SEMANTICS, and neither placement
// was ever preemptive. A Circle task runs until it yields; so does a context
// here. What changes is which core the cooperative context runs on, and
// therefore what it contends with.
//
// A build that does not make the call is not touched by any of it: no core's
// run list is ever created, every placement is the one it was before, and
// SDL_ThreadID gives the answers it always gave. The safe state is the old
// state, and it is the state of a host that has never heard of this.
//
// The switch itself is at the end of this file, in assembly, with the set of
// registers it carries and the reason that set is sufficient.
//
// The limits:
//
//   - A std::thread on core 0 is cooperative. It runs when core 0 yields,
//     which the servo and every wait in this file do constantly, but a thread
//     that computes without ever waiting keeps core 0 to itself - and core 0
//     is where every device is serviced.
//   - A context on an application core is cooperative in exactly the same
//     way, against that core. One that computes without ever waiting holds
//     the core, and on that core there is no servo doing anything else.
//   - A context's stack is fixed when it is made and comes out of the heap.
//     A std::thread has nowhere to ask for a size, so every one of them gets
//     the same as a std::thread on core 0 gets. The low bytes carry a pattern
//     that is checked on each switch away, so an overrun is reported rather
//     than left to corrupt whatever the allocator put next to it.
//   - One pinned thread per lent core at a time. A second request for a busy
//     core is refused rather than queued. A lent core is not a candidate for
//     a scheduler of its own: it is reclaimed when its thread ends, and
//     contexts made on top of it would outlive it.
//   - A wait occupies the core it waits on. Off core 0 there is nothing to
//     sleep on, so a blocked core with nothing else runnable is a spinning
//     core.
//   - A timed wait off core 0 polls. It reads the free-running system counter
//     between spins; it does not sleep to the deadline.
//   - thread_local destructors run when a thread ends, and the application
//     core and core 0's main task never end. Their thread_locals are
//     destroyed at power-off, which is to say never.
//
#include <__external_threading>

#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"
#include "threads.h"

#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/sysconfig.h>
#include <circle/timer.h>
#include <circle/types.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

static const char From[] = "sdl2cxx";

// How many cores this build can address. Circle's CORES is the board's count
// in a multicore world; a single-core build has the one.
#ifdef ARM_ALLOW_MULTI_CORE
static const unsigned MaxCores = CORES;
#else
static const unsigned MaxCores = 1;
#endif

namespace
{

// ---------------------------------------------------------------------------
// The two things everything below is built on
// ---------------------------------------------------------------------------

// The one wait: blocking anywhere in this file is this call in a loop, and
// nothing else. On the hardware core it is a scheduler yield, so whatever
// holds the thing being waited for can run; on any other core it is the
// processor's yield hint, because there is no scheduler there to hand the
// time to. It is what src/threads.cpp waits on too.
inline void WaitABit(void)
{
    SDL2Circle_ThreadWaitSpin();
}

// The one identity, and the reason the recursive mutex works at all: it has
// to be unique among everything that can hold a lock at the same moment, and
// - unlike "whichever task the scheduler is running" - it must not change
// under a holder that is simply getting on with its work.
//
// SDL_ThreadID answers with the scheduler task's own address on the hardware
// core, and with the core number plus one anywhere else. That is unique
// because a core without a scheduler runs exactly one line of execution: the
// application, or the single pinned thread this file allows it. Never zero,
// which matters because zero is what a free mutex holds, and it can never
// collide with a task address because Circle's heap starts far above the
// highest core number a board has.
inline unsigned long long Self(void)
{
    return (unsigned long long)SDL_ThreadID();
}

// Microseconds since boot, from the free-running system counter. No lock, no
// driver state, no scheduler: valid on every core, which is what a timed wait
// off core 0 needs.
inline u64 NowMicros(void)
{
    return CTimer::GetClockTicks64();
}

// ---------------------------------------------------------------------------
// std::mutex - one atomic word
// ---------------------------------------------------------------------------
//
// Zero-initialised storage is a free mutex, which the ABI requires:
// _LIBCPP_MUTEX_INITIALIZER is an empty brace pair, so a std::mutex with
// static storage duration is never constructed at all - it is simply the
// zeroed bytes the image was loaded with.

struct MutexImpl
{
    std::atomic<unsigned> m_bHeld;      // 0 free, 1 held

    void Acquire(void)
    {
        for (;;)
        {
            unsigned expected = 0;
            if (m_bHeld.compare_exchange_weak(expected, 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
                return;
            WaitABit();
        }
    }

    bool TryAcquire(void)
    {
        unsigned expected = 0;
        return m_bHeld.compare_exchange_strong(expected, 1,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed);
    }

    void Release(void)
    {
        m_bHeld.store(0, std::memory_order_release);
    }
};

static_assert(sizeof(MutexImpl) <= sizeof(std::__libcpp_mutex_t::__storage),
              "MutexImpl does not fit the storage libc++ provides");
static_assert(alignof(MutexImpl) <= alignof(std::__libcpp_mutex_t),
              "MutexImpl is more aligned than libc++'s storage");

inline MutexImpl *AsMutex(std::__libcpp_mutex_t *__m)
{
    return reinterpret_cast<MutexImpl *>(__m->__storage);
}

// ---------------------------------------------------------------------------
// std::recursive_mutex - owned by an identity rather than by a scheduler task
// ---------------------------------------------------------------------------

struct RecursiveMutexImpl
{
    std::atomic<unsigned long long> m_nOwner;   // 0 when free
    unsigned                        m_nCount;   // recursion depth

    void Acquire(void)
    {
        const unsigned long long nMe = Self();
        if (m_nOwner.load(std::memory_order_acquire) == nMe)
        {
            m_nCount++;     // already ours: recursion, no contention possible
            return;
        }
        unsigned long long expected = 0;
        while (!m_nOwner.compare_exchange_weak(expected, nMe,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed))
        {
            expected = 0;
            WaitABit();
        }
        m_nCount = 1;
    }

    bool TryAcquire(void)
    {
        const unsigned long long nMe = Self();
        if (m_nOwner.load(std::memory_order_acquire) == nMe)
        {
            m_nCount++;
            return true;
        }
        unsigned long long expected = 0;
        if (!m_nOwner.compare_exchange_strong(expected, nMe,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
            return false;
        m_nCount = 1;
        return true;
    }

    void Release(void)
    {
        // Unlocking a mutex this line of execution does not hold is undefined
        // in the standard. It is not diagnosed here: the caller that would be
        // told is the one already in the wrong, and stopping the board to say
        // so is what the fault this file replaces did.
        if (m_nCount > 0 && --m_nCount == 0)
            m_nOwner.store(0, std::memory_order_release);
    }
};

static_assert(sizeof(RecursiveMutexImpl) <= sizeof(std::__libcpp_recursive_mutex_t::__storage),
              "RecursiveMutexImpl does not fit the storage libc++ provides");
static_assert(alignof(RecursiveMutexImpl) <= alignof(std::__libcpp_recursive_mutex_t),
              "RecursiveMutexImpl is more aligned than libc++'s storage");

inline RecursiveMutexImpl *AsRecursive(std::__libcpp_recursive_mutex_t *__m)
{
    return reinterpret_cast<RecursiveMutexImpl *>(__m->__storage);
}

// ---------------------------------------------------------------------------
// Condition variables - a generation counter and a waiter count
// ---------------------------------------------------------------------------
//
// A waiter records the generation it went to sleep on and returns when that
// is no longer the current one. A signal bumps the generation, which releases
// every waiter rather than one - and that is allowed, because the standard
// permits a condition variable to wake a thread spuriously and every correct
// use of one re-tests its predicate in a loop.
//
// The order in a wait is what stops a signal being lost: the generation is
// read and the waiter counted while the caller still holds the mutex, and
// only then is the mutex released. A signal issued under that mutex therefore
// either happens before the read - in which case the caller was never going
// to wait for it - or after it, in which case the bump is seen. Zeroed
// storage is a valid unused condition variable, as the ABI requires.

struct CondvarImpl
{
    std::atomic<u64>      m_nGeneration;
    std::atomic<unsigned> m_nWaiters;
};

static_assert(sizeof(CondvarImpl) <= sizeof(std::__libcpp_condvar_t::__storage),
              "CondvarImpl does not fit the storage libc++ provides");
static_assert(alignof(CondvarImpl) <= alignof(std::__libcpp_condvar_t),
              "CondvarImpl is more aligned than libc++'s storage");

inline CondvarImpl *AsCondvar(std::__libcpp_condvar_t *__cv)
{
    return reinterpret_cast<CondvarImpl *>(__cv->__storage);
}

// The deadline of an absolute time, in the epoch libc++ builds it in. Its
// condition variables convert a wait through std::chrono::system_clock, which
// on this platform is gettimeofday, which is Circle's calendar time - so the
// same clock has to be read here or the difference is meaningless. The result
// is then held against the free-running counter, which needs no lock and is
// the clock every other timed wait in this library uses.
//
// The calendar read goes through SDL2Circle_KernelTimeUTC, which puts it on
// core 0: the timer object is a device, and every caller of this is a thread
// waiting on some other core. A clock the kernel cannot give - a wait issued
// before SDL_Init - leaves the reading at zero, which makes the deadline the
// caller's whole absolute time and the wait a long one rather than a hang.
u64 DeadlineFromAbsolute(const std::__libcpp_timespec_t *__ts)
{
    unsigned nSeconds = 0;
    unsigned nMicros  = 0;
    SDL2Circle_KernelTimeUTC(&nSeconds, &nMicros);

    const long long nWanted = (long long)__ts->tv_sec * 1000000LL
                            + (long long)__ts->tv_nsec / 1000LL;
    const long long nNow    = (long long)nSeconds * 1000000LL + (long long)nMicros;

    const long long nDelta = nWanted - nNow;
    return NowMicros() + (nDelta > 0 ? (u64)nDelta : 0);
}

// ---------------------------------------------------------------------------
// Per-thread storage
// ---------------------------------------------------------------------------
//
// Two different things are called thread-local, and both of them live in the
// block below.
//
//   thread_local        variables the compiler placed in .tdata/.tbss and
//                       reaches as offsets from TPIDR_EL0. Each thread needs
//                       a private copy of that image; m_pBlock is it.
//
//   __libcpp_tls_*      a key/value store libc++ uses internally. m_pValue
//                       is it, and s_pDestructor below holds what to run
//                       against each key when a thread ends.
//
// Where the block is found: there are three kinds of caller, and each keeps
// its storage somewhere different:
//
//   a cooperative task on core 0    in the task's own libc++ user-data slot,
//                                   which is what that slot is for
//   a pinned thread on a lent core  in the thread's record
//   a bare core running no thread   in that core's own slot - this is the
//                                   application core, and it is the case the
//                                   vendored runtime cannot have: asking the
//                                   scheduler for "the current task" from a
//                                   core that has no scheduler answers with
//                                   whatever core 0 is running instead

const unsigned MaxTLSKeys = 32;

// A thread_local destructor, registered by __cxa_thread_atexit. Held as a
// list rather than an array because a program registers as many as it
// registers, and a fixed ceiling would silently drop the ones past it.
struct CXAAtexitNode
{
    void          (*m_pDestructor)(void *);
    void           *m_pObject;
    CXAAtexitNode  *m_pNext;
};

struct ThreadStorage
{
    void          *m_pBlock;                // hardware TLS block (TPIDR_EL0)
    void          *m_pValue[MaxTLSKeys];    // __libcpp_tls_get/set
    CXAAtexitNode *m_pAtexit;               // thread_local destructors, LIFO
};

void (*s_pDestructor[MaxTLSKeys])(void *);
std::atomic<unsigned> s_nNextKey{0};

// The storage of a core that is running no thread of ours: the application
// core, the presentation core, or core 0 before there is a scheduler.
ThreadStorage *s_pCoreStorage[MaxCores];

// ---------------------------------------------------------------------------
// A cooperative context, and the run list of the core it belongs to
// ---------------------------------------------------------------------------
//
// The processor state a context switch has to carry, and nothing else.
//
// Under AAPCS64 a function call already destroys x0-x18 and v0-v7, v16-v31,
// so a switch that happens at a call boundary - and every switch here does -
// need not save one of them: the compiler has already written down whatever
// it wanted to keep. What a call must NOT destroy is what is saved below.
//
//   x19-x28    the callee-saved general registers
//   x29        the frame pointer, which is the chain a backtrace walks
//   x30        the link register: where the switch returns to
//   sp         the stack, which is the whole of what makes a context one
//   TPIDR_EL0  the thread pointer. This library already uses it for thread
//              identity and as the base every thread_local is an offset
//              from, so a switch that did not carry it would hand the next
//              context the previous one's thread-local storage.
//   d8-d15     the callee-saved halves of the floating-point registers. Only
//              the low 64 bits of v8-v15 are callee-saved; the upper halves
//              and every other vector register are the caller's problem and
//              are already spilled by the time a call is made.
//
// NZCV is not saved: the flags are not live across a call. Neither is x18,
// which AAPCS64 reserves only where a platform ABI claims it - nothing in
// these builds does, so the compiler treats it as a temporary and has already
// spilled anything it wanted to keep. A world built with -ffixed-x18 would
// have to be saved here as well.
//
// The layout is fixed here and hard-coded as byte offsets in the assembly at
// the end of this file, so the two are held together by the static_asserts
// below rather than by anyone remembering.
struct CoopRegs
{
    u64 m_x19, m_x20, m_x21, m_x22, m_x23;
    u64 m_x24, m_x25, m_x26, m_x27, m_x28;
    u64 m_fp;                               // x29
    u64 m_lr;                               // x30
    u64 m_sp;
    u64 m_tpidr;
    u64 m_d8, m_d9, m_d10, m_d11, m_d12, m_d13, m_d14, m_d15;
};

// One line of execution on one core. The run list is circular and belongs to
// that core alone, so there is no lock on it anywhere: the only code that
// links, unlinks or walks it is code running on the core it describes.
//
// There is no blocked state and no wait list. Every wait in this library is a
// loop that re-tests its own condition, so a context that is waiting is still
// runnable - it simply finds its condition unmet on its next turn and gives
// the core up again. That is what makes the whole of the scheduling here two
// pointers and a switch.
//
// IT KNOWS NOTHING ABOUT EITHER THREADING SURFACE. A context is a stack, a
// register set, a block of per-thread storage and two function pointers, and
// both surfaces are expressed in those terms: std::thread from this file and
// SDL_CreateThread from src/threads.cpp. Neither one's handle, refcount or
// completion rule appears here, which is what lets one scheduler carry both
// without either learning about the other.
//
//   m_pBody     the thread's work. Called once, on this context's own stack,
//               and its return is what ends the thread.
//   m_pFinish   how this surface publishes that the thread has ended, called
//               at the one instant that is safe: after the body, after the
//               context is off the run list, and immediately before the core
//               is handed on. The instant it returns, the surface's own
//               handle may be freed by a joiner on any core, so nothing
//               after it may touch that handle.
struct CoopContext
{
    CoopContext   *m_pNext;                 // circular; never null once linked
    void         (*m_pBody)(void *);        // null for the context that ran main
    void         (*m_pFinish)(void *);
    void          *m_pArg;
    ThreadStorage *m_pTLS;                  // what CurrentStorage answers with
    ThreadStorage  m_Storage;               // what m_pTLS points at, except in
                                            // the context that ran main, which
                                            // keeps the storage its core had
    unsigned long long m_nId;               // what SDL_ThreadID answers inside it
    u8            *m_pStack;                // heap; null for the main context
    size_t         m_nStackBytes;
    bool           m_bGuardReported;
    CoopRegs       m_Regs;
};

// The pattern at the low end of a context's stack, and how much of it.
//
// A STACK THAT IS TOO SMALL DOES NOT ANNOUNCE ITSELF. It writes past its own
// end into whatever the allocator put there, and the fault appears somewhere
// unrelated and much later. This board has no memory management to ask for a
// guard page, so the low bytes are written with a pattern when the context is
// made and read back on every switch away from it and once more when it ends.
// A context that has overrun says so, once, at the next switch instead of at
// the eventual corruption.
const u8       CoopGuardByte  = 0xA5;
const unsigned CoopGuardBytes = 16;

// The whole of a core's scheduler. Every field is written and read by that
// core and by nothing else, which is why none of them is atomic.
//
// The three reap slots are what a context that has ended could not free for
// itself: the stack it was standing on, the thread-local block its own thread
// pointer still addressed, and the context record holding the very registers
// the switch away from it was reading. The next context to get the core frees
// all three, which is the first moment none of them is in use.
struct CoreCoop
{
    CoopContext *m_pCurrent;                // null: this core was never asked
    void        *m_pReapStack;
    void        *m_pReapTLS;
    CoopContext *m_pReapContext;
};

CoreCoop s_Coop[MaxCores];

// What a std::thread is, on a core that has Circle's scheduler or a lent core
// that has none. The record outlives the thread's execution, because join and
// detach may arrive after it has finished; the two sides each drop a reference
// and the last one frees it.
//
// A std::thread that is a cooperative context uses the same record for the
// same reasons - the handle outlives the thread either way - but its
// per-thread storage is the context's rather than the m_TLS below, because
// there the context is what a thread is.
struct ThreadRecord
{
    void              *(*m_pFn)(void *);
    void               *m_pArg;
    unsigned            m_nCore;            // 0 cooperative on core 0, else pinned
    unsigned long long  m_nId;              // what SDL_ThreadID answers inside it
    std::atomic<unsigned> m_bFinished;
    std::atomic<unsigned> m_nRefs;          // the runner, and the handle
    ThreadStorage       m_TLS;
};

// The pinned thread each lent core is running, if any. It is also what says
// the core is busy.
std::atomic<ThreadRecord *> s_pRunning[MaxCores];

ThreadStorage *CurrentStorage(void)
{
    const unsigned nCore = SDL2Circle_ThisCore() % MaxCores;

    // A core that schedules its own contexts answers from the running one,
    // and it answers first: on such a core "which thread is this" has exactly
    // one right answer and the core no longer is it. The context that ran
    // main carries whichever storage the core had before there was a
    // scheduler, so nothing about this changes what an application core
    // answered before it asked for one.
    CoopContext *pContext = s_Coop[nCore].m_pCurrent;
    if (pContext != nullptr)
        return pContext->m_pTLS;

    ThreadRecord *pPinned = s_pRunning[nCore].load(std::memory_order_acquire);
    if (pPinned != nullptr)
        return &pPinned->m_TLS;

    if (nCore == 0 && CScheduler::IsActive())
    {
        CTask *pTask = CScheduler::Get()->GetCurrentTask();
        if (pTask != nullptr)
        {
            auto *pTLS = (ThreadStorage *)pTask->GetUserData(TASK_USER_DATA_LIBCXX);
            if (pTLS == nullptr)
            {
                // Core 0's main task, and any task a host kernel made for
                // itself. Each is a thread like any other and gets storage
                // like any other; its hardware block was armed by
                // SDL2Circle_ArmCoreRuntime.
                pTLS = new ThreadStorage{};
                pTask->SetUserData(pTLS, TASK_USER_DATA_LIBCXX);
            }
            return pTLS;
        }
    }

    if (s_pCoreStorage[nCore] == nullptr)
        s_pCoreStorage[nCore] = new ThreadStorage{};
    return s_pCoreStorage[nCore];
}

// Run everything an ending thread registered, in the order the standard
// wants: thread_local destructors last-registered first, then the libc++ key
// destructors. Both are application code and both may allocate, so this runs
// with the thread still fully addressable and before its block is freed.
void ReleaseStorage(ThreadStorage *pTLS)
{
    while (pTLS->m_pAtexit != nullptr)
    {
        CXAAtexitNode *pNode = pTLS->m_pAtexit;
        pTLS->m_pAtexit = pNode->m_pNext;
        pNode->m_pDestructor(pNode->m_pObject);
        delete pNode;
    }

    // A key destructor may set its own key again, which is why this repeats
    // rather than sweeping once. Four passes is what pthreads guarantees and
    // what libc++ is written against.
    unsigned nKeys = s_nNextKey.load(std::memory_order_acquire);
    if (nKeys > MaxTLSKeys)
        nKeys = MaxTLSKeys;

    for (unsigned nPass = 0; nPass < 4; nPass++)
    {
        bool bRanAny = false;
        for (unsigned nKey = 0; nKey < nKeys; nKey++)
        {
            void *pValue = pTLS->m_pValue[nKey];
            if (pValue != nullptr && s_pDestructor[nKey] != nullptr)
            {
                pTLS->m_pValue[nKey] = nullptr;
                s_pDestructor[nKey](pValue);
                bRanAny = true;
            }
        }
        if (!bRanAny)
            break;
    }
}

void DropReference(ThreadRecord *pRecord)
{
    if (pRecord->m_nRefs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete pRecord;
}

// The body of a thread, whichever core it runs on: give it its own view of
// thread_local storage, run it, then take that view down again.
void RunThreadBody(ThreadRecord *pRecord)
{
    void *pPrevious = SDL2Circle_GetThreadPointer();

    pRecord->m_TLS.m_pBlock = SDL2Circle_AllocTLSBlock();
    SDL2Circle_SetThreadPointer(pRecord->m_TLS.m_pBlock);

    pRecord->m_pFn(pRecord->m_pArg);

    // Destructors first, and only then the block: they are thread_local
    // objects, and reaching one is a read through the pointer about to be
    // restored.
    ReleaseStorage(&pRecord->m_TLS);

    SDL2Circle_SetThreadPointer(pPrevious);
    SDL2Circle_FreeTLSBlock(pRecord->m_TLS.m_pBlock);
    pRecord->m_TLS.m_pBlock = nullptr;

    pRecord->m_bFinished.store(1, std::memory_order_release);
}

}   // namespace

// ---------------------------------------------------------------------------
// The cooperative scheduler an application core runs its own threads on
// ---------------------------------------------------------------------------
//
// Two entry points, both written in assembly at the end of this file because
// what they do is exactly the thing C cannot describe: change the stack under
// the code that is running.
//
//   sdl2circle_coop_switch  save the leaving context, restore the entering
//                           one, and return - into the entering context's own
//                           last call to this function, which is where it
//                           gave the core up.
//   sdl2circle_coop_enter   restore only. For the first turn a context ever
//                           takes, whose saved state was written out rather
//                           than saved, and for the last act of one that is
//                           ending and has no state worth keeping.
extern "C" void sdl2circle_coop_switch(CoopRegs *pLeaving, CoopRegs *pEntering);
extern "C" __attribute__((noreturn)) void sdl2circle_coop_enter(CoopRegs *pEntering);

// Where a context's first instruction is: arrived at by the `ret` at the end
// of sdl2circle_coop_enter, on a stack nothing has run on, with no caller to
// go back to.
extern "C" __attribute__((noreturn)) void sdl2circle_coop_start(void);

namespace
{

// Free what the last context to end left behind: its stack, its thread-local
// block and the context record itself. It could free none of the three - it
// was standing on the first, its thread pointer addressed the second, and the
// switch away from it was reading the third.
void ReapPending(CoreCoop &Core)
{
    void        *pStack   = Core.m_pReapStack;
    void        *pBlock   = Core.m_pReapTLS;
    CoopContext *pContext = Core.m_pReapContext;
    Core.m_pReapStack   = nullptr;
    Core.m_pReapTLS     = nullptr;
    Core.m_pReapContext = nullptr;

    if (pStack != nullptr)
        delete[] (u8 *)pStack;
    if (pBlock != nullptr)
        SDL2Circle_FreeTLSBlock(pBlock);
    if (pContext != nullptr)
        delete pContext;
}

// Has this context written past the low end of its own stack? Reported once:
// nothing puts the pattern back, so a second report would say the same thing
// on every switch from here to power-off.
//
// It reports rather than halting. This is a library inside somebody else's
// program, and an overrun found here has already happened - stopping the
// board denies the application the chance to say anything about it, and
// denies the operator the log line that names which thread did it.
void CheckStackGuard(CoopContext *pContext)
{
    if (pContext->m_pStack == nullptr || pContext->m_bGuardReported)
        return;

    for (unsigned i = 0; i < CoopGuardBytes; i++)
    {
        if (pContext->m_pStack[i] == CoopGuardByte)
            continue;

        pContext->m_bGuardReported = true;
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "std::thread %llu on core %u has written past the low "
                       "end of its %lu byte stack; memory beyond it belongs to "
                       "something else",
                       (unsigned long long)pContext->m_nId,
                       SDL2Circle_ThisCore(),
                       (unsigned long)pContext->m_nStackBytes);
        return;
    }
}

// Give the core to the next context that can use it. True when the core was
// handed over and has since come back; false when this core has no scheduler
// or this context is the only one on it, which is what makes the caller fall
// back to the processor's yield hint.
bool ScheduleNext(void)
{
    CoreCoop &Core = s_Coop[SDL2Circle_ThisCore() % MaxCores];

    CoopContext *pCurrent = Core.m_pCurrent;
    if (pCurrent == nullptr)
        return false;                   // no host asked this core for one

    // The list is circular and the walk always steps to the successor, so
    // turns go round the ring rather than to whichever context was made
    // first. A ring of one is a core with nothing else to run.
    CoopContext *pNext = pCurrent->m_pNext;
    if (pNext == pCurrent)
        return false;

    CheckStackGuard(pCurrent);

    Core.m_pCurrent = pNext;

    // That store has to be in memory BEFORE the switch, not after it. Sunk
    // past the call it would land in the entering context's flow, which
    // resumes inside its own switch and never returns here - so the store
    // would simply never happen and the core would go on believing the
    // leaving context is the running one. The barrier costs nothing and says
    // so; it is not a barrier against another core, which never reads this.
    asm volatile("" ::: "memory");

    sdl2circle_coop_switch(&pCurrent->m_Regs, &pNext->m_Regs);

    // Resumed, on this context's own stack again. Whatever the switch that
    // gave the core back left behind is this context's to clear, because the
    // context that left it could not clear it itself.
    ReapPending(Core);
    return true;
}

// The last act of a context whose function has returned. It runs on that
// context's own stack and does not return: there is nowhere to return to.
__attribute__((noreturn)) void CoopExit(void)
{
    CoreCoop &Core = s_Coop[SDL2Circle_ThisCore() % MaxCores];

    CoopContext *pMe = Core.m_pCurrent;

    // Destructors first, while this context is still whole: they are
    // thread_local objects and application code, they may allocate, and
    // reaching one is a read through the thread pointer that is about to
    // stop being this context's.
    ReleaseStorage(pMe->m_pTLS);

    CheckStackGuard(pMe);

    CoopContext *pNext = pMe->m_pNext;
    if (pNext == pMe)
    {
        // The context that ran main on this core is on the run list and never
        // ends, so this cannot happen. It is said rather than assumed because
        // the alternative to saying it is a jump through whatever is in the
        // link register.
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "std::thread %llu ended and core %u has no context left "
                       "to give the core to; this core stops here",
                       (unsigned long long)pMe->m_nId, SDL2Circle_ThisCore());
        for (;;)
            asm volatile("wfe" ::: "memory");
    }

    // Off the run list before anything else can be given away: from here on
    // no turn can reach this context.
    CoopContext *pPrevious = pNext;
    while (pPrevious->m_pNext != pMe)
        pPrevious = pPrevious->m_pNext;
    pPrevious->m_pNext = pNext;

    // None of these can be freed from here. This is the stack this code is
    // standing on, that is the block TPIDR_EL0 still points at, and the third
    // holds the registers the switch below is about to read.
    Core.m_pReapStack   = pMe->m_pStack;
    Core.m_pReapTLS     = pMe->m_pTLS->m_pBlock;
    Core.m_pReapContext = pMe;
    pMe->m_pStack        = nullptr;
    pMe->m_pTLS->m_pBlock = nullptr;

    Core.m_pCurrent = pNext;

    // How this surface says the thread has ended. The instant it returns, a
    // joiner on any core may free the handle it published against - so it is
    // the last thing that reads anything of the surface's, and nothing below
    // touches m_pArg again. pNext is a local, and the stack under it is held
    // in the reap slot rather than freed.
    if (pMe->m_pFinish != nullptr)
        pMe->m_pFinish(pMe->m_pArg);

    // Every store above must be in memory before the switch, for the same
    // reason as in ScheduleNext: sunk past it, they would land in the
    // entering context's flow and never happen at all.
    asm volatile("" ::: "memory");

    sdl2circle_coop_enter(&pNext->m_Regs);
}

}   // namespace

// Put a new context on the calling core's run list, with its state written
// out rather than saved: a stack nothing has run on, the thread pointer it is
// to have, and sdl2circle_coop_start in the link register, which is where
// sdl2circle_coop_enter's own return arrives.
//
// Both threading surfaces start a thread through this one call, because there
// is one thing a thread on this core is and it is the same for both. What
// differs between them - a handle, a refcount, a status word, when a joiner
// may free what - is entirely in the two function pointers the caller hands
// over, and none of it is known here.
//
// Answers with the new thread's identity, which is what SDL_ThreadID reports
// inside it, and zero if the calling core has no scheduler or the heap could
// not carry another stack. The identity is settled before the context is on
// the run list, so the caller can publish it in its own handle before the
// thread ever runs: nothing on this core can run the new context until this
// core next waits, and that cannot happen between here and the caller's next
// statement.
unsigned long long SDL2Circle_ThreadStartHere(void (*pBody)(void *),
                                             void (*pFinish)(void *),
                                             void *pArg, size_t nStackBytes)
{
    CoreCoop &Core = s_Coop[SDL2Circle_ThisCore() % MaxCores];
    if (Core.m_pCurrent == nullptr)
        return 0;                       // no host asked this core for one

    if (nStackBytes < (size_t)TASK_STACK_SIZE)
        nStackBytes = (size_t)TASK_STACK_SIZE;
    nStackBytes = (nStackBytes + 15) & ~(size_t)15;

    CoopContext *pContext = new (std::nothrow) CoopContext{};
    if (pContext == nullptr)
        return 0;

    u8 *pStack = new (std::nothrow) u8[nStackBytes];
    if (pStack == nullptr)
    {
        delete pContext;
        return 0;
    }

    // Allocated here, by the creator, so that a heap too full for one is this
    // call's failure rather than a fault inside a thread that has already
    // been reported as started.
    void *pBlock = SDL2Circle_AllocTLSBlock();
    if (pBlock == nullptr)
    {
        delete[] pStack;
        delete pContext;
        return 0;
    }

    memset(pStack, CoopGuardByte, CoopGuardBytes);

    pContext->m_pBody          = pBody;
    pContext->m_pFinish        = pFinish;
    pContext->m_pArg           = pArg;
    pContext->m_pTLS           = &pContext->m_Storage;
    pContext->m_Storage.m_pBlock = pBlock;
    pContext->m_nId            = (unsigned long long)(uintptr_t)pContext;
    pContext->m_pStack         = pStack;
    pContext->m_nStackBytes    = nStackBytes;
    pContext->m_bGuardReported = false;

    pContext->m_Regs.m_sp    = ((u64)(uintptr_t)pStack + nStackBytes) & ~(u64)15;
    pContext->m_Regs.m_lr    = (u64)(uintptr_t)&sdl2circle_coop_start;
    pContext->m_Regs.m_fp    = 0;   // where a frame chain ends
    pContext->m_Regs.m_tpidr = (u64)(uintptr_t)pBlock;

    // Immediately after the context that made it, so a newly made thread is
    // the next to get a turn.
    pContext->m_pNext = Core.m_pCurrent->m_pNext;
    Core.m_pCurrent->m_pNext = pContext;

    return pContext->m_nId;
}

// Whether the calling core schedules its own threads. Both surfaces ask
// before they build anything, so that a core that was never asked takes the
// path it always took, with nothing allocated and nothing to undo.
bool SDL2Circle_ThreadSchedulesHere(void)
{
    return s_Coop[SDL2Circle_ThisCore() % MaxCores].m_pCurrent != nullptr;
}

extern "C" void sdl2circle_coop_start(void)
{
    CoreCoop &Core = s_Coop[SDL2Circle_ThisCore() % MaxCores];

    // The switch that started this context could not clear what the context
    // before it left behind: it never returned into its own code.
    ReapPending(Core);

    CoopContext *pMe = Core.m_pCurrent;

    // An exception that escaped here would be unwound past a stack frame
    // written out by hand, with a null frame pointer and a link register that
    // points at this function - so there is nothing above to unwind into and
    // nothing to describe how. Caught here, the thread ends the way a thread
    // that returned ends, and whoever joins it is released.
    //
    // Each surface catches its own first, where it can turn the failure into
    // the answer its API owes a joiner. This is the backstop for what neither
    // of them caught.
    try
    {
        pMe->m_pBody(pMe->m_pArg);
    }
    catch (...)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "thread %llu on core %u ended by an exception it did "
                       "not catch",
                       (unsigned long long)pMe->m_nId, SDL2Circle_ThisCore());
    }

    CoopExit();
}

// What every blocking wait in this library calls (src/threads.cpp). It is the
// same question the hardware core answers with a scheduler yield: is there
// anything else on this core that could use the time.
bool SDL2Circle_ThreadScheduleNext(void)
{
    return ScheduleNext();
}

// Who the caller is, when the caller is a cooperative context. Zero on a core
// that has no scheduler, which is what makes SDL_ThreadID answer exactly as
// it did before on every core and every build that never asked for one.
unsigned long long SDL2Circle_ThreadContextID(void)
{
    CoopContext *pContext = s_Coop[SDL2Circle_ThisCore() % MaxCores].m_pCurrent;
    return pContext != nullptr ? pContext->m_nId : 0;
}

namespace
{

// A std::thread on core 0: an ordinary Circle scheduler task. Four times the
// usual stack, because a C++ thread carries the unwinder's state and the
// default does not hold it - the same sizing circle-stdlib's own version
// used.
class CLibCXXTask : public CTask
{
public:
    CLibCXXTask(ThreadRecord *pRecord)
    :   CTask(TASK_STACK_SIZE * 4, TRUE),   // suspended: started once its id is set
        m_pRecord(pRecord)
    {
        SetName("std::thread");

        // The task's own libc++ slot points at the record's storage, so
        // CurrentStorage finds it from inside the task with no search.
        SetUserData(&pRecord->m_TLS, TASK_USER_DATA_LIBCXX);
    }

    void Run(void) override
    {
        RunThreadBody(m_pRecord);
        SetUserData(nullptr, TASK_USER_DATA_LIBCXX);
        DropReference(m_pRecord);
        // Circle deletes this task object itself once Run returns, so
        // nothing may touch it from here.
    }

private:
    ThreadRecord *m_pRecord;
};

// Construct the task, settle the record's identity, and only then let it run:
// an identity read from inside the thread would otherwise race the line that
// writes it.
void StartCooperativeThread(ThreadRecord *pRecord)
{
    CLibCXXTask *pTask = new CLibCXXTask(pRecord);
    pRecord->m_nId = (unsigned long long)(uintptr_t)pTask;
    pTask->Start();
}

// Creating a task registers it with the scheduler, and the scheduler is core
// 0's. So a creation issued from another core is posted here and a core-0
// task does the constructing. One request outstanding at a time, which is
// ample: creating a thread is a rare event, and the wait for the answer is
// the ordinary one.
//
// Both threading surfaces come through here - std::thread from this file and
// SDL_CreateThread from src/threads.cpp - because there is one reason to need
// core 0 and it is the same for both. What is posted is a plain function and
// its argument, so the box knows nothing about either surface's idea of a
// thread.
struct alignas(64) CreateBox
{
    std::atomic<u64> m_nRequested{0};
    std::atomic<u64> m_nServed{0};
    void           (*m_pFn)(void *);
    void            *m_pArg;
};

CreateBox g_Create;
MutexImpl g_CreateLock;                 // serialises the posters
std::atomic<unsigned> g_bCreatorUp{0};

class CLibCXXCreatorTask : public CTask
{
public:
    CLibCXXCreatorTask(void) : CTask(TASK_STACK_SIZE) { SetName("thread-create"); }

    void Run(void) override
    {
        // Circle hands a new task a null thread pointer (see the note in
        // split.cpp's servo). This task serves thread creation for every
        // other core and must never be the one that falls over.
        SDL2Circle_SetThreadPointer(SDL2Circle_AllocTLSBlock());

        for (;;)
        {
            const u64 nRequested = g_Create.m_nRequested.load(std::memory_order_acquire);
            if (nRequested > g_Create.m_nServed.load(std::memory_order_relaxed))
            {
                g_Create.m_pFn(g_Create.m_pArg);
                g_Create.m_nServed.store(nRequested, std::memory_order_release);
            }
            CScheduler::Get()->Yield();
        }
    }
};

// Post one request and wait for it. The wait is SDL2Circle_ThreadWaitSpin, so
// on the calling core it costs what every other wait in this library costs;
// the creator, meanwhile, is a scheduler task like any other and the servo
// keeps its own lap running beside it.
void PostToCreator(void (*pFn)(void *), void *pArg)
{
    g_CreateLock.Acquire();

    g_Create.m_pFn  = pFn;
    g_Create.m_pArg = pArg;
    const u64 nSeq = g_Create.m_nRequested.load(std::memory_order_relaxed) + 1;
    g_Create.m_nRequested.store(nSeq, std::memory_order_release);

    while (g_Create.m_nServed.load(std::memory_order_acquire) < nSeq)
        WaitABit();

    g_CreateLock.Release();
}

// What the creator does for a std::thread.
void create_std_thread(void *pArg)
{
    StartCooperativeThread((ThreadRecord *)pArg);
}

// What a std::thread is, expressed as the two things a cooperative context
// asks of a surface: the work, and how the end of it is published.
//
// The context owns the per-thread storage in this placement, so the record's
// own m_TLS goes unused - a thread that is a context IS the thread, and there
// is no task to hang storage off.
void run_std_thread_context(void *pArg)
{
    ThreadRecord *pRecord = (ThreadRecord *)pArg;
    pRecord->m_pFn(pRecord->m_pArg);
}

void finish_std_thread_context(void *pArg)
{
    ThreadRecord *pRecord = (ThreadRecord *)pArg;

    // The order the rest of this file already uses: publish that the thread
    // has ended, then drop the runner's reference. A joiner released by the
    // flag drops the other one, and whichever of the two is last frees the
    // record.
    pRecord->m_bFinished.store(1, std::memory_order_release);
    DropReference(pRecord);
}

// Core placement. One pending pin request per core, because two cores may
// each be about to create a thread and neither should be able to take the
// other's placement.
std::atomic<unsigned> s_nPinRequest[MaxCores];

// Cores a host kernel has lent (SDL2Circle_ThreadCoreOffer).
std::atomic<unsigned> s_nCoresOffered{0};

unsigned TakePin(void)
{
    return s_nPinRequest[SDL2Circle_ThisCore() % MaxCores]
               .exchange(0, std::memory_order_acq_rel);
}

}   // namespace

// Cores that have been told, once each, where the threads they make went.
//
// The default placement is silent everywhere else, and that silence is the
// whole reason this exists: a port that was moved off core 0 on purpose,
// creating a thread that lands back on core 0, has no way to find that out
// from anything the board prints. One line per core, at that core's first
// creation through either surface, says it and names the call that changes
// it.
static std::atomic<unsigned> s_nPlacementAnnounced{0};

void SDL2Circle_ThreadAnnounceCore0(void)
{
    const unsigned nBit = 1u << (SDL2Circle_ThisCore() % MaxCores);
    if (s_nPlacementAnnounced.fetch_or(nBit, std::memory_order_acq_rel) & nBit)
        return;

    SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                   "a thread created on core %u runs on core 0, as a "
                   "cooperative Circle task, beside every device this board "
                   "services; SDL2Circle_ThreadsStayOnThisCore keeps core %u's "
                   "threads on core %u instead",
                   SDL2Circle_ThisCore(), SDL2Circle_ThisCore(),
                   SDL2Circle_ThisCore());
}

// ---------------------------------------------------------------------------
// The ABI itself
// ---------------------------------------------------------------------------

_LIBCPP_BEGIN_NAMESPACE_STD

int __libcpp_mutex_lock(__libcpp_mutex_t *__m)
{
    AsMutex(__m)->Acquire();
    return 0;
}

bool __libcpp_mutex_trylock(__libcpp_mutex_t *__m)
{
    return AsMutex(__m)->TryAcquire();
}

int __libcpp_mutex_unlock(__libcpp_mutex_t *__m)
{
    AsMutex(__m)->Release();
    return 0;
}

int __libcpp_mutex_destroy(__libcpp_mutex_t *)
{
    return 0;   // the storage is the whole object
}

int __libcpp_recursive_mutex_init(__libcpp_recursive_mutex_t *__m)
{
    memset(__m->__storage, 0, sizeof(__m->__storage));
    return 0;
}

int __libcpp_recursive_mutex_lock(__libcpp_recursive_mutex_t *__m)
{
    AsRecursive(__m)->Acquire();
    return 0;
}

bool __libcpp_recursive_mutex_trylock(__libcpp_recursive_mutex_t *__m)
{
    return AsRecursive(__m)->TryAcquire();
}

int __libcpp_recursive_mutex_unlock(__libcpp_recursive_mutex_t *__m)
{
    AsRecursive(__m)->Release();
    return 0;
}

int __libcpp_recursive_mutex_destroy(__libcpp_recursive_mutex_t *)
{
    return 0;   // the storage is the whole object
}

int __libcpp_condvar_signal(__libcpp_condvar_t *__cv)
{
    CondvarImpl *pCV = AsCondvar(__cv);
    if (pCV->m_nWaiters.load(std::memory_order_acquire) > 0)
        pCV->m_nGeneration.fetch_add(1, std::memory_order_release);
    return 0;
}

int __libcpp_condvar_broadcast(__libcpp_condvar_t *__cv)
{
    // The same call: a generation bump already releases every waiter.
    return __libcpp_condvar_signal(__cv);
}

int __libcpp_condvar_wait(__libcpp_condvar_t *__cv, __libcpp_mutex_t *__m)
{
    CondvarImpl *pCV = AsCondvar(__cv);
    MutexImpl   *pM  = AsMutex(__m);

    const u64 nSleptOn = pCV->m_nGeneration.load(std::memory_order_acquire);
    pCV->m_nWaiters.fetch_add(1, std::memory_order_release);
    pM->Release();

    while (pCV->m_nGeneration.load(std::memory_order_acquire) == nSleptOn)
        WaitABit();

    pCV->m_nWaiters.fetch_sub(1, std::memory_order_release);
    pM->Acquire();
    return 0;
}

int __libcpp_condvar_timedwait(__libcpp_condvar_t *__cv, __libcpp_mutex_t *__m,
                               __libcpp_timespec_t *__ts)
{
    CondvarImpl *pCV = AsCondvar(__cv);
    MutexImpl   *pM  = AsMutex(__m);

    const u64 nDeadline = DeadlineFromAbsolute(__ts);

    const u64 nSleptOn = pCV->m_nGeneration.load(std::memory_order_acquire);
    pCV->m_nWaiters.fetch_add(1, std::memory_order_release);
    pM->Release();

    bool bTimedOut = false;
    while (pCV->m_nGeneration.load(std::memory_order_acquire) == nSleptOn)
    {
        if (NowMicros() >= nDeadline)
        {
            // Read the generation once more before giving up, so a signal
            // that arrived while this pass was reading the clock is reported
            // as a wakeup rather than as a timeout.
            bTimedOut = pCV->m_nGeneration.load(std::memory_order_acquire) == nSleptOn;
            break;
        }
        WaitABit();
    }

    pCV->m_nWaiters.fetch_sub(1, std::memory_order_release);
    pM->Acquire();
    return bTimedOut ? ETIMEDOUT : 0;
}

int __libcpp_condvar_destroy(__libcpp_condvar_t *)
{
    return 0;   // the storage is the whole object
}

// std::call_once, and the guard on a function-local static.
//
// Three states: 0 nobody has tried, 1 someone is running the routine, 2 done.
// A routine that throws is an exceptional call under the standard - the flag
// goes back to 0 so the next caller tries again, and the exception is that
// caller's, not ours.
int __libcpp_execute_once(__libcpp_exec_once_flag *__flag,
                          void (*__init_routine)())
{
    static_assert(sizeof(std::atomic<int>) == sizeof(__libcpp_exec_once_flag),
                  "the once-flag is not the size of the atomic overlaid on it");
    auto *pFlag = reinterpret_cast<std::atomic<int> *>(__flag);

    for (;;)
    {
        int nState = pFlag->load(std::memory_order_acquire);
        if (nState == 2)
            return 0;

        if (nState == 0
            && pFlag->compare_exchange_strong(nState, 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
        {
            try
            {
                __init_routine();
            }
            catch (...)
            {
                pFlag->store(0, std::memory_order_release);
                throw;
            }
            pFlag->store(2, std::memory_order_release);
            return 0;
        }

        WaitABit();
    }
}

int __libcpp_thread_create(__libcpp_thread_t *__t, void *(*__func)(void *),
                           void *__arg)
{
    ThreadRecord *pRecord = new (std::nothrow) ThreadRecord{};
    if (pRecord == nullptr)
        return ENOMEM;

    pRecord->m_pFn  = __func;
    pRecord->m_pArg = __arg;
    pRecord->m_nRefs.store(2, std::memory_order_relaxed);

    const unsigned nPin = TakePin();

    if (nPin != 0)
    {
        pRecord->m_nCore = nPin;
        pRecord->m_nId   = nPin + 1;    // what SDL_ThreadID answers on that core

        ThreadRecord *pExpected = nullptr;
        if (!s_pRunning[nPin].compare_exchange_strong(pExpected, pRecord,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed))
        {
            delete pRecord;
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                           "std::thread pinned to core %u: that core is already "
                           "running one", nPin);
            return EAGAIN;
        }

        // A lent core sleeps between threads, so it has to be woken.
        asm volatile("dsb ish; sev" ::: "memory");
    }
    else if (SDL2Circle_ThreadSchedulesHere())
    {
        // This core schedules its own threads, because its host asked it to
        // (SDL2Circle_ThreadsStayOnThisCore). The thread becomes a
        // cooperative context here: no Circle task is built, the scheduler is
        // not called, and nothing is posted to core 0.
        //
        // A std::thread carries no stack size - the type has nowhere to put
        // one - so the size is this implementation's choice, and it is the
        // same one a std::thread on core 0 gets: four times Circle's task
        // stack. A C++ thread holds the unwinder's state as well as its own
        // locals, which is what that sizing was for, and one number for both
        // placements means moving a thread between them never changes how
        // much stack it has.
        const unsigned long long nId =
            SDL2Circle_ThreadStartHere(run_std_thread_context,
                                       finish_std_thread_context,
                                       pRecord, (size_t)TASK_STACK_SIZE * 4);
        if (nId == 0)
        {
            delete pRecord;
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                           "std::thread on core %u: no memory for its stack or "
                           "its thread-local block", SDL2Circle_ThisCore());
            return ENOMEM;
        }
        pRecord->m_nId = nId;
    }
    else if (!CScheduler::IsActive())
    {
        delete pRecord;
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "std::thread: this system has no CScheduler, so there is "
                       "nothing on core 0 for a thread to run on");
        return EAGAIN;
    }
    else if (SDL2Circle_ThisCore() != 0)
    {
        // Off core 0, and this core was never asked to schedule its own: post
        // the record and let the creator task build the scheduler task.
        // Refusing instead would make std::thread unusable from the very core
        // this library exists to put applications on.
        //
        // The caller's core is running the application, by definition of who
        // is making this call, so nothing may be pinned onto it later.
        SDL2Circle_ClaimCore(SDL2Circle_ThisCore());

        // Said once per core, because it is the placement nothing else
        // announces: an application that has been moved off core 0 to get
        // away from the devices has just put a thread back among them.
        SDL2Circle_ThreadAnnounceCore0();

        if (!SDL2Circle_ThreadCreateOn0(create_std_thread, pRecord))
        {
            delete pRecord;
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                           "std::thread from core %u: the core-0 creator task is "
                           "not running, so this kernel never armed core 0",
                           SDL2Circle_ThisCore());
            return EAGAIN;
        }
    }
    else
    {
        StartCooperativeThread(pRecord);
    }

    __t->__opaque = pRecord;
    return 0;
}

__libcpp_thread_id __libcpp_thread_get_current_id()
{
    // The same answer SDL_ThreadID gives, deliberately: one identity across
    // both threading surfaces means a lock held through one and inspected
    // through the other agrees about who holds it.
    return (__libcpp_thread_id)Self();
}

__libcpp_thread_id __libcpp_thread_get_id(__libcpp_thread_t const *__t)
{
    const ThreadRecord *pRecord = (const ThreadRecord *)__t->__opaque;
    return (__libcpp_thread_id)pRecord->m_nId;
}

int __libcpp_thread_join(__libcpp_thread_t *__t)
{
    ThreadRecord *pRecord = (ThreadRecord *)__t->__opaque;

    while (!pRecord->m_bFinished.load(std::memory_order_acquire))
        WaitABit();

    DropReference(pRecord);
    __t->__opaque = nullptr;
    return 0;
}

int __libcpp_thread_detach(__libcpp_thread_t *__t)
{
    // Nothing waits for it now, so the handle's reference goes; the running
    // thread holds the other and the record is freed when it ends.
    DropReference((ThreadRecord *)__t->__opaque);
    __t->__opaque = nullptr;
    return 0;
}

void __libcpp_thread_yield()
{
    WaitABit();
}

void __libcpp_thread_sleep_for(chrono::nanoseconds const &__ns)
{
    const long long nNanos = __ns.count();
    if (nNanos <= 0)
    {
        WaitABit();
        return;
    }

    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
    {
        // Core 0 has somewhere to hand the time to, so it does.
        const unsigned nMicros = (unsigned)(nNanos / 1000);
        CScheduler::Get()->usSleep(nMicros != 0 ? nMicros : 1);
        return;
    }

    // Anywhere else there is nothing to sleep on: the wait occupies the core
    // for its whole duration.
    const u64 nDeadline = NowMicros() + (u64)(nNanos / 1000);
    while (NowMicros() < nDeadline)
        WaitABit();
}

int __libcpp_tls_create(__libcpp_tls_key *__key, void (*__at_exit)(void *))
{
    const unsigned nKey = s_nNextKey.fetch_add(1, std::memory_order_acq_rel);
    if (nKey >= MaxTLSKeys)
        return EAGAIN;
    s_pDestructor[nKey] = __at_exit;
    *__key = nKey;
    return 0;
}

void *__libcpp_tls_get(__libcpp_tls_key __key)
{
    if (__key >= MaxTLSKeys)
        return nullptr;
    return CurrentStorage()->m_pValue[__key];
}

int __libcpp_tls_set(__libcpp_tls_key __key, void *__p)
{
    if (__key >= MaxTLSKeys)
        return EINVAL;
    CurrentStorage()->m_pValue[__key] = __p;
    return 0;
}

_LIBCPP_END_NAMESPACE_STD

// ---------------------------------------------------------------------------
// thread_local destructors
// ---------------------------------------------------------------------------
//
// The compiler emits a call to this for every thread_local object with a
// non-trivial destructor. The list is per thread and runs when that thread
// ends - so on core 0's main task and on the application core, neither of
// which ever ends, it is recorded and never run. That is a real limit and it
// is stated rather than worked around: a thread_local whose destructor must
// run belongs on a thread that finishes.

extern "C" int __cxa_thread_atexit(void (*pDestructor)(void *), void *pObject,
                                   void *)
{
    auto *pNode = new (std::nothrow) CXAAtexitNode;
    if (pNode == nullptr)
        return -1;

    ThreadStorage *pTLS = CurrentStorage();
    pNode->m_pDestructor = pDestructor;
    pNode->m_pObject     = pObject;
    pNode->m_pNext       = pTLS->m_pAtexit;
    pTLS->m_pAtexit      = pNode;
    return 0;
}

// ---------------------------------------------------------------------------
// What this library's start-up and a host kernel call
// ---------------------------------------------------------------------------

void SDL2Circle_ThreadRuntimeInit(void)
{
    if (SDL2Circle_ThisCore() != 0)
        return;
    if (g_bCreatorUp.load(std::memory_order_acquire))
        return;
    if (!CScheduler::IsActive())
        return;    // called again from SDL2Circle_SplitInit, which makes one

    new CLibCXXCreatorTask;    // a CTask registers itself with the scheduler
    g_bCreatorUp.store(1, std::memory_order_release);
}

bool SDL2Circle_ThreadCreateOn0(void (*pFn)(void *), void *pArg)
{
    // Already on the core the scheduler belongs to: nothing to post to, and
    // posting anyway would be a request the caller then waits for the creator
    // to serve - from inside the very core the creator has to run on.
    if (SDL2Circle_ThisCore() == 0)
    {
        pFn(pArg);
        return true;
    }

    if (!g_bCreatorUp.load(std::memory_order_acquire))
        return false;

    PostToCreator(pFn, pArg);
    return true;
}

extern "C" unsigned SDL2Circle_ThreadCoresFree(void)
{
    // Lent by a host kernel, less anything this library has spoken for and
    // anything already running a thread. Asking rather than assuming is the
    // point: which core runs the application and which runs presentation is
    // a host kernel's decision, and pinning on top of either would put two
    // lines of execution on one core with no scheduler to share it.
    unsigned nFree = s_nCoresOffered.load(std::memory_order_acquire);

    nFree &= ~SDL2Circle_ClaimedCores();

    for (unsigned nCore = 0; nCore < MaxCores; nCore++)
        if (s_pRunning[nCore].load(std::memory_order_acquire) != nullptr)
            nFree &= ~(1u << nCore);

    return nFree;
}

extern "C" int SDL2Circle_ThreadPinNext(unsigned nCore)
{
    if (nCore == 0 || nCore >= MaxCores)
        return SDL_SetError("SDL2Circle_ThreadPinNext: core %u is not a core "
                            "this board can run a pinned thread on", nCore);

    const unsigned nFree = SDL2Circle_ThreadCoresFree();
    if (!(nFree & (1u << nCore)))
        return SDL_SetError("SDL2Circle_ThreadPinNext: core %u is not free "
                            "(free cores: 0x%x)", nCore, nFree);

    // The core doing the asking is running the caller, so it is spoken for.
    SDL2Circle_ClaimCore(SDL2Circle_ThisCore());

    s_nPinRequest[SDL2Circle_ThisCore() % MaxCores].store(nCore,
                                                          std::memory_order_release);
    return 0;
}

extern "C" int SDL2Circle_ThreadsStayOnThisCore(void)
{
    const unsigned nCore = SDL2Circle_ThisCore() % MaxCores;

    if (nCore == 0)
        return SDL_SetError("SDL2Circle_ThreadsStayOnThisCore: core 0 already "
                            "has Circle's scheduler, and a std::thread there "
                            "is one of its tasks");

    if (s_pRunning[nCore].load(std::memory_order_acquire) != nullptr)
        return SDL_SetError("SDL2Circle_ThreadsStayOnThisCore: core %u is "
                            "running a pinned thread, and that thread's core "
                            "is reclaimed when it ends", nCore);

    if (s_Coop[nCore].m_pCurrent != nullptr)
        return 0;                       // already scheduling; asked again

    // The line of execution that is making this call becomes a context like
    // any other, and the one that never ends. It has no stack of its own to
    // describe - it is standing on the core's - and it keeps whatever
    // thread-local storage and whatever identity the core already answered
    // with, so a lock taken before this call and released after it still
    // agrees about who holds it.
    CoopContext *pRoot = new (std::nothrow) CoopContext{};
    if (pRoot == nullptr)
        return SDL_SetError("SDL2Circle_ThreadsStayOnThisCore: no memory for "
                            "core %u's first context", nCore);

    pRoot->m_pNext = pRoot;
    pRoot->m_pTLS  = CurrentStorage();
    pRoot->m_nId   = (unsigned long long)(nCore + 1);

    s_Coop[nCore].m_pCurrent = pRoot;

    // Whatever runs here now, this core has a job, so nothing may be pinned
    // on top of it later.
    SDL2Circle_ClaimCore(nCore);

    SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                   "core %u schedules its own std::threads: each one is a "
                   "cooperative context on this core, with a %lu byte stack, "
                   "and none of them reaches core 0", nCore,
                   (unsigned long)((size_t)TASK_STACK_SIZE * 4));
    return 0;
}

extern "C" void SDL2Circle_ThreadCoreOffer(void)
{
    const unsigned nCore = SDL2Circle_ThisCore() % MaxCores;

    // This core is about to run application code, so it needs the same C
    // runtime every other core gets before anything can throw.
    SDL2Circle_ArmCoreRuntime();

    s_nCoresOffered.fetch_or(1u << nCore, std::memory_order_release);

    SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                   "core %u lent to the C++ threading runtime", nCore);

    for (;;)
    {
        ThreadRecord *pRecord = s_pRunning[nCore].load(std::memory_order_acquire);
        if (pRecord == nullptr)
        {
            asm volatile("wfe" ::: "memory");
            continue;
        }

        RunThreadBody(pRecord);

        // Cleared only now: the slot is what says this core is busy, and it
        // is also where CurrentStorage looks, so it must outlive the body.
        s_pRunning[nCore].store(nullptr, std::memory_order_release);
        DropReference(pRecord);
        asm volatile("dsb ish; sev" ::: "memory");
    }
}

// ---------------------------------------------------------------------------
// The context switch
// ---------------------------------------------------------------------------
//
// Written in assembly because changing the stack pointer under the code that
// is running is precisely what a compiler will not let a function do: it has
// already decided where this function's locals are, relative to a stack that
// is about to become a different one.
//
// The register set is CoopRegs, above, and these are its byte offsets. They
// are checked against the structure by the static_asserts that follow, so a
// field added or moved there stops the build rather than corrupting a switch.
//
// AArch64 only. A 32-bit build cannot reach this: src/coreruntime.cpp refuses
// to compile at all without a per-core thread pointer, which is the same
// register this switch carries.
#if AARCH != 64
#error "the cooperative context switch is AArch64. See src/coreruntime.cpp."
#endif

#define COOP_OFF_X19   0
#define COOP_OFF_FP    80
#define COOP_OFF_SP    96
#define COOP_OFF_D8    112

static_assert(offsetof(CoopRegs, m_x19)   == COOP_OFF_X19, "CoopRegs x19 moved");
static_assert(offsetof(CoopRegs, m_x20)   == COOP_OFF_X19 + 8,  "CoopRegs is not one packed run of u64");
static_assert(offsetof(CoopRegs, m_fp)    == COOP_OFF_FP,  "CoopRegs fp moved");
static_assert(offsetof(CoopRegs, m_lr)    == COOP_OFF_FP + 8,  "CoopRegs lr must follow fp");
static_assert(offsetof(CoopRegs, m_sp)    == COOP_OFF_SP,  "CoopRegs sp moved");
static_assert(offsetof(CoopRegs, m_tpidr) == COOP_OFF_SP + 8,  "CoopRegs tpidr must follow sp");
static_assert(offsetof(CoopRegs, m_d8)    == COOP_OFF_D8,  "CoopRegs d8 moved");
static_assert(sizeof(CoopRegs)            == COOP_OFF_D8 + 64, "CoopRegs has grown past what the switch saves");

asm(
"   .section .text.sdl2circle_coop,\"ax\",%progbits\n"
"   .balign 4\n"
"\n"
"   .globl  sdl2circle_coop_switch\n"
"   .type   sdl2circle_coop_switch, %function\n"
"sdl2circle_coop_switch:\n"          // x0 = leaving, x1 = entering
"   stp x19, x20, [x0, #0]\n"
"   stp x21, x22, [x0, #16]\n"
"   stp x23, x24, [x0, #32]\n"
"   stp x25, x26, [x0, #48]\n"
"   stp x27, x28, [x0, #64]\n"
"   stp x29, x30, [x0, #80]\n"       // frame pointer, and where to come back to
"   mov x2, sp\n"
"   mrs x3, tpidr_el0\n"             // thread identity and thread-local storage
"   stp x2, x3, [x0, #96]\n"
"   stp d8,  d9,  [x0, #112]\n"
"   stp d10, d11, [x0, #128]\n"
"   stp d12, d13, [x0, #144]\n"
"   stp d14, d15, [x0, #160]\n"
"   mov x0, x1\n"                    // and fall through into the restore
"   .size   sdl2circle_coop_switch, . - sdl2circle_coop_switch\n"
"\n"
"   .globl  sdl2circle_coop_enter\n"
"   .type   sdl2circle_coop_enter, %function\n"
"sdl2circle_coop_enter:\n"           // x0 = entering; does not return
"   ldp d8,  d9,  [x0, #112]\n"
"   ldp d10, d11, [x0, #128]\n"
"   ldp d12, d13, [x0, #144]\n"
"   ldp d14, d15, [x0, #160]\n"
"   ldp x2, x3, [x0, #96]\n"
"   mov sp, x2\n"                    // from here the locals are the other one's
"   msr tpidr_el0, x3\n"
"   ldp x19, x20, [x0, #0]\n"
"   ldp x21, x22, [x0, #16]\n"
"   ldp x23, x24, [x0, #32]\n"
"   ldp x25, x26, [x0, #48]\n"
"   ldp x27, x28, [x0, #64]\n"
"   ldp x29, x30, [x0, #80]\n"
"   ret\n"                           // into its own last switch, or, for a
                                     // context that has never run, into
                                     // sdl2circle_coop_start on a fresh stack
"   .size   sdl2circle_coop_enter, . - sdl2circle_coop_enter\n"
"   .text\n"
);
