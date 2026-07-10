//
// input.cpp — USB HID keyboard -> SDL event queue.
//
// SDL scancodes ARE USB HID usage-page-7 codes, so the translation is the
// identity; only SDL keycodes (syms) need a small mapping. The raw report
// handler can run in IRQ context: it only snapshots the report. Diffing and
// event synthesis happen in SDL2Circle_InputPump() on the main loop.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/devicenameservice.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/atomic.h>
#include <cstring>

namespace
{

CUSBHCIDevice *s_usb = nullptr;
CUSBKeyboardDevice *s_keyboard = nullptr;

// IRQ-side snapshot of the latest HID report
struct RawReport
{
    unsigned char mods;
    unsigned char keys[6];
};
RawReport s_report;                 // written by IRQ, read by pump
volatile u32 s_reportSeq = 0;

// pump-side state
RawReport s_prev;                   // last report translated into events
Uint16 s_modState = KMOD_NONE;
Uint8 s_keyState[SDL_NUM_SCANCODES];

void RawKeyHandler(unsigned char ucModifiers, const unsigned char RawKeys[6])
{
    s_report.mods = ucModifiers;
    memcpy((void *)s_report.keys, RawKeys, 6);
    AtomicIncrement((volatile int *)&s_reportSeq);
}

void KeyboardRemovedHandler(CDevice *, void *)
{
    s_keyboard = nullptr;
    // release everything so no key stays stuck down
    s_report.mods = 0;
    memset((void *)s_report.keys, 0, 6);
    AtomicIncrement((volatile int *)&s_reportSeq);
}

SDL_Keycode KeycodeFor(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'a' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);

    switch (sc)
    {
    case SDL_SCANCODE_0:            return '0';
    case SDL_SCANCODE_RETURN:       return SDLK_RETURN;
    case SDL_SCANCODE_ESCAPE:       return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE:    return SDLK_BACKSPACE;
    case SDL_SCANCODE_TAB:          return SDLK_TAB;
    case SDL_SCANCODE_SPACE:        return SDLK_SPACE;
    case SDL_SCANCODE_MINUS:        return '-';
    case SDL_SCANCODE_EQUALS:       return '=';
    case SDL_SCANCODE_LEFTBRACKET:  return '[';
    case SDL_SCANCODE_RIGHTBRACKET: return ']';
    case SDL_SCANCODE_BACKSLASH:    return '\\';
    case SDL_SCANCODE_SEMICOLON:    return ';';
    case SDL_SCANCODE_APOSTROPHE:   return '\'';
    case SDL_SCANCODE_GRAVE:        return '`';
    case SDL_SCANCODE_COMMA:        return ',';
    case SDL_SCANCODE_PERIOD:       return '.';
    case SDL_SCANCODE_SLASH:        return '/';
    default:
        return sc | SDLK_SCANCODE_MASK;
    }
}

// USB modifier-bit index (0..7) -> SDL
const Uint16 ModMask[8] = {KMOD_LCTRL, KMOD_LSHIFT, KMOD_LALT, KMOD_LGUI,
                           KMOD_RCTRL, KMOD_RSHIFT, KMOD_RALT, KMOD_RGUI};
const SDL_Scancode ModScancode[8] = {
    SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI};

void PushKeyEvent(SDL_Scancode sc, bool down)
{
    if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES)
        return;

    s_keyState[sc] = down ? 1 : 0;

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.windowID = 1;   // the single window owns the keyboard
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat = 0;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = KeycodeFor(sc);
    ev.key.keysym.mod = s_modState;
    SDL_PushEvent(&ev);
}

bool InReport(const RawReport &r, unsigned char key)
{
    for (int i = 0; i < 6; i++)
        if (r.keys[i] == key)
            return true;
    return false;
}

} // namespace

void SDL2Circle_InputInit(void)
{
    if (s_usb)
        return;

    s_usb = new CUSBHCIDevice(CInterruptSystem::Get(), CTimer::Get(),
                              TRUE /* plug-and-play */);
    if (!s_usb->Initialize())
    {
        // No USB is not fatal — headless/keyboardless payloads are valid.
        delete s_usb;
        s_usb = (CUSBHCIDevice *)-1;   // tried and failed; don't retry
    }
}

void SDL2Circle_InputPump(void)
{
    if (!s_usb || s_usb == (CUSBHCIDevice *)-1)
        return;

    boolean bChanged = s_usb->UpdatePlugAndPlay();

    if (!s_keyboard && bChanged)
    {
        CDevice *pDevice =
            CDeviceNameService::Get()->GetDevice("ukbd1", FALSE);
        if (pDevice)
        {
            s_keyboard = (CUSBKeyboardDevice *)pDevice;
            s_keyboard->RegisterKeyStatusHandlerRaw(RawKeyHandler);
            s_keyboard->RegisterRemovedHandler(KeyboardRemovedHandler);
        }
    }

    static u32 lastSeq = 0;
    u32 seq = s_reportSeq;
    if (seq == lastSeq)
        return;
    lastSeq = seq;

    RawReport now = s_report;   // struct copy; handler may overwrite, next
                                // pump picks up the newer sequence

    // modifier diffs
    unsigned char modDiff = now.mods ^ s_prev.mods;
    for (int bit = 0; bit < 8; bit++)
    {
        if (!(modDiff & (1 << bit)))
            continue;
        bool down = now.mods & (1 << bit);
        if (down)
            s_modState |= ModMask[bit];
        else
            s_modState &= ~ModMask[bit];
        PushKeyEvent(ModScancode[bit], down);
    }

    // key releases, then presses
    for (int i = 0; i < 6; i++)
        if (s_prev.keys[i] > 3 && !InReport(now, s_prev.keys[i]))
            PushKeyEvent((SDL_Scancode)s_prev.keys[i], false);
    for (int i = 0; i < 6; i++)
        if (now.keys[i] > 3 && !InReport(s_prev, now.keys[i]))
            PushKeyEvent((SDL_Scancode)now.keys[i], true);

    s_prev = now;
}

extern "C" const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    if (numkeys)
        *numkeys = SDL_NUM_SCANCODES;
    return s_keyState;
}

extern "C" SDL_Keymod SDL_GetModState(void)
{
    return (SDL_Keymod)s_modState;
}

extern "C" SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode)
{
    return KeycodeFor(scancode);
}

extern "C" void SDL_StartTextInput(void) {}
extern "C" void SDL_StopTextInput(void) {}
extern "C" SDL_bool SDL_IsTextInputActive(void) { return SDL_FALSE; }
