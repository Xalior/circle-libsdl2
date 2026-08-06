//
// kernel.cpp — USB mouse on screen, via the SDL2 API only.
//
// What it proves, and what to look at while it runs:
//
//   the pointer      a crosshair at SDL_GetMouseState's coordinates. Moving
//                    the mouse moves it; pushing it at any edge parks it
//                    there, because a mouse reports how far it moved and the
//                    library is what clamps that to the screen
//   button lights    left, middle, right, X1, X2 — lit from the button mask
//                    SDL_GetMouseState returns, so they show HELD state, not
//                    the events
//   event bars       one bar per event type received (motion, down, up,
//                    wheel), so a click that produced no event is visible as
//                    a bar that did not grow
//   wheel readout    the running total of every SDL_MOUSEWHEEL, in hex, and
//                    a bar that follows it
//   relative box     SDL_GetRelativeMouseState is CONSUMING, so this reads it
//                    once a frame and draws the frame's own displacement as a
//                    line from the box's centre. A pointer held against an
//                    edge still draws one: the relative reading is what the
//                    mouse reported, not what the clamp allowed
//
// The serial port is handed to the library's robot-hands channel, so the
// same screen can be driven from the bench with no hand on the desk:
//
//   mouse to 100 100      put the pointer at a coordinate
//   mouse move -40 0      nudge it
//   mouse tap left        one self-timed click
//   mouse down left / mouse move 60 0 / mouse up left     a drag
//   mouse wheel 3
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cstring>

static const char From[] = "mouseview";

// The library's robot-hands channel. A consumer declares it for itself; it is
// glue between a host kernel and the library, not part of the SDL surface.
void SDL2Circle_SetInjectSerial(CSerialDevice *pSerial);

// 5x7 hex glyphs
static const unsigned char FONT[16][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
};

// The virtual display, settled at start-up from the physical one.
static int W = 0, H = 0;

// The physical display, asked of the firmware directly — the example's own
// query, exactly as keyecho's. FALSE when the firmware will not answer.
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

static Uint32 *s_pix;
static int s_pitch32;

static void fill(int x, int y, int w, int h, Uint32 c)
{
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

static void glyph(int digit, int x, int y, int scale, Uint32 c)
{
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (FONT[digit][row] & (0x10 >> col))
                fill(x + col * scale, y + row * scale, scale, scale, c);
}

// A signed value in hex, four digits, with a leading bar for the sign.
static void number(int value, int x, int y, int scale, Uint32 c)
{
    if (value < 0)
    {
        fill(x, y + 3 * scale, 4 * scale, scale, c);
        value = -value;
    }
    x += 5 * scale;
    for (int shift = 12; shift >= 0; shift -= 4)
    {
        glyph((value >> shift) & 15, x, y, scale, c);
        x += 6 * scale;
    }
}

// A line from (x0,y0) by (dx,dy), drawn as dots so no rasterizer is needed.
static void ray(int x0, int y0, int dx, int dy, Uint32 c)
{
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                    ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0)
        return;
    if (steps > 256)
        steps = 256;
    for (int i = 0; i <= steps; i++)
        fill(x0 + dx * i / steps, y0 + dy * i / steps, 2, 2, c);
}

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
    return bOK;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice, "circle-libsdl2 mouseview test");

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return ShutdownHalt;

    SDL_Window *win = SDL_CreateWindow("mouseview", 0, 0, W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               W, H) : nullptr;
    if (!tex)
    {
        m_Logger.Write(From, LogError, "video: %s", SDL_GetError());
        return ShutdownHalt;
    }

    // Robot hands over the debug UART: the same events, typed instead of
    // waved. The serial is already the log's target, so a session reads its
    // own commands back interleaved with what they caused.
    SDL2Circle_SetInjectSerial(&m_Serial);

    unsigned nMotion = 0, nDown = 0, nUp = 0, nWheel = 0;
    int wheelTotal = 0;

    for (;;)
    {
        int frameRelX = 0, frameRelY = 0;

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_MOUSEMOTION:
                nMotion++;
                break;

            case SDL_MOUSEBUTTONDOWN:
                nDown++;
                m_Logger.Write(From, LogNotice, "DOWN button=%u at %d,%d",
                               (unsigned)ev.button.button, ev.button.x,
                               ev.button.y);
                break;

            case SDL_MOUSEBUTTONUP:
                nUp++;
                m_Logger.Write(From, LogNotice, "UP   button=%u at %d,%d",
                               (unsigned)ev.button.button, ev.button.x,
                               ev.button.y);
                break;

            case SDL_MOUSEWHEEL:
                nWheel++;
                wheelTotal += ev.wheel.y;
                m_Logger.Write(From, LogNotice, "WHEEL y=%d total=%d",
                               (int)ev.wheel.y, wheelTotal);
                break;

            default:
                break;
            }
        }

        // Consuming, so exactly one reader once a frame.
        SDL_GetRelativeMouseState(&frameRelX, &frameRelY);

        int mx = 0, my = 0;
        Uint32 buttons = SDL_GetMouseState(&mx, &my);

        void *pixels;
        int pitch;
        SDL_LockTexture(tex, nullptr, &pixels, &pitch);
        s_pix = (Uint32 *)pixels;
        s_pitch32 = pitch / 4;

        memset(pixels, 0, (size_t)pitch * H);

        // liveness sweep along the top edge
        fill((int)((SDL_GetTicks64() / 4) % W), 0, 24, 8, 0xFF00FFFF);

        // the pointer: a crosshair, and a box showing it is inside the screen
        Uint32 pc = buttons ? 0xFFFFC020 : 0xFF00E060;
        fill(mx - 20, my - 1, 41, 3, pc);
        fill(mx - 1, my - 20, 3, 41, pc);
        number(mx, 16, H - 40, 3, 0xFF808080);
        number(my, 16 + 30 * 3, H - 40, 3, 0xFF808080);

        // button lights: left middle right x1 x2, in SDL's own order
        for (int i = 0; i < 5; i++)
            fill(16 + i * 52, 24, 40, 40,
                 (buttons & SDL_BUTTON(i + 1)) ? 0xFFFFC020 : 0xFF282828);

        // event bars: motion, down, up, wheel
        const unsigned counts[4] = {nMotion, nDown, nUp, nWheel};
        const Uint32 colors[4] = {0xFF3060C0, 0xFF30C060, 0xFFC06030, 0xFFA030C0};
        for (int i = 0; i < 4; i++)
        {
            unsigned len = counts[i] > 200 ? 200 : counts[i];
            fill(16, 88 + i * 16, (int)len + 1, 10, colors[i]);
        }

        // wheel total, as a number and a bar either side of a centre line
        number(wheelTotal, 16, 160, 3, 0xFFA030C0);
        fill(16 + 30 * 3, 160, 2, 22, 0xFF404040);
        {
            int bar = wheelTotal * 4;
            if (bar > 200) bar = 200;
            if (bar < -200) bar = -200;
            if (bar >= 0) fill(16 + 30 * 3, 166, bar, 10, 0xFFA030C0);
            else          fill(16 + 30 * 3 + bar, 166, -bar, 10, 0xFFA030C0);
        }

        // this frame's relative reading, drawn from the centre of a box
        int bx = W - 140, by = 40;
        fill(bx, by, 120, 120, 0xFF141414);
        fill(bx + 58, by + 58, 4, 4, 0xFF606060);
        ray(bx + 60, by + 60, frameRelX, frameRelY, 0xFFFF4040);

        SDL_UnlockTexture(tex);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
}
