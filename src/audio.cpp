//
// audio.cpp — SDL callback audio over Circle's HDMI sound device.
//
// One output device, S16 stereo. The application's audio callback runs on
// the main loop out of SDL_PumpEvents (never in IRQ context): each pump
// tops the hardware queue back up, invoking the callback once per chunk of
// free space. The queue depth (~100ms) is the underrun budget between
// pumps — any app that presents frames or polls events keeps it full.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/interrupt.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <cstring>
#include <cstdlib>

namespace
{

CHDMISoundBaseDevice *s_device = nullptr;
SDL_AudioSpec s_spec;
bool s_paused = true;
bool s_started = false;
int s_lock = 0;
Uint8 *s_chunk = nullptr;
unsigned s_queueFrames = 0;

constexpr unsigned QUEUE_MSECS = 100;

} // namespace

extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int iscapture,
                                                 const SDL_AudioSpec *desired,
                                                 SDL_AudioSpec *obtained,
                                                 int)
{
    if (iscapture || !desired)
    {
        SDL_SetError("capture devices are not implemented");
        return 0;
    }
    if (s_device)
    {
        SDL_SetError("only one audio device is available");
        return 0;
    }

    int freq = desired->freq > 0 ? desired->freq : 48000;
    Uint16 samples = desired->samples > 0 ? desired->samples : 1024;

    s_device = new CHDMISoundBaseDevice(CInterruptSystem::Get(), freq);
    s_device->SetWriteFormat(SoundFormatSigned16, 2);
    if (!s_device->AllocateQueue(QUEUE_MSECS))
    {
        delete s_device;
        s_device = nullptr;
        SDL_SetError("cannot allocate sound queue");
        return 0;
    }
    s_queueFrames = s_device->GetQueueSizeFrames();

    s_spec = *desired;
    s_spec.freq = freq;
    s_spec.format = AUDIO_S16SYS;   // the only format the device speaks
    s_spec.channels = 2;
    s_spec.samples = samples;
    s_spec.size = (Uint32)samples * 2 * 2;
    s_spec.silence = 0;
    if (obtained)
        *obtained = s_spec;

    s_chunk = (Uint8 *)malloc(s_spec.size);
    s_paused = true;
    s_started = false;

    return 2;   // SDL device ids for opened devices start at 2
}

void SDL2Circle_AudioPump(void)
{
    if (!s_device || s_paused || !s_spec.callback || s_lock > 0)
        return;

    unsigned queued = s_device->GetQueueFramesAvail();
    unsigned space = s_queueFrames > queued ? s_queueFrames - queued : 0;

    while (space >= s_spec.samples)
    {
        s_spec.callback(s_spec.userdata, s_chunk, (int)s_spec.size);
        if (s_device->Write(s_chunk, s_spec.size) <= 0)
            break;
        space -= s_spec.samples;
    }
}

extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on)
{
    if (!s_device)
        return;
    s_paused = (pause_on != 0);
    if (!s_paused && !s_started)
    {
        s_started = s_device->Start();
        if (!s_started)
            SDL_SetError("HDMI sound failed to start "
                         "(display without audio support? hdmi_drive=2?)");
    }
}

extern "C" void SDL_LockAudioDevice(SDL_AudioDeviceID)   { s_lock++; }
extern "C" void SDL_UnlockAudioDevice(SDL_AudioDeviceID) { if (s_lock > 0) s_lock--; }

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID)
{
    if (!s_device)
        return;
    s_device->Cancel();
    delete s_device;
    s_device = nullptr;
    free(s_chunk);
    s_chunk = nullptr;
    s_paused = true;
}

extern "C" SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID)
{
    if (!s_device || !s_started)
        return SDL_AUDIO_STOPPED;
    return s_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

extern "C" int SDL_GetNumAudioDevices(int iscapture)
{
    return iscapture ? 0 : 1;
}

extern "C" const char *SDL_GetAudioDeviceName(int index, int iscapture)
{
    return (!iscapture && index == 0) ? "HDMI" : nullptr;
}

extern "C" const char *SDL_GetCurrentAudioDriver(void)
{
    return "circle";
}
