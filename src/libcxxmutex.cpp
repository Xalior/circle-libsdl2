//
// libcxxmutex.cpp — libc++'s mutex primitives, for the cores this library
// actually puts applications on.
//
// WHY THIS FILE EXISTS, and the shape of the fault is worth having in front
// of you before the code:
//
// std::recursive_mutex could not be used on ANY core except core 0. Not
// merely across cores — taken and released microseconds apart on the same
// core, it asserted and halted the board.
//
// circle-stdlib backs the two standard mutex types differently. std::mutex
// gets a semaphore-based non-recursive implementation; std::recursive_mutex
// is Circle's CMutex directly. CMutex is a SCHEDULER object: Acquire records
// the task the scheduler is currently running, and Release asserts that the
// same task is still current. There is one scheduler and it belongs to core
// 0, so a caller on any other core is stamped with whatever task core 0
// happened to be running at that instant — and this library's own servo
// yields on every pass, so that answer changes continuously. Acquire records
// one task, Release compares against another, and Circle asserts.
//
// Two more, on the success path and silent: Acquire under contention waits
// on a scheduler synchronisation object from a core that has no scheduler,
// and Release calls Yield() on CORE 0's scheduler from another core.
//
// THE REASON IT HID FOR SO LONG IS WORTH KEEPING. It is a property of WHICH
// MUTEX TYPE the code reaches for, not of what the code does. std::mutex
// works everywhere, so nearly all locking is fine, and only the code that
// happens to want recursion falls over — one game through its audio manager,
// another through a logging library that takes the lock on every line. The
// two have nothing in common but the type, which is why it looked like an
// application problem twice over and was not.
//
// It is the library's to fix because the library is what puts applications
// on a core without a scheduler. A framework that owns the core split owes
// the code it hosts a C++ runtime that works on the cores it chose.
//
// HOW THE OVERRIDE WORKS, since it is unusual. All nine of these symbols
// live in ONE object inside liblibcxx-threading.a. Defining any of them here
// means defining all of them: leave one out and the linker still pulls that
// object in to resolve it, and every other symbol then collides. Consumers
// link this archive whole, so these definitions are always present and the
// vendored object is never pulled. Nothing vendored is edited.
//
// THE NON-RECURSIVE FOUR ARE DELIBERATELY UNCHANGED. They delegate to
// circle-stdlib's own NonRecursiveMutexImpl, byte for byte, for two reasons:
// they already work on every core, and condvar.cpp — which stays, being a
// different object — reinterprets __libcpp_mutex_t's storage AS a
// NonRecursiveMutexImpl and drives it directly. Changing that layout here
// would corrupt every condition variable in the system. Only the recursive
// five change, because only they were broken.
//
#include <__external_threading>

// circle-stdlib's own definition, included rather than copied so the two
// cannot drift: condvar.cpp casts the same storage to this type, and a
// second declaration here that fell out of step would be undetectable.
// The include path is set in this library's makefile.
#include "mutex_impl.h"

#include <SDL2/SDL.h>
#include "threads.h"

#include <atomic>
#include <cstring>

_LIBCPP_BEGIN_NAMESPACE_STD

// ---------------------------------------------------------------------------
// std::mutex — unchanged, and delegating to the implementation condvar.cpp
// shares. See the note above: this is here to displace the object, not to
// alter the behaviour.
// ---------------------------------------------------------------------------

int __libcpp_mutex_lock(__libcpp_mutex_t *__m)
{
    as_nonrecursive_mutex(__m)->Acquire();
    return 0;
}

bool __libcpp_mutex_trylock(__libcpp_mutex_t *__m)
{
    return as_nonrecursive_mutex(__m)->TryAcquire();
}

int __libcpp_mutex_unlock(__libcpp_mutex_t *__m)
{
    as_nonrecursive_mutex(__m)->Release();
    return 0;
}

int __libcpp_mutex_destroy(__libcpp_mutex_t *__m)
{
    as_nonrecursive_mutex(__m)->~NonRecursiveMutexImpl();
    return 0;
}

// ---------------------------------------------------------------------------
// std::recursive_mutex — owned by an IDENTITY rather than by a scheduler task
// ---------------------------------------------------------------------------

namespace
{

// Ownership is recorded as SDL_ThreadID's answer, and that is the whole of
// the fix. It is unique among everything that can hold a lock at one time —
// the task object's address on the hardware core, the core number anywhere
// else — and, unlike "whichever task the scheduler is running", it does not
// change under a holder that is simply getting on with its work.
//
// Zero-initialised storage is a free mutex, which matters because libc++
// may hand over memory it has only zeroed.
struct RecursiveMutexImpl
{
    std::atomic<unsigned long long> m_owner;   // 0 when free
    unsigned                        m_count;   // recursion depth

    static unsigned long long Identity(void)
    {
        // Never zero: SDL_ThreadID answers the core number plus one off the
        // hardware core, and a heap address on it.
        return (unsigned long long)SDL_ThreadID();
    }

    void Acquire(void)
    {
        const unsigned long long me = Identity();
        if (m_owner.load(std::memory_order_acquire) == me)
        {
            m_count++;      // already ours: recursion, no contention possible
            return;
        }
        unsigned long long expected = 0;
        while (!m_owner.compare_exchange_weak(expected, me,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
        {
            expected = 0;
            // Yields to the scheduler on the hardware core and spins
            // elsewhere, which is the one wait in this library that is
            // correct on every core.
            SDL2Circle_ThreadWaitSpin();
        }
        m_count = 1;
    }

    bool TryAcquire(void)
    {
        const unsigned long long me = Identity();
        if (m_owner.load(std::memory_order_acquire) == me)
        {
            m_count++;
            return true;
        }
        unsigned long long expected = 0;
        if (!m_owner.compare_exchange_strong(expected, me,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed))
            return false;
        m_count = 1;
        return true;
    }

    void Release(void)
    {
        // Unlocking a mutex this line of execution does not hold is
        // undefined in the standard. It is not diagnosed here: the caller
        // that would be told is the one already in the wrong, and stopping
        // the board to say so is what the fault this file replaces did.
        if (m_count > 0 && --m_count == 0)
            m_owner.store(0, std::memory_order_release);
    }
};

static_assert(sizeof(RecursiveMutexImpl) <= sizeof(__libcpp_recursive_mutex_t::__storage),
              "RecursiveMutexImpl does not fit the storage libc++ provides");
static_assert(alignof(RecursiveMutexImpl) <= alignof(__libcpp_recursive_mutex_t),
              "RecursiveMutexImpl is more aligned than libc++'s storage");

RecursiveMutexImpl *as_recursive(__libcpp_recursive_mutex_t *__m)
{
    return reinterpret_cast<RecursiveMutexImpl *>(__m->__storage);
}

}   // namespace

int __libcpp_recursive_mutex_init(__libcpp_recursive_mutex_t *__m)
{
    memset(__m->__storage, 0, sizeof(__m->__storage));
    return 0;
}

int __libcpp_recursive_mutex_lock(__libcpp_recursive_mutex_t *__m)
{
    as_recursive(__m)->Acquire();
    return 0;
}

bool __libcpp_recursive_mutex_trylock(__libcpp_recursive_mutex_t *__m)
{
    return as_recursive(__m)->TryAcquire();
}

int __libcpp_recursive_mutex_unlock(__libcpp_recursive_mutex_t *__m)
{
    as_recursive(__m)->Release();
    return 0;
}

int __libcpp_recursive_mutex_destroy(__libcpp_recursive_mutex_t *)
{
    return 0;   // nothing owns anything: the storage is the whole object
}

_LIBCPP_END_NAMESPACE_STD
