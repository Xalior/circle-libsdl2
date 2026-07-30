//
// kernel.cpp — virtdev: the display an application declares for itself.
//
// Left alone, the library gives an application the physical display the
// firmware reports, and has nothing to scale. An application that has to
// render at one size instead says so in its own code, before SDL_Init, with
// SDL2Circle_DeclareVirtualDevice(depth, width, height). Every SDL answer
// about the display then reports that size, and the library scales each
// frame from it onto whatever the panel is really doing.
//
// The two are separate: this declares the VIRTUAL display, and cmdline.txt
// width=/height= asks the firmware for a PHYSICAL mode. Neither is a way of
// setting the other.
//
// This kernel declares one, and then:
//
//   - asks SDL about the display every way SDL offers, and checks each
//     answer against what was declared;
//   - creates a window at a deliberately different size, to show that the
//     window is the declared device and not what the application asked for;
//   - makes the declarations the library refuses — a depth other than 32, an
//     impossible size, a second declaration, and one made after the display
//     size has been settled — and prints the reason given for each;
//   - draws a frame that fills the declared device, so the shape on the glass
//     can be read against the numbers on the serial log.
//
// Everything it reports goes to the serial console. The picture is a border,
// four corner blocks, a centre cross and a marker sweeping along the top: the
// corners and the border show whether the whole declared device reaches the
// screen, and the marker shows that a still photograph came from a running
// loop.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>

static const char From[] = "virtdev";

// The virtual device this application declares. Not a display mode any panel
// offers, on purpose: an application declares the world it wants to draw in,
// and the library is what makes it fit the screen.
static const int DEV_W = 800, DEV_H = 450;

// What SDL_CreateWindow is asked for, which the library does not use — there
// is one screen and the application is given all of it, at the size declared
// above. Different from the declaration so the log can show the difference.
static const int ASK_W = 1920, ASK_H = 1080;

// ---------------------------------------------------------------------------
// declaring
// ---------------------------------------------------------------------------

// One declaration attempt, with the library's own answer for it.
static void Declare(const char *pWhat, unsigned nDepth, int nWidth, int nHeight)
{
    SDL_ClearError();
    int nResult = SDL2Circle_DeclareVirtualDevice(nDepth, nWidth, nHeight);

    if (nResult == 0)
        CLogger::Get()->Write(From, LogNotice, "%s: depth %u, %dx%d: ACCEPTED",
                              pWhat, nDepth, nWidth, nHeight);
    else
        CLogger::Get()->Write(From, LogNotice,
                              "%s: depth %u, %dx%d: REFUSED (%s)",
                              pWhat, nDepth, nWidth, nHeight, SDL_GetError());
}

// ---------------------------------------------------------------------------
// asking
// ---------------------------------------------------------------------------

static void Expect(const char *pWhat, int nWidth, int nHeight)
{
    boolean bMatch = nWidth == DEV_W && nHeight == DEV_H;
    CLogger::Get()->Write(From, bMatch ? LogNotice : LogError,
                          "%s: %dx%d — %s the declared %dx%d",
                          pWhat, nWidth, nHeight,
                          bMatch ? "matches" : "DOES NOT MATCH", DEV_W, DEV_H);
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static Uint32 *s_pPixels;
static int s_nPitch32;

static void Fill(int x, int y, int w, int h, Uint32 nColor)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DEV_W) w = DEV_W - x;
    if (y + h > DEV_H) h = DEV_H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = y; j < y + h; j++)
    {
        Uint32 *pRow = s_pPixels + (size_t) j * s_nPitch32;
        for (int i = x; i < x + w; i++)
            pRow[i] = nColor;
    }
}

static void DrawFrame(Uint64 nTicks)
{
    static const Uint32 Background = 0xFF101018;
    static const Uint32 Edge       = 0xFFFFFFFF;
    static const Uint32 Corner     = 0xFF00D0FF;
    static const Uint32 Cross      = 0xFF404058;
    static const int    Thickness  = 2;
    static const int    CornerSize = 24;

    Fill(0, 0, DEV_W, DEV_H, Background);

    // The outline of the whole declared device. Anything cut off it says the
    // device is not reaching the screen whole.
    Fill(0, 0, DEV_W, Thickness, Edge);
    Fill(0, DEV_H - Thickness, DEV_W, Thickness, Edge);
    Fill(0, 0, Thickness, DEV_H, Edge);
    Fill(DEV_W - Thickness, 0, Thickness, DEV_H, Edge);

    Fill(0, 0, CornerSize, CornerSize, Corner);
    Fill(DEV_W - CornerSize, 0, CornerSize, CornerSize, Corner);
    Fill(0, DEV_H - CornerSize, CornerSize, CornerSize, Corner);
    Fill(DEV_W - CornerSize, DEV_H - CornerSize, CornerSize, CornerSize, Corner);

    Fill(DEV_W / 2 - 1, 0, 2, DEV_H, Cross);
    Fill(0, DEV_H / 2 - 1, DEV_W, 2, Cross);

    // A marker that sweeps along the top, so a still capture shows the loop
    // is turning.
    Fill((int) ((nTicks / 8) % (Uint64) DEV_W), CornerSize + 8, 20, 6, Corner);
}

// ---------------------------------------------------------------------------

CKernel::CKernel(void)
    // Serial device 0 is the GPIO14/15 header UART on every board. Named
    // explicitly because Circle's RASPPI >= 5 default (SERIAL_DEVICE_DEFAULT
    // = 10) is the Pi 5's dedicated debug connector, not the header.
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 virtual device test");

    // The values the library cannot serve, before the one it can. Each is
    // refused, and refusing changes nothing: the declaration that follows
    // them is still the first one.
    m_Logger.Write(From, LogNotice, "----- declarations the library refuses -----");
    Declare("16 bits per pixel", 16, DEV_W, DEV_H);
    Declare("8 bits per pixel", 8, DEV_W, DEV_H);
    Declare("no depth at all", 0, DEV_W, DEV_H);
    Declare("no width", 32, 0, DEV_H);
    Declare("negative height", 32, DEV_W, -1);

    m_Logger.Write(From, LogNotice, "----- the declaration -----");
    Declare("the virtual device", 32, DEV_W, DEV_H);

    // Fixed at the declaration: a second one is refused even though its
    // values are perfectly good, and even though it asks for what was
    // already granted.
    Declare("a second declaration", 32, DEV_W, DEV_H);
    Declare("a second declaration, different size", 32, 1280, 720);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    // Every route SDL offers to the display's size. The first of these is
    // also what settles the display size for the run.
    m_Logger.Write(From, LogNotice, "----- what SDL reports about the display -----");

    SDL_DisplayMode Mode;
    SDL_GetCurrentDisplayMode(0, &Mode);
    Expect("SDL_GetCurrentDisplayMode", Mode.w, Mode.h);
    m_Logger.Write(From, LogNotice,
                   "SDL_GetCurrentDisplayMode: format 0x%08X, %d Hz",
                   (unsigned) Mode.format, Mode.refresh_rate);

    SDL_GetDesktopDisplayMode(0, &Mode);
    Expect("SDL_GetDesktopDisplayMode", Mode.w, Mode.h);

    SDL_GetDisplayMode(0, 0, &Mode);
    Expect("SDL_GetDisplayMode", Mode.w, Mode.h);

    SDL_Rect Bounds;
    SDL_GetDisplayBounds(0, &Bounds);
    Expect("SDL_GetDisplayBounds", Bounds.w, Bounds.h);

    // The display size is settled now, so the declaration can no longer be
    // made or changed by anyone.
    m_Logger.Write(From, LogNotice, "----- declaring after the display is settled -----");
    Declare("after the first display query", 32, DEV_W, DEV_H);

    m_Logger.Write(From, LogNotice, "----- the window -----");
    m_Logger.Write(From, LogNotice, "SDL_CreateWindow asked for %dx%d", ASK_W, ASK_H);

    SDL_Window *pWindow = SDL_CreateWindow("virtdev", 0, 0, ASK_W, ASK_H, 0);
    if (!pWindow)
    {
        m_Logger.Write(From, LogError, "SDL_CreateWindow: %s", SDL_GetError());
        return ShutdownHalt;
    }

    int nWindowW = 0, nWindowH = 0;
    SDL_GetWindowSize(pWindow, &nWindowW, &nWindowH);
    Expect("SDL_GetWindowSize", nWindowW, nWindowH);

    SDL_Renderer *pRenderer = SDL_CreateRenderer(pWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (pRenderer)
    {
        int nOutputW = 0, nOutputH = 0;
        SDL_GetRendererOutputSize(pRenderer, &nOutputW, &nOutputH);
        Expect("SDL_GetRendererOutputSize", nOutputW, nOutputH);
    }

    SDL_Texture *pTexture = pRenderer
        ? SDL_CreateTexture(pRenderer, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, DEV_W, DEV_H)
        : nullptr;
    if (!pTexture)
    {
        m_Logger.Write(From, LogError, "renderer/texture: %s", SDL_GetError());
        return ShutdownHalt;
    }

    m_Logger.Write(From, LogNotice, "drawing %dx%d; power-cycle to exit",
                   DEV_W, DEV_H);

    for (;;)
    {
        SDL_Event Event;
        while (SDL_PollEvent(&Event))
            ;

        void *pPixels;
        int nPitch;
        SDL_LockTexture(pTexture, nullptr, &pPixels, &nPitch);
        s_pPixels = (Uint32 *) pPixels;
        s_nPitch32 = nPitch / 4;
        DrawFrame(SDL_GetTicks64());
        SDL_UnlockTexture(pTexture);

        SDL_RenderCopy(pRenderer, pTexture, nullptr, nullptr);
        SDL_RenderPresent(pRenderer);
    }
}
