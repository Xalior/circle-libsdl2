//
// kernel.cpp — audio test: 1kHz sine over HDMI via the SDL callback API.
//
// Screen: liveness sweep on top; a green bar grows with every audio
// callback invocation (wraps at screen width), so a still frame proves the
// callback is being serviced.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cmath>
#include <cstring>

static const char From[] = "tone";

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

static const int FREQ = 48000;
static const double TONE_HZ = 1000.0;

static unsigned s_callbacks = 0;
static double s_phase = 0.0;

static void AudioCallback(void *, Uint8 *stream, int len)
{
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4;
    for (int i = 0; i < frames; i++)
    {
        Sint16 s = (Sint16)(9000.0 * sin(s_phase));
        s_phase += 2.0 * M_PI * TONE_HZ / FREQ;
        if (s_phase >= 2.0 * M_PI)
            s_phase -= 2.0 * M_PI;
        *out++ = s;   // left
        *out++ = s;   // right
    }
    s_callbacks++;
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 tone test");

    // Match the virtual display to the physical one: ask the firmware what
    // the panel is, then declare that. The declaration is required and has no
    // fallback, so an example that cannot learn the size has nothing honest
    // to declare and stops here rather than guessing one.
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
        return ShutdownHalt;

    SDL_Window *win = SDL_CreateWindow("tone", 0, 0, W, H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               W, H) : nullptr;
    if (!tex)
    {
        m_Logger.Write(From, LogError, "video: %s", SDL_GetError());
        return ShutdownHalt;
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = AudioCallback;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0)
    {
        m_Logger.Write(From, LogError, "audio: %s", SDL_GetError());
        return ShutdownHalt;
    }
    SDL_PauseAudioDevice(dev, 0);

    m_Logger.Write(From, LogNotice, "1kHz tone playing at %d Hz", have.freq);

    for (;;)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            ;   // pump drives audio + input

        void *pixels;
        int pitch;
        SDL_LockTexture(tex, nullptr, &pixels, &pitch);
        memset(pixels, 0, (size_t)pitch * H);

        Uint32 *p = (Uint32 *)pixels;
        int stride = pitch / 4;

        // liveness sweep
        int sweep = (int)((SDL_GetTicks64() / 4) % W);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 24; x++)
                p[(size_t)y * stride + (sweep + x) % W] = 0xFF00FFFF;

        // callback progress bar
        int bar = (int)(s_callbacks % (unsigned)W);
        for (int y = H / 2 - 30; y < H / 2 + 30; y++)
            for (int x = 0; x < bar; x++)
                p[(size_t)y * stride + x] = 0xFF00D060;

        // red block when the device is not actually playing
        if (SDL_GetAudioDeviceStatus(dev) != SDL_AUDIO_PLAYING)
            for (int y = H / 4 - 60; y < H / 4 + 60; y++)
                for (int x = W / 2 - 120; x < W / 2 + 120; x++)
                    p[(size_t)y * stride + x] = 0xFFE02020;

        SDL_UnlockTexture(tex);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
}
