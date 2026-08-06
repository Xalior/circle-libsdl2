//
// audiocvt.cpp — SDL2's audio conversion, and the WAV loader that feeds it.
//
// The one sound device speaks 16-bit signed stereo at 48 kHz and nothing
// else. Almost no application produces exactly that, so something has to
// convert, and SDL's answer is SDL_BuildAudioCVT / SDL_ConvertAudio: the
// caller describes both ends, is told how much room the result needs, and
// converts in place in a buffer of its own.
//
// HOW IT CONVERTS. Everything goes through 32-bit float samples in the
// range [-1, 1]: the source is widened into floats, resampled, mapped onto
// the destination's channel count, and narrowed into the destination format.
// One path serves every combination, so there is no pairing that happens to
// be missing, and the intermediate is wide enough that nothing is lost on
// the way through. It costs more than a purpose-built loop for one pairing
// would, and it is paid on the application's own core.
//
// WHAT IS CONVERTED: U8, S8, U16, S16, S32 and F32 in either byte order, one
// or two channels, any rate ratio by linear interpolation. More than two
// channels is refused rather than approximated — there is nowhere for a
// third channel to go on a stereo device, and silently dropping it would be
// a mix nobody asked for.
//
// WHERE THE CHANNEL COUNTS LIVE. SDL_AudioCVT carries the two formats and
// the rate ratio in named fields, but not the two channel counts: SDL2 keeps
// those inside the chain of filter functions it builds, and there is no such
// chain here. The structure's layout is SDL's ABI and cannot grow a field,
// so the counts ride in the first two slots of the otherwise unused
// `filters` array. That array belongs to the caller's own structure, which
// already has to survive between the two calls, so nothing is allocated and
// nothing can be left behind.
//
#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

// The filter slots, borrowed to carry the channel counts from
// SDL_BuildAudioCVT to SDL_ConvertAudio.
void PutChannels(SDL_AudioCVT *cvt, int src, int dst)
{
    cvt->filters[0] = (SDL_AudioFilter)(uintptr_t)src;
    cvt->filters[1] = (SDL_AudioFilter)(uintptr_t)dst;
}

int SrcChannels(const SDL_AudioCVT *cvt)
{
    return (int)(uintptr_t)cvt->filters[0];
}

int DstChannels(const SDL_AudioCVT *cvt)
{
    return (int)(uintptr_t)cvt->filters[1];
}

bool FormatSupported(SDL_AudioFormat fmt)
{
    switch (fmt)
    {
    case AUDIO_U8:
    case AUDIO_S8:
    case AUDIO_U16LSB: case AUDIO_U16MSB:
    case AUDIO_S16LSB: case AUDIO_S16MSB:
    case AUDIO_S32LSB: case AUDIO_S32MSB:
    case AUDIO_F32LSB: case AUDIO_F32MSB:
        return true;
    default:
        return false;
    }
}

Uint16 Swap16(Uint16 v) { return (Uint16)((v >> 8) | (v << 8)); }

Uint32 Swap32(Uint32 v)
{
    return ((v >> 24) & 0x000000FF) | ((v >> 8) & 0x0000FF00)
         | ((v << 8) & 0x00FF0000) | ((v << 24) & 0xFF000000);
}

// One sample out of the source buffer, as a float in [-1, 1].
float ReadSample(const Uint8 *p, SDL_AudioFormat fmt)
{
    const bool swap = (SDL_AUDIO_ISBIGENDIAN(fmt) != 0) != (SDL_BYTEORDER == SDL_BIG_ENDIAN);

    switch (SDL_AUDIO_BITSIZE(fmt))
    {
    case 8:
        if (SDL_AUDIO_ISSIGNED(fmt))
            return (float)*(const Sint8 *)p / 128.0f;
        return ((float)*p - 128.0f) / 128.0f;

    case 16:
    {
        Uint16 raw;
        memcpy(&raw, p, 2);
        if (swap)
            raw = Swap16(raw);
        if (SDL_AUDIO_ISSIGNED(fmt))
            return (float)(Sint16)raw / 32768.0f;
        return ((float)raw - 32768.0f) / 32768.0f;
    }

    default:
    {
        Uint32 raw;
        memcpy(&raw, p, 4);
        if (swap)
            raw = Swap32(raw);
        if (SDL_AUDIO_ISFLOAT(fmt))
        {
            float f;
            memcpy(&f, &raw, 4);
            return f;
        }
        return (float)(Sint32)raw / 2147483648.0f;
    }
    }
}

// One float sample into the destination buffer, clamped so that a mix that
// went past full scale distorts rather than wrapping round to the opposite
// polarity — which is heard as a click, not as loudness.
void WriteSample(Uint8 *p, SDL_AudioFormat fmt, float v)
{
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;

    const bool swap = (SDL_AUDIO_ISBIGENDIAN(fmt) != 0) != (SDL_BYTEORDER == SDL_BIG_ENDIAN);

    switch (SDL_AUDIO_BITSIZE(fmt))
    {
    case 8:
        if (SDL_AUDIO_ISSIGNED(fmt))
            *(Sint8 *)p = (Sint8)(v * 127.0f);
        else
            *p = (Uint8)(v * 127.0f + 128.0f);
        return;

    case 16:
    {
        Uint16 raw;
        if (SDL_AUDIO_ISSIGNED(fmt))
            raw = (Uint16)(Sint16)(v * 32767.0f);
        else
            raw = (Uint16)(v * 32767.0f + 32768.0f);
        if (swap)
            raw = Swap16(raw);
        memcpy(p, &raw, 2);
        return;
    }

    default:
    {
        Uint32 raw;
        if (SDL_AUDIO_ISFLOAT(fmt))
            memcpy(&raw, &v, 4);
        else
            raw = (Uint32)(Sint32)(v * 2147483647.0f);
        if (swap)
            raw = Swap32(raw);
        memcpy(p, &raw, 4);
        return;
    }
    }
}

} // namespace

// Returns 1 when a conversion is needed, 0 when the two ends already agree,
// and -1 when this pairing is not one that can be converted. SDL's contract
// is that the caller then allocates cvt->len * cvt->len_mult bytes.
extern "C" int SDL_BuildAudioCVT(SDL_AudioCVT *cvt,
                                 SDL_AudioFormat src_format, Uint8 src_channels,
                                 int src_rate,
                                 SDL_AudioFormat dst_format, Uint8 dst_channels,
                                 int dst_rate)
{
    if (!cvt)
        return SDL_InvalidParamError("cvt");
    if (!FormatSupported(src_format))
        return SDL_SetError("SDL_BuildAudioCVT: source format 0x%04x is not "
                            "one this build converts", (unsigned)src_format);
    if (!FormatSupported(dst_format))
        return SDL_SetError("SDL_BuildAudioCVT: destination format 0x%04x is "
                            "not one this build converts", (unsigned)dst_format);
    if (src_channels < 1 || src_channels > 2 || dst_channels < 1 || dst_channels > 2)
        return SDL_SetError("SDL_BuildAudioCVT: %u to %u channels is not a "
                            "conversion this build makes (one or two either "
                            "way)", (unsigned)src_channels, (unsigned)dst_channels);
    if (src_rate <= 0 || dst_rate <= 0)
        return SDL_SetError("SDL_BuildAudioCVT: rates must be positive");

    memset(cvt, 0, sizeof(*cvt));
    cvt->src_format = src_format;
    cvt->dst_format = dst_format;
    cvt->rate_incr = (double)dst_rate / (double)src_rate;
    PutChannels(cvt, src_channels, dst_channels);

    const int src_bytes = SDL_AUDIO_BITSIZE(src_format) / 8;
    const int dst_bytes = SDL_AUDIO_BITSIZE(dst_format) / 8;

    // How the buffer grows: bytes per frame, then frames.
    const double frame_ratio = ((double)dst_bytes * dst_channels)
                             / ((double)src_bytes * src_channels);
    cvt->len_ratio = frame_ratio * cvt->rate_incr;

    // len_mult is a whole number and the caller sizes the buffer with it, so
    // it rounds UP — a buffer one byte short is a write past the end of an
    // allocation with nothing underneath to catch it.
    int mult = (int)cvt->len_ratio;
    if ((double)mult < cvt->len_ratio)
        mult++;
    if (mult < 1)
        mult = 1;
    cvt->len_mult = mult;

    cvt->needed = (src_format != dst_format || src_channels != dst_channels
                   || src_rate != dst_rate) ? 1 : 0;
    return cvt->needed;
}

// Converts cvt->buf in place, reading cvt->len bytes and setting cvt->len_cvt
// to what it produced.
extern "C" int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    if (!cvt || !cvt->buf)
        return SDL_InvalidParamError("cvt");
    if (!cvt->needed)
    {
        cvt->len_cvt = cvt->len;
        return 0;
    }

    const int src_ch = SrcChannels(cvt);
    const int dst_ch = DstChannels(cvt);
    const int src_bytes = SDL_AUDIO_BITSIZE(cvt->src_format) / 8;
    const int dst_bytes = SDL_AUDIO_BITSIZE(cvt->dst_format) / 8;
    if (src_ch < 1 || dst_ch < 1 || src_bytes < 1 || dst_bytes < 1)
        return SDL_SetError("SDL_ConvertAudio: this SDL_AudioCVT was not "
                            "built by SDL_BuildAudioCVT");

    const int src_frames = cvt->len / (src_bytes * src_ch);
    if (src_frames <= 0)
    {
        cvt->len_cvt = 0;
        return 0;
    }

    int dst_frames = (int)(src_frames * cvt->rate_incr);
    if (dst_frames < 1)
        dst_frames = 1;

    // The conversion cannot run in place: a destination frame is generally
    // read from two source frames, and writing forward would overwrite input
    // still to be read. So the source is taken aside first.
    const size_t src_len = (size_t)cvt->len;
    Uint8 *src = (Uint8 *)SDL_malloc(src_len);
    if (!src)
        return SDL_OutOfMemory();
    memcpy(src, cvt->buf, src_len);

    Uint8 *dst = cvt->buf;

    for (int i = 0; i < dst_frames; i++)
    {
        // Where this output frame falls between two input frames.
        const double pos = (double)i / cvt->rate_incr;
        int i0 = (int)pos;
        if (i0 > src_frames - 1)
            i0 = src_frames - 1;
        int i1 = i0 + 1;
        if (i1 > src_frames - 1)
            i1 = src_frames - 1;
        const float frac = (float)(pos - (double)i0);

        // Read the source frame, interpolated, into left and right.
        float left = 0.0f, right = 0.0f;
        for (int c = 0; c < src_ch; c++)
        {
            const Uint8 *p0 = src + ((size_t)i0 * src_ch + c) * src_bytes;
            const Uint8 *p1 = src + ((size_t)i1 * src_ch + c) * src_bytes;
            const float a = ReadSample(p0, cvt->src_format);
            const float b = ReadSample(p1, cvt->src_format);
            const float v = a + (b - a) * frac;
            if (c == 0) left = v; else right = v;
        }
        if (src_ch == 1)
            right = left;

        // Onto the destination's channels. One channel from two is their
        // average, which is the mix that keeps both rather than discarding
        // one; two from one is the same sample in both ears.
        for (int c = 0; c < dst_ch; c++)
        {
            float v;
            if (dst_ch == 1)
                v = (src_ch == 1) ? left : (left + right) * 0.5f;
            else
                v = (c == 0) ? left : right;
            WriteSample(dst + ((size_t)i * dst_ch + c) * dst_bytes,
                        cvt->dst_format, v);
        }
    }

    SDL_free(src);
    cvt->len_cvt = dst_frames * dst_ch * dst_bytes;
    return 0;
}

// ---------------------------------------------------------------------------
// Mixing one buffer into another
// ---------------------------------------------------------------------------

// SDL mixes with saturation rather than wrapping: two loud sounds together
// clip, which is heard as distortion, where a wrap would be heard as a
// crack at full amplitude in the wrong direction.
extern "C" void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src,
                                   SDL_AudioFormat format, Uint32 len,
                                   int volume)
{
    if (!dst || !src || volume <= 0 || !FormatSupported(format))
        return;
    if (volume > SDL_MIX_MAXVOLUME)
        volume = SDL_MIX_MAXVOLUME;

    const int bytes = SDL_AUDIO_BITSIZE(format) / 8;
    const float gain = (float)volume / (float)SDL_MIX_MAXVOLUME;
    const Uint32 samples = len / (Uint32)bytes;

    for (Uint32 i = 0; i < samples; i++)
    {
        Uint8 *d = dst + (size_t)i * bytes;
        const Uint8 *s = src + (size_t)i * bytes;
        const float mixed = ReadSample(d, format) + ReadSample(s, format) * gain;
        WriteSample(d, format, mixed);   // WriteSample clamps
    }
}

extern "C" void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len,
                             int volume)
{
    // SDL_MixAudio predates named formats and mixes in whatever the open
    // device speaks.
    SDL_AudioSpec spec;
    if (SDL_GetAudioDeviceSpec(0, 0, &spec) < 0)
        return;
    SDL_MixAudioFormat(dst, src, spec.format, len, volume);
}

// ---------------------------------------------------------------------------
// WAV
//
// RIFF/WAVE carrying PCM or IEEE float, which is what every WAV a game ships
// actually is. A compressed WAV — ADPCM, mu-law — is refused by name rather
// than played as noise.
//
// SDL's contract: the spec, buffer and length are filled in on success, the
// caller releases the buffer with SDL_FreeWAV, and the source is closed when
// freesrc is set whatever the outcome.
// ---------------------------------------------------------------------------

namespace
{

// The WAVE format tags that mean "samples, uncompressed".
const Uint16 WAVE_FORMAT_PCM        = 0x0001;
const Uint16 WAVE_FORMAT_IEEE_FLOAT = 0x0003;
const Uint16 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

} // namespace

extern "C" SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc,
                                         SDL_AudioSpec *spec, Uint8 **audio_buf,
                                         Uint32 *audio_len)
{
    SDL_AudioSpec *result = nullptr;

    if (!src || !spec || !audio_buf || !audio_len)
    {
        SDL_InvalidParamError("SDL_LoadWAV_RW");
        if (src && freesrc)
            SDL_RWclose(src);
        return nullptr;
    }

    *audio_buf = nullptr;
    *audio_len = 0;

    char riff[4], wave[4];
    if (SDL_RWread(src, riff, 1, 4) != 4)
    {
        SDL_SetError("SDL_LoadWAV_RW: not a RIFF file");
        goto done;
    }
    SDL_ReadLE32(src);   // the RIFF chunk size, which is not needed
    if (SDL_RWread(src, wave, 1, 4) != 4
        || memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0)
    {
        SDL_SetError("SDL_LoadWAV_RW: not a RIFF/WAVE file");
        goto done;
    }

    {
        Uint16 encoding = 0, channels = 0, bits = 0;
        Uint32 rate = 0;
        bool have_fmt = false;

        // Walk the chunks. `fmt ` describes the samples and `data` holds
        // them; anything else — LIST, fact, cue — is skipped by its own
        // length, which is why the length is read before the body.
        for (;;)
        {
            char id[4];
            if (SDL_RWread(src, id, 1, 4) != 4)
                break;
            const Uint32 len = SDL_ReadLE32(src);

            if (memcmp(id, "fmt ", 4) == 0)
            {
                if (len < 16)
                {
                    SDL_SetError("SDL_LoadWAV_RW: truncated format chunk");
                    goto done;
                }
                encoding = SDL_ReadLE16(src);
                channels = SDL_ReadLE16(src);
                rate     = SDL_ReadLE32(src);
                SDL_ReadLE32(src);              // bytes per second
                SDL_ReadLE16(src);              // block alignment
                bits     = SDL_ReadLE16(src);
                have_fmt = true;

                // An extensible header restates the real encoding in a GUID
                // whose first two bytes are the tag.
                if (len > 16)
                {
                    Sint64 extra = (Sint64)len - 16;
                    if (encoding == WAVE_FORMAT_EXTENSIBLE && extra >= 10)
                    {
                        SDL_ReadLE16(src);      // extension size
                        SDL_ReadLE16(src);      // valid bits per sample
                        SDL_ReadLE32(src);      // channel mask
                        encoding = SDL_ReadLE16(src);
                        extra -= 10;
                    }
                    if (extra > 0)
                        SDL_RWseek(src, extra, RW_SEEK_CUR);
                }
                // Chunks are padded to an even length.
                if (len & 1)
                    SDL_RWseek(src, 1, RW_SEEK_CUR);
            }
            else if (memcmp(id, "data", 4) == 0)
            {
                if (!have_fmt)
                {
                    SDL_SetError("SDL_LoadWAV_RW: sample data before its format");
                    goto done;
                }
                if (encoding != WAVE_FORMAT_PCM && encoding != WAVE_FORMAT_IEEE_FLOAT)
                {
                    SDL_SetError("SDL_LoadWAV_RW: WAVE encoding 0x%04x is "
                                 "compressed; only PCM and IEEE float are read",
                                 (unsigned)encoding);
                    goto done;
                }

                SDL_AudioFormat fmt;
                if (encoding == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
                    fmt = AUDIO_F32LSB;
                else if (bits == 8)
                    fmt = AUDIO_U8;             // 8-bit WAV is unsigned
                else if (bits == 16)
                    fmt = AUDIO_S16LSB;
                else if (bits == 32)
                    fmt = AUDIO_S32LSB;
                else
                {
                    SDL_SetError("SDL_LoadWAV_RW: %u bits per sample is not "
                                 "a width this build reads", (unsigned)bits);
                    goto done;
                }

                Uint8 *buf = (Uint8 *)SDL_malloc(len ? len : 1);
                if (!buf)
                {
                    SDL_OutOfMemory();
                    goto done;
                }
                if (SDL_RWread(src, buf, 1, len) != len)
                {
                    SDL_free(buf);
                    SDL_SetError("SDL_LoadWAV_RW: the sample data ended early");
                    goto done;
                }

                memset(spec, 0, sizeof(*spec));
                spec->freq = (int)rate;
                spec->format = fmt;
                spec->channels = (Uint8)channels;
                spec->samples = 4096;
                spec->size = len;

                *audio_buf = buf;
                *audio_len = len;
                result = spec;
                goto done;
            }
            else
            {
                if (SDL_RWseek(src, (Sint64)len + (len & 1), RW_SEEK_CUR) < 0)
                    break;
            }
        }

        SDL_SetError("SDL_LoadWAV_RW: no sample data in the file");
    }

done:
    if (freesrc)
        SDL_RWclose(src);
    return result;
}

extern "C" void SDL_FreeWAV(Uint8 *audio_buf)
{
    SDL_free(audio_buf);
}
