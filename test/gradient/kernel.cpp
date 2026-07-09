//
// kernel.cpp — circle-libsdl2 first light: animated gradient via the
// SDL2 API only. If this is on the HDMI port, the shim's video path works.
//
#include "kernel.h"
#include <SDL2/SDL.h>

static const char From[] = "gradient";

CKernel::CKernel(void)
    : m_Timer(&m_Interrupt),
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 gradient test");

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    const int W = 1920, H = 1080;

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
