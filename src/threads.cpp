//
// threads.cpp — SDL's atomics, locks and threads on Circle
//
// WHAT BACKS WHAT, AND WHERE. Circle gives each core one line of execution,
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
//                            on any other core it spins, because there is no
//                            scheduler there to hand the time to and the core
//                            is dedicated to the application anyway.
//
//   Threads                  Circle scheduler tasks. A task is a cooperative
//                            thread and that is exactly what an application's
//                            helper thread wants — but the scheduler runs on
//                            the hardware core only, so a thread can only be
//                            started from there.
//
// WHAT AN APPLICATION CAN RELY ON.
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
//   SDL_CreateThread SUCCEEDS ONLY ON THE HARDWARE CORE, and only where a
//   CScheduler exists. Off it — which is where the application runs when the
//   host kernel has armed the core split — it fails, sets the error and says
//   so on the log, because Circle's scheduler may not be touched from another
//   core and a thread that silently never ran would be worse than one that
//   never started. An application that needs helper threads either runs on the
//   hardware core, or does the work in its own main loop.
//
//   A thread that does start is cooperative. It runs when something gives up
//   the core, which the application's per-frame pump and SDL_Delay both do. A
//   thread that computes without ever calling into SDL or sleeping keeps the
//   hardware core to itself, and the hardware core is also where the device
//   servicing lives.
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
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0)
        asm volatile("yield" ::: "memory");
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
    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
    {
        SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_YIELD);
        CScheduler::Get()->Yield();
        return;
    }

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
// same time, and there are two kinds of those. On the hardware core, one per
// scheduler task, so the task object's own address answers. On any other core
// there is exactly one line of execution, so the core number answers — offset
// by one so that no identity is zero, which is what a free mutex holds.
//
// The two cannot collide: Circle's heap starts far above the highest core
// number a board has.

extern "C" SDL_threadID SDL_ThreadID(void)
{
    if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
    {
        CTask *pTask = CScheduler::Get()->GetCurrentTask();
        if (pTask != nullptr)
            return (SDL_threadID)(uintptr_t)pTask;
    }

    return (SDL_threadID)(SDL2Circle_ThisCore() + 1);
}

// ---------------------------------------------------------------------------
// Mutexes
// ---------------------------------------------------------------------------
//
// SDL's mutex is recursive: the same thread may lock it repeatedly and must
// unlock it as many times. Circle's own CMutex is recursive too, but it is
// built on the scheduler's blocking primitives and so exists on the hardware
// core alone. This one is the same shape without that restriction — the owner
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
// a token. That is what keeps a signal from being lost — the token is issued
// whether or not the waiter has reached its loop yet — and what keeps a
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

struct SDL_Thread
{
    SDL_SpinLock       lock;        // guards finished and detached
    SDL_ThreadFunction fn;
    void              *data;
    int                status;
    int                finished;
    int                detached;
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

        const int status = pThread->fn(pThread->data);

        SDL2Circle_TLSRelease((unsigned long)pThread->id);

        SDL_AtomicLock(&pThread->lock);
        pThread->status   = status;
        pThread->finished = 1;
        const int detached = pThread->detached;
        SDL_AtomicUnlock(&pThread->lock);

        // Whoever arrives last frees it: a detached thread has nobody left to
        // wait for it, so ending is the last event in its life.
        if (detached)
            free(pThread);
    }

private:
    SDL_Thread *m_pThread;
};

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

    // A thread is a Circle scheduler task, and the scheduler belongs to the
    // hardware core. Saying so is the whole of the honest answer: creating a
    // task from another core would corrupt the scheduler's own state, and
    // running the function inline instead would run it before the caller
    // rather than beside it, which is a different program.
    if (SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "SDL_CreateThread(\"%s\") refused on core %u: threads "
                       "are scheduler tasks and the scheduler runs on core 0",
                       label, SDL2Circle_ThisCore());
        SDL_SetError("SDL_CreateThread: a thread is a Circle scheduler task "
                     "and the scheduler runs on the hardware core only; "
                     "\"%s\" cannot be started from core %u",
                     label, SDL2Circle_ThisCore());
        return nullptr;
    }

    if (!CScheduler::IsActive())
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "SDL_CreateThread(\"%s\") refused: no CScheduler in "
                       "this system", label);
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

    size_t stack = stacksize != 0 ? stacksize : (size_t)TASK_STACK_SIZE;
    if (stack < (size_t)TASK_STACK_SIZE)
        stack = (size_t)TASK_STACK_SIZE;
    stack = (stack + 15) & ~(size_t)15;

    CSDLThreadTask *task = new CSDLThreadTask(thread, (unsigned)stack);
    if (task == nullptr)
    {
        free(thread);
        SDL_SetError("Out of memory");
        return nullptr;
    }

    // The task's identity is its object address, which is what SDL_ThreadID
    // will report from inside it. Setting it before the task is released is
    // what makes SDL_GetThreadID answer correctly from the very first call.
    thread->id = (SDL_threadID)(uintptr_t)task;
    task->Start();

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

    for (;;)
    {
        SDL_AtomicLock(&thread->lock);
        const int finished = thread->finished;
        SDL_AtomicUnlock(&thread->lock);
        if (finished)
            break;

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

    SDL_AtomicLock(&thread->lock);
    const int finished = thread->finished;
    thread->detached = 1;
    SDL_AtomicUnlock(&thread->lock);

    if (finished)
        free(thread);
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
