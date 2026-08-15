//
// threads.h - the few things the threading, timer and event files share.
// Not installed; consumers see only <SDL2/*.h>.
//
#ifndef _sdl2_circle_threads_h
#define _sdl2_circle_threads_h

#include <stddef.h>

// Give up the calling core for a moment, from inside a blocking wait
// (src/threads.cpp). On the hardware core with Circle's scheduler running it
// is a scheduler yield, so the task that holds the thing being waited for can
// run; anywhere else it is the processor's own yield hint, because there is no
// scheduler on that core to hand the time to.
//
// Never call this while holding a spin lock: on the hardware core it can
// switch tasks, and the task switched to may want the same lock.
void SDL2Circle_ThreadWaitSpin(void);

// Hand the calling core to the next cooperative context on it, if its host
// asked for one (SDL2Circle_ThreadsStayOnThisCore) and there is another
// context to hand it to (src/libcxxthreading.cpp). False when either answer
// is no, which is every build that never made the call - so a caller falls
// back to whatever it did before.
//
// It touches nothing but that core's own run list: no device, no lock, no
// audio, nothing that could re-enter a section the caller is inside. That is
// what makes it safe to call from a raw spin lock, which the waits above are
// not.
bool SDL2Circle_ThreadScheduleNext(void);

// The identity of the cooperative context running on the calling core, or
// zero where that core has none (src/libcxxthreading.cpp). SDL_ThreadID is
// built on it, so that the two threading surfaces agree about who a caller
// is however that caller was started.
unsigned long long SDL2Circle_ThreadContextID(void);

// Whether the calling core schedules its own threads, because a host asked it
// to (SDL2Circle_ThreadsStayOnThisCore). Both threading surfaces ask this
// before they build anything, so that a core nobody asked about takes the
// path it always took with nothing allocated and nothing to undo.
bool SDL2Circle_ThreadSchedulesHere(void);

// Start a thread as a cooperative context on the calling core
// (src/libcxxthreading.cpp). Both threading surfaces start a thread through
// this one call: what a thread on this core IS is the same for both, and
// everything that differs between them - a handle, a refcount, a status word,
// when a joiner may free what - lives in the two functions passed in.
//
//   pBody     the thread's work, run once on its own stack. Its return ends
//             the thread.
//   pFinish   how this surface publishes that the thread has ended. Called
//             after the body, after the context is off the run list, and
//             immediately before the core is handed on. The instant it
//             returns, a joiner on any core may free the handle it published
//             against, so it must be the last thing that reads it.
//
// Answers with the thread's identity - what SDL_ThreadID reports inside it -
// or zero if the calling core has no scheduler or the heap could not carry
// another stack. The identity is settled before the context reaches the run
// list, so a caller can publish it in its own handle before the thread runs.
unsigned long long SDL2Circle_ThreadStartHere(void (*pBody)(void *),
                                              void (*pFinish)(void *),
                                              void *pArg, size_t nStackBytes);

// Say, once per core and only for a core that is not core 0, that a thread it
// made went to core 0 (src/libcxxthreading.cpp). Both surfaces call it at
// their first such creation. The default placement is otherwise silent, and
// that silence is the point: a port moved off core 0 on purpose, whose worker
// lands back among the devices, has nothing on the board to tell it so.
void SDL2Circle_ThreadAnnounceCore0(void);

// Say, once per core, that this core has a runnable thread it has not given a
// turn in some seconds (src/libcxxthreading.cpp). Nothing here is preemptive,
// so a main loop that never waits, yields or sleeps keeps its core and its own
// threads never start - which from outside is a working board doing nothing,
// with no fault reported anywhere.
//
// Driven from the application's per-frame beat rather than from the scheduler,
// because the fault IS that the scheduler is never entered. Costs two loads
// and a compare per call; the run list is walked only once the interval has
// gone by, and not at all on a core that schedules nothing.
void SDL2Circle_ThreadStallCheck(void);

// Release everything the ending thread put in thread-local storage: run each
// value's destructor and free the slot (src/threads.cpp). Called when a thread
// finishes and by SDL_TLSCleanup.
void SDL2Circle_TLSRelease(unsigned long thread);

// Run every SDL_AddTimer callback whose deadline has passed (src/timer.cpp).
//
// There is no timer thread on this platform, so the callbacks run at the two
// points an application is certain to reach: the per-frame pump and SDL_Delay.
// It refuses to re-enter, so a callback may call either of them.
void SDL2Circle_TimerService(void);

#endif
