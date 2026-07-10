//
// kernel.cpp — audio test: 1kHz sine over HDMI via the SDL callback API.
//
// Screen: liveness sweep on top; a green bar grows with every audio
// callback invocation (wraps at screen width), so a still frame proves the
// callback is being serviced.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <cstring>

static const char From[] = "tone";

static const int W = 1920, H = 1080;
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
    m_Logger.Write(From, LogNotice, "circle-libsdl2 tone test");

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
