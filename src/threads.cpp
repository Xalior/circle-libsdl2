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
//                            helper thread wants — but a task registers itself
//                            with the scheduler while it is being built, so
//                            the BUILDING has to happen on the hardware core.
//                            A request from anywhere else is handed to the
//                            core-0 creator task and waited for.
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
//   A WAIT KEEPS THE AUDIO DEVICE FED. On the application core the audio
//   callback is run by whatever calls SDL_PumpEvents, which is the application
//   itself, so a wait that only spun would stop the sound for as long as it
//   lasted — and would deadlock outright an application whose callback is what
//   ends the wait. The spin loop runs the audio pump, which is as close to a
//   desktop's separate audio thread as this gets; SDL_LockAudioDevice is still
//   how an application keeps its callback out of a section.
//
//   SDL_CreateThread WORKS FROM ANY CORE, and so do SDL_WaitThread,
//   SDL_DetachThread and SDL_GetThreadID on what it returns. That matters
//   because an application that creates its main game thread through SDL — and
//   several do — runs on the application core by the time it gets there, which
//   is never core 0 under the split. What it needs is a CScheduler somewhere in
//   the system; without one there is nothing anywhere for a thread to run on
//   and the call fails, sets the error and says so on the log.
//
//   A thread that does start is cooperative AND IT RUNS ON CORE 0, whichever
//   core asked for it. It runs when something gives up that core, which the
//   servo's every lap, every wait in this file and SDL_Delay all do. A thread
//   that computes without ever calling into SDL or sleeping keeps the hardware
//   core to itself, and the hardware core is also where the device servicing
//   lives — so a long-running worker created this way is a decision about core
//   0's time, not free parallelism.
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

    // A WAIT MUST NOT STOP THE SOUND. The audio callback here is run by
    // whichever context calls SDL_PumpEvents, which on the application core is
    // the application's own loop — so a wait that only spins is a wait with the
    // device unfed. On a desktop that never happens: SDL gives the callback a
    // thread of its own and it keeps going while the application waits.
    //
    // Applications depend on that, and not only for the sound. A game whose
    // audio callback drives its music clock — an emulated sound chip, say —
    // waits at start-up for that clock to reach a mark, and it can only reach
    // it if the callback runs. Spinning alone, such a wait is a wait for
    // something the waiter is itself preventing, and it never ends.
    //
    // ONLY FOR THE CORE THAT ALREADY OWNS AUDIO PRODUCTION. This runs on every
    // core, and both places produced audio goes have exactly one writer, so a
    // wait must never make the waiting core a second producer — that does not
    // sound late, it sounds torn and out of order. The owner-only entry point
    // is where that rule lives; on any other core this costs a comparison.
    //
    // An application that must keep the callback out of a section already has
    // SDL's own answer for it, SDL_LockAudioDevice, which the pump obeys.
    SDL2Circle_AudioPumpIfOwner();

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

// THE HANDLE, AND WHO FREES IT. The thread runs on core 0 and whoever waits
// for it or detaches it may be on another core, so the two sides are really
// concurrent and the handle needs an owner at every instant.
//
// One word decides it. Each side sets its own bit — the thread when it ends,
// the application when it detaches — and reads back what the other side had
// already set. Whichever side finds the other's bit already there is the last
// one out and frees the handle. A side that does NOT find it has just handed
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

        const int status = pThread->fn(pThread->data);

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
    // what makes SDL_GetThreadID answer correctly from the very first call —
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

    // A thread is a Circle scheduler task, so the system needs a scheduler —
    // wherever the asking is done from. This is the one refusal left: without
    // one there is nothing anywhere for the thread to run on, and saying so is
    // better than a thread that silently never ran.
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

    // HOW BIG A THREAD NOBODY SIZED IS. SDL_CreateThread carries no stack
    // size, so what an application gets for one is entirely this
    // implementation's choice, and on every platform SDL normally runs on that
    // choice is megabytes. Circle's TASK_STACK_SIZE is 32 KB, which is the
    // right size for a Circle helper task and nowhere near the right size for
    // application code: the thread an application creates through SDL is
    // frequently its MAIN thread — the whole game, all its locals and every
    // library it calls — and a game's own core stack in this project had to be
    // measured in megabytes for exactly that reason. A megabyte is cheap on
    // these boards and an application that knows better says so through
    // SDL_CreateThreadWithStackSize, which is what that entry point is for.
    const size_t DefaultStack = 0x100000;

    size_t stack = stacksize != 0 ? stacksize : DefaultStack;
    if (stack < (size_t)TASK_STACK_SIZE)
        stack = (size_t)TASK_STACK_SIZE;
    stack = (stack + 15) & ~(size_t)15;

    // Constructing the task is what registers it with the scheduler, and the
    // scheduler is core 0's. On core 0 this builds it here and now; anywhere
    // else the core-0 creator task builds it and this returns once it has.
    // Either way the handle is complete before the call returns.
    SpawnRequest req{thread, (unsigned)stack, false};
    if (!SDL2Circle_ThreadCreateOn0(spawn_on0, &req))
    {
        free(thread);
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                       "SDL_CreateThread(\"%s\") from core %u: the core-0 "
                       "creator task is not running, so this kernel never "
                       "armed core 0", label, SDL2Circle_ThisCore());
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

    // Valid on every core. The thread being waited for runs on core 0, so on
    // core 0 this wait yields to it and elsewhere it spins while core 0 gets
    // on with it.
    while (!(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) & THREAD_FINISHED))
        SDL2Circle_ThreadWaitSpin();

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
