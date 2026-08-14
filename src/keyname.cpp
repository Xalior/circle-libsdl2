//
// keyname.cpp - the names SDL gives keys, and the keys it gives names back.
//
// Applications use these to show a key binding to a person and to read one
// back out of a configuration file, so the names have to be SDL's own: a
// file written by a desktop build has to be readable here, and one written
// here readable there.
//
// The scancode names are therefore the canonical SDL2 strings, and the two
// keycode functions are built on them exactly as SDL2 builds them - a key
// carrying the scancode mask is named by its scancode, and anything else is
// a Unicode code point named by its own character in upper case.
//
#include <SDL2/SDL.h>

#include <cstring>

namespace
{

// SDL2's canonical name for each scancode. A scancode with no name is not an
// error: SDL answers with the empty string, and so does this.
const char *ScancodeName(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_A: return "A";
    case SDL_SCANCODE_B: return "B";
    case SDL_SCANCODE_C: return "C";
    case SDL_SCANCODE_D: return "D";
    case SDL_SCANCODE_E: return "E";
    case SDL_SCANCODE_F: return "F";
    case SDL_SCANCODE_G: return "G";
    case SDL_SCANCODE_H: return "H";
    case SDL_SCANCODE_I: return "I";
    case SDL_SCANCODE_J: return "J";
    case SDL_SCANCODE_K: return "K";
    case SDL_SCANCODE_L: return "L";
    case SDL_SCANCODE_M: return "M";
    case SDL_SCANCODE_N: return "N";
    case SDL_SCANCODE_O: return "O";
    case SDL_SCANCODE_P: return "P";
    case SDL_SCANCODE_Q: return "Q";
    case SDL_SCANCODE_R: return "R";
    case SDL_SCANCODE_S: return "S";
    case SDL_SCANCODE_T: return "T";
    case SDL_SCANCODE_U: return "U";
    case SDL_SCANCODE_V: return "V";
    case SDL_SCANCODE_W: return "W";
    case SDL_SCANCODE_X: return "X";
    case SDL_SCANCODE_Y: return "Y";
    case SDL_SCANCODE_Z: return "Z";

    case SDL_SCANCODE_1: return "1";
    case SDL_SCANCODE_2: return "2";
    case SDL_SCANCODE_3: return "3";
    case SDL_SCANCODE_4: return "4";
    case SDL_SCANCODE_5: return "5";
    case SDL_SCANCODE_6: return "6";
    case SDL_SCANCODE_7: return "7";
    case SDL_SCANCODE_8: return "8";
    case SDL_SCANCODE_9: return "9";
    case SDL_SCANCODE_0: return "0";

    case SDL_SCANCODE_RETURN:       return "Return";
    case SDL_SCANCODE_ESCAPE:       return "Escape";
    case SDL_SCANCODE_BACKSPACE:    return "Backspace";
    case SDL_SCANCODE_TAB:          return "Tab";
    case SDL_SCANCODE_SPACE:        return "Space";
    case SDL_SCANCODE_MINUS:        return "-";
    case SDL_SCANCODE_EQUALS:       return "=";
    case SDL_SCANCODE_LEFTBRACKET:  return "[";
    case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_BACKSLASH:    return "\\";
    case SDL_SCANCODE_NONUSHASH:    return "#";
    case SDL_SCANCODE_SEMICOLON:    return ";";
    case SDL_SCANCODE_APOSTROPHE:   return "'";
    case SDL_SCANCODE_GRAVE:        return "`";
    case SDL_SCANCODE_COMMA:        return ",";
    case SDL_SCANCODE_PERIOD:       return ".";
    case SDL_SCANCODE_SLASH:        return "/";
    case SDL_SCANCODE_CAPSLOCK:     return "CapsLock";

    case SDL_SCANCODE_F1:  return "F1";
    case SDL_SCANCODE_F2:  return "F2";
    case SDL_SCANCODE_F3:  return "F3";
    case SDL_SCANCODE_F4:  return "F4";
    case SDL_SCANCODE_F5:  return "F5";
    case SDL_SCANCODE_F6:  return "F6";
    case SDL_SCANCODE_F7:  return "F7";
    case SDL_SCANCODE_F8:  return "F8";
    case SDL_SCANCODE_F9:  return "F9";
    case SDL_SCANCODE_F10: return "F10";
    case SDL_SCANCODE_F11: return "F11";
    case SDL_SCANCODE_F12: return "F12";
    case SDL_SCANCODE_F13: return "F13";
    case SDL_SCANCODE_F14: return "F14";
    case SDL_SCANCODE_F15: return "F15";
    case SDL_SCANCODE_F16: return "F16";
    case SDL_SCANCODE_F17: return "F17";
    case SDL_SCANCODE_F18: return "F18";
    case SDL_SCANCODE_F19: return "F19";
    case SDL_SCANCODE_F20: return "F20";
    case SDL_SCANCODE_F21: return "F21";
    case SDL_SCANCODE_F22: return "F22";
    case SDL_SCANCODE_F23: return "F23";
    case SDL_SCANCODE_F24: return "F24";

    case SDL_SCANCODE_PRINTSCREEN: return "PrintScreen";
    case SDL_SCANCODE_SCROLLLOCK:  return "ScrollLock";
    case SDL_SCANCODE_PAUSE:       return "Pause";
    case SDL_SCANCODE_INSERT:      return "Insert";
    case SDL_SCANCODE_HOME:        return "Home";
    case SDL_SCANCODE_PAGEUP:      return "PageUp";
    case SDL_SCANCODE_DELETE:      return "Delete";
    case SDL_SCANCODE_END:         return "End";
    case SDL_SCANCODE_PAGEDOWN:    return "PageDown";
    case SDL_SCANCODE_RIGHT:       return "Right";
    case SDL_SCANCODE_LEFT:        return "Left";
    case SDL_SCANCODE_DOWN:        return "Down";
    case SDL_SCANCODE_UP:          return "Up";

    case SDL_SCANCODE_NUMLOCKCLEAR: return "Numlock";
    case SDL_SCANCODE_KP_DIVIDE:    return "Keypad /";
    case SDL_SCANCODE_KP_MULTIPLY:  return "Keypad *";
    case SDL_SCANCODE_KP_MINUS:     return "Keypad -";
    case SDL_SCANCODE_KP_PLUS:      return "Keypad +";
    case SDL_SCANCODE_KP_ENTER:     return "Keypad Enter";
    case SDL_SCANCODE_KP_1:         return "Keypad 1";
    case SDL_SCANCODE_KP_2:         return "Keypad 2";
    case SDL_SCANCODE_KP_3:         return "Keypad 3";
    case SDL_SCANCODE_KP_4:         return "Keypad 4";
    case SDL_SCANCODE_KP_5:         return "Keypad 5";
    case SDL_SCANCODE_KP_6:         return "Keypad 6";
    case SDL_SCANCODE_KP_7:         return "Keypad 7";
    case SDL_SCANCODE_KP_8:         return "Keypad 8";
    case SDL_SCANCODE_KP_9:         return "Keypad 9";
    case SDL_SCANCODE_KP_0:         return "Keypad 0";
    case SDL_SCANCODE_KP_PERIOD:    return "Keypad .";
    case SDL_SCANCODE_KP_EQUALS:    return "Keypad =";
    case SDL_SCANCODE_KP_COMMA:     return "Keypad ,";

    case SDL_SCANCODE_NONUSBACKSLASH: return "\\";
    case SDL_SCANCODE_APPLICATION:    return "Application";
    case SDL_SCANCODE_POWER:          return "Power";
    case SDL_SCANCODE_EXECUTE:        return "Execute";
    case SDL_SCANCODE_HELP:           return "Help";
    case SDL_SCANCODE_MENU:           return "Menu";
    case SDL_SCANCODE_SELECT:         return "Select";
    case SDL_SCANCODE_STOP:           return "Stop";
    case SDL_SCANCODE_AGAIN:          return "Again";
    case SDL_SCANCODE_UNDO:           return "Undo";
    case SDL_SCANCODE_CUT:            return "Cut";
    case SDL_SCANCODE_COPY:           return "Copy";
    case SDL_SCANCODE_PASTE:          return "Paste";
    case SDL_SCANCODE_FIND:           return "Find";
    case SDL_SCANCODE_MUTE:           return "Mute";
    case SDL_SCANCODE_VOLUMEUP:       return "VolumeUp";
    case SDL_SCANCODE_VOLUMEDOWN:     return "VolumeDown";

    case SDL_SCANCODE_LCTRL:  return "Left Ctrl";
    case SDL_SCANCODE_LSHIFT: return "Left Shift";
    case SDL_SCANCODE_LALT:   return "Left Alt";
    case SDL_SCANCODE_LGUI:   return "Left GUI";
    case SDL_SCANCODE_RCTRL:  return "Right Ctrl";
    case SDL_SCANCODE_RSHIFT: return "Right Shift";
    case SDL_SCANCODE_RALT:   return "Right Alt";
    case SDL_SCANCODE_RGUI:   return "Right GUI";
    case SDL_SCANCODE_MODE:   return "ModeSwitch";

    case SDL_SCANCODE_AUDIONEXT:  return "AudioNext";
    case SDL_SCANCODE_AUDIOPREV:  return "AudioPrev";
    case SDL_SCANCODE_AUDIOSTOP:  return "AudioStop";
    case SDL_SCANCODE_AUDIOPLAY:  return "AudioPlay";
    case SDL_SCANCODE_AUDIOMUTE:  return "AudioMute";
    case SDL_SCANCODE_MEDIASELECT: return "MediaSelect";
    case SDL_SCANCODE_WWW:        return "WWW";
    case SDL_SCANCODE_MAIL:       return "Mail";
    case SDL_SCANCODE_CALCULATOR: return "Calculator";
    case SDL_SCANCODE_COMPUTER:   return "Computer";
    case SDL_SCANCODE_AC_SEARCH:  return "AC Search";
    case SDL_SCANCODE_AC_HOME:    return "AC Home";
    case SDL_SCANCODE_AC_BACK:    return "AC Back";
    case SDL_SCANCODE_AC_FORWARD: return "AC Forward";
    case SDL_SCANCODE_AC_STOP:    return "AC Stop";
    case SDL_SCANCODE_AC_REFRESH: return "AC Refresh";
    case SDL_SCANCODE_AC_BOOKMARKS: return "AC Bookmarks";

    default: return "";
    }
}

// One code point as UTF-8, which is how SDL names a printable key. Returns
// the number of bytes written; the caller terminates.
int Utf8Encode(Uint32 cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// The first code point of a UTF-8 string, for turning a one-character name
// back into the key it names. Returns 0 on a malformed sequence.
Uint32 Utf8Decode(const char *s, int *used)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80)              { *used = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0)    { *used = 2; return ((Uint32)(p[0] & 0x1F) << 6)
                                                    | (p[1] & 0x3F); }
    if ((p[0] & 0xF0) == 0xE0)    { *used = 3; return ((Uint32)(p[0] & 0x0F) << 12)
                                                    | ((Uint32)(p[1] & 0x3F) << 6)
                                                    | (p[2] & 0x3F); }
    if ((p[0] & 0xF8) == 0xF0)    { *used = 4; return ((Uint32)(p[0] & 0x07) << 18)
                                                    | ((Uint32)(p[1] & 0x3F) << 12)
                                                    | ((Uint32)(p[2] & 0x3F) << 6)
                                                    | (p[3] & 0x3F); }
    *used = 1;
    return 0;
}

} // namespace

extern "C" const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES)
    {
        SDL_InvalidParamError("scancode");
        return "";
    }
    return ScancodeName(scancode);
}

extern "C" SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
    if (!name || !*name)
    {
        SDL_InvalidParamError("name");
        return SDL_SCANCODE_UNKNOWN;
    }
    for (int i = 0; i < SDL_NUM_SCANCODES; i++)
    {
        const char *n = ScancodeName((SDL_Scancode)i);
        if (*n && SDL_strcasecmp(n, name) == 0)
            return (SDL_Scancode)i;
    }
    SDL_SetError("SDL_GetScancodeFromName: no key named `%s`", name);
    return SDL_SCANCODE_UNKNOWN;
}

// SDL2's own construction: a key carrying the scancode mask is named by its
// scancode, the handful of control keys with ASCII keycodes are named by
// theirs, and everything else is a code point named by its own character in
// upper case.
extern "C" const char *SDL_GetKeyName(SDL_Keycode key)
{
    // One buffer, overwritten by each call, as SDL2's is. A caller wanting
    // to keep the text copies it before asking about another key.
    static char s_name[8];

    if (key & SDLK_SCANCODE_MASK)
        return SDL_GetScancodeName((SDL_Scancode)(key & ~SDLK_SCANCODE_MASK));

    switch (key)
    {
    case SDLK_UNKNOWN:   return "";
    case SDLK_RETURN:    return SDL_GetScancodeName(SDL_SCANCODE_RETURN);
    case SDLK_ESCAPE:    return SDL_GetScancodeName(SDL_SCANCODE_ESCAPE);
    case SDLK_BACKSPACE: return SDL_GetScancodeName(SDL_SCANCODE_BACKSPACE);
    case SDLK_TAB:       return SDL_GetScancodeName(SDL_SCANCODE_TAB);
    case SDLK_SPACE:     return SDL_GetScancodeName(SDL_SCANCODE_SPACE);
    case SDLK_DELETE:    return SDL_GetScancodeName(SDL_SCANCODE_DELETE);
    default:             break;
    }

    // Letter keys are labelled in upper case on the keyboards this runs on.
    Uint32 cp = (Uint32)key;
    if (cp >= 'a' && cp <= 'z')
        cp -= 32;

    const int n = Utf8Encode(cp, s_name);
    s_name[n] = '\0';
    return s_name;
}

extern "C" SDL_Keycode SDL_GetKeyFromName(const char *name)
{
    if (!name || !*name)
    {
        SDL_InvalidParamError("name");
        return SDLK_UNKNOWN;
    }

    // A name SDL would have produced from a scancode resolves through the
    // scancode, so that the keycode matches what this keyboard reports.
    const SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN)
        return SDL_GetKeyFromScancode(sc);

    // Otherwise a single character names itself. SDL keycodes for letters
    // are lower case, so an upper-case name comes back down.
    int used = 0;
    Uint32 cp = Utf8Decode(name, &used);
    if (cp == 0 || name[used] != '\0')
    {
        SDL_SetError("SDL_GetKeyFromName: no key named `%s`", name);
        return SDLK_UNKNOWN;
    }
    if (cp >= 'A' && cp <= 'Z')
        cp += 32;
    return (SDL_Keycode)cp;
}

// The reverse of SDL_GetKeyFromScancode, over the scancodes this keyboard
// can produce. Asked through the public function so the two can never
// disagree about the layout.
extern "C" SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key)
{
    if (key == SDLK_UNKNOWN)
        return SDL_SCANCODE_UNKNOWN;
    for (int i = 0; i < SDL_NUM_SCANCODES; i++)
        if (SDL_GetKeyFromScancode((SDL_Scancode)i) == key)
            return (SDL_Scancode)i;
    return SDL_SCANCODE_UNKNOWN;
}
