//
// mouse.cpp - Circle's USB mouse behind the SDL mouse API.
//
// Circle binds a USB mouse and publishes it as the character device "mouse1".
// This file takes it in raw mode - no Setup(), no Circle-drawn cursor -
// because the pointer belongs to whoever is drawing the screen, which here is
// the application. What arrives is a stream of reports: how far the mouse
// moved since the last one, which buttons are down, and how far the wheel
// turned. Never where the pointer is; a mouse has no idea.
//
// So an absolute position exists only because something clamps the movement
// to a screen. That is what this file does, against the rectangle the video
// layer calls the pointer bounds - the application's window while one exists,
// and the declared canvas before that. SDL_GetMouseState's coordinates are
// that clamped position and nothing else.
//
// Which core does what is exactly as joystick.cpp. USB belongs to core 0, so
// attach, detach, report decoding and event synthesis all happen there, from
// SDL2Circle_MousePump. Everything an application asks afterwards - where is
// the pointer, what is held - is answered from the atomics below by whichever
// core asks, with no call to core 0 at all.
//
// Circle's report callback runs in interrupt context, and unlike a gamepad's
// a mouse report cannot be reduced to a snapshot: reports are queued whole
// and in order, never folded into a running displacement, because a button
// edge arriving with a movement is a different event from the same movement
// without it. Press-drag-release is exactly that ordering, and a game reads
// the position each button edge carries.
//
// Circle offers the report in two handler shapes and they order their
// arguments differently, so both fold onto one internal path rather than
// being decoded twice. The robot-hands mouse domain in input.cpp reaches that
// same path, which is why an injected report and a physical one are
// indistinguishable from here down.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "shim_internal.h"

#include <circle/devicenameservice.h>
#include <circle/input/mouse.h>
#include <circle/spinlock.h>
#include <atomic>
#include <cstring>

namespace
{

// ---------------------------------------------------------------------------
// The pointer: written by the core-0 pump, read from any core.
// ---------------------------------------------------------------------------

std::atomic<int>    s_x{0};
std::atomic<int>    s_y{0};
std::atomic<Uint32> s_buttons{0};       // SDL's mask, SDL_BUTTON_LMASK & co.

// SDL_GetRelativeMouseState is consuming: it returns the movement since the
// last call and resets the accumulator. The producer adds to these, the
// caller takes the whole value away.
std::atomic<int> s_relx{0};
std::atomic<int> s_rely{0};

std::atomic<bool> s_attached{false};

// SDL's relative mode: the pointer is hidden, held inside the window, and
// motion is reported as deltas. Nothing here draws a cursor to hide, and the
// position is clamped to the window in both modes, so what the flag actually
// changes is that entering it starts the delta accumulator from zero - the
// application asked for movement from now, not movement since whenever it
// last looked.
SDL_bool s_relative = SDL_FALSE;

// SDL_ShowCursor is a counter, not a boolean: every hide has to be matched by
// a show. A query (-1) must not disturb it.
int s_cursor_shown = 1;

// SDL hands applications an opaque SDL_Cursor*, and a null return means
// failure. There is no cursor to draw - the application owns every pixel on
// this screen - so every constructor hands back one shared non-null token:
// callers keep working, and nothing here dereferences it. SDL_FreeCursor must
// therefore not free it.
SDL_Cursor *const s_cursor = reinterpret_cast<SDL_Cursor *>(-1);

// The single window's ID, the same constant the keyboard path stamps on its
// events. There is one screen and one window, and a bare-metal application
// never has a second to give focus to.
const Uint32 WINDOW_ID = 1;

// SDL's mouse instance ID. SDL2 has one pointer whatever is plugged in, so
// there is one instance and it is 0.
//
// Circle does not merge mice: each one it binds gets its own device number,
// so a second mouse becomes "mouse2" and this file never looks at it. One
// pointer, driven by the first mouse to arrive.
const Uint32 MOUSE_ID = 0;

// ---------------------------------------------------------------------------
// The report queue
//
// Deep enough that a burst of reports never outruns one pass of the pump: a
// mouse reports at up to 1 kHz and a frame is 16 ms, so a stall of a quarter
// of a second is what it takes to fill this.
//
// Overflowing costs at most a click, never a stuck button: the queue's tail
// always carries the newest button state, and edges are found by comparing
// against it, so the pump's view of the buttons agrees with the hardware
// again at the end of every pass. Only a press and its release both arriving
// inside one overflow can vanish.
//
// The lock is taken at IRQ level because the producer is Circle's report
// callback, which runs in interrupt context on the core the pump runs on.
// ---------------------------------------------------------------------------

const unsigned QUEUE_SIZE = 256;

struct Report
{
    int      dx, dy;
    int      wheel;
    unsigned buttons;           // Circle's MOUSE_BUTTON_* mask
};

Report    s_queue[QUEUE_SIZE];
unsigned  s_qcount = 0;
CSpinLock s_qlock(IRQ_LEVEL);

// Two hands can be on this mouse: the physical device, and the robot-hands
// macro channel in input.cpp. Each owns its own button word and every report
// carries the union of them, so a script pressing a button cannot release one
// the operator is holding, and a script releasing its own cannot release the
// operator's. Both are only ever touched under the queue lock.
unsigned s_physButtons = 0;
unsigned s_injButtons  = 0;

// Core 0 only.
CMouseDevice *s_mouse = nullptr;

// Set from the device-removed callback, drained by the pump, so the SDL work
// never happens inside a destructor.
std::atomic<bool> s_removed{false};

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

// Circle's button mask is the USB boot protocol's bit order; SDL numbers its
// buttons from one and masks them with SDL_BUTTON(n).
Uint32 SdlButtonMask(unsigned circle)
{
    Uint32 mask = 0;
    if (circle & MOUSE_BUTTON_LEFT)   mask |= SDL_BUTTON_LMASK;
    if (circle & MOUSE_BUTTON_RIGHT)  mask |= SDL_BUTTON_RMASK;
    if (circle & MOUSE_BUTTON_MIDDLE) mask |= SDL_BUTTON_MMASK;
    if (circle & MOUSE_BUTTON_SIDE1)  mask |= SDL_BUTTON_X1MASK;
    if (circle & MOUSE_BUTTON_SIDE2)  mask |= SDL_BUTTON_X2MASK;
    return mask;
}

int ClampTo(int value, int limit)
{
    if (limit <= 0 || value < 0)
        return 0;
    return value > limit - 1 ? limit - 1 : value;
}

void PushEvent(SDL_Event &ev)
{
    ev.common.timestamp = SDL_GetTicks();
    SDL_PushEvent(&ev);
}

// ---------------------------------------------------------------------------
// Attach / detach (core 0)
// ---------------------------------------------------------------------------

void MouseStatusHandler(unsigned nButtons, int nDisplacementX,
                        int nDisplacementY, int nWheelMove, void *pArg)
{
    (void) pArg;
    SDL2Circle_MouseReport(nDisplacementX, nDisplacementY, nButtons, nWheelMove);
}

void MouseRemovedHandler(CDevice *, void *)
{
    // The report the hardware can no longer send. A mouse pulled out with a
    // button held would otherwise never produce the release edge, and a drag
    // begun with it would run until the machine was rebooted - so the removal
    // posts an all-buttons-up report down the ordinary path and lets the pump
    // find the edge as usual.
    SDL2Circle_MouseReport(0, 0, 0, 0);
    s_removed.store(true, std::memory_order_release);
}

void AttachMouse(CMouseDevice *pMouse)
{
    s_mouse = pMouse;

    pMouse->RegisterRemovedHandler(MouseRemovedHandler, nullptr);

    // Raw mode: no Setup(), no Circle cursor. The pointer is drawn by whoever
    // is drawing the screen, from relative movement.
    pMouse->RegisterStatusHandler(MouseStatusHandler, nullptr);

    // A pointer that has never been placed starts in the middle of the
    // screen, which is where a desktop puts one and where a game that grabs
    // it expects to find it.
    int w = 0, h = 0;
    SDL2Circle_PointerBounds(&w, &h);
    s_x.store(ClampTo(w / 2, w), std::memory_order_relaxed);
    s_y.store(ClampTo(h / 2, h), std::memory_order_relaxed);

    s_attached.store(true, std::memory_order_release);

    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_NOTICE,
                   "mouse1 attached: %u buttons%s",
                   pMouse->GetButtonCount(),
                   pMouse->HasWheel() ? ", wheel" : "");
}

void DetachMouse(void)
{
    s_mouse = nullptr;
    s_attached.store(false, std::memory_order_release);

    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_NOTICE, "mouse1 detached");
}

// ---------------------------------------------------------------------------
// One report, turned into events (core 0)
// ---------------------------------------------------------------------------

void PumpReport(const Report &r)
{
    int w = 0, h = 0;
    SDL2Circle_PointerBounds(&w, &h);

    int x = s_x.load(std::memory_order_relaxed);
    int y = s_y.load(std::memory_order_relaxed);

    if (r.dx || r.dy)
    {
        x = ClampTo(x + r.dx, w);
        y = ClampTo(y + r.dy, h);
        s_x.store(x, std::memory_order_release);
        s_y.store(y, std::memory_order_release);

        // The relative reading is the movement the mouse reported, not the
        // movement the clamp allowed: an application in relative mode is
        // steering with the device, and a pointer parked against the edge of
        // the screen must not silently stop feeding it.
        s_relx.fetch_add(r.dx, std::memory_order_relaxed);
        s_rely.fetch_add(r.dy, std::memory_order_relaxed);
    }

    Uint32 buttons = SdlButtonMask(r.buttons);
    Uint32 before  = s_buttons.load(std::memory_order_relaxed);
    s_buttons.store(buttons, std::memory_order_release);

    if (r.dx || r.dy)
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type            = SDL_MOUSEMOTION;
        ev.motion.windowID = WINDOW_ID;
        ev.motion.which    = MOUSE_ID;
        ev.motion.state    = buttons;
        ev.motion.x        = x;
        ev.motion.y        = y;
        ev.motion.xrel     = r.dx;
        ev.motion.yrel     = r.dy;
        PushEvent(ev);
    }

    // Button edges after the movement in the same report, so a press is
    // decided with the pointer where that report put it.
    Uint32 diff = buttons ^ before;
    for (int i = 0; diff && i < 5; i++)
    {
        Uint32 bit = SDL_BUTTON(i + 1);
        if (!(diff & bit))
            continue;
        diff &= ~bit;

        bool down = (buttons & bit) != 0;

        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type            = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.windowID = WINDOW_ID;
        ev.button.which    = MOUSE_ID;
        ev.button.button   = (Uint8)(i + 1);
        ev.button.state    = down ? SDL_PRESSED : SDL_RELEASED;
        ev.button.clicks   = 1;
        ev.button.x        = x;
        ev.button.y        = y;
        PushEvent(ev);
    }

    if (r.wheel)
    {
        // USB and SDL agree on the sign: positive is away from the user.
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type             = SDL_MOUSEWHEEL;
        ev.wheel.windowID   = WINDOW_ID;
        ev.wheel.which      = MOUSE_ID;
        ev.wheel.x          = 0;
        ev.wheel.y          = r.wheel;
        ev.wheel.direction  = SDL_MOUSEWHEEL_NORMAL;
        ev.wheel.preciseX   = 0.0f;
        ev.wheel.preciseY   = (float)r.wheel;
        ev.wheel.mouseX     = x;
        ev.wheel.mouseY     = y;
        PushEvent(ev);
    }
}

// Queue one report, having first recorded the reporting hand's buttons.
// Safe from interrupt context; it only queues.
void QueueReport(int dx, int dy, int wheel, unsigned *pHand, unsigned buttons)
{
    s_qlock.Acquire();

    *pHand = buttons;
    unsigned all = s_physButtons | s_injButtons;

    if (s_qcount < QUEUE_SIZE)
    {
        Report *r = &s_queue[s_qcount++];
        r->dx      = dx;
        r->dy      = dy;
        r->wheel   = wheel;
        r->buttons = all;
    }
    else
    {
        // A queue this deep only fills if the pump has stopped draining it.
        // Fold the displacement into the newest report and let the newest
        // button state stand: displacement is additive, so no motion is lost,
        // and the tail always carrying the latest button state is what
        // guarantees the pump's view of the buttons is right again by the end
        // of the pass.
        Report *r = &s_queue[QUEUE_SIZE - 1];
        r->dx      += dx;
        r->dy      += dy;
        r->wheel   += wheel;
        r->buttons  = all;
    }

    s_qlock.Release();
}

} // namespace

// ---------------------------------------------------------------------------
// Producer entry points (core 0)
// ---------------------------------------------------------------------------

// The physical mouse's reports.
void SDL2Circle_MouseReport(int dx, int dy, unsigned buttons, int wheel)
{
    QueueReport(dx, dy, wheel, &s_physButtons, buttons);
}

// The robot-hands macro channel's reports (src/input.cpp), on the same terms.
void SDL2Circle_MouseInject(int dx, int dy, unsigned buttons, int wheel)
{
    QueueReport(dx, dy, wheel, &s_injButtons, buttons);
}

void SDL2Circle_MousePump(bool bPlugAndPlayChanged)
{
    if (s_removed.exchange(false, std::memory_order_acq_rel))
        DetachMouse();

    // Circle names the mouse the moment it has configured it, and one can turn
    // up at any time. UpdatePlugAndPlay saying "something changed" is the only
    // attach signal there is.
    if (!s_mouse && bPlugAndPlayChanged)
    {
        CDevice *pDevice = CDeviceNameService::Get()->GetDevice("mouse", 1, FALSE);
        if (pDevice)
            AttachMouse((CMouseDevice *)pDevice);
    }

    // Take the whole queue in one go, then translate outside the lock: pushing
    // events can be a cross-core ring write, and the report callback must not
    // be held off for the length of that.
    Report batch[QUEUE_SIZE];
    unsigned n;

    s_qlock.Acquire();
    n = s_qcount;
    if (n)
    {
        memcpy(batch, s_queue, n * sizeof(Report));
        s_qcount = 0;
    }
    s_qlock.Release();

    for (unsigned i = 0; i < n; i++)
        PumpReport(batch[i]);
}

extern "C" {

// --- position and buttons ---------------------------------------------------
//
// The out-parameters are optional in SDL, so both are written only when asked
// for.

Uint32 SDL_GetMouseState(int *x, int *y)
{
    if (x) *x = s_x.load(std::memory_order_acquire);
    if (y) *y = s_y.load(std::memory_order_acquire);
    return s_buttons.load(std::memory_order_acquire);
}

// Bare metal has one screen and no desktop, so global and window coordinates
// are the same thing.
Uint32 SDL_GetGlobalMouseState(int *x, int *y)
{
    return SDL_GetMouseState(x, y);
}

// Consuming: this returns the movement since the last call and resets the
// accumulator, so two callers cannot both have it.
Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    int dx = s_relx.exchange(0, std::memory_order_acq_rel);
    int dy = s_rely.exchange(0, std::memory_order_acq_rel);
    if (x) *x = dx;
    if (y) *y = dy;
    return s_buttons.load(std::memory_order_acquire);
}

// Focus follows the pointer. With one window there is nowhere else for it to
// be, so the answer is that window whenever a mouse is attached, and NULL -
// SDL's "no window has mouse focus" - when there is no pointer at all.
SDL_Window *SDL_GetMouseFocus(void)
{
    if (!s_attached.load(std::memory_order_acquire))
        return nullptr;
    return SDL_GetWindowFromID(WINDOW_ID);
}

// --- modes ------------------------------------------------------------------

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    if (enabled && !s_relative)
    {
        // Movement from now, not movement since whoever last looked.
        s_relx.store(0, std::memory_order_relaxed);
        s_rely.store(0, std::memory_order_relaxed);
    }
    s_relative = enabled;
    return 0;
}

SDL_bool SDL_GetRelativeMouseMode(void)
{
    return s_relative;
}

// Capture routes events to one window while a button is held. With one screen
// and one window there is nothing to route away from, so accepting it changes
// nothing and refusing it would fail callers for no reason.
int SDL_CaptureMouse(SDL_bool enabled)
{
    (void) enabled;
    return 0;
}

// --- warping ----------------------------------------------------------------
//
// Nothing has to move a physical mouse for this: the position is the shim's,
// so a warp is a store. It is a store from the application's core while core 0
// may be applying a report, and the next report wins - which is what a mouse
// being moved during a warp means anyway.
//
// SDL delivers a motion event for a warp unless relative mode has it turned
// off, and applications that recentre the pointer every frame rely on being
// able to tell that event apart by its coordinates.

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
    (void) window;

    int w = 0, h = 0;
    SDL2Circle_PointerBounds(&w, &h);

    int nx = ClampTo(x, w);
    int ny = ClampTo(y, h);
    int ox = s_x.exchange(nx, std::memory_order_acq_rel);
    int oy = s_y.exchange(ny, std::memory_order_acq_rel);

    if (s_relative)
        return;

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type            = SDL_MOUSEMOTION;
    ev.motion.windowID = WINDOW_ID;
    ev.motion.which    = MOUSE_ID;
    ev.motion.state    = s_buttons.load(std::memory_order_acquire);
    ev.motion.x        = nx;
    ev.motion.y        = ny;
    ev.motion.xrel     = nx - ox;
    ev.motion.yrel     = ny - oy;
    PushEvent(ev);
}

int SDL_WarpMouseGlobal(int x, int y)
{
    SDL_WarpMouseInWindow(nullptr, x, y);
    return 0;
}

// --- cursors ----------------------------------------------------------------

SDL_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask,
                             int w, int h, int hot_x, int hot_y)
{
    (void) data; (void) mask; (void) w; (void) h; (void) hot_x; (void) hot_y;
    return s_cursor;
}

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    (void) surface; (void) hot_x; (void) hot_y;
    return s_cursor;
}

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    (void) id;
    return s_cursor;
}

void SDL_SetCursor(SDL_Cursor *cursor)
{
    (void) cursor;
}

SDL_Cursor *SDL_GetCursor(void)
{
    return s_cursor;
}

SDL_Cursor *SDL_GetDefaultCursor(void)
{
    return s_cursor;
}

// Never frees: every cursor above is the same shared token, and freeing it
// would hand the next caller a dangling pointer.
void SDL_FreeCursor(SDL_Cursor *cursor)
{
    (void) cursor;
}

// SDL_ENABLE shows, SDL_DISABLE hides, SDL_QUERY (-1) asks without changing
// anything. The return is always the resulting visibility. Nothing draws a
// cursor here, so the counter is all there is to keep - and keeping it is what
// lets an application that hides and shows in pairs read back what it set.
int SDL_ShowCursor(int toggle)
{
    if (toggle == SDL_ENABLE)       s_cursor_shown = 1;
    else if (toggle == SDL_DISABLE) s_cursor_shown = 0;
    return s_cursor_shown;
}

}   // extern "C"
