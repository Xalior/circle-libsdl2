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
#include <circle/serial.h>
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

// --- Debug UART key injection ------------------------------------------------
// Active only when the kernel hands us a serial (SetInjectSerial), which it
// does solely for --rapi-debug-uart. A serial console drives the emulated
// keyboard so the bench can dismiss a +3 Loader, type a C64 LOAD"...", or
// unlock MAME's UI (Scroll Lock) and open its menu (Tab).
//
// The wire format is a LINE-ORIENTED macro language (one command per '\n'):
//
//   <domain> <command...>
//
// The first token routes to a subsystem, so this is the shim's general
// robot-hands channel over SDL's input surface, not a keyboard hack — mouse,
// gamepad/joystick and whatever input device comes next each register a new
// domain (s_injectDomains, below) without touching the transport. Today one
// domain exists:
//
//   key down <key>     press and HOLD <key> (stays down; combine for chords)
//   key up   <key>     release <key>
//   key tap  <key>     self-timed press+release of one <key>
//   key type <text>    self-timed taps for each character of <text>
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
CSerialDevice *s_injectSerial = nullptr;

struct InjKey { SDL_Scancode sc; bool shift; };
InjKey s_injQueue[64];
unsigned s_injHead = 0;
unsigned s_injCount = 0;

char s_injLine[128];           // accumulates serial RX until a '\n'
int s_injLineLen = 0;

SDL_Scancode s_injHeldSc = SDL_SCANCODE_UNKNOWN;
bool s_injHeldShift = false;
int s_injPhase = 0;            // 0 idle, 1 holding a key, 2 inter-key gap
u64 s_injUntil = 0;           // wall-clock (us) the current phase ends at

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
        s_injQueue[(s_injHead + s_injCount) % 64] = InjKey{sc, shift};
        s_injCount++;
    }
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

// The robot-hands domain table. New subsystems (mouse, pad, grab, ...) register
// here; the transport and line parser never change.
struct InjectDomain { const char *name; void (*fn)(char *args); };
const InjectDomain s_injectDomains[] = {
    {"key", InjectKeyCmd},
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

void SDL2Circle_SetInjectSerial(CSerialDevice *pSerial)
{
    s_injectSerial = pSerial;
}

void SDL2Circle_InjectPump(void)
{
    if (!s_injectSerial)
        return;

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
    if (s_injPhase == 1)                     // holding a key down
    {
        if (now < s_injUntil)
            return;
        PushKeyEvent(s_injHeldSc, false);
        if (s_injHeldShift)
            InjectShift(false);
        s_injHeldSc = SDL_SCANCODE_UNKNOWN;
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
        InjKey k = s_injQueue[s_injHead];
        s_injHead = (s_injHead + 1) % 64;
        s_injCount--;
        if (k.shift)
            InjectShift(true);
        PushKeyEvent(k.sc, true);
        s_injHeldSc = k.sc;
        s_injHeldShift = k.shift;
        s_injPhase = 1;
        s_injUntil = now + INJ_HOLD_US;
    }
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
