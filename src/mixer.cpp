//
// mixer.cpp — SDL_mixer: several sounds at once, over the one audio device.
//
// The device plays one stream. An application wants to start a sound effect
// without stopping the one already playing, and wants music underneath both.
// That is the whole of what a mixer is, and it is why it sits above SDL's own
// audio API rather than beside it: SDL_mixer opens the device, installs its
// own callback, and every Mix_ call below adds to or takes away from what
// that callback is summing.
//
// IN THE SAME ARCHIVE, for the reasons given in image.cpp: no packages, no
// shared objects, nothing for a separate archive to decouple. SDL_mixer.h is
// the upstream header.
//
// EVERYTHING IS STORED IN THE DEVICE'S FORMAT. A chunk is converted once,
// when it is loaded, into exactly what the device plays; the callback then
// only has to add samples together. Converting during the callback would put
// a resampler in the path of every buffer the device asks for, and a late
// buffer is heard immediately.
//
// WHAT PLAYS: WAV, through SDL_LoadWAV_RW, in any of the formats that reads.
// A compressed or synthesised format is REFUSED BY NAME — see the note on
// music below. Silence that says why is recoverable; noise, or a game that
// hangs waiting for a track to finish, is not.
//
// WHAT IS NOT HERE: a MIDI synthesiser. A MIDI file is a score, not a
// recording — playing one means synthesising every instrument in it, which
// is a sound engine in its own right and far larger than this file.
// Mix_SetSoundFonts and Mix_SetTimidityCfg are accepted and remembered so an
// application's configuration still works, but nothing reads them yet, and
// Mix_LoadMUS on a MIDI file fails and says exactly this.
//
// EFFECTS ARE HOW AN APPLICATION BRINGS ITS OWN SOUND ENGINE. An effect
// registered on MIX_CHANNEL_POST is handed the finished mix, in the device's
// format, and may rewrite it — which is the door through which an
// application that HAS a synthesiser of its own puts its output into the
// stream without needing one here. Chocolate Doom is the case that matters:
// it emulates an OPL2 chip itself and adds the chip's samples to the mix
// from a post effect, so its music plays even though nothing above can read
// a MIDI file. Mix_SetPostMix is the same door one stage further down.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "sdl2circle.h"

#include <cstdlib>
#include <cstring>

namespace
{

// The device's own format, which is what everything is stored in.
SDL_AudioSpec s_spec;
bool s_open = false;
int s_open_count = 0;      // Mix_OpenAudio may be called more than once

const int MIX_MAX_CHANNELS = 64;
const int MIX_DEFAULT_ALLOCATED = 8;

struct Channel
{
    Mix_Chunk *chunk;
    Uint32 pos;            // bytes into chunk->abuf
    int loops;             // remaining repeats; -1 is forever
    int volume;            // 0..MIX_MAX_VOLUME
    bool paused;
    bool playing;
    Uint32 expire_ms;      // 0 when it plays to its end
    Uint32 started_ms;
    float left, right;     // panning gains, from Mix_SetPosition
};

Channel s_channel[MIX_MAX_CHANNELS];
int s_allocated = 0;

struct MusicState
{
    Uint8 *buf;
    Uint32 len;
    Uint32 pos;
    int loops;
    bool playing;
    bool paused;
} s_music = {};

Mix_Music *s_music_handle = nullptr;   // what the application was given
int s_music_volume = MIX_MAX_VOLUME;

void (*s_channel_finished)(int) = nullptr;
void (*s_music_finished)(void) = nullptr;
void (*s_music_hook)(void *, Uint8 *, int) = nullptr;
void *s_music_hook_data = nullptr;

// Remembered for an application's benefit; no synthesiser reads them.
char *s_soundfonts = nullptr;
char *s_timidity_cfg = nullptr;

// ---------------------------------------------------------------------------
// Effects
//
// A list per channel, plus one for MIX_CHANNEL_POST which runs on the
// finished mix. The order they were added in is the order they run in, which
// is SDL_mixer's contract: an application may stack two and expect the second
// to see the first one's output.
//
// The limit is a fixed array rather than a linked list because registration
// happens while the device is playing and the callback walks the list on
// every buffer: a fixed array needs no allocation on either side. Eight per
// channel is far past what any application registers — Chocolate Doom, the
// heaviest user here, registers one.
// ---------------------------------------------------------------------------

const int MIX_MAX_EFFECTS = 8;

struct Effect
{
    Mix_EffectFunc_t f;
    Mix_EffectDone_t done;
    void *arg;
};

struct EffectChain
{
    Effect e[MIX_MAX_EFFECTS];
    int count;
};

EffectChain s_channel_effect[MIX_MAX_CHANNELS] = {};
EffectChain s_post_effect = {};

void (SDLCALL *s_postmix)(void *, Uint8 *, int) = nullptr;
void *s_postmix_arg = nullptr;

void RunEffects(const EffectChain &chain, int chan, void *stream, int len)
{
    for (int i = 0; i < chain.count; i++)
        if (chain.e[i].f)
            chain.e[i].f(chan, stream, len, chain.e[i].arg);
}

// SDL_mixer tells an effect when its channel stops, which is how an effect
// holding state per sound knows to drop it. The post chain never stops — the
// mix is always playing — so this is only ever reached for a real channel.
void RunEffectsDone(const EffectChain &chain, int chan)
{
    for (int i = 0; i < chain.count; i++)
        if (chain.e[i].done)
            chain.e[i].done(chan, chain.e[i].arg);
}

// The last stage of every buffer: the post effects on the finished mix, in
// the order they were registered, then the post-mix callback. Reached even
// when the mix above could not be made, because an application's own sound
// engine writes from here and keeps its time by counting the samples it is
// asked for — a buffer it never sees is time it never advances.
void RunPost(Uint8 *stream, int len)
{
    RunEffects(s_post_effect, MIX_CHANNEL_POST, stream, len);
    if (s_postmix)
        s_postmix(s_postmix_arg, stream, len);
}

// A channel has stopped, for whatever reason. Everything that wanted telling
// gets told from here, so a sound running out, a time limit arriving and
// Mix_HaltChannel all say the same thing in the same order.
void ChannelDone(int c)
{
    RunEffectsDone(s_channel_effect[c], c);
    if (s_channel_finished)
        s_channel_finished(c);
}

// Add one source into the accumulator with a gain per side. Everything is
// 16-bit signed stereo here, which is what the device speaks.
void MixInto(Sint32 *acc, const Uint8 *src, int frames, float left, float right)
{
    const Sint16 *s = (const Sint16 *)src;
    for (int i = 0; i < frames; i++)
    {
        acc[i * 2]     += (Sint32)(s[i * 2]     * left);
        acc[i * 2 + 1] += (Sint32)(s[i * 2 + 1] * right);
    }
}

// The same, into a buffer of the device's own 16-bit samples. This is what a
// channel carrying effects is rendered into, because an effect is handed its
// channel in the format the device plays and not in the wider accumulator
// the summing uses. No sample can exceed the range: every gain here is at
// most 1 and the source is already 16-bit.
void MixIntoS16(Sint16 *dst, const Uint8 *src, int frames, float left, float right)
{
    const Sint16 *s = (const Sint16 *)src;
    for (int i = 0; i < frames; i++)
    {
        dst[i * 2]     = (Sint16)(s[i * 2]     * left);
        dst[i * 2 + 1] = (Sint16)(s[i * 2 + 1] * right);
    }
}

// The callback the device pulls from. Everything playing is summed into a
// wider accumulator and clamped once at the end, so two loud sounds together
// distort rather than wrapping to the opposite polarity — a wrap is heard as
// a crack, which sounds like a fault rather than like loudness.
void SDLCALL MixerCallback(void *, Uint8 *stream, int len)
{
    memset(stream, 0, (size_t)len);

    const int bytes_per_frame = 4;            // 16-bit stereo
    const int frames = len / bytes_per_frame;
    if (frames <= 0)
        return;

    // An application-installed music hook replaces the mixer's own music
    // entirely, which is SDL_mixer's contract for it.
    if (s_music_hook)
        s_music_hook(s_music_hook_data, stream, len);

    Sint32 *acc = (Sint32 *)SDL_calloc((size_t)frames * 2, sizeof(Sint32));
    if (!acc)
    {
        RunPost((Uint8 *)stream, len);
        return;
    }

    // A channel carrying effects is rendered on its own before it joins the
    // sum, so its effect chain is handed that channel's samples and nothing
    // else. The buffer is only allocated when some channel has effects at
    // all, which is usually none of them.
    Sint16 *chs = nullptr;
    for (int c = 0; c < s_allocated; c++)
        if (s_channel_effect[c].count > 0)
        {
            chs = (Sint16 *)SDL_malloc((size_t)frames * 2 * sizeof(Sint16));
            if (!chs)
            {
                SDL_free(acc);
                RunPost((Uint8 *)stream, len);
                return;
            }
            break;
        }

    // Whatever the hook already wrote is part of the mix.
    if (s_music_hook)
    {
        const Sint16 *existing = (const Sint16 *)stream;
        for (int i = 0; i < frames * 2; i++)
            acc[i] += existing[i];
    }

    const Uint32 now = SDL_GetTicks();

    // Music first, so effects sit on top of it.
    if (!s_music_hook && s_music.playing && !s_music.paused && s_music.buf)
    {
        int done = 0;
        while (done < frames && s_music.playing)
        {
            const Uint32 left_bytes = s_music.len - s_music.pos;
            int take = (int)(left_bytes / bytes_per_frame);
            if (take > frames - done)
                take = frames - done;

            if (take > 0)
            {
                const float g = (float)s_music_volume / (float)MIX_MAX_VOLUME;
                MixInto(acc + done * 2, s_music.buf + s_music.pos, take, g, g);
                s_music.pos += (Uint32)take * bytes_per_frame;
                done += take;
            }

            if (s_music.pos >= s_music.len)
            {
                if (s_music.loops < 0 || --s_music.loops > 0)
                {
                    s_music.pos = 0;
                }
                else
                {
                    s_music.playing = false;
                    if (s_music_finished)
                        s_music_finished();
                }
            }
            if (take <= 0 && s_music.playing && s_music.pos < s_music.len)
                break;   // nothing consumed and nothing to loop: stop trying
        }
    }

    for (int c = 0; c < s_allocated; c++)
    {
        Channel &ch = s_channel[c];
        if (!ch.playing || ch.paused || !ch.chunk || !ch.chunk->abuf)
            continue;

        // A channel given a time limit stops when it is reached, wherever in
        // the sound it happens to be.
        if (ch.expire_ms && now >= ch.expire_ms)
        {
            ch.playing = false;
            ChannelDone(c);
            continue;
        }

        const bool effects = chs && s_channel_effect[c].count > 0;
        if (effects)
            memset(chs, 0, (size_t)frames * 2 * sizeof(Sint16));

        int done = 0;
        while (done < frames && ch.playing)
        {
            const Uint32 left_bytes = ch.chunk->alen - ch.pos;
            int take = (int)(left_bytes / bytes_per_frame);
            if (take > frames - done)
                take = frames - done;

            if (take > 0)
            {
                // The channel's volume and the chunk's own multiply, which
                // is what SDL_mixer does: a quiet sound stays quiet on a
                // loud channel.
                const float g = ((float)ch.volume / (float)MIX_MAX_VOLUME)
                              * ((float)ch.chunk->volume / (float)MIX_MAX_VOLUME);
                if (effects)
                    MixIntoS16(chs + done * 2, ch.chunk->abuf + ch.pos, take,
                               ch.left * g, ch.right * g);
                else
                    MixInto(acc + done * 2, ch.chunk->abuf + ch.pos, take,
                            ch.left * g, ch.right * g);
                ch.pos += (Uint32)take * bytes_per_frame;
                done += take;
            }

            if (ch.pos >= ch.chunk->alen)
            {
                if (ch.loops < 0 || ch.loops-- > 0)
                {
                    ch.pos = 0;
                }
                else
                {
                    ch.playing = false;
                    ChannelDone(c);
                }
            }
            if (take <= 0 && ch.playing && ch.pos < ch.chunk->alen)
                break;
        }

        // The channel's own effects, then what they left behind joins the sum.
        if (effects)
        {
            RunEffects(s_channel_effect[c], c, chs, frames * bytes_per_frame);
            for (int i = 0; i < frames * 2; i++)
                acc[i] += chs[i];
        }
    }

    Sint16 *out = (Sint16 *)stream;
    for (int i = 0; i < frames * 2; i++)
    {
        Sint32 v = acc[i];
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (Sint16)v;
    }
    SDL_free(acc);
    SDL_free(chs);

    RunPost((Uint8 *)stream, len);
}

// Everything a chunk or a track has to become before it can be mixed: the
// device's own format. Returns a freshly allocated buffer, or null with the
// error set.
Uint8 *ConvertToDevice(const SDL_AudioSpec &from, Uint8 *data, Uint32 len,
                       Uint32 *out_len)
{
    SDL_AudioCVT cvt;
    const int need = SDL_BuildAudioCVT(&cvt, from.format, from.channels,
                                       from.freq, s_spec.format,
                                       s_spec.channels, s_spec.freq);
    if (need < 0)
        return nullptr;   // SDL_BuildAudioCVT has set the error

    if (need == 0)
    {
        Uint8 *copy = (Uint8 *)SDL_malloc(len ? len : 1);
        if (!copy)
        {
            SDL_OutOfMemory();
            return nullptr;
        }
        memcpy(copy, data, len);
        *out_len = len;
        return copy;
    }

    cvt.len = (int)len;
    cvt.buf = (Uint8 *)SDL_malloc((size_t)len * cvt.len_mult + 1);
    if (!cvt.buf)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    memcpy(cvt.buf, data, len);
    if (SDL_ConvertAudio(&cvt) < 0)
    {
        SDL_free(cvt.buf);
        return nullptr;
    }
    *out_len = (Uint32)cvt.len_cvt;
    return cvt.buf;
}

// Which channel a request lands on. SDL_mixer's -1 means the first one not
// already playing.
int PickChannel(int channel)
{
    if (channel >= 0)
        return (channel < s_allocated) ? channel : -1;
    for (int c = 0; c < s_allocated; c++)
        if (!s_channel[c].playing)
            return c;
    return -1;
}

void ResetChannel(Channel &ch)
{
    ch.chunk = nullptr;
    ch.pos = 0;
    ch.loops = 0;
    ch.volume = MIX_MAX_VOLUME;
    ch.paused = false;
    ch.playing = false;
    ch.expire_ms = 0;
    ch.started_ms = 0;
    ch.left = 1.0f;
    ch.right = 1.0f;
}

} // namespace

// Mix_Music is opaque to the application, so its shape is ours. One track
// plays at a time, but a program may hold several loaded at once.
struct _Mix_Music
{
    Uint8 *buf;
    Uint32 len;
};

// ---------------------------------------------------------------------------
// Opening and closing
// ---------------------------------------------------------------------------

extern "C" const SDL_version *Mix_Linked_Version(void)
{
    static SDL_version version;
    SDL_MIXER_VERSION(&version);
    return &version;
}

// As with IMG_Init: the answer is which of the ASKED-FOR decoders are
// available. WAV needs no flag in SDL_mixer, and every flag names a format
// this build has no decoder for, so the honest answer is none of them.
extern "C" int Mix_Init(int) { return 0; }

extern "C" void Mix_Quit(void) {}

extern "C" int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels,
                                   int chunksize, const char *, int)
{
    if (s_open)
    {
        s_open_count++;
        return 0;   // already open; SDL_mixer counts the opens
    }

    SDL_AudioSpec desired;
    memset(&desired, 0, sizeof(desired));
    desired.freq = frequency > 0 ? frequency : MIX_DEFAULT_FREQUENCY;
    desired.format = format ? format : MIX_DEFAULT_FORMAT;
    desired.channels = (Uint8)(channels > 0 ? channels : MIX_DEFAULT_CHANNELS);
    desired.samples = (Uint16)(chunksize > 0 ? chunksize : 1024);
    desired.callback = MixerCallback;
    desired.userdata = nullptr;

    // The device states its own terms and everything is converted to them;
    // asking it to match the application would be asking it to do the
    // conversion this file exists to do.
    if (SDL_OpenAudioDevice(nullptr, 0, &desired, &s_spec,
                            SDL_AUDIO_ALLOW_ANY_CHANGE) == 0)
        return -1;   // SDL_OpenAudioDevice has set the error

    s_open = true;
    s_open_count = 1;

    Mix_AllocateChannels(MIX_DEFAULT_ALLOCATED);
    SDL_PauseAudioDevice(1, 0);

    SDL2Circle_Log("mixer", SDL2CIRCLE_LOG_NOTICE,
                   "open: %d Hz, %d channels, %u-sample buffer",
                   s_spec.freq, s_spec.channels, (unsigned)s_spec.samples);
    return 0;
}

extern "C" int Mix_OpenAudio(int frequency, Uint16 format, int channels,
                             int chunksize)
{
    return Mix_OpenAudioDevice(frequency, format, channels, chunksize,
                               nullptr, 0);
}

extern "C" void Mix_CloseAudio(void)
{
    if (!s_open)
        return;
    if (--s_open_count > 0)
        return;

    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    SDL_CloseAudioDevice(1);
    s_open = false;
    s_allocated = 0;
}

extern "C" int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels)
{
    if (!s_open)
        return 0;
    if (frequency) *frequency = s_spec.freq;
    if (format)    *format = s_spec.format;
    if (channels)  *channels = s_spec.channels;
    return s_open_count;
}

extern "C" int Mix_AllocateChannels(int numchans)
{
    if (numchans < 0)
        return s_allocated;
    if (numchans > MIX_MAX_CHANNELS)
        numchans = MIX_MAX_CHANNELS;

    for (int c = numchans; c < s_allocated; c++)
        ResetChannel(s_channel[c]);
    for (int c = s_allocated; c < numchans; c++)
        ResetChannel(s_channel[c]);

    s_allocated = numchans;
    return s_allocated;
}

// ---------------------------------------------------------------------------
// Chunks
// ---------------------------------------------------------------------------

extern "C" Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
    if (!src)
    {
        Mix_SetError("Mix_LoadWAV_RW: no source");
        return nullptr;
    }
    if (!s_open)
    {
        if (freesrc)
            SDL_RWclose(src);
        Mix_SetError("Mix_LoadWAV_RW: the audio device is not open");
        return nullptr;
    }

    SDL_AudioSpec wav;
    Uint8 *data = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_RW(src, freesrc, &wav, &data, &len))
        return nullptr;   // SDL_LoadWAV_RW has set the error

    Uint32 converted_len = 0;
    Uint8 *converted = ConvertToDevice(wav, data, len, &converted_len);
    SDL_FreeWAV(data);
    if (!converted)
        return nullptr;

    Mix_Chunk *chunk = (Mix_Chunk *)SDL_malloc(sizeof(Mix_Chunk));
    if (!chunk)
    {
        SDL_free(converted);
        SDL_OutOfMemory();
        return nullptr;
    }
    chunk->allocated = 1;
    chunk->abuf = converted;
    chunk->alen = converted_len;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

extern "C" Mix_Chunk *Mix_LoadWAV(const char *file)
{
    SDL_RWops *src = SDL_RWFromFile(file, "rb");
    if (!src)
    {
        Mix_SetError("Mix_LoadWAV: cannot open %s", file ? file : "(null)");
        return nullptr;
    }
    return Mix_LoadWAV_RW(src, 1);
}

// Samples already in the device's format, which the caller keeps ownership
// of — SDL_mixer's contract for this one.
extern "C" Mix_Chunk *Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len)
{
    Mix_Chunk *chunk = (Mix_Chunk *)SDL_malloc(sizeof(Mix_Chunk));
    if (!chunk)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    chunk->allocated = 0;
    chunk->abuf = mem;
    chunk->alen = len;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

extern "C" void Mix_FreeChunk(Mix_Chunk *chunk)
{
    if (!chunk)
        return;

    // A chunk still being mixed would be read after this returns.
    for (int c = 0; c < s_allocated; c++)
        if (s_channel[c].chunk == chunk)
        {
            s_channel[c].playing = false;
            s_channel[c].chunk = nullptr;
        }

    if (chunk->allocated)
        SDL_free(chunk->abuf);
    SDL_free(chunk);
}

extern "C" int Mix_VolumeChunk(Mix_Chunk *chunk, int volume)
{
    if (!chunk)
        return -1;
    const int was = chunk->volume;
    if (volume >= 0)
        chunk->volume = (Uint8)(volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume);
    return was;
}

// ---------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------

extern "C" int Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops,
                                    int ticks)
{
    if (!s_open || !chunk)
    {
        Mix_SetError("Mix_PlayChannelTimed: nothing to play");
        return -1;
    }
    const int c = PickChannel(channel);
    if (c < 0)
    {
        Mix_SetError("Mix_PlayChannelTimed: no channel free");
        return -1;
    }

    Channel &ch = s_channel[c];
    ch.chunk = chunk;
    ch.pos = 0;
    ch.loops = loops;
    ch.paused = false;
    ch.playing = true;
    ch.started_ms = SDL_GetTicks();
    ch.expire_ms = (ticks > 0) ? ch.started_ms + (Uint32)ticks : 0;
    return c;
}

extern "C" int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    return Mix_PlayChannelTimed(channel, chunk, loops, -1);
}

// Fading is not synthesised here; a fade-in starts at full volume and plays.
// The sound is right, its first moments are louder than asked for.
extern "C" int Mix_FadeInChannelTimed(int channel, Mix_Chunk *chunk, int loops,
                                      int, int ticks)
{
    return Mix_PlayChannelTimed(channel, chunk, loops, ticks);
}

extern "C" int Mix_FadeInChannel(int channel, Mix_Chunk *chunk, int loops,
                                 int ms)
{
    return Mix_FadeInChannelTimed(channel, chunk, loops, ms, -1);
}

extern "C" int Mix_HaltChannel(int channel)
{
    if (channel < 0)
    {
        for (int c = 0; c < s_allocated; c++)
            if (s_channel[c].playing)
            {
                s_channel[c].playing = false;
                ChannelDone(c);
            }
        return 0;
    }
    if (channel < s_allocated && s_channel[channel].playing)
    {
        s_channel[channel].playing = false;
        ChannelDone(channel);
    }
    return 0;
}

extern "C" int Mix_FadeOutChannel(int channel, int)
{
    return Mix_HaltChannel(channel);   // stops now; nothing fades
}

extern "C" int Mix_ExpireChannel(int channel, int ticks)
{
    const Uint32 when = (ticks > 0) ? SDL_GetTicks() + (Uint32)ticks : 0;
    if (channel < 0)
    {
        for (int c = 0; c < s_allocated; c++)
            s_channel[c].expire_ms = when;
        return s_allocated;
    }
    if (channel < s_allocated)
    {
        s_channel[channel].expire_ms = when;
        return 1;
    }
    return 0;
}

extern "C" void Mix_Pause(int channel)
{
    if (channel < 0)
        for (int c = 0; c < s_allocated; c++)
            s_channel[c].paused = true;
    else if (channel < s_allocated)
        s_channel[channel].paused = true;
}

extern "C" void Mix_Resume(int channel)
{
    if (channel < 0)
        for (int c = 0; c < s_allocated; c++)
            s_channel[c].paused = false;
    else if (channel < s_allocated)
        s_channel[channel].paused = false;
}

extern "C" int Mix_Paused(int channel)
{
    if (channel < 0)
    {
        int n = 0;
        for (int c = 0; c < s_allocated; c++)
            if (s_channel[c].paused)
                n++;
        return n;
    }
    return (channel < s_allocated && s_channel[channel].paused) ? 1 : 0;
}

extern "C" int Mix_Playing(int channel)
{
    if (channel < 0)
    {
        int n = 0;
        for (int c = 0; c < s_allocated; c++)
            if (s_channel[c].playing)
                n++;
        return n;
    }
    return (channel < s_allocated && s_channel[channel].playing) ? 1 : 0;
}

extern "C" int Mix_Volume(int channel, int volume)
{
    if (channel < 0)
    {
        int total = 0;
        for (int c = 0; c < s_allocated; c++)
        {
            total += s_channel[c].volume;
            if (volume >= 0)
                s_channel[c].volume =
                    volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume;
        }
        return s_allocated ? total / s_allocated : 0;
    }
    if (channel >= s_allocated)
        return -1;

    const int was = s_channel[channel].volume;
    if (volume >= 0)
        s_channel[channel].volume =
            volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume;
    return was;
}

extern "C" Mix_Fading Mix_FadingChannel(int) { return MIX_NO_FADING; }

extern "C" Mix_Chunk *Mix_GetChunk(int channel)
{
    return (channel >= 0 && channel < s_allocated) ? s_channel[channel].chunk
                                                   : nullptr;
}

extern "C" void Mix_ChannelFinished(void (*channel_finished)(int channel))
{
    s_channel_finished = channel_finished;
}

// ---------------------------------------------------------------------------
// Placing a sound in the stereo field
// ---------------------------------------------------------------------------

// SDL_mixer's angle is degrees clockwise from straight ahead, and distance
// is 0 (here) to 255 (as far away as it gets). Both become a gain per side.
extern "C" int Mix_SetPosition(int channel, Sint16 angle, Uint8 distance)
{
    if (channel < 0 || channel >= s_allocated)
        return 0;

    Channel &ch = s_channel[channel];
    const float near_gain = 1.0f - (float)distance / 255.0f;

    // Straight ahead or behind is even; the sides are where it separates.
    int a = angle % 360;
    if (a < 0)
        a += 360;
    float pan;                            // -1 hard left, +1 hard right
    if (a <= 180)
        pan = (a <= 90) ? (float)a / 90.0f : (float)(180 - a) / 90.0f;
    else
        pan = (a <= 270) ? -(float)(a - 180) / 90.0f : -(float)(360 - a) / 90.0f;

    ch.right = near_gain * (0.5f + 0.5f * pan);
    ch.left  = near_gain * (0.5f - 0.5f * pan);

    // Dead centre must not be quieter than no positioning at all.
    if (pan == 0.0f)
        ch.left = ch.right = near_gain;
    return 1;
}

extern "C" int Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
    if (channel < 0 || channel >= s_allocated)
        return 0;
    s_channel[channel].left = (float)left / 255.0f;
    s_channel[channel].right = (float)right / 255.0f;
    return 1;
}

extern "C" int Mix_SetDistance(int channel, Uint8 distance)
{
    return Mix_SetPosition(channel, 0, distance);
}

// ---------------------------------------------------------------------------
// Effects
//
// An effect is a function handed a buffer of samples in the device's format,
// free to rewrite it in place. Registered on a channel it sees that channel
// alone; registered on MIX_CHANNEL_POST it sees the whole finished mix.
//
// The post channel is the one that matters most here, because it is how an
// application with a sound engine of its own — Chocolate Doom's emulated OPL
// chip, for one — puts that engine's output into the stream. Nothing above
// has to know what it is synthesising.
// ---------------------------------------------------------------------------

namespace
{

// Which chain a channel number names. Null for a channel that does not exist,
// which is how a bad registration is turned away.
EffectChain *ChainFor(int channel)
{
    if (channel == MIX_CHANNEL_POST)
        return &s_post_effect;
    if (channel >= 0 && channel < s_allocated)
        return &s_channel_effect[channel];
    return nullptr;
}

} // namespace

extern "C" int Mix_RegisterEffect(int chan, Mix_EffectFunc_t f,
                                  Mix_EffectDone_t d, void *arg)
{
    EffectChain *chain = ChainFor(chan);
    if (!chain)
    {
        Mix_SetError("Mix_RegisterEffect: no channel %d", chan);
        return 0;
    }
    if (!f)
    {
        Mix_SetError("Mix_RegisterEffect: no effect function");
        return 0;
    }
    if (chain->count >= MIX_MAX_EFFECTS)
    {
        Mix_SetError("Mix_RegisterEffect: channel %d already carries %d "
                     "effects", chan, MIX_MAX_EFFECTS);
        return 0;
    }

    // The device's callback walks this chain. Fill the slot before the count
    // admits it exists, so a callback landing between the two reads a chain
    // that is one effect short rather than one effect of rubbish.
    chain->e[chain->count].f = f;
    chain->e[chain->count].done = d;
    chain->e[chain->count].arg = arg;
    chain->count++;
    return 1;
}

extern "C" int Mix_UnregisterEffect(int channel, Mix_EffectFunc_t f)
{
    EffectChain *chain = ChainFor(channel);
    if (!chain)
    {
        Mix_SetError("Mix_UnregisterEffect: no channel %d", channel);
        return 0;
    }

    int found = 0;
    for (int i = 0; i < chain->count; )
    {
        if (chain->e[i].f != f)
        {
            i++;
            continue;
        }

        Effect gone = chain->e[i];
        for (int j = i; j + 1 < chain->count; j++)
            chain->e[j] = chain->e[j + 1];
        chain->count--;
        found++;

        // Told after it has left the chain, so an effect that unregisters
        // something from inside its own done function finds the chain as it
        // will be and not as it was.
        if (gone.done)
            gone.done(channel, gone.arg);
    }

    if (!found)
    {
        Mix_SetError("Mix_UnregisterEffect: that effect is not on channel %d",
                     channel);
        return 0;
    }
    return 1;
}

// Panning is an effect in SDL_mixer, so clearing a channel's effects clears
// its panning with them. It is not one here — a gain per side is part of what
// the mixing loop already does — so the panning is put back by hand.
extern "C" int Mix_UnregisterAllEffects(int channel)
{
    EffectChain *chain = ChainFor(channel);
    if (!chain)
    {
        Mix_SetError("Mix_UnregisterAllEffects: no channel %d", channel);
        return 0;
    }

    const EffectChain was = *chain;
    chain->count = 0;
    for (int i = 0; i < was.count; i++)
        if (was.e[i].done)
            was.e[i].done(channel, was.e[i].arg);

    if (channel >= 0 && channel < s_allocated)
    {
        s_channel[channel].left = 1.0f;
        s_channel[channel].right = 1.0f;
    }
    return 1;
}

// The very last thing to touch a buffer before the device gets it, after
// every channel, the music and the post effects.
extern "C" void Mix_SetPostMix(void (SDLCALL *mix_func)(void *, Uint8 *, int),
                               void *arg)
{
    s_postmix = mix_func;
    s_postmix_arg = arg;
}

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------

extern "C" Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc)
{
    if (!src)
    {
        Mix_SetError("Mix_LoadMUS_RW: no source");
        return nullptr;
    }
    if (!s_open)
    {
        if (freesrc)
            SDL_RWclose(src);
        Mix_SetError("Mix_LoadMUS_RW: the audio device is not open");
        return nullptr;
    }

    // Enough of the head to tell what this is, so that a format with no
    // decoder is named rather than played as noise.
    const Sint64 start = SDL_RWtell(src);
    Uint8 head[4] = { 0, 0, 0, 0 };
    SDL_RWread(src, head, 1, sizeof(head));
    SDL_RWseek(src, start, RW_SEEK_SET);

    const char *what = nullptr;
    if (memcmp(head, "MThd", 4) == 0)
        what = "MIDI";
    else if (memcmp(head, "OggS", 4) == 0)
        what = "Ogg Vorbis";
    else if (memcmp(head, "fLaC", 4) == 0)
        what = "FLAC";
    else if (memcmp(head, "ID3", 3) == 0 || (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0))
        what = "MP3";

    if (what)
    {
        if (freesrc)
            SDL_RWclose(src);
        if (memcmp(head, "MThd", 4) == 0)
            Mix_SetError("Mix_LoadMUS_RW: this is a MIDI score, and there is "
                         "no synthesiser in this build to perform it");
        else
            Mix_SetError("Mix_LoadMUS_RW: %s is not a format this build "
                         "decodes; supply the music as WAV", what);
        return nullptr;
    }

    SDL_AudioSpec wav;
    Uint8 *data = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_RW(src, freesrc, &wav, &data, &len))
        return nullptr;

    Uint32 converted_len = 0;
    Uint8 *converted = ConvertToDevice(wav, data, len, &converted_len);
    SDL_FreeWAV(data);
    if (!converted)
        return nullptr;

    Mix_Music *music = (Mix_Music *)SDL_malloc(sizeof(Mix_Music));
    if (!music)
    {
        SDL_free(converted);
        SDL_OutOfMemory();
        return nullptr;
    }
    music->buf = converted;
    music->len = converted_len;
    return music;
}

extern "C" Mix_Music *Mix_LoadMUS(const char *file)
{
    SDL_RWops *src = SDL_RWFromFile(file, "rb");
    if (!src)
    {
        Mix_SetError("Mix_LoadMUS: cannot open %s", file ? file : "(null)");
        return nullptr;
    }
    return Mix_LoadMUS_RW(src, 1);
}

extern "C" Mix_Music *Mix_LoadMUSType_RW(SDL_RWops *src, Mix_MusicType, int freesrc)
{
    return Mix_LoadMUS_RW(src, freesrc);
}

extern "C" void Mix_FreeMusic(Mix_Music *music)
{
    if (!music)
        return;
    if (s_music_handle == music)
    {
        s_music.playing = false;
        s_music.buf = nullptr;
        s_music_handle = nullptr;
    }
    SDL_free(music->buf);
    SDL_free(music);
}

extern "C" int Mix_PlayMusic(Mix_Music *music, int loops)
{
    if (!s_open || !music)
        return Mix_SetError("Mix_PlayMusic: nothing to play");

    s_music.buf = music->buf;
    s_music.len = music->len;
    s_music.pos = 0;
    s_music.loops = loops;
    s_music.playing = true;
    s_music.paused = false;
    s_music_handle = music;
    return 0;
}

extern "C" int Mix_FadeInMusic(Mix_Music *music, int loops, int)
{
    return Mix_PlayMusic(music, loops);   // starts at volume; nothing fades
}

extern "C" int Mix_FadeInMusicPos(Mix_Music *music, int loops, int ms,
                                  double position)
{
    if (Mix_FadeInMusic(music, loops, ms) < 0)
        return -1;
    return Mix_SetMusicPosition(position);
}

extern "C" int Mix_SetMusicPosition(double position)
{
    if (!s_music.buf || !s_music.playing)
        return Mix_SetError("Mix_SetMusicPosition: no music is playing");
    if (position < 0.0)
        position = 0.0;

    // Seconds into bytes, at the device's own rate and width.
    const Uint32 bytes_per_second =
        (Uint32)s_spec.freq * s_spec.channels * (SDL_AUDIO_BITSIZE(s_spec.format) / 8);
    Uint32 pos = (Uint32)(position * (double)bytes_per_second);
    pos -= pos % 4;                       // keep it on a frame boundary
    if (pos > s_music.len)
        pos = s_music.len;
    s_music.pos = pos;
    return 0;
}

extern "C" int Mix_HaltMusic(void)
{
    const bool was = s_music.playing;
    s_music.playing = false;
    if (was && s_music_finished)
        s_music_finished();
    return 0;
}

extern "C" int Mix_FadeOutMusic(int) { return Mix_HaltMusic(); }

extern "C" void Mix_PauseMusic(void)  { s_music.paused = true; }
extern "C" void Mix_ResumeMusic(void) { s_music.paused = false; }
extern "C" void Mix_RewindMusic(void) { s_music.pos = 0; }

extern "C" int Mix_PausedMusic(void)  { return s_music.paused ? 1 : 0; }
extern "C" int Mix_PlayingMusic(void) { return s_music.playing ? 1 : 0; }

extern "C" int Mix_VolumeMusic(int volume)
{
    const int was = s_music_volume;
    if (volume >= 0)
        s_music_volume = volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume;
    return was;
}

extern "C" Mix_Fading Mix_FadingMusic(void) { return MIX_NO_FADING; }

// SDL_mixer plays music through an external program by handing the track to
// a command line and letting a separate process do the work. There are no
// processes here and no shell to start one from, so this refuses rather than
// accepting a command it will never run — an application that asked for an
// external player and got silence would have nothing to go on.
extern "C" int Mix_SetMusicCMD(const char *command)
{
    return Mix_SetError("Mix_SetMusicCMD: `%s' cannot be run — this build has "
                        "no way to start a separate program",
                        command ? command : "");
}

extern "C" Mix_MusicType Mix_GetMusicType(const Mix_Music *)
{
    return MUS_WAV;   // the only kind that loads
}

extern "C" void Mix_HookMusic(void (SDLCALL *mix_func)(void *, Uint8 *, int),
                              void *arg)
{
    s_music_hook = mix_func;
    s_music_hook_data = arg;
}

extern "C" void *Mix_GetMusicHookData(void) { return s_music_hook_data; }

extern "C" void Mix_HookMusicFinished(void (*music_finished)(void))
{
    s_music_finished = music_finished;
}

// ---------------------------------------------------------------------------
// MIDI configuration
//
// Remembered so an application's own setup still works, and read by nothing:
// there is no synthesiser here. Mix_LoadMUS says so when handed a score.
// ---------------------------------------------------------------------------

extern "C" int Mix_SetSoundFonts(const char *paths)
{
    SDL_free(s_soundfonts);
    s_soundfonts = paths ? SDL_strdup(paths) : nullptr;
    return 1;
}

extern "C" const char *Mix_GetSoundFonts(void) { return s_soundfonts; }

extern "C" int Mix_SetTimidityCfg(const char *path)
{
    SDL_free(s_timidity_cfg);
    s_timidity_cfg = path ? SDL_strdup(path) : nullptr;
    return 1;
}

extern "C" const char *Mix_GetTimidityCfg(void) { return s_timidity_cfg; }

extern "C" int Mix_EachSoundFont(int (*)(const char *, void *), void *)
{
    return 0;
}

// ---------------------------------------------------------------------------
// Decoder enumeration
// ---------------------------------------------------------------------------

extern "C" int Mix_GetNumChunkDecoders(void) { return 1; }

extern "C" const char *Mix_GetChunkDecoder(int index)
{
    return index == 0 ? "WAVE" : nullptr;
}

extern "C" SDL_bool Mix_HasChunkDecoder(const char *name)
{
    return (name && SDL_strcasecmp(name, "WAVE") == 0) ? SDL_TRUE : SDL_FALSE;
}

extern "C" int Mix_GetNumMusicDecoders(void) { return 1; }

extern "C" const char *Mix_GetMusicDecoder(int index)
{
    return index == 0 ? "WAVE" : nullptr;
}

extern "C" SDL_bool Mix_HasMusicDecoder(const char *name)
{
    return (name && SDL_strcasecmp(name, "WAVE") == 0) ? SDL_TRUE : SDL_FALSE;
}
