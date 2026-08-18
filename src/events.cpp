//
// events.cpp - the event queue, and SDL_PumpEvents, the shim's per-frame
// heartbeat.
//
// One queue, on the core that reads it. The application's queue is an array
// held wherever the application runs. Under the core split the producers are
// on the hardware core - the USB input pump, window events from marshalled
// calls - and they publish into the cross-core ring instead; the application
// core's pump drains that ring into this queue. So the queue itself is only
// ever touched by the side that reads it, and the spin lock around it is
// there for the single-core case, where a Circle task may push while the
// application is looking.
//
// Filters and watchers run where the application is. They are application
// code, so they are called at the point an event enters the application's own
// queue: in SDL_PushEvent when the pushing side is the application's, and in
// the ring drain when the event came from the hardware core. That is the same
// promise SDL makes - the filter sees every event before the program does -
// without ever running the application's code on the wrong core.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "threads.h"
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <cstring>

namespace
{
constexpr unsigned QUEUE_SIZE = 256;

SDL_Event    s_queue[QUEUE_SIZE];
unsigned     s_head = 0;   // next slot to read
unsigned     s_count = 0;
SDL_SpinLock s_qlock;

// The event filter and the watchers share one lock, because SDL_SetEventFilter
// changes the queue as well and the two must not interleave.
constexpr unsigned MAX_WATCHERS = 8;

struct Watcher
{
    SDL_EventFilter callback;
    void           *userdata;
};

SDL_SpinLock    s_watchLock;
SDL_EventFilter s_filter = nullptr;
void           *s_filterData = nullptr;
Watcher         s_watchers[MAX_WATCHERS];
unsigned        s_watcherCount = 0;

// The next event type SDL_RegisterEvents will hand out.
Uint32 s_nextUserEvent = SDL_USEREVENT;

// Queue helpers. Every one of them expects s_qlock to be held.

SDL_Event &At(unsigned index)
{
    return s_queue[(s_head + index) % QUEUE_SIZE];
}

bool Append(const SDL_Event *event)
{
    if (s_count >= QUEUE_SIZE)
        return false;
    At(s_count) = *event;
    s_count++;
    return true;
}

void RemoveAt(unsigned index)
{
    if (index == 0)
    {
        s_head = (s_head + 1) % QUEUE_SIZE;
        s_count--;
        return;
    }
    for (unsigned i = index + 1; i < s_count; i++)
        At(i - 1) = At(i);
    s_count--;
}

unsigned QueueCount(void)
{
    SDL_AtomicLock(&s_qlock);
    unsigned count = s_count;
    SDL_AtomicUnlock(&s_qlock);
    return count;
}

// Offer an event to the filter and the watchers, then queue it. Returns 1 if
// it was queued, 0 if the filter refused it, -1 if the queue is full.
//
// The callbacks are read out under the lock and called outside it, so a
// filter that pushes an event of its own does not meet its own lock.
int Deliver(SDL_Event *event)
{
    SDL_AtomicLock(&s_watchLock);
    SDL_EventFilter filter     = s_filter;
    void           *filterData = s_filterData;
    Watcher         watchers[MAX_WATCHERS];
    unsigned        watcherCount = s_watcherCount;
    memcpy(watchers, s_watchers, watcherCount * sizeof(Watcher));
    SDL_AtomicUnlock(&s_watchLock);

    if (filter != nullptr && !filter(filterData, event))
        return 0;

    for (unsigned i = 0; i < watcherCount; i++)
        watchers[i].callback(watchers[i].userdata, event);

    SDL_AtomicLock(&s_qlock);
    bool queued = Append(event);
    SDL_AtomicUnlock(&s_qlock);

    return queued ? 1 : -1;
}

}   // namespace

extern "C" int SDL_PushEvent(SDL_Event *event)
{
    if (event == nullptr)
        return SDL_SetError("Passed a NULL event");

    event->common.timestamp = SDL_GetTicks();

    // Core split: producers on core 0 (USB input pump, window events from
    // proxied calls) publish through the cross-core ring; the application core's
    // pump drains it into this local queue, and runs the filter and the
    // watchers there - they are the application's own code and belong on its
    // core.
    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() == 0)
        return SDL2Circle_EventRingPush(event)
                   ? 1 : SDL_SetError("event ring full");

    int result = Deliver(event);
    if (result < 0)
        return SDL_SetError("event queue full");
    return result;   // 1 queued, 0 refused by the filter
}

extern "C" void SDL_PumpEvents(void)
{
    // Callback timers have no thread to run on, so this is one of the two
    // points they are serviced at, and it is first so that an event a
    // callback pushes is seen by the poll that led here rather than the next
    // one. See src/timer.cpp.
    SDL2Circle_TimerService();

    // Core split: the application core's pump touches nothing but shared memory.
    // Everything the single-core pump did on the platform's behalf (USB
    // PnP, scheduler yield, throttle tick, deadman) belongs to the core-0
    // servo and watchdog now; here the pump drains the event ring, mirrors
    // key state, runs the audio callback into its ring, and beats the
    // heartbeat the watchdog listens to.
    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_HeartbeatBump();

        SDL_Event ev;
        while (QueueCount() < QUEUE_SIZE && SDL2Circle_EventRingPop(&ev))
        {
            SDL2Circle_ApplyEventState(&ev);
            Deliver(&ev);
        }

        SDL2Circle_AudioPump();

        // The same liveness beacon the single-core pump prints below. It
        // used to be unreachable here, because printing meant touching the
        // console and this is not the core that owns it; the log ring
        // removes that objection. The deadman itself stays on core 0 - the
        // watchdog task is the split's version of it, and it can see a
        // wedged application core that an in-band timer cannot.
        {
            static u64 lastBeat = 0;
            u64 now = CTimer::GetClockTicks64();
            if (now - lastBeat > 10000000)
            {
                lastBeat = now;
                if (SDL2Circle_DebugUartArmed())
                    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_DEBUG, "pump alive t=%us",
                                   (unsigned)(now / 1000000));
            }
        }
        return;
    }

    // Everything below this line is core 0's: the scheduler, the kernel
    // timer, the USB host controller, the serial port, the sound device and
    // the CPU throttle's firmware mailbox. There is exactly one other way to
    // arrive here off core 0 - a pinned thread on a core a host kernel lent
    // (SDL2Circle_ThreadCoreOffer) with the split never activated - and it
    // has to stop here rather than reach any of them. Such a caller has no
    // pumping to do anyway: the timers above are serviced, and the devices
    // belong to the core that owns them.
    if (SDL2Circle_ThisCore() != 0)
        return;

    // The shim's cooperative heartbeat: called every frame by any SDL app
    // (via SDL_PollEvent), it services USB plug-and-play, translates HID
    // reports, and yields so cooperative std::threads make progress.
    if (CScheduler::IsActive())
    {
        SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_YIELD);
        CScheduler::Get()->Yield();
    }

    // Liveness beacon + deadman. The beacon is a debug line every 10 s
    // proving the application's main loop is still pumping, and it is printed
    // only where --rapi-debug-uart asked for it: it says the machine is well,
    // on a timer, for as long as the machine is up. The deadman below is not
    // conditional - it is what reports a main loop that stopped. A kernel timer re-armed on every beat
    // fires from IRQ context if the pump goes silent for 30 s and dumps
    // the scheduler's task list - the wedged system's own post-mortem.
    {
        static u64 lastBeat = 0;
        static TKernelTimerHandle deadman = 0;
        u64 now = CTimer::GetClockTicks64();
        if (now - lastBeat > 10000000)
        {
            lastBeat = now;
            if (SDL2Circle_DebugUartArmed())
                SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_DEBUG, "pump alive t=%us",
                                      (unsigned)(now / 1000000));

            if (deadman != 0)
                CTimer::Get()->CancelKernelTimer(deadman);
            deadman = CTimer::Get()->StartKernelTimer(
                30 * HZ,
                [](TKernelTimerHandle, void *, void *) {
                    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_ERROR,
                                          "pump stalled 30s, task dump:");
                    if (CScheduler::IsActive())
                        CScheduler::Get()->ListTasks(CLogger::Get()->GetTarget());
                });
        }
    }

    // The CPU clock and the case fan. This library owns them; this is the
    // heartbeat that drives them when the application runs on core 0. Under
    // the core split it is the servo instead, because this tail never runs
    // there - the application core's pump returns above.
    SDL2Circle_HardwareTick();

    {
        SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_INPUT);
        SDL2Circle_InputPump();
        SDL2Circle_InjectPump();   // inert unless --rapi-debug-uart armed it
    }
    {
        SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_AUDIO);
        SDL2Circle_AudioPump();
    }

    SDL2Circle_PerfTick();
}

// ---------------------------------------------------------------------------
// Reading the queue
// ---------------------------------------------------------------------------

extern "C" int SDL_PeepEvents(SDL_Event *events, int numevents,
                              SDL_eventaction action,
                              Uint32 minType, Uint32 maxType)
{
    if (action == SDL_ADDEVENT)
    {
        if (events == nullptr)
            return SDL_SetError("Passed a NULL event array");

        // The type range does not filter an add - SDL ignores it here - and
        // the events go on the back of the queue exactly as SDL_PushEvent
        // would put them there, by the same route, so the split's producers
        // still publish through the ring.
        int added = 0;
        for (int i = 0; i < numevents; i++)
        {
            if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() == 0)
            {
                if (!SDL2Circle_EventRingPush(&events[i]))
                    break;
            }
            else
            {
                SDL_AtomicLock(&s_qlock);
                bool queued = Append(&events[i]);
                SDL_AtomicUnlock(&s_qlock);
                if (!queued)
                    break;
            }
            added++;
        }

        if (added < numevents)
            SDL_SetError("event queue full");
        return added;
    }

    if (action != SDL_PEEKEVENT && action != SDL_GETEVENT)
        return SDL_SetError("Unknown event action");

    // A null array asks how many events match rather than for the events
    // themselves, and takes nothing off the queue - which is what
    // SDL_HasEvents is.
    int used = 0;

    SDL_AtomicLock(&s_qlock);
    unsigned i = 0;
    while (i < s_count && (events == nullptr || used < numevents))
    {
        Uint32 type = At(i).type;
        if (type < minType || type > maxType)
        {
            i++;
            continue;
        }

        if (events != nullptr)
        {
            events[used] = At(i);
            if (action == SDL_GETEVENT)
            {
                RemoveAt(i);        // the next event shifts into this index
                used++;
                continue;
            }
        }
        used++;
        i++;
    }
    SDL_AtomicUnlock(&s_qlock);

    return used;
}

extern "C" int SDL_PollEvent(SDL_Event *event)
{
    SDL_PumpEvents();
    return SDL_PeepEvents(event, 1, SDL_GETEVENT,
                          SDL_FIRSTEVENT, SDL_LASTEVENT) > 0 ? 1 : 0;
}

extern "C" int SDL_WaitEventTimeout(SDL_Event *event, int timeout)
{
    // Pumping is what keeps the machine alive: on the hardware core it is the
    // USB service and the scheduler yield, and on the application core it is
    // the ring drain. A wait that did not pump would be a wait for something
    // that could never arrive.
    const Uint64 expiry = timeout > 0 ? SDL_GetTicks64() + (Uint64)timeout : 0;

    for (;;)
    {
        SDL_PumpEvents();

        if (SDL_PeepEvents(event, 1, SDL_GETEVENT,
                           SDL_FIRSTEVENT, SDL_LASTEVENT) > 0)
            return 1;

        if (timeout == 0)
            return 0;
        if (timeout > 0 && SDL_GetTicks64() >= expiry)
            return 0;

        SDL_Delay(1);
    }
}

extern "C" int SDL_WaitEvent(SDL_Event *event)
{
    return SDL_WaitEventTimeout(event, -1);
}

extern "C" SDL_bool SDL_HasEvents(Uint32 minType, Uint32 maxType)
{
    SDL_bool found = SDL_FALSE;

    SDL_AtomicLock(&s_qlock);
    for (unsigned i = 0; i < s_count; i++)
    {
        Uint32 type = At(i).type;
        if (type >= minType && type <= maxType)
        {
            found = SDL_TRUE;
            break;
        }
    }
    SDL_AtomicUnlock(&s_qlock);

    return found;
}

extern "C" SDL_bool SDL_HasEvent(Uint32 type)
{
    return SDL_HasEvents(type, type);
}

extern "C" void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    SDL_AtomicLock(&s_qlock);
    unsigned i = 0;
    while (i < s_count)
    {
        Uint32 type = At(i).type;
        if (type >= minType && type <= maxType)
            RemoveAt(i);
        else
            i++;
    }
    SDL_AtomicUnlock(&s_qlock);
}

extern "C" void SDL_FlushEvent(Uint32 type)
{
    SDL_FlushEvents(type, type);
}

// ---------------------------------------------------------------------------
// Filters and watchers
// ---------------------------------------------------------------------------

extern "C" void SDL_SetEventFilter(SDL_EventFilter filter, void *userdata)
{
    SDL_AtomicLock(&s_watchLock);
    s_filter     = filter;
    s_filterData = userdata;
    SDL_AtomicUnlock(&s_watchLock);

    // SDL discards whatever was already queued when a filter is installed,
    // because those events never passed through it.
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

extern "C" SDL_bool SDL_GetEventFilter(SDL_EventFilter *filter, void **userdata)
{
    SDL_AtomicLock(&s_watchLock);
    SDL_EventFilter current     = s_filter;
    void           *currentData = s_filterData;
    SDL_AtomicUnlock(&s_watchLock);

    if (current == nullptr)
        return SDL_FALSE;

    if (filter != nullptr)
        *filter = current;
    if (userdata != nullptr)
        *userdata = currentData;
    return SDL_TRUE;
}

extern "C" void SDL_AddEventWatch(SDL_EventFilter filter, void *userdata)
{
    if (filter == nullptr)
        return;

    bool room;

    SDL_AtomicLock(&s_watchLock);
    room = s_watcherCount < MAX_WATCHERS;
    if (room)
    {
        s_watchers[s_watcherCount].callback = filter;
        s_watchers[s_watcherCount].userdata = userdata;
        s_watcherCount++;
    }
    SDL_AtomicUnlock(&s_watchLock);

    // SDL_AddEventWatch reports nothing, so a watch that was not installed
    // would silently never fire. Say it on the log instead.
    if (!room)
        SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_ERROR,
                       "SDL_AddEventWatch: %u watch slots all in use: watch "
                       "not installed", MAX_WATCHERS);
}

extern "C" void SDL_DelEventWatch(SDL_EventFilter filter, void *userdata)
{
    SDL_AtomicLock(&s_watchLock);
    for (unsigned i = 0; i < s_watcherCount; i++)
    {
        if (s_watchers[i].callback != filter || s_watchers[i].userdata != userdata)
            continue;

        for (unsigned j = i + 1; j < s_watcherCount; j++)
            s_watchers[j - 1] = s_watchers[j];
        s_watcherCount--;
        break;
    }
    SDL_AtomicUnlock(&s_watchLock);
}

extern "C" void SDL_FilterEvents(SDL_EventFilter filter, void *userdata)
{
    if (filter == nullptr)
        return;

    // The queue lock is held while the filter runs, which is what SDL does
    // with its own queue lock. A filter passed here must therefore not call
    // back into the event queue.
    SDL_AtomicLock(&s_qlock);
    unsigned i = 0;
    while (i < s_count)
    {
        if (filter(userdata, &At(i)))
            i++;
        else
            RemoveAt(i);
    }
    SDL_AtomicUnlock(&s_qlock);
}

extern "C" Uint32 SDL_RegisterEvents(int numevents)
{
    if (numevents < 0
        || (Uint64)s_nextUserEvent + (Uint64)numevents > (Uint64)SDL_LASTEVENT)
        return (Uint32)-1;

    Uint32 base = s_nextUserEvent;
    s_nextUserEvent += (Uint32)numevents;
    return base;
}

extern "C" Uint8 SDL_EventState(Uint32, int state)
{
    return (state == SDL_QUERY) ? SDL_ENABLE : (Uint8)state;
}
