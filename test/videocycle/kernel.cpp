//
// kernel.cpp — repeated video restarts, hands-free.
//
// A consumer that offers a video-settings menu tears its whole SDL video
// world down and builds a new one whenever a setting changes: audio closed,
// texture, renderer and window destroyed, then all four created again at the
// new source geometry. Nothing about that needs a human, so this test does
// it in a loop, forever, while frames keep flowing to the presentation core.
//
// Each cycle alternates the source raster between the narrow and the wide
// shape a consumer would switch between, draws frames from the new world,
// then restarts. What it proves is that the restart releases everything it
// took: the shim's present path holds a DMA channel out of a small pool
// shared with the sound device, and full-screen shadow or staging buffers
// measured in megabytes.
//
// On screen: the cycle number and the current source geometry, large enough
// to read from a capture, plus a moving marker so a still frame shows the
// loop is running.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/startup.h>
#include <atomic>
#include <cstring>
#include <cstdio>

static const char From[] = "videocycle";

// The canvas the shim gives us is decided by cmdline.txt width=/height=, not
// by what SDL_CreateWindow is asked for. These are what the window is asked
// for all the same, exactly as a consumer would.
static const int WIN_W = 1280, WIN_H = 720;

// The two source rasters a widescreen toggle switches between.
static const int SRC_NARROW = 320, SRC_WIDE = 398, SRC_H = 224;

// Frames drawn from each freshly built world before it is torn down again.
// Enough that the presentation worker is genuinely mid-flight when teardown
// starts, which is the state a restart has to survive.
static const int FRAMES_PER_CYCLE = 30;

// ---------------------------------------------------------------------------
// A 5x7 pixel font, digits and capitals only — this test says very little.
// Within a row bit 0x10 is the leftmost of five pixels.
// ---------------------------------------------------------------------------

static const unsigned char FONT_DIGIT[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};

static Uint32 *s_pix;
static int s_pitch32, s_w, s_h;

static void fill(int x, int y, int w, int h, Uint32 c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = y; j < y + h; j++)
    {
        Uint32 *row = s_pix + (size_t)j * s_pitch32;
        for (int i = x; i < x + w; i++)
            row[i] = c;
    }
}

static void digit(int d, int x, int y, int scale, Uint32 c)
{
    const unsigned char *g = FONT_DIGIT[d % 10];
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col))
                fill(x + col * scale, y + row * scale, scale, scale, c);
}

static void number(unsigned v, int x, int y, int scale, Uint32 c)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%u", v);
    for (const char *p = buf; *p; p++, x += 6 * scale)
        digit(*p - '0', x, y, scale, c);
}

// ---------------------------------------------------------------------------
// Audio: silence, because the point is the device and not the sound. A
// consumer closes and reopens it around a video restart, and the sound device
// draws from the same DMA channel pool the present path does.
// ---------------------------------------------------------------------------

static void audio_callback(void *, Uint8 *stream, int len)
{
    memset(stream, 0, (size_t)len);
}

// ---------------------------------------------------------------------------
// The gate between core 0 and the application core: the application must not
// begin until the shim's split is armed.
// ---------------------------------------------------------------------------

static std::atomic<int> s_AppGate{0};

static inline void PublishToOtherCores(void)
{
    asm volatile("dsb ish; sev" ::: "memory");
}

static void ParkCore(void)
{
    for (;;)
        asm volatile("wfe" ::: "memory");
}

static void app_main(void);

void CSplitCores::Run(unsigned nCore)
{
    // Every core here may reach code that throws, and a throw reads the
    // thread pointer this arms before anything else.
    SDL2Circle_ArmCoreRuntime();

    switch (nCore)
    {
    case 1:
        while (!s_AppGate.load(std::memory_order_acquire))
            asm volatile("wfe" ::: "memory");
        app_main();
        ParkCore();
        break;

    case 2:
        SDL2Circle_SplitPresentCore();   // never returns
        break;

    default:
        ParkCore();
        break;
    }
}

// ---------------------------------------------------------------------------

static void app_main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR, "SDL_Init: %s", SDL_GetError());
        return;
    }

    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    int src_w = SRC_WIDE;
    SDL_Window *win = SDL_CreateWindow("videocycle", 0, 0, WIN_W, WIN_H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               src_w, SRC_H) : nullptr;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (dev)
        SDL_PauseAudioDevice(dev, 0);

    if (!tex)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR, "video: %s", SDL_GetError());
        return;
    }

    for (unsigned cycle = 1;; cycle++)
    {
        for (int f = 0; f < FRAMES_PER_CYCLE; f++)
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                ;

            void *pixels;
            int pitch;
            SDL_LockTexture(tex, nullptr, &pixels, &pitch);
            s_pix = (Uint32 *)pixels;
            s_pitch32 = pitch / 4;
            s_w = src_w;
            s_h = SRC_H;

            fill(0, 0, src_w, SRC_H, 0xFF101018);
            fill(0, 0, src_w, 3, 0xFF303048);
            // A marker that sweeps across the top, so a still capture shows
            // the loop turning.
            fill((int)((SDL_GetTicks64() / 8) % (Uint64)src_w), 4, 20, 5, 0xFF00D0FF);
            number(cycle, 12, 40, 5, 0xFFFFFFFF);
            number((unsigned)src_w, 12, 120, 4, 0xFF00D0FF);

            SDL_UnlockTexture(tex);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }

        // The restart, in the order a settings menu performs it: audio out
        // first, then the whole video world, then all of it back.
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE, "cycle %u: restarting video", cycle);

        SDL_PauseAudioDevice(dev, 1);
        SDL_CloseAudioDevice(dev);
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);

        src_w = (src_w == SRC_WIDE) ? SRC_NARROW : SRC_WIDE;

        win = SDL_CreateWindow("videocycle", 0, 0, WIN_W, WIN_H, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, src_w, SRC_H);
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (dev)
            SDL_PauseAudioDevice(dev, 0);

        if (!tex)
        {
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_ERROR,
                           "cycle %u: video gone: %s", cycle, SDL_GetError());
            return;
        }
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "cycle %u: video back, source %dx%d, audio device %u",
                       cycle, src_w, SRC_H, (unsigned)dev);
    }
}

// ---------------------------------------------------------------------------

CKernel::CKernel(void)
    // Serial device 0 is the GPIO14/15 header UART on every board. Named
    // explicitly because Circle's RASPPI >= 5 default (SERIAL_DEVICE_DEFAULT
    // = 10) is the Pi 5's dedicated debug connector, not the header.
    : m_Serial(0, FALSE, 0),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer),
      m_CPUThrottle(CPUSpeedMaximum)
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
    // Started last: the world the secondary cores work in has to be complete
    // before they run, and they park until Run() arms the split.
    if (bOK) bOK = m_Cores.Initialize();
    return bOK;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice,
                   "circle-libsdl2 videocycle test: hardware core 0, "
                   "application core 1, presentation core 2");

    SDL2Circle_SplitInit();
    s_AppGate.store(1, std::memory_order_release);
    PublishToOtherCores();

    // Core 0's idle loop: yielding is what gives the servo the core, and the
    // servo is what answers the other two.
    for (;;)
        m_Scheduler.Yield();
}
