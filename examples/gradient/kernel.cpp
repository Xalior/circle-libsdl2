//
// kernel.cpp — circle-libsdl2 first light: animated gradient via the
// SDL2 API only. If this is on the HDMI port, the shim's video path works.
//
// It also shows the ordinary way to make the virtual display match the
// physical one: ask the firmware how big the panel is, and declare that.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cstring>

static const char From[] = "gradient";

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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 gradient test");

    // Match the virtual display to the physical one: ask the firmware what
    // the panel is, then declare that. Nothing requires this call - the
    // library falls back to the same physical size on its own, as a last
    // resort - but asking and declaring explicitly is what this example is
    // for, so it stops here rather than letting a firmware that will not
    // answer surface later as someone else's failure.
    int W = 0, H = 0;
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
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    SDL_Window *win = SDL_CreateWindow("circle-libsdl2", 0, 0, W, H, 0);
    if (!win)
    {
        m_Logger.Write(From, LogError, "SDL_CreateWindow: %s", SDL_GetError());
        return ShutdownHalt;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, W, H);
    if (!ren || !tex)
    {
        m_Logger.Write(From, LogError, "renderer/texture: %s", SDL_GetError());
        return ShutdownHalt;
    }

    m_Logger.Write(From, LogNotice, "rendering; power-cycle to exit");

    Uint64 t0 = SDL_GetTicks64();
    unsigned frames = 0;

    for (;;)
    {
        Uint64 t = SDL_GetTicks64();

        void *pixels;
        int pitch;
        SDL_LockTexture(tex, nullptr, &pixels, &pitch);
        for (int y = 0; y < H; y++)
        {
            Uint32 *row = (Uint32 *)((Uint8 *)pixels + (size_t)y * pitch);
            unsigned ty = y + (unsigned)(t / 20);
            for (int x = 0; x < W; x++)
            {
                unsigned tx = x + (unsigned)(t / 10);
                row[x] = 0xFF000000u | ((tx & 255) << 16) | ((ty & 255) << 8)
                         | ((x ^ y) & 255);
            }
        }
        SDL_UnlockTexture(tex);

        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        if (++frames % 300 == 0)
        {
            Uint64 now = SDL_GetTicks64();
            m_Logger.Write(From, LogNotice, "%u frames, %u ms/frame avg",
                           frames, (unsigned)((now - t0) / frames));
        }
    }
}
