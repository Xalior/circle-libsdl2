# Audio

## Overview

Audio is delivered through the callback API: `SDL_OpenAudioDevice` with a callback function. The device plays 16-bit signed stereo into `CHDMISoundBaseDevice` with a ~100 ms hardware queue.

**Format handling.** An application that asks for a format and does not permit a change gets what it asked for, converted at the device boundary; `obtained` reports the conversion spec. Permit a change and `obtained` reports the device's 16-bit stereo instead, which has no conversion cost.

**Off core 0**, the application fills an audio ring with its callback; core 0's servo task feeds the device from the ring at its own cadence.

**On core 0**, audio pulls: the callback runs in the context of `SDL_Delay` and presentation, feeding the device on demand.

## Audio conversion

Full conversion support: `SDL_BuildAudioCVT`, `SDL_ConvertAudio`, `SDL_LoadWAV_RW`, `SDL_MixAudioFormat` — in memory, through float for precision. Covers every sample width, either byte order, one or two channels, and any rate ratio.

## Mixer

`SDL_mixer` is implemented above the audio device: multiple sounds at once, channels, volumes, panning, and music. Chunks are converted at load time, never in the callback — the cost is front-loaded where it can be measured.

## MIDI

**No MIDI synthesiser.** `Mix_LoadMUS` reads WAV. A MIDI file is a score rather than a recording, and performing one is a sound engine in its own right.

**No fades in the mixer.** `Mix_FadeInMusic` and its relatives start and stop at volume.

## Timers

`SDL_AddTimer` and `SDL_RemoveTimer` use the system timer. A callback has no thread of its own here, so it runs in the calling line of execution — at `SDL_PumpEvents`, which `SDL_PollEvent` calls, and at `SDL_Delay`, on whichever core makes the call. A callback is never dropped, only deferred until one of those two points is reached, so an application inside a long stretch of work with neither a delay nor an event poll in it runs its timers late.

## Examples

`examples/tone` generates a 1 kHz sine wave over HDMI via the callback API.
