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
#include "shim_internal.h"

#include <circle/timer.h>
#include <circle/devicenameservice.h>
#include <circle/usb/usbcontroller.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/serial.h>
#include <circle/atomic.h>
#include <circle/sched/scheduler.h>
#include <cstring>

namespace
{

// The host controller this library PUMPS but does not own — see
// SDL2Circle_InputInit. Held as the generic controller interface because
// pumping plug-and-play is the only thing done with it, and that is the one
// method every board's controller has in common.
CUSBController *s_usb = nullptr;

// The sentinel for "asked, and there is none": distinct from nullptr, which
// means "not asked yet", so a board with no USB is not searched on every
// pass for the rest of the run.
CUSBController *const USB_NONE = (CUSBController *)-1;

// Set on core 0 when the controller is missing and injection is armed; acted
// on by the caller, off core 0. See SDL2Circle_InputInit.
bool s_bNoInputFatal = false;

// How often the refusal repeats itself. Long enough not to bury anything else
// on the console, short enough that nobody attaching serial has to wait to
// find out why the board is doing nothing.
const unsigned SDL2CIRCLE_NOINPUT_REPEAT_SECONDS = 5;

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

// Whether an application is collecting typed text. SDL starts with it ON,
// and an application that never calls SDL_StartTextInput still receives
// SDL_TEXTINPUT — which is what makes a program with a text field work
// without having asked for anything.
bool s_textInputActive = true;

// The character a printable key produces under the modifiers in force, on a
// US layout. Returns 0 for a key that types nothing.
//
// This is the forward direction of AsciiToKey below, and the two describe
// the same keyboard: that one turns a byte into the key that would produce
// it, this one turns a key press back into the byte a person would see.
char TypedCharacter(SDL_Scancode sc, Uint16 mod)
{
    // A key held with control or a GUI key is a command, not text, and SDL
    // sends no SDL_TEXTINPUT for one. Alt is left out for the same reason.
    if (mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
        return 0;

    const bool shift = (mod & KMOD_SHIFT) != 0;
    const bool caps = (mod & KMOD_CAPS) != 0;

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        const char base = (char)('a' + (sc - SDL_SCANCODE_A));
        // Caps lock and shift each invert the case, so both together give
        // lower case again — which is what a keyboard does.
        return (shift != caps) ? (char)(base - 32) : base;
    }

    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
    {
        const char digit = (char)('1' + (sc - SDL_SCANCODE_1));
        if (!shift)
            return digit;
        static const char shifted[9] = { '!', '@', '#', '$', '%', '^', '&', '*', '(' };
        return shifted[sc - SDL_SCANCODE_1];
    }

    switch (sc)
    {
    case SDL_SCANCODE_0:            return shift ? ')' : '0';
    case SDL_SCANCODE_SPACE:        return ' ';
    case SDL_SCANCODE_MINUS:        return shift ? '_' : '-';
    case SDL_SCANCODE_EQUALS:       return shift ? '+' : '=';
    case SDL_SCANCODE_LEFTBRACKET:  return shift ? '{' : '[';
    case SDL_SCANCODE_RIGHTBRACKET: return shift ? '}' : ']';
    case SDL_SCANCODE_BACKSLASH:    return shift ? '|' : '\\';
    case SDL_SCANCODE_SEMICOLON:    return shift ? ':' : ';';
    case SDL_SCANCODE_APOSTROPHE:   return shift ? '"' : '\'';
    case SDL_SCANCODE_GRAVE:        return shift ? '~' : '`';
    case SDL_SCANCODE_COMMA:        return shift ? '<' : ',';
    case SDL_SCANCODE_PERIOD:       return shift ? '>' : '.';
    case SDL_SCANCODE_SLASH:        return shift ? '?' : '/';

    // The keypad types its digits and operators regardless of shift; num
    // lock is not tracked, and a keypad that navigates instead would send
    // its own scancodes.
    case SDL_SCANCODE_KP_DIVIDE:   return '/';
    case SDL_SCANCODE_KP_MULTIPLY: return '*';
    case SDL_SCANCODE_KP_MINUS:    return '-';
    case SDL_SCANCODE_KP_PLUS:     return '+';
    case SDL_SCANCODE_KP_PERIOD:   return '.';
    case SDL_SCANCODE_KP_0:        return '0';
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_3:
    case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_5: case SDL_SCANCODE_KP_6:
    case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8: case SDL_SCANCODE_KP_9:
        return (char)('1' + (sc - SDL_SCANCODE_KP_1));

    default:
        return 0;
    }
}

// Return, tab, backspace and escape have ASCII codes but are NOT text: SDL
// delivers them as key events only, and an application that inserted them as
// characters would put a control byte in its text field.
void PushTextInputEvent(SDL_Scancode sc, Uint16 mod)
{
    if (!s_textInputActive)
        return;

    const char c = TypedCharacter(sc, mod);
    if (c == 0)
        return;

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_TEXTINPUT;
    ev.text.timestamp = SDL_GetTicks();
    ev.text.windowID = 1;
    ev.text.text[0] = c;
    ev.text.text[1] = '\0';
    SDL_PushEvent(&ev);
}

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

    // SDL sends the key event first and the text second, and applications
    // rely on that order — one that reads text usually checks the key event
    // for editing keys in the same pass.
    if (down)
        PushTextInputEvent(sc, s_modState);
}

bool InReport(const RawReport &r, unsigned char key)
{
    for (int i = 0; i < 6; i++)
        if (r.keys[i] == key)
            return true;
    return false;
}

// --- Debug UART key injection ------------------------------------------------
// Active only when --rapi-debug-uart is stamped into the boot argument block;
// the serial device it reads is the library's own to find. A serial console
// drives the emulated keyboard so the bench can dismiss a +3 Loader, type a
// C64 LOAD"...", or unlock MAME's UI (Scroll Lock) and open its menu (Tab).
//
// The wire format is a LINE-ORIENTED macro language (one command per '\n'):
//
//   <domain> <command...>
//
// The first token routes to a subsystem, so this is the shim's general
// robot-hands channel over SDL's input surface, not a keyboard hack —
// gamepad/joystick and whatever input device comes next each register a new
// domain (s_injectDomains, below) without touching the transport. Today there
// are two:
//
//   key down <key>     press and HOLD <key> (stays down; combine for chords)
//   key up   <key>     release <key>
//   key tap  <key>     self-timed press+release of one <key>
//   key type <text>    self-timed taps for each character of <text>
//   mouse move <dx> <dy>   move the pointer by a displacement
//   mouse to   <x> <y>     move the pointer TO a screen coordinate
//   mouse down <button>    press and HOLD <button> (left right middle x1 x2)
//   mouse up   <button>    release <button>
//   mouse tap  <button>    self-timed press+release of one <button>
//   mouse wheel <n>        turn the wheel; positive is away from the user
//   # ...              comment; blank lines ignored
//
// Explicit down/up exist because real machines need CHORDS — keys held down
// together — that a stream of self-releasing taps can never express (there is
// no single byte for "both shifts at once", the Sinclair reset):
//   key down lshift / key down rshift / key up rshift / key up lshift
//
// <key> is a single printable character (US-layout, shift auto-applied for
// tap/type) or a name: lshift rshift lctrl rctrl lalt ralt lgui rgui
// scrolllock capslock tab esc enter space bs del ins up down left right
// home end pgup pgdn f1..f12. down/up use the PHYSICAL key only (no auto
// shift) — a chord names its own modifiers. tap/type feed a self-timed hold
// queue (below) because MAME scans the keyboard per emulated frame; down/up
// post immediately and the SCRIPT owns the timing between them.
//
// <button> is left, right, middle, x1 or x2. The mouse verbs mirror the
// keyboard's for the same reason: a click that a per-frame scan can miss is
// worse than no click at all, so tap goes through the hold queue while
// down/up post immediately.
//
// The device injection reads from: whatever the library found for itself, or
// whatever a kernel lent it through SDL2Circle_SetInjectSerial.
CSerialDevice *s_injectSerial = nullptr;

// One self-timed press-and-release, of either kind. Keys and mouse buttons
// share the hold state machine below because they need the identical timing —
// what differs is only which device the press is posted to.
struct InjTap
{
    bool         button;       // true: `mask` is a Circle MOUSE_BUTTON_*
    SDL_Scancode sc;
    unsigned     mask;
    bool         shift;
};
InjTap s_injQueue[64];
unsigned s_injHead = 0;
unsigned s_injCount = 0;

char s_injLine[128];           // accumulates serial RX until a '\n'
int s_injLineLen = 0;

SDL_Scancode s_injHeldSc = SDL_SCANCODE_UNKNOWN;
bool s_injHeldShift = false;
unsigned s_injHeldButton = 0;  // Circle mask being held by a tap; 0 for a key
int s_injPhase = 0;            // 0 idle, 1 holding a key, 2 inter-key gap
u64 s_injUntil = 0;           // wall-clock (us) the current phase ends at

// The buttons the robot's own hand is holding. The mouse driver keeps this
// hand's buttons apart from the physical mouse's and reports the union, so a
// script pressing a button never releases one the operator has down on the
// real mouse.
unsigned s_injMouseButtons = 0;

// Timing is WALL-CLOCK, not pump calls: SDL_PumpEvents runs many times per
// emulated frame (MAME drains the event queue in a loop), so a frame counter
// would expire in a fraction of one real frame and MAME's per-frame keyboard
// scan would miss the key. 80 ms down / 50 ms up survives that scan reliably.
const u64 INJ_HOLD_US = 80000;
const u64 INJ_GAP_US = 50000;

// ASCII byte -> US-layout scancode + shift. false => ignore the byte.
bool AsciiToKey(char c, SDL_Scancode &sc, bool &shift)
{
    shift = false;
    if (c >= 'a' && c <= 'z') { sc = (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { sc = (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A')); shift = true; return true; }
    if (c >= '1' && c <= '9') { sc = (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1')); return true; }
    switch (c)
    {
    case '0':  sc = SDL_SCANCODE_0;            return true;
    case '\r': case '\n': sc = SDL_SCANCODE_RETURN; return true;
    case ' ':  sc = SDL_SCANCODE_SPACE;        return true;
    case 0x1b: sc = SDL_SCANCODE_ESCAPE;       return true;
    case 0x08: case 0x7f: sc = SDL_SCANCODE_BACKSPACE; return true;
    case '\t': sc = SDL_SCANCODE_TAB;          return true;
    case '-':  sc = SDL_SCANCODE_MINUS;        return true;
    case '=':  sc = SDL_SCANCODE_EQUALS;       return true;
    case '[':  sc = SDL_SCANCODE_LEFTBRACKET;  return true;
    case ']':  sc = SDL_SCANCODE_RIGHTBRACKET; return true;
    case ';':  sc = SDL_SCANCODE_SEMICOLON;    return true;
    case '\'': sc = SDL_SCANCODE_APOSTROPHE;   return true;
    case '`':  sc = SDL_SCANCODE_GRAVE;        return true;
    case ',':  sc = SDL_SCANCODE_COMMA;        return true;
    case '.':  sc = SDL_SCANCODE_PERIOD;       return true;
    case '/':  sc = SDL_SCANCODE_SLASH;        return true;
    case '\\': sc = SDL_SCANCODE_BACKSLASH;    return true;
    case '!':  sc = SDL_SCANCODE_1;          shift = true; return true;
    case '@':  sc = SDL_SCANCODE_2;          shift = true; return true;
    case '#':  sc = SDL_SCANCODE_3;          shift = true; return true;
    case '$':  sc = SDL_SCANCODE_4;          shift = true; return true;
    case '%':  sc = SDL_SCANCODE_5;          shift = true; return true;
    case '^':  sc = SDL_SCANCODE_6;          shift = true; return true;
    case '&':  sc = SDL_SCANCODE_7;          shift = true; return true;
    case '*':  sc = SDL_SCANCODE_8;          shift = true; return true;
    case '(':  sc = SDL_SCANCODE_9;          shift = true; return true;
    case ')':  sc = SDL_SCANCODE_0;          shift = true; return true;
    case '_':  sc = SDL_SCANCODE_MINUS;      shift = true; return true;
    case '+':  sc = SDL_SCANCODE_EQUALS;     shift = true; return true;
    case ':':  sc = SDL_SCANCODE_SEMICOLON;  shift = true; return true;
    case '"':  sc = SDL_SCANCODE_APOSTROPHE; shift = true; return true;
    case '<':  sc = SDL_SCANCODE_COMMA;      shift = true; return true;
    case '>':  sc = SDL_SCANCODE_PERIOD;     shift = true; return true;
    case '?':  sc = SDL_SCANCODE_SLASH;      shift = true; return true;
    default:   return false;
    }
}

void InjectShift(bool down)
{
    if (down) s_modState |= KMOD_LSHIFT;
    else      s_modState &= ~KMOD_LSHIFT;
    PushKeyEvent(SDL_SCANCODE_LSHIFT, down);
}

// Named non-printable keys the command protocol accepts (printables go through
// AsciiToKey). f1..f12 are resolved arithmetically, not tabled.
struct KeyName { const char *name; SDL_Scancode sc; };
const KeyName s_keyNames[] = {
    {"lshift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
    {"lctrl", SDL_SCANCODE_LCTRL},   {"rctrl", SDL_SCANCODE_RCTRL},
    {"lalt", SDL_SCANCODE_LALT},     {"ralt", SDL_SCANCODE_RALT},
    {"lgui", SDL_SCANCODE_LGUI},     {"rgui", SDL_SCANCODE_RGUI},
    {"scrolllock", SDL_SCANCODE_SCROLLLOCK}, {"capslock", SDL_SCANCODE_CAPSLOCK},
    {"tab", SDL_SCANCODE_TAB},       {"esc", SDL_SCANCODE_ESCAPE},
    {"enter", SDL_SCANCODE_RETURN},  {"return", SDL_SCANCODE_RETURN},
    {"space", SDL_SCANCODE_SPACE},
    {"bs", SDL_SCANCODE_BACKSPACE},  {"del", SDL_SCANCODE_DELETE},
    {"ins", SDL_SCANCODE_INSERT},
    {"up", SDL_SCANCODE_UP},         {"down", SDL_SCANCODE_DOWN},
    {"left", SDL_SCANCODE_LEFT},     {"right", SDL_SCANCODE_RIGHT},
    {"home", SDL_SCANCODE_HOME},     {"end", SDL_SCANCODE_END},
    {"pgup", SDL_SCANCODE_PAGEUP},   {"pgdn", SDL_SCANCODE_PAGEDOWN},
};

// Resolve a token to a scancode. Single char -> US-layout via AsciiToKey (which
// also reports whether shift is implied, used only by tap/type). Otherwise a
// name from the table, or fN. shift is always false for named keys.
bool KeyByName(const char *name, SDL_Scancode &sc, bool &shift)
{
    shift = false;
    if (name[0] && !name[1])
        return AsciiToKey(name[0], sc, shift);

    if ((name[0] == 'f' || name[0] == 'F') && name[1])
    {
        int n = 0;
        for (const char *p = name + 1; *p; p++)
        {
            if (*p < '0' || *p > '9') { n = 0; break; }
            n = n * 10 + (*p - '0');
        }
        if (n >= 1 && n <= 12) { sc = (SDL_Scancode)(SDL_SCANCODE_F1 + (n - 1)); return true; }
    }

    for (const KeyName &k : s_keyNames)
        if (!strcmp(name, k.name)) { sc = k.sc; return true; }
    return false;
}

// Immediate press/release for a named key, keeping s_modState coherent so a
// chord built from named modifiers reports the right mod on later events.
void ManualKey(SDL_Scancode sc, bool down)
{
    Uint16 m = 0;
    switch (sc)
    {
    case SDL_SCANCODE_LSHIFT: m = KMOD_LSHIFT; break;
    case SDL_SCANCODE_RSHIFT: m = KMOD_RSHIFT; break;
    case SDL_SCANCODE_LCTRL:  m = KMOD_LCTRL;  break;
    case SDL_SCANCODE_RCTRL:  m = KMOD_RCTRL;  break;
    case SDL_SCANCODE_LALT:   m = KMOD_LALT;   break;
    case SDL_SCANCODE_RALT:   m = KMOD_RALT;   break;
    case SDL_SCANCODE_LGUI:   m = KMOD_LGUI;   break;
    case SDL_SCANCODE_RGUI:   m = KMOD_RGUI;   break;
    default: break;
    }
    if (m)
    {
        if (down) s_modState |= m;
        else      s_modState &= ~m;
    }
    PushKeyEvent(sc, down);
}

// Enqueue one self-timed tap for the hold state machine.
void InjectEnqueue(SDL_Scancode sc, bool shift)
{
    if (s_injCount < 64)
    {
        s_injQueue[(s_injHead + s_injCount) % 64] = InjTap{false, sc, 0, shift};
        s_injCount++;
    }
}

void InjectEnqueueButton(unsigned mask)
{
    if (s_injCount < 64)
    {
        s_injQueue[(s_injHead + s_injCount) % 64] =
            InjTap{true, SDL_SCANCODE_UNKNOWN, mask, false};
        s_injCount++;
    }
}

// One displacement, delivered as however many reports it takes: a real mouse
// carries at most 127 per axis in a report, so a long move arrives as a burst
// of full-scale reports exactly as a fast hand's would. A zero displacement
// still sends one report — that is how a button-only change is delivered.
const int INJ_MOUSE_STEP_MAX = 127;

void InjectMouseSend(int dx, int dy)
{
    do
    {
        int stepx = dx;
        if (stepx >  INJ_MOUSE_STEP_MAX) stepx =  INJ_MOUSE_STEP_MAX;
        if (stepx < -INJ_MOUSE_STEP_MAX) stepx = -INJ_MOUSE_STEP_MAX;

        int stepy = dy;
        if (stepy >  INJ_MOUSE_STEP_MAX) stepy =  INJ_MOUSE_STEP_MAX;
        if (stepy < -INJ_MOUSE_STEP_MAX) stepy = -INJ_MOUSE_STEP_MAX;

        dx -= stepx;
        dy -= stepy;

        SDL2Circle_MouseInject(stepx, stepy, s_injMouseButtons, 0);
    }
    while (dx != 0 || dy != 0);
}

// Press or release one button in the robot's own hand and report it. A button
// already in the requested state changes nothing and sends nothing — a real
// mouse does not report what did not happen.
void InjectButton(unsigned mask, bool down)
{
    unsigned wanted = down ? (s_injMouseButtons | mask)
                           : (s_injMouseButtons & ~mask);
    if (wanted == s_injMouseButtons)
        return;
    s_injMouseButtons = wanted;
    InjectMouseSend(0, 0);
}

struct ButtonName { const char *name; unsigned mask; };
const ButtonName s_buttonNames[] = {
    {"left",   MOUSE_BUTTON_LEFT},
    {"right",  MOUSE_BUTTON_RIGHT},
    {"middle", MOUSE_BUTTON_MIDDLE},
    {"x1",     MOUSE_BUTTON_SIDE1},
    {"x2",     MOUSE_BUTTON_SIDE2},
};

bool ButtonByName(const char *name, unsigned &mask)
{
    for (const ButtonName &b : s_buttonNames)
        if (!strcmp(name, b.name)) { mask = b.mask; return true; }
    return false;
}

// Base-ten integer with an optional sign. Rejects anything else outright:
// a mistyped coordinate must not silently become a move to zero.
bool InjectParseInt(const char *tok, int &out)
{
    if (!*tok)
        return false;

    bool neg = false;
    if (*tok == '-' || *tok == '+') { neg = (*tok == '-'); tok++; }
    if (!*tok)
        return false;

    int value = 0;
    for (; *tok; tok++)
    {
        if (*tok < '0' || *tok > '9')
            return false;
        value = value * 10 + (*tok - '0');
    }
    out = neg ? -value : value;
    return true;
}

// Split off the first whitespace-delimited token of `s`, NUL-terminate it, and
// return the remainder (leading whitespace trimmed). *s is advanced past the
// token. Empty remainder is "".
char *InjectNextToken(char **s)
{
    char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    char *tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p++ = 0; while (*p == ' ' || *p == '\t') p++; }
    *s = p;
    return tok;
}

// Domain: keyboard. `args` is everything after "key" — "<verb> <operand...>".
void InjectKeyCmd(char *args)
{
    char *verb = InjectNextToken(&args);   // args now points at the operand

    if (!strcmp(verb, "type"))
    {
        for (char *p = args; *p; p++)
        {
            SDL_Scancode sc; bool shift;
            if (AsciiToKey(*p, sc, shift))
                InjectEnqueue(sc, shift);
        }
        return;
    }

    SDL_Scancode sc; bool shift;
    if (!KeyByName(args, sc, shift))
        return;

    if (!strcmp(verb, "down"))     ManualKey(sc, true);
    else if (!strcmp(verb, "up"))  ManualKey(sc, false);
    else if (!strcmp(verb, "tap")) InjectEnqueue(sc, shift);
}

// Domain: mouse. `args` is everything after "mouse".
void InjectMouseCmd(char *args)
{
    char *verb = InjectNextToken(&args);

    if (!strcmp(verb, "move"))
    {
        int dx, dy;
        if (InjectParseInt(InjectNextToken(&args), dx)
            && InjectParseInt(InjectNextToken(&args), dy))
            InjectMouseSend(dx, dy);
        return;
    }

    // "mouse to X Y" — an absolute position out of a device that only speaks
    // displacements. A mouse reports how far it moved, never where it is, so
    // there is no coordinate to send; but the pointer is CLAMPED to the
    // surface, so a displacement at least as large as the surface parks it in
    // the top-left corner no matter where it started. From a known corner a
    // relative move is an absolute one.
    //
    // It lives here so that driving a pointer from the bench is a coordinate,
    // not a technique to be rediscovered at each session.
    if (!strcmp(verb, "to"))
    {
        int x, y;
        if (InjectParseInt(InjectNextToken(&args), x)
            && InjectParseInt(InjectNextToken(&args), y))
        {
            int w = 0, h = 0;
            SDL2Circle_PointerBounds(&w, &h);
            InjectMouseSend(-w, -h);     // to the corner; InjectMouseSend steps it
            InjectMouseSend(x, y);
        }
        return;
    }

    if (!strcmp(verb, "wheel"))
    {
        int n;
        if (InjectParseInt(InjectNextToken(&args), n))
            SDL2Circle_MouseInject(0, 0, s_injMouseButtons, n);
        return;
    }

    unsigned mask;
    if (!ButtonByName(InjectNextToken(&args), mask))
        return;

    if (!strcmp(verb, "down"))      InjectButton(mask, true);
    else if (!strcmp(verb, "up"))   InjectButton(mask, false);
    else if (!strcmp(verb, "tap"))  InjectEnqueueButton(mask);
}

// The robot-hands domain table. New subsystems (pad, grab, ...) register here;
// the transport and the line parser never change.
struct InjectDomain { const char *name; void (*fn)(char *args); };
const InjectDomain s_injectDomains[] = {
    {"key",   InjectKeyCmd},
    {"mouse", InjectMouseCmd},
};

// Route one command line (already NUL-terminated, no '\n') to its domain.
void InjectDispatch(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0 || *line == '#')
        return;

    char *domain = InjectNextToken(&line);   // line now points at the args
    for (const InjectDomain &d : s_injectDomains)
        if (!strcmp(domain, d.name)) { d.fn(line); return; }
}

} // namespace

// FIND THE HOST CONTROLLER; NEVER BUILD ONE.
//
// A USB host controller is a device, and every device on this board belongs
// to the host kernel — brought up in CKernel::Initialize alongside the serial
// console, the interrupt system, the timer and the SD card, and brought up
// there because that is the one place on the machine where a long, blocking,
// interrupt-driven bring-up is allowed to take as much time as it needs.
//
// This library used to construct and initialise one here instead, and because
// SDL_Init marshals its work to core 0, that construction ran INSIDE the
// servo's call handler. The servo is the only thing that makes core 0 answer
// anybody — the console, the scheduler, the log drain, the watchdog and USB
// itself are all downstream of it — so a bring-up that took a long time took
// the whole machine with it, silently, with nothing left running to report it.
// The invariant this library documents is that nothing on the servo's path
// may block; a host controller constructed on the servo could never honour it.
//
// So the kernel builds it and this finds it, which is the same arrangement
// the serial console already has, in the other direction. Neither side
// constructs what the other owns.
//
// Found through the controller's own static accessor rather than the device
// name service: a host controller registers no name (its enumerated devices
// do — "ukbd1", "upad1", "umouse1", which is how the pumps below reach them),
// but every board's controller class answers IsActive()/Get(). CUSBHCIDevice
// is Circle's alias for whichever class that is on this board, so one
// spelling serves the Pi 3's DWHCI, the Pi 4's xHCI and the Pi 5's USB
// sub-system.
void SDL2Circle_InputInit(void)
{
    if (s_usb)
        return;

    if (CUSBHCIDevice::IsActive())
    {
        s_usb = CUSBHCIDevice::Get();
        return;
    }

    // No USB is not fatal on its own — headless and keyboardless payloads are
    // valid, and this library will not stop a board that is running fine
    // without input. But it is almost never intended, and the reason is not
    // guessable from a game that simply never responds to a key, so it is
    // said once and it names the fix.
    s_usb = USB_NONE;
    SDL2Circle_Log("input", SDL2CIRCLE_LOG_WARNING,
                   "no USB host controller on this board: input is off. "
                   "The host kernel brings USB up (a CUSBHCIDevice member "
                   "initialised in CKernel::Initialize); this library only "
                   "finds and pumps it.");

    // WITH INJECTION ARMED IT IS FATAL, and the reason is that this exact
    // pair hides itself.
    //
    // Injection does not go through USB. It reads a serial device and puts
    // events straight into the queue, so it works perfectly on a board where
    // no input device was ever enumerated. Every automated check therefore
    // passes: keys arrive, menus move, screenshots come out right. The board
    // is declared working and shipped, and the first person to plug a real
    // keyboard into it finds nothing happens — with the one line that
    // explained why scrolled off the console hours earlier.
    //
    // So a build that asks for robot hands and has no controller is refused
    // rather than run. Only the flag is set here: this runs on core 0 inside
    // a marshalled call, and stopping on core 0 would take the console, the
    // scheduler and the servo down with it and print nothing further. The
    // stop happens where the caller is, which is not core 0.
    if (SDL2Circle_DebugUartArmed())
        s_bNoInputFatal = true;
}

bool SDL2Circle_NoInputFatal(void)
{
    return s_bNoInputFatal;
}

// The refusal. Says what is wrong, keeps saying it, and never returns.
//
// SAYS IT FIRST AND REPEATS IT. A board that stops silently has traded a
// correct diagnosis for nothing at all, and one line at boot is very nearly
// as bad: whoever attaches a console afterwards finds a quiet board and no
// explanation. So the message goes out before anything stops, and goes out
// again for as long as the board is powered.
//
// KEEPS THE MACHINE ALIVE WHILE IT DOES. This is a halt, not a hang. On any
// core but 0 the wait is a plain spin on the free-running counter, and core 0
// carries on serving everything as usual. Called on core 0 — a payload with
// no split, or one that reached here before the split armed — it yields to
// the scheduler on every pass instead, so the console, the log drain and the
// watchdog all keep running. A stop that stops core 0 would print the second
// copy of its own message nowhere.
void SDL2Circle_NoInputHalt(void)
{
    for (unsigned nSaid = 0;; nSaid++)
    {
        SDL2Circle_Log("input", SDL2CIRCLE_LOG_ERROR,
                       "STOPPED: robot hands are armed (--rapi-debug-uart) and "
                       "this KERNEL never brought USB up, so no keyboard, mouse "
                       "or pad can ever work on it. Nothing here built a host "
                       "controller. Do not go looking at the board, the ports "
                       "or anything plugged in — the fault is not there. FIX: "
                       "give the host kernel a CUSBHCIDevice member and call "
                       "Initialize() on it in CKernel::Initialize, beside the "
                       "SD card and the console. Stopped rather than run "
                       "because injection does not go through USB: it would "
                       "have worked, every automated check would have passed, "
                       "and only a person with a real keyboard would ever have "
                       "found out. The application has not been started and "
                       "will not be.");

        const u64 nUntil = CTimer::GetClockTicks64()
                           + (u64) SDL2CIRCLE_NOINPUT_REPEAT_SECONDS * CLOCKHZ;
        while (CTimer::GetClockTicks64() < nUntil)
        {
            if (SDL2Circle_ThisCore() == 0 && CScheduler::IsActive())
                CScheduler::Get()->Yield();
        }
    }
}

void SDL2Circle_InputPump(void)
{
    if (!s_usb || s_usb == USB_NONE)
        return;

    boolean bChanged = s_usb->UpdatePlugAndPlay();

    // Gamepads and the mouse first, and unconditionally: the keyboard path
    // below returns early when no new key report has arrived, and another
    // device's attach, detach and report traffic has nothing to do with that.
    SDL2Circle_JoystickPump(bChanged != FALSE);
    SDL2Circle_MousePump(bChanged != FALSE);

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

// An OVERRIDE, for a kernel that wants injection to read from a device other
// than the console UART the library finds on its own. It wins because it is
// set before the first pump, which is the only place the library looks.
//
// A kernel that says nothing gets the console UART, so the old failure —
// declaring this function, meaning to call it, and never calling it — no
// longer costs anything: the switch on its own is now enough.
void SDL2Circle_SetInjectSerial(CSerialDevice *pSerial)
{
    s_injectSerial = pSerial;
    if (pSerial && !SDL2Circle_DebugUartArmed())
        SDL2Circle_Log("input", SDL2CIRCLE_LOG_DEBUG,
                       "serial lent for key injection; "
                       "stamp --rapi-debug-uart to arm it");
}

// Borrow the kernel's console UART from Circle's device name service.
//
// BORROW, never construct: CSerialDevice claims its slot in its constructor
// and asserts if the slot is taken, so a second one on device 0 would halt
// the board. The name is Circle's own — CSerialDevice::Initialize ends with
//
//     CDeviceNameService::Get ()->AddDevice ("ttyS", m_nDevice+1, this, FALSE)
//
// so serial device 0, the console the bench is wired to, is "ttyS1". Asked
// for by literal name rather than by prefix-and-index because the index form
// formats a CString, and this runs on the hardware core's servo.
//
// LOOKED UP LAZILY, at the first pump. The device registers itself when the
// kernel initialises it, which is long after the library first reads the boot
// argument block; looking at arming time would find nothing on a kernel that
// happened to parse its arguments first. By the first pump every device the
// kernel owns is up, whichever order it built them in.
static void ResolveInjectSerial(void)
{
    CDevice *pDevice = CDeviceNameService::Get()->GetDevice("ttyS1", FALSE);
    if (!pDevice)
        return;

    s_injectSerial = static_cast<CSerialDevice *>(pDevice);
    SDL2Circle_Log("input", SDL2CIRCLE_LOG_DEBUG,
                   "serial key injection reads the kernel's ttyS1");
}

void SDL2Circle_InjectPump(void)
{
    if (!SDL2Circle_DebugUartArmed())
        return;

    static bool bResolved = false;
    if (!bResolved)
    {
        bResolved = true;
        if (!s_injectSerial)
            ResolveInjectSerial();
    }

    // Armed, and there is no serial device to read — not through omission any
    // more, since the library looks for one itself, but because this kernel
    // has no console UART registered at all. Without this it says so nowhere:
    // the boot log carries "serial key injection armed" from the switch, and
    // then every command sent from the bench disappears with the machine
    // looking healthy — a game that draws, animates and answers nothing. Said
    // once, because this runs every pass.
    if (!s_injectSerial)
    {
        static bool bComplained = false;
        if (!bComplained)
        {
            bComplained = true;
            SDL2Circle_Log("input", SDL2CIRCLE_LOG_WARNING,
                           "--rapi-debug-uart is armed but there is no serial "
                           "device to read: no ttyS1 is registered and no kernel "
                           "lent one, so nothing sent to the console can reach "
                           "the application");
        }
        return;
    }

    // Drain serial RX, accumulating command lines; dispatch on each newline.
    char buf[64];
    int n = s_injectSerial->Read(buf, sizeof buf);
    for (int i = 0; i < n; i++)
    {
        char c = buf[i];
        if (c == '\n' || c == '\r')
        {
            s_injLine[s_injLineLen] = 0;
            InjectDispatch(s_injLine);
            s_injLineLen = 0;
        }
        else if (s_injLineLen < (int)sizeof(s_injLine) - 1)
        {
            s_injLine[s_injLineLen++] = c;
        }
        else
        {
            s_injLineLen = 0;   // overlong line: drop it rather than overflow
        }
    }

    u64 now = CTimer::GetClockTicks64();

    // Hold the current key until its wall-clock deadline, then release into a
    // gap; only start the next key once the gap has elapsed.
    if (s_injPhase == 1)                     // holding a key or button down
    {
        if (now < s_injUntil)
            return;
        if (s_injHeldButton)
        {
            InjectButton(s_injHeldButton, false);
            s_injHeldButton = 0;
        }
        else
        {
            PushKeyEvent(s_injHeldSc, false);
            if (s_injHeldShift)
                InjectShift(false);
            s_injHeldSc = SDL_SCANCODE_UNKNOWN;
        }
        s_injPhase = 2;
        s_injUntil = now + INJ_GAP_US;
        return;
    }
    if (s_injPhase == 2)                     // inter-key gap (key released)
    {
        if (now < s_injUntil)
            return;
        s_injPhase = 0;
    }

    // Idle: start the next queued keystroke.
    if (s_injCount > 0)
    {
        InjTap k = s_injQueue[s_injHead];
        s_injHead = (s_injHead + 1) % 64;
        s_injCount--;
        if (k.button)
        {
            InjectButton(k.mask, true);
            s_injHeldButton = k.mask;
        }
        else
        {
            if (k.shift)
                InjectShift(true);
            PushKeyEvent(k.sc, true);
            s_injHeldSc = k.sc;
            s_injHeldShift = k.shift;
        }
        s_injPhase = 1;
        s_injUntil = now + INJ_HOLD_US;
    }
}

// Core split: the application core mirrors keyboard state from the events it drains
// off the cross-core ring, so SDL_GetKeyboardState / SDL_GetModState answer
// from core-local memory. The producer-side state above keeps serving the
// diffing logic on core 0.
void SDL2Circle_ApplyEventState(const SDL_Event *ev)
{
    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP)
        return;
    SDL_Scancode sc = ev->key.keysym.scancode;
    if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES)
        return;
    s_keyState[sc] = (ev->type == SDL_KEYDOWN) ? 1 : 0;
    s_modState = ev->key.keysym.mod;
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

// SDL lets an application state the modifier state rather than only read it.
// An on-screen keyboard uses it to hold shift down while the physical
// keyboard has nothing held, and the next SDL_GetModState must report what
// was set. It stands until the next real modifier key changes it, which is
// SDL's behaviour too.
extern "C" void SDL_SetModState(SDL_Keymod modstate)
{
    s_modState = (Uint16)modstate;
}

extern "C" SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode)
{
    return KeycodeFor(scancode);
}

// An application turns text collection on before showing a text field and
// off afterwards, and asks whether it is on before drawing a caret. SDL
// starts with it on.
extern "C" void SDL_StartTextInput(void) { s_textInputActive = true; }
extern "C" void SDL_StopTextInput(void)  { s_textInputActive = false; }

extern "C" SDL_bool SDL_IsTextInputActive(void)
{
    return s_textInputActive ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_bool SDL_IsTextInputShown(void)
{
    // There is no on-screen keyboard to be shown.
    return SDL_FALSE;
}

extern "C" void SDL_ClearComposition(void) {}   // there is no IME to clear

// The one window always has the keyboard: there is nothing else on screen
// for focus to be anywhere else.
extern "C" SDL_Window *SDL_GetKeyboardFocus(void)
{
    return SDL_GetWindowFromID(1);
}

// Where an input method should put its candidate window. There is no input
// method, so the rectangle is noted and nothing is drawn from it.
extern "C" void SDL_SetTextInputRect(const SDL_Rect *) {}
