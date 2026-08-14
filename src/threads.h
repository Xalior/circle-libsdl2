//
// threads.h - the few things the threading, timer and event files share.
// Not installed; consumers see only <SDL2/*.h>.
//
#ifndef _sdl2_circle_threads_h
#define _sdl2_circle_threads_h

// Give up the calling core for a moment, from inside a blocking wait
// (src/threads.cpp). On the hardware core with Circle's scheduler running it
// is a scheduler yield, so the task that holds the thing being waited for can
// run; anywhere else it is the processor's own yield hint, because there is no
// scheduler on that core to hand the time to.
//
// Never call this while holding a spin lock: on the hardware core it can
// switch tasks, and the task switched to may want the same lock.
void SDL2Circle_ThreadWaitSpin(void);

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
