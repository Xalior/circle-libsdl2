//
// audio.cpp — SDL callback audio over Circle's HDMI sound device.
//
// One output device, S16 stereo. The application's audio callback runs on
// the main loop out of SDL_PumpEvents (never in IRQ context): each pump
// tops the buffers back up, invoking the callback once per block of free
// space.
//
// LATENCY IS THE BUDGET HERE, not capacity. Every sample sitting in a buffer
// is time between the game starting a sound and a player hearing it, and the
// stages add up: what the application core has produced and not yet handed
// over, what the sound device has been given and not yet played, and what the
// DMA is holding. A gunshot heard after its muzzle flash has gone is this sum
// being too large, and it is the reason none of the three is sized "generously".
//
// The three, smallest first, and what each one is protecting against:
//
//   RING_BLOCKS       how much finished audio may wait on the application
//                     core. It covers the gap between one SDL_PumpEvents and
//                     the next, which is one frame of the game's own loop.
//
//   QUEUE_MSECS       the sound device's own queue, filled by core 0's servo.
//                     It covers the gap between servo laps, and a lap is far
//                     under a millisecond — this is mostly protection against
//                     a lap that goes long, and it must stay comfortably
//                     larger than one DMA chunk, which leaves it in one go.
//
//   DMA_CHUNK_WORDS   what the DMA holds. Two buffers of this size are always
//                     in flight, so it is charged twice.
//
// WHEN THE GAME CORE IS LATE the downstream stages keep playing: the device
// queue and the DMA together are the runway, and only a stall longer than
// that is heard, as a break rather than as a delay. This library gives the
// game a core of its own and the servo another, so both gaps are far shorter
// than a desktop's would be, and the buffers are cut to match.
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

// TWO SPECS, and the difference between them is the whole of this file's
// contract with an application.
//
// s_spec is what the APPLICATION writes: the format, channel count and rate
// its callback fills a buffer in. It is what SDL_OpenAudioDevice reports in
// `obtained`, and it is the application's own spec unless the application
// gave permission for it to be changed.
//
// s_dev is what the DEVICE plays. The HDMI sound device speaks 16-bit signed
// stereo and nothing else.
//
// Where they differ, the library converts between them — see s_cvt. That is
// the point: an application that asks for mono and does NOT set
// SDL_AUDIO_ALLOW_CHANNELS_CHANGE has been promised mono, and handing its
// callback a stereo-sized buffer to fill produces garbled sound with no
// error anywhere. Either match what was asked, or change it and SAY SO in
// `obtained`. Ignoring the request is the one answer that is never right.
SDL_AudioSpec s_spec;
SDL_AudioSpec s_dev;

bool s_paused = true;
bool s_started = false;
int s_lock = 0;
unsigned s_queueFrames = 0;

// The callback's buffer. Large enough to hold the conversion's output as
// well as its input, because SDL_ConvertAudio works in place.
Uint8 *s_chunk = nullptr;

// Set when the two specs differ in any respect.
bool s_convert = false;
SDL_AudioCVT s_cvt;

// One chunk's worth, measured on the DEVICE's side of the conversion. The
// queue is counted in device frames, so these are what the pump and the
// drain must use — the application's own figures describe a different
// number of bytes whenever a conversion is in play.
unsigned s_dev_bytes = 0;
unsigned s_dev_frames = 0;

// The sound device's queue. Big enough that a DMA chunk leaving it does not
// empty it and the servo has room to put the next one in behind; small enough
// that what is waiting in it is not heard as delay.
constexpr unsigned QUEUE_MSECS = 30;

// What the DMA carries, in the words Circle counts it in — one word per
// stereo channel, so two words to the frame, and a multiple of 384 as the
// device requires. Circle's own default is 384*10, which at 44.1kHz is 43ms
// in each of the two buffers: sized for an application that might be slow to
// produce a chunk. Ours is never slow, because a chunk is copied out of a
// queue that is already full, so a shorter one costs nothing but a more
// frequent interrupt and saves most of that time.
constexpr unsigned DMA_CHUNK_WORDS = 384 * 4;

// How much finished audio may wait on the application core, counted in the
// blocks the application's own callback produces. Two would be the least that
// can play without a gap — one being handed over and one behind it — and this
// is three, so there is a whole spare block of slack for a frame that runs
// long. A block is over 20ms at the rates games ask for, which is longer than
// a frame takes to draw on a core with nothing else on it.
//
// Counted in blocks rather than in milliseconds because the callback is
// all-or-nothing: it produces a whole block or none, so a budget that did not
// come out as a whole number of them would round down, and a budget below one
// block would round down to no sound at all.
constexpr unsigned RING_BLOCKS = 3;

// ---------------------------------------------------------------------------
// WHO MAY PRODUCE
// ---------------------------------------------------------------------------
//
// Producing audio means running the application's callback and putting the
// result somewhere: into the cross-core ring under the split, or straight into
// the sound device's queue without it. BOTH destinations have exactly one
// writer by construction — the ring is single-producer, and the device's queue
// is filled by core 0's servo and by nobody else — and neither of them says so
// anywhere. The rule was a convention: production happened in SDL_PumpEvents,
// and the application core was the only core that called it.
//
// A convention is not a rule. Two cores producing at once do not merely
// interleave, they overwrite: they read the same ring tail, write the same
// bytes, and both publish a tail that accounts for one of the two blocks. They
// also share s_chunk, the single buffer the callback fills, so one block can
// be half of one core's audio and half of another's. What comes out is not
// late, it is WRONG — chunks in the wrong order and torn across the seam.
//
// So the owner is named, AND IT IS NAMED WHEN THE DEVICE OPENS rather than at
// the first pump. That timing is not a detail. An application may wait for the
// sound to advance before it has pumped even once — Chocolate Doom does: it
// sets up its emulated OPL chip immediately after opening the device, and
// setting it up waits for a chip timer that only the audio callback advances.
// With ownership still unclaimed at that moment, the wait asks who may produce,
// is told nobody, and waits for something only it could have started. The board
// stops there, having printed that the mixer opened and nothing after it.
//
// Ownership is not the same question as re-entrancy and one does not answer the
// other: s_pumping stops a core being asked to produce from inside its own
// callback, which is a different accident and needs its own guard.
int s_owner_core = -1;    // the core production belongs to; -1 until claimed
bool s_pumping = false;   // a callback is running on the owning core

// Settle who produces, at the one moment that is both early enough and
// unambiguous: an application opens its own audio device, so the core that
// opened it is the core whose loop will drive the callback.
void claim_production(void)
{
    const int core = (int)SDL2Circle_ThisCore();

    // The one core that can never own it is core 0 under the split, which
    // drains and never produces. An application that opened its device from
    // there — from a thread, say, since SDL threads are core-0 tasks — leaves
    // the question to the first core that legitimately pumps.
    s_owner_core = (SDL2Circle_SplitActive() && core == 0) ? -1 : core;
}

// Whether the calling core may produce. ONE rule, in one place, for every
// caller — the event pump and a blocking wait alike. A wait needs the same
// answer the pump gets: it is asking because the thing it is waiting for may
// be on the other side of the callback, and a rule that were stricter here
// than there would be a rule that can deadlock.
bool may_produce(void)
{
    if (!s_device || s_paused || !s_spec.callback || s_lock > 0 || s_pumping)
        return false;

    // Under the split, core 0 drains and never produces: its servo is already
    // the device queue's only writer, and a pump here would take the
    // direct-to-device path below and write into that queue beside it.
    const int core = (int)SDL2Circle_ThisCore();
    if (SDL2Circle_SplitActive() && core == 0)
        return false;

    // Normally settled at open. Unclaimed here means the device was opened
    // from a core that cannot own it, and then the first eligible core to ask
    // takes it — never TAKES IT FROM anyone, which is what keeps two cores off
    // the ring. Letting the asker have it is what stops a wait finding nobody
    // at home, and having nobody at home is the one outcome that never
    // recovers.
    if (s_owner_core < 0)
        s_owner_core = core;
    return s_owner_core == core;
}

// Fill in the derived fields SDL leaves to the implementation.
void complete_spec(SDL_AudioSpec &spec)
{
    spec.silence = SDL_AUDIO_ISSIGNED(spec.format) ? 0 : 0x80;
    spec.size = (Uint32)spec.samples * spec.channels
              * (SDL_AUDIO_BITSIZE(spec.format) / 8);
}

// Run the application's callback and leave one chunk in DEVICE format in
// s_chunk. Returns the number of bytes to hand the device, or 0 if the
// conversion failed.
unsigned fill_chunk(void)
{
    s_spec.callback(s_spec.userdata, s_chunk, (int)s_spec.size);
    if (!s_convert)
        return s_spec.size;

    s_cvt.buf = s_chunk;
    s_cvt.len = (int)s_spec.size;
    if (SDL_ConvertAudio(&s_cvt) < 0)
        return 0;
    return (unsigned)s_cvt.len_cvt;
}

} // namespace

// Device construction (interrupt registration, queue allocation) belongs to
// core 0; under the core split it marshals through the call mailbox.
struct OpenRequest
{
    int      freq;
    unsigned queue_ms;
};

static void open_device_on0(void *p)
{
    const OpenRequest *req = (const OpenRequest *)p;
    s_device = new CHDMISoundBaseDevice(CInterruptSystem::Get(), req->freq,
                                        DMA_CHUNK_WORDS);
    s_device->SetWriteFormat(SoundFormatSigned16, 2);
    if (!s_device->AllocateQueue(req->queue_ms))
    {
        delete s_device;
        s_device = nullptr;
        return;
    }
    s_queueFrames = s_device->GetQueueSizeFrames();
}

static void close_device_on0(void *);

extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int iscapture,
                                                 const SDL_AudioSpec *desired,
                                                 SDL_AudioSpec *obtained,
                                                 int allowed_changes)
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

    // The queue is a latency budget, but WITHOUT THE SPLIT it has a floor it
    // may not go under. There the application's callback writes to the device
    // directly, and it can only produce a WHOLE block at a time, so a queue
    // that never has room for one is never written to and the device plays
    // nothing at all — silence, not delay. An application that asks for a
    // large block therefore gets a larger queue, and the delay that comes with
    // it is its own choice of block rather than this file's.
    //
    // Under the split the floor does not apply: the servo fills the queue from
    // the ring and takes whatever fits, down to a single frame, so the queue
    // is free to be shorter than a block.
    OpenRequest req;
    req.freq = freq;
    req.queue_ms = QUEUE_MSECS;
    if (!SDL2Circle_SplitActive())
    {
        const unsigned block_ms =
            (unsigned)(((Uint64)samples * 1000 + (unsigned)freq - 1) / (unsigned)freq);
        if (req.queue_ms < block_ms * 2)
            req.queue_ms = block_ms * 2;
    }
    if (req.queue_ms > 1000)      // Circle's own limit on a queue's length
        req.queue_ms = 1000;

    SDL2Circle_CallOn0(open_device_on0, &req);
    if (!s_device)
    {
        SDL_SetError("cannot allocate sound queue");
        return 0;
    }

    // What the device plays. The rate is the one it was just constructed
    // with, so an application's requested rate is met by the hardware rather
    // than by resampling; the format and the channel count are fixed.
    s_dev.freq     = freq;
    s_dev.format   = AUDIO_S16SYS;
    s_dev.channels = 2;
    s_dev.samples  = samples;
    s_dev.callback = nullptr;
    s_dev.userdata = nullptr;
    complete_spec(s_dev);

    // What the application writes. Each attribute is the one it asked for,
    // unless it gave permission for that attribute to change — in which case
    // it becomes the device's, and `obtained` reports the change so the
    // application can adapt. An attribute it did not give permission to
    // change stays as asked and is converted below.
    const int allowed = allowed_changes;
    s_spec = *desired;
    if (allowed & SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)
        s_spec.freq = s_dev.freq;
    else if (s_spec.freq <= 0)
        s_spec.freq = s_dev.freq;

    if (allowed & SDL_AUDIO_ALLOW_FORMAT_CHANGE)
        s_spec.format = s_dev.format;
    else if (s_spec.format == 0)
        s_spec.format = s_dev.format;

    if (allowed & SDL_AUDIO_ALLOW_CHANNELS_CHANGE)
        s_spec.channels = s_dev.channels;
    else if (s_spec.channels == 0)
        s_spec.channels = s_dev.channels;

    if (allowed & SDL_AUDIO_ALLOW_SAMPLES_CHANGE)
        s_spec.samples = s_dev.samples;
    else if (s_spec.samples == 0)
        s_spec.samples = s_dev.samples;
    complete_spec(s_spec);

    // A rate this library cannot deliver by construction: the device was
    // built at the rate that was asked for, so the only way the two differ
    // is a caller that asked for one rate and permitted a change to another.
    s_convert = (s_spec.format   != s_dev.format)
             || (s_spec.channels != s_dev.channels)
             || (s_spec.freq     != s_dev.freq);

    size_t chunk_bytes = s_spec.size;
    if (s_convert)
    {
        if (SDL_BuildAudioCVT(&s_cvt, s_spec.format, s_spec.channels,
                              s_spec.freq, s_dev.format, s_dev.channels,
                              s_dev.freq) < 0)
        {
            // The conversion is not one this build can make, so the promise
            // in `obtained` could not be kept. Say so rather than open a
            // device that will be fed the wrong shape.
            SDL2Circle_CallOn0(close_device_on0, nullptr);
            return 0;   // SDL_BuildAudioCVT has set the error
        }
        chunk_bytes = (size_t)s_spec.size * s_cvt.len_mult + 1;
        s_dev_bytes = (unsigned)((double)s_spec.size * s_cvt.len_ratio);
    }
    else
    {
        s_dev_bytes = s_spec.size;
    }

    // The queue is counted in device frames, so a chunk's length has to be
    // measured on the device's side of any conversion.
    const unsigned dev_frame_bytes = s_dev.channels
                                   * (SDL_AUDIO_BITSIZE(s_dev.format) / 8);
    s_dev_frames = dev_frame_bytes ? s_dev_bytes / dev_frame_bytes : 0;
    if (s_dev_frames == 0)
        s_dev_frames = 1;

    s_chunk = (Uint8 *)malloc(chunk_bytes);
    if (!s_chunk)
    {
        SDL2Circle_CallOn0(close_device_on0, nullptr);
        SDL_OutOfMemory();
        return 0;
    }

    if (obtained)
        *obtained = s_spec;

    // Before anything can wait on the sound advancing, and that is the point.
    claim_production();

    if (s_convert)
        SDL2Circle_Log("sdl2audio", SDL2CIRCLE_LOG_NOTICE,
                       "application writes %d Hz format 0x%04x %d channel(s); "
                       "device plays %d Hz format 0x%04x %d channel(s) — "
                       "converting",
                       s_spec.freq, (unsigned)s_spec.format, s_spec.channels,
                       s_dev.freq, (unsigned)s_dev.format, s_dev.channels);

    s_paused = true;
    s_started = false;

    return 2;   // SDL device ids for opened devices start at 2
}


void SDL2Circle_AudioPump(void)
{
    if (!may_produce())
        return;

    struct Guard
    {
        Guard()  { s_pumping = true; }
        ~Guard() { s_pumping = false; }
    } guard;

    // Core split inverts audio from pull to push: the application core runs its
    // callback into the cross-core sample ring; the hardware-core servo feeds the
    // device from the ring at its own cadence (SDL2Circle_AudioDrain).
    // Audio stops being hostage to frame granularity.
    if (SDL2Circle_SplitActive())
    {
        // Stop at the budget, NOT when the ring is full. The ring's storage is
        // many times this, and filling it would put a third of a second of
        // finished audio in front of every new sound — the game would be heard
        // acting long after it was seen doing it. What is not produced now is
        // produced at the next pump, one frame away.
        const unsigned budget = s_dev_bytes * RING_BLOCKS;
        while (SDL2Circle_AudioRingUsed() < budget
               && SDL2Circle_AudioRingSpace() >= s_dev_bytes)
        {
            const unsigned n = fill_chunk();
            if (n == 0)
                break;
            SDL2Circle_AudioRingWrite(s_chunk, n);
        }
        return;
    }

    unsigned queued = s_device->GetQueueFramesAvail();
    unsigned space = s_queueFrames > queued ? s_queueFrames - queued : 0;

    while (space >= s_dev_frames)
    {
        const unsigned n = fill_chunk();
        if (n == 0)
            break;
        if (s_device->Write(s_chunk, n) <= 0)
            break;
        space -= s_dev_frames;
    }
}

// Core-0 servo: sample ring -> sound device.
void SDL2Circle_AudioDrain(void)
{
    if (!s_device || s_paused)
        return;

    // The ring carries DEVICE-format bytes: the conversion happens on the
    // application's own core, before the samples cross.
    static u8 *drainChunk = nullptr;
    static unsigned drainBytes = 0;
    if (!drainChunk || drainBytes < s_dev_bytes)
    {
        free(drainChunk);
        drainBytes = s_dev_bytes ? s_dev_bytes : 4096;
        drainChunk = (u8 *)malloc(drainBytes);
        if (!drainChunk)
        {
            drainBytes = 0;
            return;
        }
    }

    const unsigned frame_bytes = s_dev_bytes / (s_dev_frames ? s_dev_frames : 1);

    unsigned queued = s_device->GetQueueFramesAvail();
    unsigned space = s_queueFrames > queued ? s_queueFrames - queued : 0;

    while (space > 0)
    {
        // Never more than will certainly fit. Circle's Write takes what it can
        // and reports how much, and the ring has already given up whatever was
        // read — so a write that came up short would lose those samples with
        // nothing to say so. Asking only for the free space means it cannot.
        unsigned want = space * frame_bytes;
        if (want > s_dev_bytes)
            want = s_dev_bytes;

        unsigned n = SDL2Circle_AudioRingRead(drainChunk, want);
        if (n == 0)
            break;
        if (s_device->Write(drainChunk, n) <= 0)
            break;
        space -= n / frame_bytes;
    }
}

static void start_device_on0(void *)
{
    s_started = s_device->Start();
}

extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on)
{
    if (!s_device)
        return;
    s_paused = (pause_on != 0);
    if (!s_paused && !s_started)
    {
        SDL2Circle_CallOn0(start_device_on0, nullptr);
        if (!s_started)
            SDL_SetError("HDMI sound failed to start "
                         "(display without audio support? hdmi_drive=2?)");
    }
}

extern "C" void SDL_LockAudioDevice(SDL_AudioDeviceID)   { s_lock++; }
extern "C" void SDL_UnlockAudioDevice(SDL_AudioDeviceID) { if (s_lock > 0) s_lock--; }

// SDL2's older, device-less spelling of the same lock. There is one audio
// device here, so both spellings guard the same callback.
extern "C" void SDL_LockAudio(void)   { SDL_LockAudioDevice(1); }
extern "C" void SDL_UnlockAudio(void) { SDL_UnlockAudioDevice(1); }

// Device destruction gives back the interrupt registration, the queue and
// the device's DMA channel, so it belongs to core 0 for the same reason
// construction does.
static void close_device_on0(void *)
{
    s_device->Cancel();
    delete s_device;
    s_device = nullptr;
}

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID)
{
    if (!s_device)
        return;
    s_paused = true;   // stop the servo feeding a device that is going away
    s_owner_core = -1; // the next device settles it again when it is opened
    SDL2Circle_CallOn0(close_device_on0, nullptr);
    free(s_chunk);
    s_chunk = nullptr;
    s_started = false;
}

extern "C" SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID)
{
    if (!s_device || !s_started)
        return SDL_AUDIO_STOPPED;
    return s_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

// ---------------------------------------------------------------------------
// The device-less spelling SDL2 keeps from SDL 1.2
//
// These address the one audio device without naming it. There is exactly one
// here, so each is its device-taking form applied to that device — the same
// relationship SDL_LockAudio already has with SDL_LockAudioDevice above.
// ---------------------------------------------------------------------------

extern "C" int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (!desired)
        return SDL_SetError("SDL_OpenAudio: no desired spec");

    // SDL's contract differs by whether the caller gave somewhere to report a
    // change. Given nowhere, it undertakes to deliver EXACTLY the spec that
    // was asked for and to convert on the application's behalf — so no change
    // is permitted here, and the conversion happens at the device boundary.
    // Given somewhere, any change is allowed and is reported into it.
    SDL_AudioSpec got;
    if (SDL_OpenAudioDevice(nullptr, 0, desired, &got,
                            obtained ? SDL_AUDIO_ALLOW_ANY_CHANGE : 0) == 0)
        return -1;   // SDL_OpenAudioDevice has set the error

    if (obtained)
        *obtained = got;
    return 0;
}

extern "C" void SDL_CloseAudio(void)  { SDL_CloseAudioDevice(1); }
extern "C" void SDL_PauseAudio(int p) { SDL_PauseAudioDevice(1, p); }

extern "C" SDL_AudioStatus SDL_GetAudioStatus(void)
{
    return SDL_GetAudioDeviceStatus(1);
}

extern "C" int SDL_GetNumAudioDevices(int iscapture)
{
    return iscapture ? 0 : 1;
}

extern "C" const char *SDL_GetAudioDeviceName(int index, int iscapture)
{
    return (!iscapture && index == 0) ? "HDMI" : nullptr;
}

// The one device's native format: 48kHz S16 stereo (what the HDMI sound
// device speaks). Enumerators that report an error here get dropped from
// callers' device lists — MAME builds its sink list from this answer.
extern "C" int SDL_GetAudioDeviceSpec(int index, int iscapture,
                                      SDL_AudioSpec *spec)
{
    if (iscapture || index != 0 || !spec)
        return -1;
    memset(spec, 0, sizeof(*spec));
    spec->freq = 48000;
    spec->format = AUDIO_S16SYS;
    spec->channels = 2;
    return 0;
}

extern "C" int SDL_GetDefaultAudioInfo(char **name, SDL_AudioSpec *spec,
                                       int iscapture)
{
    if (iscapture)
    {
        if (name)
            *name = nullptr;
        return -1;
    }
    if (name)
        *name = strdup("HDMI");   // caller frees with SDL_free (libc free)
    if (spec)
        SDL_GetAudioDeviceSpec(0, 0, spec);
    return 0;
}

extern "C" const char *SDL_GetCurrentAudioDriver(void)
{
    return "circle";
}

// One driver, always the one in use. Applications enumerate these to offer a
// choice; there is nothing to choose between, and saying so is better than
// reporting none and being dropped from the list.
extern "C" int SDL_GetNumAudioDrivers(void) { return 1; }

extern "C" const char *SDL_GetAudioDriver(int index)
{
    return index == 0 ? "circle" : nullptr;
}

// The subsystem is brought up with the rest of the shim, so selecting a
// driver by name succeeds for the only name there is.
extern "C" int SDL_AudioInit(const char *driver)
{
    if (driver && strcmp(driver, "circle") != 0)
        return SDL_SetError("SDL_AudioInit: no audio driver named `%s`", driver);
    return 0;
}

extern "C" void SDL_AudioQuit(void)
{
    SDL_CloseAudioDevice(1);
}
