//
// shim_internal.h - cross-file plumbing inside circle-libsdl2 (not installed)
//
#ifndef _sdl2_circle_shim_internal_h
#define _sdl2_circle_shim_internal_h

#include <SDL2/SDL_joystick.h>

// keyboard.cpp - USB HID keyboard producer
void SDLCircle_InitKeyboard(void);
void SDLCircle_PumpKeyboard(void);

// ---- joystick.cpp ----------------------------------------------------------

// Attach, detach and report translation for every Circle gamepad. Core 0
// only: called from SDL2Circle_InputPump, and given whatever
// CUSBHCIDevice::UpdatePlugAndPlay just reported, since that is the only
// signal Circle gives that a device may have appeared.
void SDL2Circle_JoystickPump(bool bPlugAndPlayChanged);

// A pad's slot in the joystick table. Slots are stable for as long as a pad
// is plugged in, which neither a device index nor an instance ID is, so this
// is what the game-controller layer holds on to between calls. -1 means the
// pad asked about is not attached.
int SDL2Circle_JoySlotForDeviceIndex(int device_index);
int SDL2Circle_JoySlotForInstance(SDL_JoystickID instance);
int SDL2Circle_JoySlotOf(SDL_Joystick *joystick);

struct SDL2CircleJoyInfo
{
    const char      *name;
    const char      *path;         // the Circle device name, "upadN"
    SDL_JoystickGUID guid;
    Uint16           vendor, product, version;
    int              naxes, nhats, nbuttons;
    SDL_JoystickID   instance;
    unsigned         properties;   // Circle's TGamePadProperty bit mask
};

int SDL2Circle_JoyInfo(int slot, SDL2CircleJoyInfo *out);   // 0 if not attached

Sint16 SDL2Circle_JoySlotAxis(int slot, int axis);
Uint8  SDL2Circle_JoySlotHat(int slot, int hat);
Uint8  SDL2Circle_JoySlotButton(int slot, int button);

SDL_Joystick *SDL2Circle_JoyOpenSlot(int slot);

// ---- mouse.cpp -------------------------------------------------------------

// Attach, detach and report translation for Circle's USB mouse. Core 0
// only, on the same terms as the joystick pump above, and given the same
// plug-and-play signal.
void SDL2Circle_MousePump(bool bPlugAndPlayChanged);

// A mouse report: how far the mouse moved, which buttons the reporting hand
// has down (Circle's MOUSE_BUTTON_* mask), and how far the wheel turned. Both
// forms queue onto the same path and are safe to call from interrupt context.
// The physical mouse and the robot-hands macro channel each have their own,
// so that neither one's buttons can release the other's.
void SDL2Circle_MouseReport(int dx, int dy, unsigned buttons, int wheel);
void SDL2Circle_MouseInject(int dx, int dy, unsigned buttons, int wheel);

// ---- video.cpp -------------------------------------------------------------

// The rectangle the pointer lives in: the application's window while one
// exists, and the declared canvas before that. A mouse reports how far it
// moved and never where it is, so an absolute position exists only because
// this is what the movement is clamped to.
void SDL2Circle_PointerBounds(int *w, int *h);

// ---- gamecontroller.cpp ----------------------------------------------------

// The joystick producer calls these straight after pushing each joystick
// event, so a controller the application has opened gets its own events from
// the same report, in the same order, on the same core. The bindings are
// published by SDL_GameControllerOpen with release ordering, which is what
// makes them safe to read here when the application runs on another core.
void SDL2Circle_ControllerJoyAxis(SDL_JoystickID which, int axis, Sint16 value);
void SDL2Circle_ControllerJoyButton(SDL_JoystickID which, int button, bool down);
void SDL2Circle_ControllerJoyHat(SDL_JoystickID which, int hat, Uint8 value);
void SDL2Circle_ControllerDeviceAdded(int device_index);
void SDL2Circle_ControllerDeviceRemoved(SDL_JoystickID instance);

#endif
