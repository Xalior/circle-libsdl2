//
// mouse.cpp - Circle's USB mouse behind the SDL mouse API.
//
// Circle binds a USB mouse and publishes it as the character device "mouse1".
// This file takes it in raw mode - no Setup(), so no cursor of Circle's -
// because the pointer belongs to whoever is drawing the screen, and that is
// SDL. What arrives is a stream of reports: how far the mouse moved since the
// last one, which buttons are down, and how far the wheel turned. Never where
// the pointer is; a mouse has no idea.
//
// So an absolute position exists only because something clamps the movement
// to a screen. That is what this file does, against the rectangle the video
// layer calls the pointer bounds - the application's window while one exists,
// and the declared canvas before that. That clamped position, in canvas
// coordinates, is what is stored here and what the pointer is drawn at.
//
// It is not always what an application is told. A renderer with a logical
// size set draws in a coordinate system of its own, and the position is
// reported in that one - by the video layer, through the inverse of the same
// mapping that places the picture, so a click is reported where the arrow was
// drawn. Where no logical size is set the two spaces are the same and the
// translation does nothing.
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

// A cursor is an image and the point in it that the pointer position names.
// Whatever an application hands over - a colour surface in any format, or a
// pair of one-bit planes - is turned into ARGB8888 here and kept in a store
// of this library's own, because SDL lets the application release its surface
// the moment the call returns.
//
// SDL_Cursor is opaque to an application (SDL_mouse.h declares the tag and
// nothing else), so this definition is the whole of what a cursor is here.
struct SDL_Cursor
{
    int      w, h;
    int      hot_x, hot_y;
    Uint32  *pixels;            // ARGB8888, w * h, this library's
};

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
// motion is reported as deltas. The position is clamped to the window in both
// modes, so what the flag changes is that entering it starts the delta
// accumulator from zero - the application asked for movement from now, not
// movement since whenever it last looked - and that the cursor is not drawn,
// which is what SDL means by the pointer being hidden while it is steering.
SDL_bool s_relative = SDL_FALSE;

// SDL_ShowCursor is a counter, not a boolean: every hide has to be matched by
// a show. A query (-1) must not disturb it. It starts shown, as SDL's does.
int s_cursor_shown = 1;

// ---------------------------------------------------------------------------
// Cursors
//
// SDL promises an application a pointer from the moment it starts:
// SDL_GetDefaultCursor answers with one, that one is current until the
// application sets another, and it is on the screen unless the application
// hides it. Every desktop backend borrows that first pointer from the window
// system. There is no window system here and nothing to borrow, so this
// library carries an image of its own - the array below.
//
// Everything after that is ordinary SDL: a cursor an application makes is a
// separate object with its own pixels and its own hot spot, setting one makes
// it current, and freeing one releases it.
// ---------------------------------------------------------------------------

// The default cursor: 16x16 ARGB8888, hot spot at 0,0.
// Sprite 1 of D.'s own ui.bmp sheet from the plotit project, used with
// permission of its author. Magenta (#FC00FF) was the sheet's
// transparency key and is stored here as alpha 0.
static const unsigned int s_default_cursor[16 * 16] = {
    0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFFB4FCFF, 0xFFB4FCFF, 0xFFB4FCFF, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF6CB4FF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFF486CFF, 0xFF000000, 0x00000000, 0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFFB4FCFF, 0xFF6CB4FF, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFF486CFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

const int DEFAULT_CURSOR_W = 16;
const int DEFAULT_CURSOR_H = 16;

// Built on first use and never released: SDL_FreeCursor is defined to ignore
// the default cursor, so nothing can take it away and every later query
// answers with the same object.
SDL_Cursor *s_default = nullptr;

// What SDL_SetCursor last named. Null means the default, which is what an
// application that never sets one has, and what freeing the current cursor
// falls back to.
SDL_Cursor *s_current = nullptr;

// Cursors are made, set and freed by the application, on the application's
// own core, and read at present time on that same core. Nothing here is
// touched by core 0's pump, so none of it needs a lock or an atomic - unlike
// the position above, which core 0 writes.

SDL_Cursor *CursorAlloc(int w, int h, int hot_x, int hot_y)
{
    if (w <= 0 || h <= 0)
        return nullptr;

    SDL_Cursor *c = (SDL_Cursor *)SDL_calloc(1, sizeof *c);
    if (!c)
        return nullptr;

    c->pixels = (Uint32 *)SDL_calloc((size_t)w * (size_t)h, sizeof(Uint32));
    if (!c->pixels)
    {
        SDL_free(c);
        return nullptr;
    }

    c->w = w;
    c->h = h;
    c->hot_x = hot_x;
    c->hot_y = hot_y;
    return c;
}

SDL_Cursor *DefaultCursor(void)
{
    if (s_default)
        return s_default;

    SDL_Cursor *c = CursorAlloc(DEFAULT_CURSOR_W, DEFAULT_CURSOR_H, 0, 0);
    if (!c)
        return nullptr;
    memcpy(c->pixels, s_default_cursor, sizeof s_default_cursor);
    s_default = c;
    return c;
}

// The cursor that would be drawn if it were visible.
SDL_Cursor *CurrentCursor(void)
{
    return s_current ? s_current : DefaultCursor();
}

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

// ---------------------------------------------------------------------------
// The canvas the stored position is expressed in
//
// The position above is in canvas coordinates, so a canvas that changes size
// changes what those two numbers mean. The panel has not moved and the canvas
// is still fitted onto the whole of it, so the point of glass the pointer is
// sitting on is a different canvas coordinate afterwards - by exactly the
// ratio the canvas changed by. Left alone, a pointer at (600, 400) in a
// 640x480 canvas keeps those numbers in a 320x240 one, where the screen ends
// at (319, 239), and every click lands somewhere the user did not put it.
//
// So the position is converted in that ratio, and this is the record of what
// it is currently expressed in.
//
// CORE 0 OWNS THE POSITION AND CORE 0 DOES THE CONVERSION. That is the whole
// reason this record exists rather than the video layer simply rewriting
// s_x and s_y when it resizes the canvas. Converting is a read, a multiply
// and a write, and PumpReport on core 0 is doing its own read-add-write of
// the same two values for every report that arrives; a write from the
// application core landing between core 0's load and its store is lost
// outright, and lost only sometimes, which is the hard rule in CLAUDE.md
// about anything crossing between cores and the symptom it warns about.
//
// What crosses instead is the canvas size, which the video layer owns and
// this side only ever reads. The handover is explicit and one-way.
// ---------------------------------------------------------------------------

// Core 0 only, and it is the only writer.
int s_bounds_w = 0, s_bounds_h = 0;

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

// Bring the stored position into the canvas that is there now, if it moved.
//
// Core 0 only: it reads and writes the position, and core 0 is the only core
// allowed to do that - see the record above for why.
//
// Proportional, then clamped. The last column of a canvas scales to one past
// the last column of the new one, and the clamp is what puts it back on the
// screen; it is the same clamp every report goes through.
void RebaseToBounds(void)
{
    int w = 0, h = 0;
    SDL2Circle_PointerBounds(&w, &h);

    // Before the canvas is settled there is nothing to be expressed in, and
    // nothing to convert to.
    if (w <= 0 || h <= 0)
        return;
    if (w == s_bounds_w && h == s_bounds_h)
        return;

    // The first canvas is adopted rather than converted to: the position was
    // never in any other one, so there is no ratio.
    if (s_bounds_w > 0 && s_bounds_h > 0)
    {
        const s64 x = s_x.load(std::memory_order_relaxed);
        const s64 y = s_y.load(std::memory_order_relaxed);
        s_x.store(ClampTo((int)((x * w) / s_bounds_w), w),
                  std::memory_order_release);
        s_y.store(ClampTo((int)((y * h) / s_bounds_h), h),
                  std::memory_order_release);
    }

    s_bounds_w = w;
    s_bounds_h = h;
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

namespace
{

void RebaseOn0(void *)
{
    RebaseToBounds();
}

} // namespace

// The video layer, having moved the canvas, saying so.
//
// It runs on core 0 and it blocks until it has, so the position is already in
// the new canvas by the time the call that resized it returns. An application
// that reads SDL_GetMouseState immediately afterwards - before it has polled a
// single event, and before any mouse report has arrived - gets the converted
// position rather than a stale one, which is why this is not left to the next
// pump pass to notice.
//
// Marshalling is what keeps it a core-0 write. It is also what makes it
// indivisible against PumpReport: both are core 0's work, and core 0's servo
// runs the call mailbox and the input pump one after the other, never at the
// same time. Direct call, costing nothing, when the split is inactive or the
// caller is core 0 already - which is every single-core build.
void SDL2Circle_PointerCanvasChanged(void)
{
    SDL2Circle_CallOn0(RebaseOn0, nullptr);
}

void SDL2Circle_MousePump(bool bPlugAndPlayChanged)
{
    // Before anything is decided against the screen's size, in case it moved.
    // The video layer says so when it resizes the canvas, so this is normally
    // a pair of comparisons that find nothing - but it is what makes the
    // record self-correcting for any path that moves the canvas without
    // announcing it.
    RebaseToBounds();

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
//
// The position is stored in canvas coordinates and answered in the space the
// application draws in, which is the renderer's logical size where one is set
// and the canvas itself otherwise. Both coordinates go through the
// translation together even when only one was asked for, because the mapping
// takes a point rather than a pair of independent numbers.
//
// The translation reads the renderer, so it belongs to the application's own
// core - which is the core that asks, on the same terms as every other
// renderer call. It is the same conversion the mouse events carry, so a
// program that reads the position and one that waits for a motion event are
// told the same thing.

Uint32 SDL_GetMouseState(int *x, int *y)
{
    int cx = s_x.load(std::memory_order_acquire);
    int cy = s_y.load(std::memory_order_acquire);

    SDL2Circle_PointerToLogical(&cx, &cy);

    if (x) *x = cx;
    if (y) *y = cy;
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
//
// It is the movement the device reported, and a logical size does not scale
// it - upstream SDL2 scales the motion event's own xrel and leaves this
// accumulator alone, and there is one fraction carried per renderer for that
// scaling. A second consumer taking from the same fraction would round the
// events wrong for the first.
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
//
// The coordinate is taken in the space the application reads the pointer back
// in, so a program that reads the position and puts it back puts it back
// where it was. Reporting one space and accepting another would break that
// round trip at every logical size, and recentring the pointer is written as
// exactly that round trip. The motion event this pushes carries the canvas
// position, which the delivery point translates like any other.

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
    (void) window;

    int w = 0, h = 0;
    SDL2Circle_PointerBounds(&w, &h);

    int cx = x, cy = y;
    SDL2Circle_PointerFromLogical(&cx, &cy);

    int nx = ClampTo(cx, w);
    int ny = ClampTo(cy, h);
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

// SDL's one-bit cursor format: data and mask are (w + 7) / 8 bytes a row,
// most significant bit leftmost, and the pair of bits for a pixel says what
// that pixel is:
//
//   data 0, mask 1   white
//   data 1, mask 1   black
//   data 0, mask 0   transparent
//   data 1, mask 0   inverted where the platform can, black where it cannot
//
// Nothing here reads the screen back to invert against it - a cursor is
// composed onto a frame that is on its way to the glass, not onto pixels this
// side can sample - so the inverted case is drawn black, which is the answer
// SDL's own definition names for a platform that cannot invert.
SDL_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask,
                             int w, int h, int hot_x, int hot_y)
{
    if (!data || !mask || w <= 0 || h <= 0)
    {
        SDL_SetError("SDL_CreateCursor: no cursor data");
        return nullptr;
    }

    SDL_Cursor *c = CursorAlloc(w, h, hot_x, hot_y);
    if (!c)
    {
        SDL_OutOfMemory();
        return nullptr;
    }

    const int stride = (w + 7) / 8;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            const int    byte = y * stride + (x / 8);
            const Uint8  bit  = (Uint8)(0x80 >> (x & 7));
            const bool   d    = (data[byte] & bit) != 0;
            const bool   m    = (mask[byte] & bit) != 0;

            Uint32 px;
            if (!m)
                px = d ? 0xFF000000u : 0x00000000u;
            else
                px = d ? 0xFF000000u : 0xFFFFFFFFu;
            c->pixels[(size_t)y * w + x] = px;
        }
    }
    return c;
}

// The surface is the application's and it may free it the moment this
// returns, so the pixels are converted into a store of this library's own
// rather than referenced. Any format SDL can convert is accepted; the alpha
// channel is what decides the shape of the pointer.
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    if (!surface || surface->w <= 0 || surface->h <= 0)
    {
        SDL_SetError("SDL_CreateColorCursor: no surface");
        return nullptr;
    }

    SDL_Surface *src = surface;
    SDL_Surface *converted = nullptr;
    if (!surface->format
        || surface->format->format != SDL_PIXELFORMAT_ARGB8888)
    {
        converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0);
        if (!converted)
            return nullptr;         // SDL_ConvertSurfaceFormat set the error
        src = converted;
    }

    SDL_Cursor *c = CursorAlloc(src->w, src->h, hot_x, hot_y);
    if (!c)
    {
        SDL_FreeSurface(converted);
        SDL_OutOfMemory();
        return nullptr;
    }

    const Uint8 *row = (const Uint8 *)src->pixels;
    for (int y = 0; y < src->h; y++, row += src->pitch)
        memcpy(&c->pixels[(size_t)y * src->w], row, (size_t)src->w * 4);

    SDL_FreeSurface(converted);
    return c;
}

// There is no system cursor set on this board to ask for a shape from, so
// every one of SDL's named cursors is the pointer this library carries. Each
// call still makes its own object, because the application owns what it is
// given: it may free one of them without that reaching any of the others.
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    (void) id;

    SDL_Cursor *c = CursorAlloc(DEFAULT_CURSOR_W, DEFAULT_CURSOR_H, 0, 0);
    if (!c)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    memcpy(c->pixels, s_default_cursor, sizeof s_default_cursor);
    return c;
}

// Null asks for the current cursor to be redrawn rather than for a change,
// which is what SDL defines it as; the pointer is composed afresh on every
// frame here, so there is nothing to do for it.
void SDL_SetCursor(SDL_Cursor *cursor)
{
    if (cursor)
        s_current = cursor;
}

SDL_Cursor *SDL_GetCursor(void)
{
    return CurrentCursor();
}

SDL_Cursor *SDL_GetDefaultCursor(void)
{
    return DefaultCursor();
}

// Freeing the default cursor does nothing, and freeing the current one puts
// the default back first, both as SDL defines them. The second is what makes
// a program that frees the cursor it is using safe: it is the ordinary way an
// application replaces one cursor with another, and without the fallback the
// next frame would compose from a store that had just been released.
void SDL_FreeCursor(SDL_Cursor *cursor)
{
    if (!cursor || cursor == s_default)
        return;

    if (cursor == s_current)
        s_current = nullptr;        // null is the default; see CurrentCursor

    SDL_free(cursor->pixels);
    SDL_free(cursor);
}

// SDL_ENABLE shows, SDL_DISABLE hides, SDL_QUERY (-1) asks without changing
// anything. The return is always the resulting visibility. It is a counter in
// SDL's own description and a flag in its behaviour - every hide has to be
// matched by a show - and it is what decides whether the current cursor is
// composed onto a frame.
int SDL_ShowCursor(int toggle)
{
    if (toggle == SDL_ENABLE)       s_cursor_shown = 1;
    else if (toggle == SDL_DISABLE) s_cursor_shown = 0;
    return s_cursor_shown;
}

}   // extern "C"

// ---------------------------------------------------------------------------
// What the present path draws
// ---------------------------------------------------------------------------

// Answered on the application's own core, from inside present. See the
// declaration in shim_internal.h for what the caller may do with it.
bool SDL2Circle_CursorImage(SDL2CircleCursorImage *out)
{
    if (!out)
        return false;

    // Hidden by the application, or steering with the device rather than
    // pointing with it - SDL hides the pointer in relative mode.
    if (!s_cursor_shown || s_relative)
        return false;

    // No mouse on the board is no pointer, which is the same answer
    // SDL_GetMouseFocus already gives: there is nothing driving a position,
    // so drawing an arrow parked wherever the position happened to start
    // would be inventing a pointer that is not there.
    if (!s_attached.load(std::memory_order_acquire))
        return false;

    SDL_Cursor *c = CurrentCursor();
    if (!c || !c->pixels)
        return false;

    out->pixels = (const Uint8 *)c->pixels;
    out->pitch  = c->w * 4;
    out->w      = c->w;
    out->h      = c->h;
    out->x      = s_x.load(std::memory_order_acquire) - c->hot_x;
    out->y      = s_y.load(std::memory_order_acquire) - c->hot_y;
    return true;
}
