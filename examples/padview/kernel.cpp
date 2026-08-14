//
// kernel.cpp — every attached gamepad, joystick and wheel on screen at once,
// through the SDL2 API only.
//
// What it puts on the display, per attached device:
//
//   the SDL device index, the Circle device name, the SDL instance ID
//   whether a game-controller mapping was found for it, and its name
//   the joystick GUID, and the USB vendor, product and version behind it
//   one bar per axis, tracking the stick or wheel live
//   one lit cell per hat direction
//   one lit square per button
//   for a recognised controller, the mapped axes and buttons as well
//
// Under all of that is a plug log: every attach and detach, with the time it
// happened, so pulling a device out and putting it back is visible on screen
// and not only in the serial console.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

static const char From[] = "padview";

// Drawn at 1280x720 and scaled to whatever the display really is, so the text
// stays chunky enough to read across a room.
// The virtual display, settled at start-up from the physical one.
static int W = 0, H = 0;
// The physical display, asked of the firmware directly. This is the
// EXAMPLE's own query, not something the library provides: the library is
// told what virtual display to present and never goes looking for one, so a
// consumer that wants the two to match asks for itself. This is the whole of
// what that takes, and it uses nothing but Circle's public property tags.
//
// FALSE when the firmware will not answer.
static boolean PhysicalDisplaySize(int *pWidth, int *pHeight)
{
    CBcmPropertyTags Tags;
    TPropertyTagDisplayDimensions Dim;
    memset(&Dim, 0, sizeof Dim);
    if (!Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim))
        return FALSE;
    if (Dim.nWidth == 0 || Dim.nHeight == 0)
        return FALSE;
    *pWidth  = (int) Dim.nWidth;
    *pHeight = (int) Dim.nHeight;
    return TRUE;
}


static const Uint32 COL_BG      = 0xFF0C0C12;
static const Uint32 COL_PANEL   = 0xFF181822;
static const Uint32 COL_RULE    = 0xFF303040;
static const Uint32 COL_TEXT    = 0xFFC8C8D0;
static const Uint32 COL_BRIGHT  = 0xFFFFFFFF;
static const Uint32 COL_DIM     = 0xFF606070;
static const Uint32 COL_ACCENT  = 0xFF00D0FF;
static const Uint32 COL_ON      = 0xFF30E060;
static const Uint32 COL_OFF     = 0xFF262632;
static const Uint32 COL_WARN    = 0xFFFFC020;
static const Uint32 COL_BAD     = 0xFFE04040;

// ---------------------------------------------------------------------------
// A 5x7 pixel font, printable ASCII. Each glyph is seven rows; within a row
// bit 0x10 is the leftmost of five pixels.
// ---------------------------------------------------------------------------

static const unsigned char FONT[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // !
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, // "
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, // #
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // $
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // %
    {0x08,0x14,0x14,0x08,0x15,0x12,0x0D}, // &
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, // '
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // (
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // )
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // *
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, // ,
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, // /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, // ;
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // <
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // =
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // >
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // ?
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, // @
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // [
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, // backslash
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ]
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // _
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // a
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, // b
    {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, // c
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, // d
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // e
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, // f
    {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, // g
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, // h
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // i
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, // j
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // k
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, // l
    {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}, // m
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, // n
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // o
    {0x00,0x1E,0x11,0x11,0x1E,0x10,0x10}, // p
    {0x00,0x0F,0x11,0x11,0x0F,0x01,0x01}, // q
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, // r
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, // s
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, // t
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, // v
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, // x
    {0x00,0x11,0x11,0x11,0x0F,0x01,0x0E}, // y
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, // z
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, // {
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, // |
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, // }
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, // ~
};

// ---------------------------------------------------------------------------
// Drawing, straight into a locked streaming texture — the only drawing route
// this shim implements.
// ---------------------------------------------------------------------------

static Uint32 *s_pix;
static int s_pitch32;

static void fill(int x, int y, int w, int h, Uint32 c)
{
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = y; j < y + h; j++)
    {
        Uint32 *row = s_pix + (size_t)j * s_pitch32;
        for (int i = x; i < x + w; i++)
            row[i] = c;
    }
}

static void frame(int x, int y, int w, int h, Uint32 c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

static void glyph(char ch, int x, int y, int scale, Uint32 c)
{
    if (ch < 0x20 || ch > 0x7E)
        ch = '?';
    const unsigned char *g = FONT[ch - 0x20];
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col))
                fill(x + col * scale, y + row * scale, scale, scale, c);
}

static void text(int x, int y, int scale, Uint32 c, const char *s)
{
    for (; *s; s++, x += 6 * scale)
        glyph(*s, x, y, scale, c);
}

static int textw(int scale, const char *s)
{
    return (int)strlen(s) * 6 * scale;
}

// ---------------------------------------------------------------------------
// Devices this viewer has opened, keyed by SDL instance ID.
// ---------------------------------------------------------------------------

static const int MAX_OPEN = 8;

struct OpenDevice
{
    SDL_JoystickID      instance;
    SDL_Joystick       *joy;
    SDL_GameController *ctrl;
};

static OpenDevice s_open[MAX_OPEN];

static OpenDevice *FindOpen(SDL_JoystickID instance)
{
    for (int i = 0; i < MAX_OPEN; i++)
        if (s_open[i].joy && s_open[i].instance == instance)
            return &s_open[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// The plug log: the last few attach and detach events, kept on screen.
// ---------------------------------------------------------------------------

static const int LOG_LINES = 5;

struct LogLine
{
    char text[80];
    Uint32 colour;
    bool used;
};

static LogLine s_log[LOG_LINES];
static unsigned s_logNext = 0;
static unsigned s_attachCount = 0, s_detachCount = 0;

static void logline(Uint32 colour, const char *fmt, ...)
{
    LogLine &l = s_log[s_logNext % LOG_LINES];
    s_logNext++;

    unsigned secs = (unsigned)(SDL_GetTicks64() / 1000);
    int n = snprintf(l.text, sizeof l.text, "%4u:%02u  ", secs / 60, secs % 60);

    va_list args;
    va_start(args, fmt);
    vsnprintf(l.text + n, sizeof l.text - n, fmt, args);
    va_end(args);

    l.colour = colour;
    l.used = true;
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

// A signed axis reading as a bar with its rest point marked in the middle.
static void axisbar(int x, int y, int w, int h, Sint16 value)
{
    fill(x, y, w, h, COL_OFF);
    frame(x, y, w, h, COL_RULE);

    int mid = x + w / 2;
    fill(mid, y, 1, h, COL_DIM);

    int span = (int)((long)value * (w / 2 - 2) / 32767L);
    if (span >= 0)
        fill(mid, y + 2, span + 1, h - 4, COL_ACCENT);
    else
        fill(mid + span, y + 2, -span + 1, h - 4, COL_ACCENT);
}

static void drawHat(int x, int y, Uint8 value)
{
    static const int cell = 26;
    static const Uint8 map[3][3] = {
        {SDL_HAT_LEFTUP,   SDL_HAT_UP,   SDL_HAT_RIGHTUP},
        {SDL_HAT_LEFT,     0,            SDL_HAT_RIGHT},
        {SDL_HAT_LEFTDOWN, SDL_HAT_DOWN, SDL_HAT_RIGHTDOWN},
    };

    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
        {
            Uint8 want = map[r][c];
            bool lit = want && (value & want) == want;
            fill(x + c * cell, y + r * cell, cell - 3, cell - 3,
                 want == 0 ? COL_RULE : (lit ? COL_ON : COL_OFF));
        }
}

// One device, drawn into the box at (px, py, pw, ph).
static void drawDevice(int index, int px, int py, int pw, int ph)
{
    fill(px, py, pw, ph, COL_PANEL);
    frame(px, py, pw, ph, COL_RULE);

    SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
    OpenDevice *dev = FindOpen(instance);

    const char *name = SDL_JoystickNameForIndex(index);
    const char *path = SDL_JoystickPathForIndex(index);
    if (!name) name = "(unnamed)";
    if (!path) path = "?";

    char line[128];
    int x = px + 14, y = py + 10;

    snprintf(line, sizeof line, "#%d  %s  instance %d", index, path, (int)instance);
    text(x, y, 3, COL_BRIGHT, line);

    bool isctrl = SDL_IsGameController(index) == SDL_TRUE;
    const char *tag = isctrl ? " GAME CONTROLLER " : " JOYSTICK ONLY ";
    int tw = textw(2, tag);
    fill(px + pw - tw - 20, y, tw + 8, 22, isctrl ? COL_ON : COL_WARN);
    text(px + pw - tw - 16, y + 4, 2, 0xFF000000, tag);

    y += 28;
    text(x, y, 2, COL_TEXT, name);

    y += 20;
    char guid[40];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
    snprintf(line, sizeof line, "guid %s   usb %04x:%04x v%04x", guid,
             SDL_JoystickGetDeviceVendor(index),
             SDL_JoystickGetDeviceProduct(index),
             SDL_JoystickGetDeviceProductVersion(index));
    text(x, y, 2, COL_DIM, line);

    if (!dev || !dev->joy)
    {
        text(x, y + 24, 2, COL_BAD, "not opened");
        return;
    }

    int naxes    = SDL_JoystickNumAxes(dev->joy);
    int nhats    = SDL_JoystickNumHats(dev->joy);
    int nbuttons = SDL_JoystickNumButtons(dev->joy);

    // Axes, one bar each, down the left.
    int ay = y + 24;
    for (int a = 0; a < naxes && a < 6; a++)
    {
        Sint16 v = SDL_JoystickGetAxis(dev->joy, a);
        snprintf(line, sizeof line, "a%d", a);
        text(x, ay + 2, 2, COL_TEXT, line);
        axisbar(x + 34, ay, 300, 18, v);
        snprintf(line, sizeof line, "%6d", (int)v);
        text(x + 344, ay + 2, 2, COL_TEXT, line);
        ay += 22;
    }
    if (naxes == 0)
        text(x, ay + 2, 2, COL_DIM, "no axes");

    // Hats, middle.
    int hx = px + 440, hy = y + 26;
    text(hx, hy - 16, 2, COL_TEXT, "hats");
    for (int h = 0; h < nhats && h < 2; h++)
    {
        drawHat(hx + h * 92, hy, SDL_JoystickGetHat(dev->joy, h));
    }
    if (nhats == 0)
        text(hx, hy, 2, COL_DIM, "none");

    // Buttons, right, eight to a row, numbered as SDL numbers them: from
    // zero. These are the indices SDL_JoystickGetButton takes and the ones a
    // configuration file has to name, so the number read off the screen is
    // the number that gets written down — the axes above are labelled the
    // same way for the same reason.
    int bx = px + 640, by = y + 26;
    snprintf(line, sizeof line, "buttons (%d)", nbuttons);
    text(bx, by - 16, 2, COL_TEXT, line);
    for (int b = 0; b < nbuttons && b < 32; b++)
    {
        int cx = bx + (b % 8) * 34;
        int cy = by + (b / 8) * 30;
        bool down = SDL_JoystickGetButton(dev->joy, b) != 0;
        fill(cx, cy, 30, 26, down ? COL_ON : COL_OFF);
        snprintf(line, sizeof line, "%d", b);
        text(cx + 15 - textw(2, line) / 2, cy + 6, 2, down ? 0xFF000000 : COL_DIM, line);
    }

    // The mapped view, for a device the database recognised.
    int cy = py + ph - 46;
    if (dev->ctrl)
    {
        static const char *kAxis[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
        int cx = x;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        {
            snprintf(line, sizeof line, "%s%6d", kAxis[a],
                     (int)SDL_GameControllerGetAxis(dev->ctrl,
                                                    (SDL_GameControllerAxis)a));
            text(cx, cy, 2, COL_ACCENT, line);
            cx += textw(2, line) + 14;
        }

        cy += 20;
        cx = x;
        static const char *kBtn[] = {
            "A","B","X","Y","BK","GD","ST","LS","RS","LB","RB",
            "UP","DN","LF","RT","M1","P1","P2","P3","P4","TP"
        };
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        {
            bool down = SDL_GameControllerGetButton(dev->ctrl,
                                                    (SDL_GameControllerButton)b) != 0;
            text(cx, cy, 2, down ? COL_ON : COL_OFF, kBtn[b]);
            cx += textw(2, kBtn[b]) + 8;
        }
    }
    else
    {
        text(x, cy + 10, 2, COL_DIM,
             "no mapping in the database - raw axes and buttons only");
    }
}

// ---------------------------------------------------------------------------

CKernel::CKernel(void)
    // Serial device 0 is the GPIO14/15 header UART on every board. Named
    // explicitly because Circle's RASPPI >= 5 default (SERIAL_DEVICE_DEFAULT
    // = 10) is the Pi 5's dedicated debug connector, so taking the default
    // sends every log line somewhere nobody is listening.
    : m_Serial(0, FALSE, 0),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer)
{
    m_ActLED.Blink(3);
}

boolean CKernel::Initialize(void)
{
    boolean bOK = TRUE;
    if (bOK) bOK = m_Serial.Initialize(115200);
    if (bOK) bOK = m_Logger.Initialize(&m_Serial);
    if (bOK) bOK = m_Interrupt.Initialize();
    if (bOK) bOK = m_Timer.Initialize();
    if (bOK) SDL2Circle_ArmCoreRuntime();
    return bOK;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice, "circle-libsdl2 padview test");

    // Match the virtual display to the physical one: ask the firmware what
    // the panel is, then declare that. Nothing requires this call - the
    // library falls back to the same physical size on its own, as a last
    // resort - but asking and declaring explicitly is what this example is
    // for, so it stops here rather than letting a firmware that will not
    // answer surface later as someone else's failure.
    if (!PhysicalDisplaySize(&W, &H))
    {
        m_Logger.Write(From, LogError,
                       "the firmware will not report the display size");
        return ShutdownHalt;
    }
    if (SDL2Circle_DeclareVirtualDevice(32, W, H) != 0)
    {
        m_Logger.Write(From, LogError, "virtual device: %s", SDL_GetError());
        return ShutdownHalt;
    }
    m_Logger.Write(From, LogNotice, "virtual display %dx%d, matching the panel",
                   W, H);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    // A controller database if the boot medium carries one. Without it every
    // pad is still a working joystick; only the mapped view goes away.
    int nMappings = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    m_Logger.Write(From, LogNotice, "controller mappings loaded: %d", nMappings);

    SDL_Window *win = SDL_CreateWindow("padview", 0, 0, W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               W, H) : nullptr;
    if (!tex)
    {
        m_Logger.Write(From, LogError, "video: %s", SDL_GetError());
        return ShutdownHalt;
    }

    logline(COL_DIM, "waiting for a device");

    unsigned nEvents = 0;

    for (;;)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_JOYDEVICEADDED:
            {
                // ADDED carries a DEVICE INDEX, which is a position in the
                // current list and not an identity.
                int index = ev.jdevice.which;
                SDL_Joystick *joy = SDL_JoystickOpen(index);
                if (!joy)
                {
                    logline(COL_BAD, "open failed: %s", SDL_GetError());
                    break;
                }

                SDL_JoystickID instance = SDL_JoystickInstanceID(joy);
                OpenDevice *slot = nullptr;
                for (int i = 0; i < MAX_OPEN && !slot; i++)
                    if (!s_open[i].joy)
                        slot = &s_open[i];
                if (!slot)
                {
                    SDL_JoystickClose(joy);
                    logline(COL_BAD, "too many devices");
                    break;
                }

                slot->instance = instance;
                slot->joy      = joy;
                slot->ctrl     = SDL_IsGameController(index)
                                     ? SDL_GameControllerOpen(index) : nullptr;

                s_attachCount++;
                logline(COL_ON, "+ %s  \"%s\"  %s",
                        SDL_JoystickPathForIndex(index),
                        SDL_JoystickName(joy),
                        slot->ctrl ? "mapped controller" : "raw joystick");
                m_Logger.Write(From, LogNotice, "attached #%d \"%s\" instance %d",
                               index, SDL_JoystickName(joy), (int)instance);
                break;
            }

            case SDL_JOYDEVICEREMOVED:
            {
                // REMOVED carries an INSTANCE ID, which is the identity.
                OpenDevice *slot = FindOpen(ev.jdevice.which);
                if (!slot)
                    break;
                if (slot->ctrl)
                    SDL_GameControllerClose(slot->ctrl);
                SDL_JoystickClose(slot->joy);
                slot->joy = nullptr;
                slot->ctrl = nullptr;

                s_detachCount++;
                logline(COL_BAD, "- device gone, instance %d",
                        (int)ev.jdevice.which);
                m_Logger.Write(From, LogNotice, "detached instance %d",
                               (int)ev.jdevice.which);
                break;
            }

            case SDL_JOYAXISMOTION:
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
            case SDL_JOYHATMOTION:
            case SDL_CONTROLLERAXISMOTION:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                nEvents++;
                break;

            default:
                break;
            }
        }

        void *pixels;
        int pitch;
        SDL_LockTexture(tex, nullptr, &pixels, &pitch);
        s_pix = (Uint32 *)pixels;
        s_pitch32 = pitch / 4;

        fill(0, 0, W, H, COL_BG);

        // Header, with a sweep along the very top edge so a photograph shows
        // the loop is running.
        fill((int)((SDL_GetTicks64() / 4) % W), 0, 40, 6, COL_ACCENT);

        int ndev = SDL_NumJoysticks();
        char line[128];
        text(16, 14, 3, COL_ACCENT, "PADVIEW");
        snprintf(line, sizeof line,
                 "devices %d   mappings %d   attach %u  detach %u   events %u",
                 ndev, SDL_GameControllerNumMappings(), s_attachCount,
                 s_detachCount, nEvents);
        text(160, 18, 2, COL_TEXT, line);
        fill(16, 42, W - 32, 1, COL_RULE);

        const int PANEL_Y = 50, PANEL_H = 250;
        const int MAX_PANELS = 2;

        if (ndev == 0)
        {
            fill(16, PANEL_Y, W - 32, PANEL_H, COL_PANEL);
            frame(16, PANEL_Y, W - 32, PANEL_H, COL_RULE);
            text(W / 2 - textw(4, "NO DEVICES ATTACHED") / 2,
                 PANEL_Y + 90, 4, COL_WARN, "NO DEVICES ATTACHED");
            text(W / 2 - textw(2, "plug a gamepad, joystick or wheel into any USB port") / 2,
                 PANEL_Y + 140, 2, COL_DIM,
                 "plug a gamepad, joystick or wheel into any USB port");
        }
        else
        {
            for (int i = 0; i < ndev && i < MAX_PANELS; i++)
                drawDevice(i, 16, PANEL_Y + i * (PANEL_H + 8), W - 32, PANEL_H);
        }

        // Plug log.
        int ly = PANEL_Y + MAX_PANELS * (PANEL_H + 8) + 4;
        fill(16, ly, W - 32, H - ly - 12, COL_PANEL);
        frame(16, ly, W - 32, H - ly - 12, COL_RULE);
        text(30, ly + 8, 2, COL_TEXT, "PLUG LOG");

        for (int i = 0; i < LOG_LINES; i++)
        {
            // Oldest first, so the newest line is always at the bottom.
            unsigned idx = (s_logNext + i) % LOG_LINES;
            if (!s_log[idx].used)
                continue;
            text(30, ly + 30 + i * 18, 2, s_log[idx].colour, s_log[idx].text);
        }

        if (ndev > MAX_PANELS)
        {
            snprintf(line, sizeof line, "%d more device(s) attached, not shown",
                     ndev - MAX_PANELS);
            text(W - 16 - textw(2, line) - 14, ly + 8, 2, COL_WARN, line);
        }

        SDL_UnlockTexture(tex);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
}
