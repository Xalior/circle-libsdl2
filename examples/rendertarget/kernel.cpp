//
// kernel.cpp — circle-libsdl2 render-to-texture: a small picture composed
// into an off-screen texture, then stretched over the whole window.
//
// That shape is how a game gets a fixed low resolution look on any panel:
// everything is drawn once, at the resolution the artwork was made for, and
// the finished image is magnified in one step. Nothing in the game has to
// know what the display is.
//
// The example checks the render-target contract first and puts each answer
// on the serial console, then runs the picture forever.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cstring>

static const char From[] = "rendertarget";

// The resolution the picture is composed at. Everything drawn below is in
// these coordinates, whatever the panel turns out to be.
static const int ART_W = 320;
static const int ART_H = 180;

// The physical display, asked of the firmware directly. This is the
// EXAMPLE's own query, not something the library provides: the library is
// told what virtual display to present and never goes looking for one, so a
// consumer that wants the two to match asks for itself.
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 render-to-texture example");

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    SDL_Window *win = SDL_CreateWindow("circle-libsdl2", 0, 0, W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    if (!win || !ren)
    {
        m_Logger.Write(From, LogError, "window/renderer: %s", SDL_GetError());
        return ShutdownHalt;
    }
    m_Logger.Write(From, LogNotice, "window %dx%d, picture composed at %dx%d",
                   W, H, ART_W, ART_H);

    // ---- what the renderer says it can do ---------------------------------

    m_Logger.Write(From, LogNotice, "SDL_RenderTargetSupported: %s",
                   SDL_RenderTargetSupported(ren) == SDL_TRUE ? "yes" : "NO");

    SDL_RendererInfo info;
    memset(&info, 0, sizeof info);
    SDL_GetRendererInfo(ren, &info);
    m_Logger.Write(From, LogNotice, "SDL_RENDERER_TARGETTEXTURE: %s",
                   (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "set" : "NOT SET");

    // ---- the target, and the sprite drawn into it -------------------------

    SDL_Texture *art = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET,
                                         ART_W, ART_H);
    if (!art)
    {
        m_Logger.Write(From, LogError, "target texture: %s", SDL_GetError());
        return ShutdownHalt;
    }

    // A small sprite with a transparent border, so copying it INTO the
    // target exercises the blended path as well as the plain one.
    const int SPR = 24;
    SDL_Texture *spr = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         SPR, SPR);
    if (!spr)
    {
        m_Logger.Write(From, LogError, "sprite texture: %s", SDL_GetError());
        return ShutdownHalt;
    }
    SDL_SetTextureBlendMode(spr, SDL_BLENDMODE_BLEND);
    {
        void *pixels = nullptr;
        int pitch = 0;
        SDL_LockTexture(spr, nullptr, &pixels, &pitch);
        for (int y = 0; y < SPR; y++)
        {
            Uint32 *row = (Uint32 *)((Uint8 *)pixels + (size_t)y * pitch);
            for (int x = 0; x < SPR; x++)
            {
                const int dx = x - SPR / 2, dy = y - SPR / 2;
                row[x] = (dx * dx + dy * dy < (SPR / 2) * (SPR / 2))
                         ? 0xFFFFD040u : 0x00000000u;
            }
        }
        SDL_UnlockTexture(spr);
    }

    // ---- the contract, checked and logged ---------------------------------

    // Aiming at a texture that was not made for it is refused. An
    // application that gets this back has asked for something the texture
    // cannot do, and is told so rather than drawing into the frame.
    if (SDL_SetRenderTarget(ren, spr) == 0)
        m_Logger.Write(From, LogError,
                       "a streaming texture was ACCEPTED as a render target");
    else
        m_Logger.Write(From, LogNotice, "streaming texture refused: %s",
                       SDL_GetError());

    if (SDL_SetRenderTarget(ren, art) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_SetRenderTarget: %s", SDL_GetError());
        return ShutdownHalt;
    }

    m_Logger.Write(From, LogNotice, "SDL_GetRenderTarget while set: %s",
                   SDL_GetRenderTarget(ren) == art ? "the target" : "WRONG");

    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    m_Logger.Write(From, LogNotice,
                   "output size while the target is set: %dx%d (want %dx%d)",
                   ow, oh, ART_W, ART_H);

    // A target cannot be the source of a copy into itself: one store,
    // read and written at once.
    if (SDL_RenderCopy(ren, art, nullptr, nullptr) == 0)
        m_Logger.Write(From, LogError,
                       "the target was ACCEPTED as a copy into itself");
    else
        m_Logger.Write(From, LogNotice, "target copied into itself refused: %s",
                       SDL_GetError());

    // Something known, drawn into the target and read straight back.
    SDL_SetRenderDrawColor(ren, 0x20, 0x40, 0x80, 0xFF);
    SDL_RenderClear(ren);
    {
        Uint32 px = 0;
        const SDL_Rect one = { 10, 10, 1, 1 };
        SDL_RenderReadPixels(ren, &one, SDL_PIXELFORMAT_ARGB8888, &px, 4);
        m_Logger.Write(From, LogNotice,
                       "read back from the target: 0x%08X (want 0xFF204080)",
                       (unsigned) px);
    }

    // A viewport set here belongs to the target, and the window's own is
    // untouched by it.
    const SDL_Rect small = { 4, 4, 32, 32 };
    SDL_RenderSetViewport(ren, &small);
    SDL_SetRenderTarget(ren, nullptr);
    SDL_Rect back;
    SDL_RenderGetViewport(ren, &back);
    m_Logger.Write(From, LogNotice,
                   "window viewport after a viewport set on the target: "
                   "%dx%d+%d+%d (want %dx%d+0+0)",
                   back.w, back.h, back.x, back.y, W, H);
    m_Logger.Write(From, LogNotice, "SDL_GetRenderTarget after release: %s",
                   SDL_GetRenderTarget(ren) == nullptr ? "none" : "WRONG");

    SDL_SetRenderTarget(ren, art);
    SDL_RenderGetViewport(ren, &back);
    m_Logger.Write(From, LogNotice,
                   "the target's own viewport came back: %dx%d+%d+%d "
                   "(want 32x32+4+4)", back.w, back.h, back.x, back.y);
    SDL_RenderSetViewport(ren, nullptr);
    SDL_SetRenderTarget(ren, nullptr);

    // A blended copy into a target has to compose the destination alpha it
    // is writing over, the same as any other SDL_BLENDMODE_BLEND copy, not
    // discard it. Cleared fully transparent, then half of it covered by a
    // copy at a known partial source alpha: the covered half has to read
    // back at the composed alpha, and the half never touched has to read
    // back exactly as the clear left it.
    const int BLEND_W = 8, BLEND_H = 1;
    SDL_Texture *alphatarget = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 BLEND_W, BLEND_H);
    SDL_Texture *halfsrc = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             BLEND_W / 2, BLEND_H);
    if (!alphatarget || !halfsrc)
    {
        m_Logger.Write(From, LogError, "blend-alpha textures: %s", SDL_GetError());
        return ShutdownHalt;
    }
    SDL_SetTextureBlendMode(halfsrc, SDL_BLENDMODE_BLEND);
    {
        void *pixels = nullptr;
        int pitch = 0;
        SDL_LockTexture(halfsrc, nullptr, &pixels, &pitch);
        Uint32 *row = (Uint32 *)pixels;
        for (int x = 0; x < BLEND_W / 2; x++)
            row[x] = 0x80FFFFFFu;      // alpha 128, white
        SDL_UnlockTexture(halfsrc);
    }

    SDL_SetRenderTarget(ren, alphatarget);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
    const SDL_Rect coveredhalf = { 0, 0, BLEND_W / 2, BLEND_H };
    SDL_RenderCopy(ren, halfsrc, nullptr, &coveredhalf);

    Uint32 touched = 0, untouched = 0;
    const SDL_Rect touchedpx = { 0, 0, 1, 1 };
    const SDL_Rect untouchedpx = { BLEND_W - 1, 0, 1, 1 };
    SDL_RenderReadPixels(ren, &touchedpx, SDL_PIXELFORMAT_ARGB8888, &touched, 4);
    SDL_RenderReadPixels(ren, &untouchedpx, SDL_PIXELFORMAT_ARGB8888, &untouched, 4);
    SDL_SetRenderTarget(ren, nullptr);

    m_Logger.Write(From, LogNotice,
                   "blended copy into target, touched pixel: 0x%08X "
                   "(want 0x807F7F7F)", (unsigned) touched);
    m_Logger.Write(From, LogNotice,
                   "blended copy into target, untouched pixel: 0x%08X "
                   "(want 0x00000000)", (unsigned) untouched);

    // ---- the picture ------------------------------------------------------

    m_Logger.Write(From, LogNotice, "rendering; power-cycle to exit");

    Uint64 t0 = SDL_GetTicks64();
    unsigned frames = 0;

    for (;;)
    {
        const Uint64 t = SDL_GetTicks64();
        const int phase = (int)((t / 8) % (ART_W + SPR));

        // Everything below is drawn at ART_W x ART_H, into the texture.
        SDL_SetRenderTarget(ren, art);

        SDL_SetRenderDrawColor(ren, 0x10, 0x18, 0x30, 0xFF);
        SDL_RenderClear(ren);

        // A fixed grid, so the magnification is obvious: every square is
        // the same size on the panel, with hard edges.
        SDL_SetRenderDrawColor(ren, 0x28, 0x30, 0x50, 0xFF);
        for (int x = 0; x < ART_W; x += 20)
        {
            const SDL_Rect v = { x, 0, 1, ART_H };
            SDL_RenderFillRect(ren, &v);
        }
        for (int y = 0; y < ART_H; y += 20)
        {
            const SDL_Rect h = { 0, y, ART_W, 1 };
            SDL_RenderFillRect(ren, &h);
        }

        // A bar travelling across, drawn with the fill path.
        SDL_SetRenderDrawColor(ren, 0xC0, 0x30, 0x30, 0xFF);
        const SDL_Rect bar = { phase - SPR, ART_H / 2 - 4, SPR, 8 };
        SDL_RenderFillRect(ren, &bar);

        // The sprite, copied into the target: a texture drawn into another
        // texture, blended, and scaled on the way.
        const SDL_Rect big = { phase - SPR, 20, SPR * 2, SPR * 2 };
        SDL_RenderCopy(ren, spr, nullptr, &big);

        // Back to the frame, and the whole picture in one magnified copy.
        SDL_SetRenderTarget(ren, nullptr);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0xFF);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, art, nullptr, nullptr);
        SDL_RenderPresent(ren);

        if (++frames % 300 == 0)
        {
            const Uint64 now = SDL_GetTicks64();
            m_Logger.Write(From, LogNotice, "%u frames, %u ms/frame avg",
                           frames, (unsigned)((now - t0) / frames));
        }
    }
}
