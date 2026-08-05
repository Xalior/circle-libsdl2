//
// mouse.cpp — SDL's mouse API over a mouse that is not wired up yet.
//
// THIS IS A STUB, and a deliberate one. Circle binds USB mice and publishes
// them as "mouseN" character devices, and that path is proven in other
// projects on this framework; what does not exist yet is the part that turns
// Circle's mouse reports into SDL_MOUSEMOTION / SDL_MOUSEBUTTON events and
// answers the queries below from real state.
//
// It is here so an application that uses the mouse LINKS and RUNS while that
// work happens, rather than every port waiting on it. What it must never do
// is pretend: everything here either keeps state SDL itself owns, or reports,
// accurately, a mouse that has not moved and whose buttons are not pressed.
// A caller cannot tell "no mouse attached" from "mouse support unfinished",
// which is correct — both mean there is no pointer to read.
//
// WHAT THE REAL IMPLEMENTATION REPLACES. The bookkeeping in this file is not
// throwaway: relative mode and the cursor's visibility counter are SDL's own
// semantics and a driver has to keep them exactly as they are kept here. What
// changes is where the NUMBERS come from — position, button mask and the
// deltas consumed by SDL_GetRelativeMouseState — plus event synthesis on the
// core that owns USB, the way joystick.cpp does it.
//
// The device side of that is already proven on this framework, in CPM/os
// (its os/kernel.cpp and os/input/robothands.cpp), and it is worth reading
// before starting rather than rediscovering:
//
//   - the device is "mouse1" from the device-name service, taken in RAW mode
//     — no Setup(), no Circle-drawn cursor — because the pointer belongs to
//     whoever is drawing the screen;
//   - Circle's status handler orders its arguments (buttons, dx, dy, wheel)
//     differently from its own move handler, so both are folded onto one
//     internal path rather than being handled twice;
//   - reports are queued WHOLE and in order, never folded into a running
//     displacement, because a button edge arriving with a movement is a
//     different event from the same movement without it;
//   - a mouse reports how far it moved and never where it is, so an absolute
//     position exists only because whoever owns the screen clamps the pointer
//     to it. SDL_GetMouseState's coordinates have to be maintained here on
//     that basis, against this shim's own surface geometry.
//
#include <SDL2/SDL.h>

#include "shim_internal.h"

namespace
{
    // SDL's relative mode: the pointer is hidden, held still, and motion is
    // reported as deltas instead of a position. Kept here because it is the
    // application's setting, not the device's — a driver arriving later
    // reads this flag rather than owning one of its own.
    SDL_bool s_relative = SDL_FALSE;

    // SDL_ShowCursor is a COUNTER, not a boolean: every hide has to be
    // matched by a show. A query (-1) must not disturb it.
    int s_cursor_shown = 1;

    // SDL hands applications an opaque SDL_Cursor*, and a null return means
    // failure. There is no cursor to draw yet, so every constructor hands
    // back one shared non-null token: callers keep working, and nothing here
    // dereferences it. SDL_FreeCursor must therefore not free it.
    SDL_Cursor *const s_cursor = reinterpret_cast<SDL_Cursor *>(-1);
}

extern "C" {

// --- position and buttons ---------------------------------------------------
//
// A mouse that has not moved is at the origin with nothing pressed. The
// out-parameters are optional in SDL, so both are written only when asked for.

Uint32 SDL_GetMouseState(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

// Bare metal has one screen and no desktop, so global and window coordinates
// are the same thing. They will stay the same thing once this is real.
Uint32 SDL_GetGlobalMouseState(int *x, int *y)
{
    return SDL_GetMouseState(x, y);
}

// The relative reading is CONSUMING in SDL: it returns the movement since the
// last call and resets the accumulator. With no movement to accumulate there
// is nothing to reset.
Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

// Focus follows the pointer, and there is no pointer. NULL is what SDL
// returns when no window has mouse focus, which is the honest answer.
SDL_Window *SDL_GetMouseFocus(void)
{
    return nullptr;
}

// --- modes ------------------------------------------------------------------

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
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
// Warping moves a pointer that does not exist. The window form returns void in
// SDL, so a caller cannot be told either way; the global form can be, and is.

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
    (void) window; (void) x; (void) y;
}

int SDL_WarpMouseGlobal(int x, int y)
{
    (void) x; (void) y;
    return SDL_SetError("mouse warping is not implemented");
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
// anything. The return is always the resulting visibility.
int SDL_ShowCursor(int toggle)
{
    if (toggle == SDL_ENABLE)       s_cursor_shown = 1;
    else if (toggle == SDL_DISABLE) s_cursor_shown = 0;
    return s_cursor_shown;
}

}   // extern "C"
