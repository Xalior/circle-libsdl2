//
// threads.cpp - SDL's atomics, locks and threads on Circle
//
// What backs what, and where: Circle gives each core one line of execution,
// plus a cooperative scheduler that exists on the hardware core alone. That
// single fact decides everything in this file.
//
//   Atomics and spin locks   the processor's own atomic instructions. They
//                            work on every core and between cores, and they
//                            are what every other primitive here is built on.
//
//   Mutex, condition         one word of state guarded by a spin lock, and a
//   variable, semaphore      wait loop around it. Correct on every core and
//                            between cores: two cores contending for the same
//                            mutex really do exclude each other. On the
//                            hardware core the wait loop yields to Circle's
//                            scheduler, so the task holding the lock can run;
//                            on a core that schedules its own C++ threads it
//                            gives the core to the next of them, for the same
//                            reason; anywhere else it spins, because there is
//                            nothing on that core to hand the time to and the
//                            core is dedicated to the application anyway.
//
//   Threads                  Cooperative, always, and on one of two cores. On
//                            a core that schedules its own threads, a
//                            cooperative context on that core, built here and
//                            now. Everywhere else a Circle scheduler task -
//                            which is cooperative too, but registers itself
//                            with the scheduler while it is being built, so a
//                            request from another core is handed to the core-0
//                            creator task and waited for.
//
// What an application can rely on:
//
//   Every lock in this file excludes, from any core. None of them is a stub
//   that returns success and does nothing, so a lock taken around shared state
//   protects it whichever core the two sides are on.
//
//   A wait occupies the core it waits on. There is nothing to sleep on off the
//   hardware core, so a blocked application core is a spinning application
//   core. Waits are for handing work between parties that are really running,
//   not for idling.
//
//   A wait keeps the audio device fed. On the application core the audio
//   callback is run by whatever calls SDL_PumpEvents, which is the application
//   itself, so a wait that only spun would stop the sound for as long as it
//   lasted, and would deadlock outright an application whose callback is what
//   ends the wait. The spin loop runs the audio pump, which is as close to a
//   desktop's separate audio thread as this gets; SDL_LockAudioDevice is still
//   how an application keeps its callback out of a section.
//
//   SDL_CreateThread works from any core, and so do SDL_WaitThread,
//   SDL_DetachThread and SDL_GetThreadID on what it returns. That matters
//   because an application that creates its main game thread through SDL - and
//   several do - runs on the application core by the time it gets there, which
//   is never core 0 under the split. What it needs is a CScheduler somewhere in
//   the system; without one there is nothing anywhere for a thread to run on
//   and the call fails, sets the error and says so on the log.
//
//   A thread that does start is cooperative, and which core it is cooperative
//   against is the one thing a host kernel decides. By default it is core 0,
//   whichever core asked for the thread; on a core that has called
//   SDL2Circle_ThreadsStayOnThisCore it is that core. Either way the thread
//   runs when something gives its core up, which the servo's every lap, every
//   wait in this file and SDL_Delay all do.
//
//   THE DEFAULT IS THE PLACEMENT TO WATCH, not the other one. A thread that
//   computes without ever calling into SDL or sleeping keeps its core, and on
//   core 0 that core is where every device on the board is serviced. SDL has
//   no idea, when it makes a thread, that hardware timing is waiting behind
//   it - and an application is quite entitled to hand a thread two seconds of
//   arithmetic. On core 0 those are two seconds without the SD card, the USB
//   host or the serial port; on an application core there is nothing waiting
//   for that core at all, which is why work belongs there.
//
//   std::thread follows the same rule and the same call. The two surfaces
//   also share one identity and one wait, so a lock held through either and
//   inspected through the other agrees about who holds it.
//
//   Thread priorities do not exist. Circle's scheduler is round-robin without
//   them, so SDL_SetThreadPriority reports that it cannot do what was asked
//   rather than claiming to have done it.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "threads.h"

#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/sysconfig.h>
#include <circle/timer.h>

#include <cstdlib>
#include <cstring>
#include <cstdint>

static const char From[] = "sdl2thread";

// ---------------------------------------------------------------------------
// Atomics
// ---------------------------------------------------------------------------
//
// The compiler's atomic builtins, which on AArch64 are load-exclusive /
// store-exclusive pairs with the barriers the memory order asks for. Nothing
// here needs the scheduler, so all of it is valid on every core.

extern "C" SDL_bool SDL_AtomicTryLock(SDL_SpinLock *lock)
{
    if (lock == nullptr)
        return SDL_FALSE;
    return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" void SDL_AtomicLock(SDL_SpinLock *lock)
{
    if (lock == nullptr)
        return;

    // A plain spin, deliberately. This is SDL's rawest lock and a holder is
    // expected to keep it for a few instructions and never to block inside
    // it; Circle's scheduler is cooperative, so two tasks on the hardware
    // core cannot both be inside such a section, and contention can therefore
    // only come from another core, which is really running and will release.
    //
    // On a core that schedules its own std::threads the holder CAN be on this
    // core: another cooperative context, stopped where it gave the core up.
    // Spinning would then wait for something the spin is itself preventing,
    // so the spin hands the core on instead - and does nothing else. No audio
    // pump, no device, nothing that could re-enter the section this lock
    // guards; a raw lock stays raw.
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0)
    {
        if (!SDL2Circle_ThreadScheduleNext())
            asm volatile("yield" ::: "memory");
    }
}

extern "C" void SDL_AtomicUnlock(SDL_SpinLock *lock)
{
    if (lock == nullptr)
        return;
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

extern "C" void SDL_MemoryBarrierReleaseFunction(void)
{
    asm volatile("dmb ish" ::: "memory");
}

extern "C" void SDL_MemoryBarrierAcquireFunction(void)
{
    asm volatile("dmb ish" ::: "memory");
}

extern "C" SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval)
{
    if (a == nullptr)
        return SDL_FALSE;
    return __atomic_compare_exchange_n(&a->value, &oldval, newval, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? SDL_TRUE : SDL_FALSE;
}

extern "C" int SDL_AtomicSet(SDL_atomic_t *a, int v)
{
    if (a == nullptr)
        return 0;
    return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

extern "C" int SDL_AtomicGet(SDL_atomic_t *a)
{
    if (a == nullptr)
        return 0;
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

extern "C" int SDL_AtomicAdd(SDL_atomic_t *a, int v)
{
    if (a == nullptr)
        return 0;
    return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}

extern "C" SDL_bool SDL_AtomicCASPtr(void **a, void *oldval, void *newval)
{
    if (a == nullptr)
        return SDL_FALSE;
    return __atomic_compare_exchange_n(a, &oldval, newval, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? SDL_TRUE : SDL_FALSE;
}

extern "C" void *SDL_AtomicSetPtr(void **a, void *v)
{
    if (a == nullptr)
        return nullptr;
    return __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST);
}

extern "C" void *SDL_AtomicGetPtr(void **a)
{
    if (a == nullptr)
        return nullptr;
    return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

void SDL2Circle_ThreadWaitSpin(void)
{
    // Core 0 pumps too: without the split there is no servo to feed the
    // device, so SDL_PumpEvents is the only thing that ever does. The pump
    // itself decides which core may produce and returns at once on a core
    // that may not, so this call is a no-op where it does not apply.
    SDL2Circle_AudioPump();

    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
    {
        SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_YIELD);
        CScheduler::Get()->Yield();
        return;
    }

    // A wait must not stop the sound. The audio callback here runs inside
    // whichever context calls SDL_PumpEvents, which on the application core
    // is the application's own loop, so a wait that only spins leaves the
    // device unfed. A game whose audio callback drives its music clock - an
    // emulated sound chip, say - can wait at start-up for that clock to
    // reach a mark, and such a wait only ends if the callback runs; spinning
    // alone, it would be waiting for something it is itself preventing.
    //
    // Only the core that owns audio production produces: this call runs on
    // every core, but the pump itself enforces the single writer for each
    // destination, so a wait never turns the waiting core into a second
    // producer. On any other core this call costs a comparison and returns.
    //
    // SDL_LockAudioDevice is how an application keeps its callback out of a
    // section; the pump obeys it.
    SDL2Circle_AudioPump();

    // A core whose host asked it to schedule its own std::threads has
    // somewhere to hand the time to, exactly as core 0 does: the next
    // runnable context on this core, which may be the very context that holds
    // what this wait is waiting for. It answers false where no host asked, so
    // a build that has never heard of this waits the way it always did.
    if (SDL2Circle_ThreadScheduleNext())
        return;

    asm volatile("yield" ::: "memory");
}

namespace
{

// Whether a wait started at `start` (system timer, microseconds) has run out.
// SDL_MUTEX_MAXWAIT means never; zero means immediately, which is what makes
// SDL_SemWaitTimeout(sem, 0) the same call as SDL_SemTryWait.
bool WaitExpired(u64 start, Uint32 ms)
{
    if (ms == SDL_MUTEX_MAXWAIT)
        return false;
    return CTimer::GetClockTicks64() - start >= (u64)ms * 1000;
}

}   // namespace

// ---------------------------------------------------------------------------
// Thread identity
// ---------------------------------------------------------------------------
//
// An identity has to be unique among everything that can hold a lock at the
// same time, and there are three kinds of those. On the hardware core, one
// per scheduler task, so the task object's own address answers. On a core
// that schedules its own cooperative contexts, one per context, so the
// thread's own record address answers. On any other core there is exactly one
// line of execution, so the core number answers - offset by one so that no
// identity is zero, which is what a free mutex holds.
//
// None of the three can collide: Circle's heap starts far above the highest
// core number a board has, and the context that ran main on a core keeps that
// core's own number, so introducing a scheduler never changes the identity of
// a line of execution that was already holding a lock.

extern "C" SDL_threadID SDL_ThreadID(void)
{
    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
    {
        CTask *pTask = CScheduler::Get()->GetCurrentTask();
        if (pTask != nullptr)
            return (SDL_threadID)(uintptr_t)pTask;
    }

    const unsigned long long nContext = SDL2Circle_ThreadContextID();
    if (nContext != 0)
        return (SDL_threadID)nContext;

    return (SDL_threadID)(SDL2Circle_ThisCore() + 1);
}

// ---------------------------------------------------------------------------
// Mutexes
// ---------------------------------------------------------------------------
//
// SDL's mutex is recursive: the same thread may lock it repeatedly and must
// unlock it as many times. Circle's own CMutex is recursive too, but it is
// built on the scheduler's blocking primitives and so exists on the hardware
// core alone. This one is the same shape without that restriction - the owner
// and the recursion count guarded by a spin lock, and a wait loop that yields
// where there is a scheduler to yield to.

struct SDL_mutex
{
    SDL_SpinLock lock;        // guards the two fields below
    SDL_threadID owner;       // 0 when the mutex is free
    int          recursion;
};

extern "C" SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *mutex = (SDL_mutex *)calloc(1, sizeof(SDL_mutex));
    if (mutex == nullptr)
        SDL_SetError("Out of memory");
    return mutex;
}

extern "C" void SDL_DestroyMutex(SDL_mutex *mutex)
{
    free(mutex);
}

extern "C" int SDL_LockMutex(SDL_mutex *mutex)
{
    if (mutex == nullptr)
        return SDL_SetError("Passed a NULL mutex");

    const SDL_threadID self = SDL_ThreadID();

    for (;;)
    {
        SDL_AtomicLock(&mutex->lock);
        if (mutex->owner == 0)
        {
            mutex->owner = self;
            mutex->recursion = 1;
            SDL_AtomicUnlock(&mutex->lock);
            return 0;
        }
        if (mutex->owner == self)
        {
            mutex->recursion++;
            SDL_AtomicUnlock(&mutex->lock);
            return 0;
        }
        SDL_AtomicUnlock(&mutex->lock);

        SDL2Circle_ThreadWaitSpin();
    }
}

extern "C" int SDL_TryLockMutex(SDL_mutex *mutex)
{
    if (mutex == nullptr)
        return SDL_SetError("Passed a NULL mutex");

    const SDL_threadID self = SDL_ThreadID();
    int result = SDL_MUTEX_TIMEDOUT;

    SDL_AtomicLock(&mutex->lock);
    if (mutex->owner == 0)
    {
        mutex->owner = self;
        mutex->recursion = 1;
        result = 0;
    }
    else if (mutex->owner == self)
    {
        mutex->recursion++;
        result = 0;
    }
    SDL_AtomicUnlock(&mutex->lock);

    return result;
}

extern "C" int SDL_UnlockMutex(SDL_mutex *mutex)
{
    if (mutex == nullptr)
        return SDL_SetError("Passed a NULL mutex");

    const SDL_threadID self = SDL_ThreadID();

    SDL_AtomicLock(&mutex->lock);
    if (mutex->owner != self)
    {
        SDL_AtomicUnlock(&mutex->lock);
        return SDL_SetError("mutex not owned by this thread");
    }
    if (--mutex->recursion == 0)
        mutex->owner = 0;
    SDL_AtomicUnlock(&mutex->lock);

    return 0;
}

// ---------------------------------------------------------------------------
// Condition variables
// ---------------------------------------------------------------------------
//
// Counted wakeups rather than a wait list: a signal hands out one token, a
// broadcast hands out one per waiter, and a waiter returns when it has taken
// a token. That is what keeps a signal from being lost - the token is issued
// whether or not the waiter has reached its loop yet - and what keeps a
// signal from waking more waiters than it should, which a plain flag would.
//
// The mutex is released before the wait and re-acquired after it, which is
// SDL's contract. The token count is taken under the condition variable's own
// spin lock, so the release and the arrival of a signal cannot cross.

struct SDL_cond
{
    SDL_SpinLock lock;        // guards the two fields below
    int          waiting;     // waiters that have not yet taken a token
    int          signals;     // tokens issued and not yet taken
};

extern "C" SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *cond = (SDL_cond *)calloc(1, sizeof(SDL_cond));
    if (cond == nullptr)
        SDL_SetError("Out of memory");
    return cond;
}

extern "C" void SDL_DestroyCond(SDL_cond *cond)
{
    free(cond);
}

extern "C" int SDL_CondSignal(SDL_cond *cond)
{
    if (cond == nullptr)
        return SDL_SetError("Passed a NULL condition variable");

    SDL_AtomicLock(&cond->lock);
    if (cond->waiting > cond->signals)
        cond->signals++;
    SDL_AtomicUnlock(&cond->lock);

    return 0;
}

extern "C" int SDL_CondBroadcast(SDL_cond *cond)
{
    if (cond == nullptr)
        return SDL_SetError("Passed a NULL condition variable");

    SDL_AtomicLock(&cond->lock);
    if (cond->waiting > cond->signals)
        cond->signals = cond->waiting;
    SDL_AtomicUnlock(&cond->lock);

    return 0;
}

extern "C" int SDL_CondWaitTimeout(SDL_cond *cond, SDL_mutex *mutex, Uint32 ms)
{
    if (cond == nullptr)
        return SDL_SetError("Passed a NULL condition variable");
    if (mutex == nullptr)
        return SDL_SetError("Passed a NULL mutex");

    SDL_AtomicLock(&cond->lock);
    cond->waiting++;
    SDL_AtomicUnlock(&cond->lock);

    if (SDL_UnlockMutex(mutex) != 0)
    {
        SDL_AtomicLock(&cond->lock);
        cond->waiting--;
        SDL_AtomicUnlock(&cond->lock);
        return -1;
    }

    const u64 start = CTimer::GetClockTicks64();
    int result = SDL_MUTEX_TIMEDOUT;

    for (;;)
    {
        SDL_AtomicLock(&cond->lock);
        if (cond->signals > 0)
        {
            cond->signals--;
            cond->waiting--;
            SDL_AtomicUnlock(&cond->lock);
            result = 0;
            break;
        }
        SDL_AtomicUnlock(&cond->lock);

        if (WaitExpired(start, ms))
        {
            // A signal issued between the check above and this one is left in
            // the count for another waiter, so giving up never eats a wakeup.
            SDL_AtomicLock(&cond->lock);
            cond->waiting--;
            if (cond->signals > cond->waiting)
                cond->signals = cond->waiting;
            SDL_AtomicUnlock(&cond->lock);
            break;
        }

        SDL2Circle_ThreadWaitSpin();
    }

    // SDL says the mutex is held again on return, whichever way the wait
    // ended, so this is not conditional on the result.
    SDL_LockMutex(mutex);

    return result;
}

extern "C" int SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex)
{
    return SDL_CondWaitTimeout(cond, mutex, SDL_MUTEX_MAXWAIT);
}

// ---------------------------------------------------------------------------
// Semaphores
// ---------------------------------------------------------------------------

struct SDL_semaphore
{
    SDL_SpinLock lock;
    Uint32       count;
};

extern "C" SDL_sem *SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_sem *sem = (SDL_sem *)calloc(1, sizeof(SDL_sem));
    if (sem == nullptr)
    {
        SDL_SetError("Out of memory");
        return nullptr;
    }
    sem->count = initial_value;
    return sem;
}

extern "C" void SDL_DestroySemaphore(SDL_sem *sem)
{
    free(sem);
}

extern "C" int SDL_SemWaitTimeout(SDL_sem *sem, Uint32 timeout)
{
    if (sem == nullptr)
        return SDL_SetError("Passed a NULL semaphore");

    const u64 start = CTimer::GetClockTicks64();

    for (;;)
    {
        SDL_AtomicLock(&sem->lock);
        if (sem->count > 0)
        {
            sem->count--;
            SDL_AtomicUnlock(&sem->lock);
            return 0;
        }
        SDL_AtomicUnlock(&sem->lock);

        if (WaitExpired(start, timeout))
            return SDL_MUTEX_TIMEDOUT;

        SDL2Circle_ThreadWaitSpin();
    }
}

extern "C" int SDL_SemWait(SDL_sem *sem)
{
    return SDL_SemWaitTimeout(sem, SDL_MUTEX_MAXWAIT);
}

extern "C" int SDL_SemTryWait(SDL_sem *sem)
{
    return SDL_SemWaitTimeout(sem, 0);
}

extern "C" int SDL_SemPost(SDL_sem *sem)
{
    if (sem == nullptr)
        return SDL_SetError("Passed a NULL semaphore");

    SDL_AtomicLock(&sem->lock);
    sem->count++;
    SDL_AtomicUnlock(&sem->lock);

    return 0;
}

extern "C" Uint32 SDL_SemValue(SDL_sem *sem)
{
    if (sem == nullptr)
        return 0;

    SDL_AtomicLock(&sem->lock);
    Uint32 value = sem->count;
    SDL_AtomicUnlock(&sem->lock);

    return value;
}

// ---------------------------------------------------------------------------
// Thread-local storage
// ---------------------------------------------------------------------------
//
// A small table keyed by thread identity rather than a per-task pointer.
// Circle's CTask does carry user-data slots, but the one slot marked free for
// application use belongs to the application, and taking it would break any
// host kernel that already uses it.

namespace
{

constexpr unsigned TLS_MAX_KEYS    = 32;
constexpr unsigned TLS_MAX_THREADS = 8;

struct TLSEntry
{
    SDL_threadID              owner;      // 0 when the row is free
    void                     *value[TLS_MAX_KEYS];
    SDL_TLSDestructorCallback dtor[TLS_MAX_KEYS];
};

SDL_SpinLock s_tlsLock;
TLSEntry     s_tls[TLS_MAX_THREADS];
unsigned     s_tlsKeys = 0;

// Both callers hold s_tlsLock.
TLSEntry *TLSFind(SDL_threadID owner)
{
    for (unsigned i = 0; i < TLS_MAX_THREADS; i++)
        if (s_tls[i].owner == owner)
            return &s_tls[i];
    return nullptr;
}

TLSEntry *TLSClaim(SDL_threadID owner)
{
    TLSEntry *entry = TLSFind(owner);
    if (entry != nullptr)
        return entry;

    entry = TLSFind(0);
    if (entry != nullptr)
        entry->owner = owner;
    return entry;
}

}   // namespace

extern "C" SDL_TLSID SDL_TLSCreate(void)
{
    SDL_TLSID id = 0;

    SDL_AtomicLock(&s_tlsLock);
    if (s_tlsKeys < TLS_MAX_KEYS)
        id = (SDL_TLSID)++s_tlsKeys;
    SDL_AtomicUnlock(&s_tlsLock);

    if (id == 0)
        SDL_SetError("Out of thread-local storage keys");
    return id;
}

extern "C" void *SDL_TLSGet(SDL_TLSID id)
{
    if (id == 0 || id > TLS_MAX_KEYS)
        return nullptr;

    void *value = nullptr;

    SDL_AtomicLock(&s_tlsLock);
    TLSEntry *entry = TLSFind(SDL_ThreadID());
    if (entry != nullptr)
        value = entry->value[id - 1];
    SDL_AtomicUnlock(&s_tlsLock);

    return value;
}

extern "C" int SDL_TLSSet(SDL_TLSID id, const void *value,
                          SDL_TLSDestructorCallback destructor)
{
    if (id == 0 || id > TLS_MAX_KEYS)
        return SDL_SetError("Invalid thread-local storage key");

    SDL_AtomicLock(&s_tlsLock);
    TLSEntry *entry = TLSClaim(SDL_ThreadID());
    if (entry != nullptr)
    {
        entry->value[id - 1] = (void *)value;
        entry->dtor[id - 1]  = destructor;
    }
    SDL_AtomicUnlock(&s_tlsLock);

    if (entry == nullptr)
        return SDL_SetError("Out of thread-local storage rows");
    return 0;
}

void SDL2Circle_TLSRelease(unsigned long thread)
{
    // The row is detached under the lock and its destructors run outside it,
    // because a destructor is application code and may touch storage of its
    // own.
    TLSEntry copy;

    SDL_AtomicLock(&s_tlsLock);
    TLSEntry *entry = TLSFind((SDL_threadID)thread);
    if (entry == nullptr)
    {
        SDL_AtomicUnlock(&s_tlsLock);
        return;
    }
    copy = *entry;
    memset(entry, 0, sizeof(*entry));
    SDL_AtomicUnlock(&s_tlsLock);

    for (unsigned i = 0; i < TLS_MAX_KEYS; i++)
        if (copy.value[i] != nullptr && copy.dtor[i] != nullptr)
            copy.dtor[i](copy.value[i]);
}

extern "C" void SDL_TLSCleanup(void)
{
    SDL2Circle_TLSRelease((unsigned long)SDL_ThreadID());
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

// The handle, and who frees it: whoever waits for a thread or detaches it may
// be on a different core from the thread itself, so the two sides can be
// really concurrent and the handle needs an owner at every instant. That is
// true of both placements - a task on core 0 waited for from the application
// core, or a context on the application core waited for from core 0 - and the
// one word below settles it without either side knowing which it is.
//
// One word decides it. Each side sets its own bit - the thread when it ends,
// the application when it detaches - and reads back what the other side had
// already set. Whichever side finds the other's bit already there is the last
// one out and frees the handle. A side that does not find it has just handed
// ownership over and must not touch the handle again, not even to unlock
// something: on a second core the other side can be inside free() by the next
// instruction.
//
// SDL_WaitThread is the third case and needs no bit. Waiting says the handle
// is not detached, so the ending thread hands it to the waiter and leaves; the
// waiter reads the status and frees.
static const int THREAD_FINISHED = 1;
static const int THREAD_DETACHED = 2;

struct SDL_Thread
{
    int                state;       // THREAD_*, atomic
    SDL_ThreadFunction fn;
    void              *data;
    int                status;      // written before FINISHED is published
    SDL_threadID       id;
    char               name[64];
};

namespace
{

// The scheduler deletes a task object of its own accord once the task has
// terminated, so nothing may hold a CTask pointer across the end of Run. The
// SDL_Thread is a separate allocation for exactly that reason: it outlives
// the task and carries the result back to whoever waits for it.
class CSDLThreadTask : public CTask
{
public:
    CSDLThreadTask(SDL_Thread *pThread, unsigned nStackSize)
    :   CTask(nStackSize, TRUE),      // suspended: started once its id is set
        m_pThread(pThread)
    {
        SetName(pThread->name);
    }

    void Run(void) override
    {
        SDL_Thread *pThread = m_pThread;

        // Every Circle task starts with no thread pointer: CTask's constructor
        // memsets the saved register block and the task switch restores
        // TPIDR_EL0 from it, so a task's first instruction runs with that
        // register at zero. Unarmed, the first `throw` in this thread reads
        // the exception globals through a null pointer, and every
        // thread_local it touches aliases low memory shared with every other
        // unarmed task - which is mapped on this hardware, so it appears to
        // work and corrupts instead of faulting.
        //
        // std::thread has always been armed here (RunThreadBody in
        // libcxxthreading.cpp); a thread made through SDL's own API was not,
        // and an application cannot tell the difference from the outside.
        void *pPrevious = SDL2Circle_GetThreadPointer();
        void *pBlock = SDL2Circle_AllocTLSBlock();
        SDL2Circle_SetThreadPointer(pBlock);

        // A thread that throws still has to finish: nothing but this task can
        // publish THREAD_FINISHED, so an exception escaping here would leave
        // SDL_WaitThread spinning for the life of the board, and the board
        // would look hung with no fault reported anywhere. Whoever waits gets
        // the thread back with a failing status instead.
        int status;
        try
        {
            status = pThread->fn(pThread->data);
        }
        catch (...)
        {
            SDL2Circle_Log("sdl2thread", SDL2CIRCLE_LOG_ERROR,
                           "thread \"%s\": uncaught exception, status -1",
                           pThread->name[0] ? pThread->name : "?");
            status = -1;
        }

        SDL2Circle_SetThreadPointer(pPrevious);
        SDL2Circle_FreeTLSBlock(pBlock);

        SDL2Circle_TLSRelease((unsigned long)pThread->id);

        // The status has to be in memory before the flag that publishes it,
        // and the release below is what orders the two for a reader on
        // another core.
        pThread->status = status;

        const int prev = __atomic_fetch_or(&pThread->state, THREAD_FINISHED,
                                           __ATOMIC_ACQ_REL);
        if (prev & THREAD_DETACHED)
            free(pThread);          // nobody is waiting; this is the last exit
    }

private:
    SDL_Thread *m_pThread;
};

// What an SDL thread is when its core schedules its own: the work, and how
// the end of it is published. The cooperative context owns the stack and the
// thread-local block, and the context switch carries the thread pointer, so
// none of the arming CSDLThreadTask has to do appears here.
void sdl_context_body(void *p)
{
    SDL_Thread *thread = (SDL_Thread *)p;

    // A thread that throws still has to finish, for the same reason it does
    // as a task: nothing but this body can reach the publish below, so an
    // exception escaping here would leave SDL_WaitThread spinning for the
    // life of the board with no fault reported anywhere.
    int status;
    try
    {
        status = thread->fn(thread->data);
    }
    catch (...)
    {
        SDL2Circle_Log("sdl2thread", SDL2CIRCLE_LOG_ERROR,
                       "thread \"%s\": uncaught exception, status -1",
                       thread->name[0] ? thread->name : "?");
        status = -1;
    }

    SDL2Circle_TLSRelease((unsigned long)thread->id);

    // In memory before the flag that publishes it, which the finish hook
    // below sets under a release.
    thread->status = status;
}

void sdl_context_finish(void *p)
{
    SDL_Thread *thread = (SDL_Thread *)p;

    const int prev = __atomic_fetch_or(&thread->state, THREAD_FINISHED,
                                       __ATOMIC_ACQ_REL);
    if (prev & THREAD_DETACHED)
        free(thread);           // nobody is waiting; this is the last exit
}

// What the core-0 creator task does for an SDL thread: build the task, settle
// the handle's identity, release it. Nothing here waits, so nothing here can
// hold the creator up.
struct SpawnRequest
{
    SDL_Thread *thread;
    unsigned    stack;
    bool        ok;
};

void spawn_on0(void *p)
{
    SpawnRequest *req = (SpawnRequest *)p;

    CSDLThreadTask *task = new CSDLThreadTask(req->thread, req->stack);
    if (task == nullptr)
    {
        req->ok = false;
        return;
    }

    // The task's identity is its object address, which is what SDL_ThreadID
    // will report from inside it. Setting it before the task is released is
    // what makes SDL_GetThreadID answer correctly from the very first call -
    // and setting it here, before the creator publishes the answer, is what
    // makes that true for a caller on another core too.
    req->thread->id = (SDL_threadID)(uintptr_t)task;
    task->Start();
    req->ok = true;
}

}   // namespace

extern "C" SDL_Thread *SDL_CreateThreadWithStackSize(SDL_ThreadFunction fn,
                                                     const char *name,
                                                     const size_t stacksize,
                                                     void *data)
{
    if (fn == nullptr)
    {
        SDL_SetError("Passed a NULL thread function");
        return nullptr;
    }

    const char *label = name != nullptr ? name : "SDLThread";

    // Whether this thread stays here or goes to core 0, asked once and used
    // twice: it decides the refusal below as well as the placement, because a
    // cooperative context on this core needs no scheduler anywhere.
    const bool bStaysHere = SDL2Circle_ThreadSchedulesHere();

    // A thread that goes to core 0 is a Circle scheduler task, so the system
    // needs a scheduler - wherever the asking is done from. This is the one
    // refusal left: without one there is nothing on core 0 for the thread to
    // run on, and saying so is better than a thread that silently never ran.
    if (!bStaysHere && !CScheduler::IsActive())
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "SDL_CreateThread(\"%s\") refused: no CScheduler",
                       label);
        SDL_SetError("SDL_CreateThread: this system has no CScheduler, so "
                     "there is nothing to run \"%s\" on; the host kernel "
                     "declares one, or SDL2Circle_SplitInit provides one",
                     label);
        return nullptr;
    }

    SDL_Thread *thread = (SDL_Thread *)calloc(1, sizeof(SDL_Thread));
    if (thread == nullptr)
    {
        SDL_SetError("Out of memory");
        return nullptr;
    }

    thread->fn   = fn;
    thread->data = data;
    strncpy(thread->name, label, sizeof(thread->name) - 1);

    // SDL_CreateThread carries no stack size, so the size an application gets
    // is entirely this implementation's choice; on every platform SDL
    // normally runs on, that choice is megabytes. Circle's TASK_STACK_SIZE is
    // 32 KB, which suits a Circle helper task but not application code: the
    // thread an application creates through SDL is frequently its main
    // thread - the whole game, all its locals and every library it calls -
    // and needs a stack sized accordingly. A megabyte is cheap on these
    // boards; an application that knows better uses
    // SDL_CreateThreadWithStackSize.
    const size_t DefaultStack = 0x100000;

    size_t stack = stacksize != 0 ? stacksize : DefaultStack;
    if (stack < (size_t)TASK_STACK_SIZE)
        stack = (size_t)TASK_STACK_SIZE;
    stack = (stack + 15) & ~(size_t)15;

    if (bStaysHere)
    {
        // This core schedules its own threads, so the thread is a cooperative
        // context on it: no task, no scheduler, nothing posted to core 0, and
        // the same stack size the task would have had.
        //
        // This is the placement a worker wants and core 0 is the placement it
        // does not. SDL knows nothing about a board's hardware timing when it
        // makes a thread, and an application is entitled to create one that
        // computes for seconds without yielding - which on core 0 is seconds
        // in which the SD card, the USB host and the serial port go
        // unserviced, because core 0 is the only core that services them.
        // Nothing is waiting on this core, which is exactly why work belongs
        // here.
        const unsigned long long id =
            SDL2Circle_ThreadStartHere(sdl_context_body, sdl_context_finish,
                                       thread, stack);
        if (id == 0)
        {
            free(thread);
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                           "SDL_CreateThread(\"%s\") on core %u: no memory "
                           "for stack or thread-local block", label,
                           SDL2Circle_ThisCore());
            SDL_SetError("Out of memory");
            return nullptr;
        }

        // Settled before anything can yield, so SDL_GetThreadID answers
        // correctly from the very first call and the thread itself sees its
        // own identity: the context cannot run until this core next waits.
        thread->id = (SDL_threadID)id;
        return thread;
    }

    // Said once per core, because it is the placement nothing else announces.
    if (SDL2Circle_ThisCore() != 0)
        SDL2Circle_ThreadAnnounceCore0();

    // Constructing the task is what registers it with the scheduler, and the
    // scheduler is core 0's. On core 0 this builds it here and now; anywhere
    // else the core-0 creator task builds it and this returns once it has.
    // Either way the handle is complete before the call returns.
    SpawnRequest req{thread, (unsigned)stack, false};
    if (!SDL2Circle_ThreadCreateOn0(spawn_on0, &req))
    {
        free(thread);
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "SDL_CreateThread(\"%s\") from core %u refused: "
                       "core-0 creator task not running", label,
                       SDL2Circle_ThisCore());
        SDL_SetError("SDL_CreateThread: \"%s\" was asked for from core %u and "
                     "the core-0 creator task that builds threads is not "
                     "running; the host kernel calls SDL2Circle_ArmCoreRuntime "
                     "on core 0", label, SDL2Circle_ThisCore());
        return nullptr;
    }

    if (!req.ok)
    {
        free(thread);
        SDL_SetError("Out of memory");
        return nullptr;
    }

    return thread;
}

extern "C" SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name,
                                        void *data)
{
    return SDL_CreateThreadWithStackSize(fn, name, 0, data);
}

extern "C" void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (thread == nullptr)
        return;

    // Valid on every core, and on whichever core the thread being waited for
    // was placed: the wait yields to Circle's scheduler on core 0, hands the
    // core to the next cooperative context on a core that has them - which
    // may be the very thread being waited for - and spins anywhere else.
    //
    // It also says so when it does not end. This wait can only be ended by
    // the thread publishing THREAD_FINISHED, and nothing else in the system
    // can do it, so if that thread never finishes, this spins for the life of
    // the board in silence unless it reports. It reports once, after a few
    // seconds, rather than filling the console with it.
    const u64 started = CTimer::GetClockTicks64();
    bool reported = false;
    while (!(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) & THREAD_FINISHED))
    {
        if (!reported && CTimer::GetClockTicks64() - started >= 5000000)
        {
            reported = true;
            SDL2Circle_Log("sdl2thread", SDL2CIRCLE_LOG_ERROR,
                           "SDL_WaitThread(\"%s\") waiting 5s on core %u: "
                           "thread not finished",
                           thread->name[0] ? thread->name : "?",
                           SDL2Circle_ThisCore());
        }
        SDL2Circle_ThreadWaitSpin();
    }

    if (status != nullptr)
        *status = thread->status;

    free(thread);
}

extern "C" void SDL_DetachThread(SDL_Thread *thread)
{
    if (thread == nullptr)
        return;

    const int prev = __atomic_fetch_or(&thread->state, THREAD_DETACHED,
                                       __ATOMIC_ACQ_REL);
    if (prev & THREAD_FINISHED)
        free(thread);           // it ended first; this is the last exit
}

extern "C" SDL_threadID SDL_GetThreadID(SDL_Thread *thread)
{
    // A null argument asks about the calling thread, which is how a program
    // that never started one still gets a usable answer.
    if (thread == nullptr)
        return SDL_ThreadID();
    return thread->id;
}

extern "C" const char *SDL_GetThreadName(SDL_Thread *thread)
{
    if (thread == nullptr)
        return nullptr;
    return thread->name;
}

extern "C" int SDL_SetThreadPriority(SDL_ThreadPriority priority)
{
    (void)priority;

    // Circle's scheduler is round-robin with no priorities at all, so there
    // is nothing to set. Reporting failure is what tells an application that
    // its ordering will not be honoured, which claiming success would hide.
    return SDL_SetError("SDL_SetThreadPriority: Circle's scheduler is "
                        "round-robin and has no thread priorities");
}
