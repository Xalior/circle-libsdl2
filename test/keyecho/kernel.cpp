//
// kernel.cpp — keyboard echo via the SDL2 API only.
//
// Screen layout:
//   top edge      thin animated sweep = app alive
//   center        last scancode as two big hex digits
//                 (green while key held, dimmed red after release)
//   below         8 modifier lights (LCTRL LSHIFT LALT LGUI  RCTRL ...)
//   lower half    32x8 grid of scancodes 0..255 from SDL_GetKeyboardState
//   bottom edge   one small square per key event received
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <cstring>

static const char From[] = "keyecho";

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

static const int W = 1920, H = 1080;

static Uint32 *s_pix;
static int s_pitch32;

static void fill(int x, int y, int w, int h, Uint32 c)
{
    if (x < 0 || y < 0 || x + w > W || y + h > H)
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 keyecho test");

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return ShutdownHalt;

    SDL_Window *win = SDL_CreateWindow("keyecho", 0, 0, W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               W, H) : nullptr;
    if (!tex)
    {
        m_Logger.Write(From, LogError, "video: %s", SDL_GetError());
        return ShutdownHalt;
    }

    int lastScancode = -1;
    bool lastDown = false;
    unsigned nEvents = 0;

    for (;;)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
            {
                lastScancode = ev.key.keysym.scancode;
                lastDown = (ev.type == SDL_KEYDOWN);
                nEvents++;
                m_Logger.Write(From, LogNotice, "%s scancode=%02X sym=%d",
                               lastDown ? "DOWN" : "UP", lastScancode,
                               (int)ev.key.keysym.sym);
            }
        }

        void *pixels;
        int pitch;
        SDL_LockTexture(tex, nullptr, &pixels, &pitch);
        s_pix = (Uint32 *)pixels;
        s_pitch32 = pitch / 4;

        memset(pixels, 0, (size_t)pitch * H);   // black

        // liveness sweep along the top edge
        int sweep = (int)((SDL_GetTicks64() / 4) % W);
        fill(sweep, 0, 24, 12, 0xFF00FFFF);

        if (lastScancode < 0)
        {
            // waiting state: grey block centered
            fill(W / 2 - 120, H / 4, 240, 120, 0xFF404040);
        }
        else
        {
            Uint32 c = lastDown ? 0xFF00E060 : 0xFF803030;
            int scale = 28;                       // glyph 140x196
            int gw = 5 * scale, gap = scale;
            int x0 = W / 2 - gw - gap / 2, y0 = H / 6;
            glyph((lastScancode >> 4) & 15, x0, y0, scale, c);
            glyph(lastScancode & 15, x0 + gw + gap, y0, scale, c);
        }

        // modifier lights
        SDL_Keymod mods = SDL_GetModState();
        static const SDL_Keymod modbits[8] = {KMOD_LCTRL, KMOD_LSHIFT,
            KMOD_LALT, KMOD_LGUI, KMOD_RCTRL, KMOD_RSHIFT, KMOD_RALT, KMOD_RGUI};
        for (int i = 0; i < 8; i++)
            fill(W / 2 - 200 + i * 52, H / 6 + 260, 40, 40,
                 (mods & modbits[i]) ? 0xFFFFC020 : 0xFF282828);

        // held-key grid: scancodes 0..255, 32 x 8
        const Uint8 *state = SDL_GetKeyboardState(nullptr);
        for (int sc = 0; sc < 256; sc++)
        {
            int gx = W / 2 - 32 * 26 / 2 + (sc % 32) * 26;
            int gy = H / 2 + 100 + (sc / 32) * 26;
            fill(gx, gy, 22, 22, state[sc] ? 0xFF00C0FF : 0xFF1A1A1A);
        }

        // event counter along the bottom
        for (unsigned i = 0; i < nEvents && i < 120; i++)
            fill(16 + (int)i * 16, H - 28, 12, 12, 0xFFE0E0E0);

        SDL_UnlockTexture(tex);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
}
