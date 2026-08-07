//
// libcxxthreading.cpp — libc++'s threading runtime, for the cores this
// library actually puts applications on.
//
// WHY THIS FILE EXISTS. Not because anything upstream is broken — it is not,
// and reading its documentation first would have saved a night.
//
// Circle's scheduler is SPECIFIED as core 0 only. doc/multicore.txt, lines
// 62-64:
//
//     "The cooperative non-preemptive scheduler is intended to allow multiple
//      threads of operation on a single core. It cannot be used on more than
//      one core at a time and should always run on core 0."
//
// circle-stdlib builds libc++'s threading on that scheduler, and says so. So
// the C++ standard library's threading is core-0-only BY DESIGN and BY
// DOCUMENTATION. Every primitive it ships is right where it was meant to run.
//
// WHAT IS OUT OF SPEC IS OURS. This library starts the application on a
// SECOND core and then lets it call that runtime. There is one scheduler, it
// belongs to core 0, so on the application core "the task the scheduler is
// currently running" is whatever core 0 happens to be doing at that instant —
// and this library's own servo yields on every pass, so the answer moves
// continuously.
//
// The consequence is worth stating plainly because it is easy to under-read:
// std::recursive_mutex could not be used on the application core AT ALL. Not
// merely across cores — locked and unlocked microseconds apart on that one
// core, with no contention and no second core involved, it halted the board,
// because Circle's CMutex stamps the current task on Acquire and asserts that
// the same task still holds it on Release. Two games died on it after
// everything else about them worked: one through its audio manager, one
// through a logging library that takes the lock on every line. It hid for so
// long because it is a property of WHICH MUTEX TYPE the code reaches for, not
// of what the code does; the two games had nothing in common but the type.
//
// Behind those two were quieter faults on the SUCCESS path, which say nothing
// at all and are the reason this file covers the whole runtime rather than
// the one type that shouted. A contended std::mutex blocks on a Circle
// semaphore, which is a scheduler object, from a core that has no scheduler.
// Releasing one yields CORE 0's scheduler from another core. Every condition
// variable does both. std::thread cannot be created off core 0 at all,
// because constructing a task registers it with the same scheduler. And
// __libcpp_tls_get reads its slot out of "the current task", which off core 0
// is the wrong task by construction — so a thread_local read from the
// application core answered with something else's storage.
//
// So this is a DEBT THIS LIBRARY OWES, not a fix to someone else's bug. We
// chose to run applications off core 0; honouring that choice means supplying
// the parts of the C++ runtime that choice breaks.
//
// HOW THE OVERRIDE WORKS, since it is unusual. This file defines every symbol
// circle-stdlib's liblibcxx-threading.a defines, and sdl-app.mk — the build
// fragment every application that links this library includes — takes that
// archive out of the library list. Two complete implementations of one ABI,
// and the link is given exactly one of them. Nothing vendored is edited and
// no symbol is ever defined twice. The TYPES are unchanged: their sizes are
// baked into libc++ and into every object that ever declared a std::mutex, so
// what sits behind the opaque storage is all that differs.
//
// WHAT THE PRIMITIVES ARE. Futex-shaped, and correct on every core:
//
//   std::mutex             one atomic word. Compare-and-swap to take, store
//                          to release.
//   std::recursive_mutex   that, plus an owner and a recursion depth. The
//                          owner is an IDENTITY that does not move under its
//                          holder — which is the whole of the fix.
//   condition variable     a generation counter and a waiter count. A signal
//                          bumps the generation; a waiter returns when the
//                          generation it recorded is no longer current.
//   std::thread            a cooperative Circle task on core 0 by default,
//                          created through a core-0 proxy when the request
//                          comes from elsewhere; or a bare thread on a core
//                          a host kernel has lent (SDL_circle.h).
//   thread_local           a block of storage per thread, addressed by
//                          TPIDR_EL0, which Circle's task switch already
//                          preserves per task.
//
// TWO SURFACES, ONE PRIMITIVE UNDERNEATH. src/threads.cpp implements SDL's
// OWN locks — SDL_mutex, SDL_cond, SDL_sem — and this file implements
// libc++'s ABI. They are different APIs with different storage and different
// contracts (SDL's mutex is recursive; std::mutex must not be), so neither
// can be written in terms of the other. What they share is the part that was
// ever in doubt: ONE identity, SDL_ThreadID, and ONE wait,
// SDL2Circle_ThreadWaitSpin, which yields to Circle's scheduler on the
// hardware core and spins anywhere else. Every blocking loop in this file
// goes through that wait, and no other blocking primitive appears in it.
//
// THE LIMITS, carried honestly:
//
//   - A std::thread on core 0 is COOPERATIVE. It runs when core 0 yields,
//     which the servo and every wait in this file do constantly, but a thread
//     that computes without ever waiting keeps core 0 to itself — and core 0
//     is where every device is serviced.
//   - ONE PINNED THREAD PER LENT CORE at a time. A second request for a busy
//     core is refused rather than queued.
//   - A WAIT OCCUPIES THE CORE IT WAITS ON. Off core 0 there is nothing to
//     sleep on, so a blocked application core is a spinning application core.
//   - A TIMED WAIT off core 0 POLLS. It reads the free-running system counter
//     between spins; it does not sleep to the deadline.
//   - thread_local DESTRUCTORS RUN WHEN A THREAD ENDS, and the application
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

// THE ONE WAIT. Blocking anywhere in this file is this call in a loop, and
// nothing else: on the hardware core it is a scheduler yield, so whatever
// holds the thing being waited for can run; on any other core it is the
// processor's yield hint, because there is no scheduler there to hand the
// time to. It is what src/threads.cpp waits on too.
inline void WaitABit(void)
{
    SDL2Circle_ThreadWaitSpin();
}

// THE ONE IDENTITY, and the reason the recursive mutex works at all. It has
// to be unique among everything that can hold a lock at the same moment, and
// — unlike "whichever task the scheduler is running" — it must not change
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
// std::mutex — one atomic word
// ---------------------------------------------------------------------------
//
// Zero-initialised storage is a free mutex, which the ABI requires:
// _LIBCPP_MUTEX_INITIALIZER is an empty brace pair, so a std::mutex with
// static storage duration is never constructed at all — it is simply the
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
// std::recursive_mutex — owned by an identity rather than by a scheduler task
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
// Condition variables — a generation counter and a waiter count
// ---------------------------------------------------------------------------
//
// A waiter records the generation it went to sleep on and returns when that
// is no longer the current one. A signal bumps the generation, which releases
// every waiter rather than one — and that is allowed, because the standard
// permits a condition variable to wake a thread spuriously and every correct
// use of one re-tests its predicate in a loop.
//
// The order in a wait is what stops a signal being lost: the generation is
// read and the waiter counted while the caller still holds the mutex, and
// only then is the mutex released. A signal issued under that mutex therefore
// either happens before the read — in which case the caller was never going
// to wait for it — or after it, in which case the bump is seen. Zeroed
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
// on this platform is gettimeofday, which is Circle's calendar time — so the
// same clock has to be read here or the difference is meaningless. The RESULT
// is then held against the free-running counter, which needs no lock and is
// the clock every other timed wait in this library uses.
//
// The calendar read goes through SDL2Circle_KernelTimeUTC, which puts it on
// core 0: the timer object is a device, and every caller of this is a thread
// waiting on some other core. A clock the kernel cannot give — a wait issued
// before SDL_Init — leaves the reading at zero, which makes the deadline the
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
// WHERE THE BLOCK IS FOUND is the part that was broken and the part worth
// reading. There are three kinds of caller and each keeps its storage
// somewhere different:
//
//   a cooperative task on core 0    in the task's own libc++ user-data slot,
//                                   which is what that slot is for
//   a pinned thread on a lent core  in the thread's record
//   a bare core running no thread   in that core's own slot — this is the
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

// What a std::thread is, on either kind of core. The record outlives the
// thread's execution, because join and detach may arrive after it has
// finished; the two sides each drop a reference and the last one frees it.
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

// A std::thread on core 0: an ordinary Circle scheduler task. Four times the
// usual stack, because a C++ thread carries the unwinder's state and the
// default does not hold it — the same sizing circle-stdlib's own version
// used, kept because it was arrived at the hard way.
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
// 0's. So a creation issued from another core is POSTED here and a core-0
// task does the constructing. One request outstanding at a time, which is
// ample: creating a thread is a rare event, and the wait for the answer is
// the ordinary one.
//
// BOTH threading surfaces come through here — std::thread from this file and
// SDL_CreateThread from src/threads.cpp — because there is one reason to need
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
// A routine that throws is an exceptional call under the standard — the flag
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
        // Off core 0: post the record and let the creator task build the
        // scheduler task. Refusing instead would make std::thread unusable
        // from the very core this library exists to put applications on.
        //
        // The caller's core is running the application, by definition of who
        // is making this call, so nothing may be pinned onto it later.
        SDL2Circle_ClaimCore(SDL2Circle_ThisCore());

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
// ends — so on core 0's main task and on the application core, neither of
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
    // to serve — from inside the very core the creator has to run on.
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
