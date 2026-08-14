//
// platform.cpp - what SDL can tell an application about the machine.
//
// Everything here has a definite answer on a bare-metal board, and most of
// them are more definite than on a desktop: there is one kind of processor,
// no operating system to ask, no browser, no battery, and nobody logged in
// with a language preference.
//
// Where the answer is "this machine cannot do that", it is reported as a
// failure rather than a success that does nothing. An application that asks
// to open a web page and is told it worked has no way to find out otherwise,
// and will wait for something that is never going to happen.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/machineinfo.h>
#include <circle/multicore.h>
#include <circle/sysconfig.h>

// ---------------------------------------------------------------------------
// The processor
//
// The Raspberry Pi boards this runs on are AArch64, so NEON is always
// present and the x86 and PowerPC feature queries are always false. They are
// answered rather than omitted because portable code tests them before
// choosing a code path, and a missing symbol is a link failure where a false
// is simply the other branch.
// ---------------------------------------------------------------------------

extern "C" SDL_bool SDL_HasNEON(void)     { return SDL_TRUE; }
extern "C" SDL_bool SDL_HasARMSIMD(void)  { return SDL_TRUE; }

extern "C" SDL_bool SDL_HasAltiVec(void)  { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasMMX(void)      { return SDL_FALSE; }
extern "C" SDL_bool SDL_Has3DNow(void)    { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasSSE(void)      { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasSSE2(void)     { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasSSE3(void)     { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasSSE41(void)    { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasSSE42(void)    { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasAVX(void)      { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasAVX2(void)     { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasAVX512F(void)  { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasLSX(void)      { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasLASX(void)     { return SDL_FALSE; }
extern "C" SDL_bool SDL_HasRDTSC(void)    { return SDL_FALSE; }

// The cache line the AArch64 boards use, and the reason SDL asks: an
// application sizing a buffer to avoid false sharing needs the real figure.
extern "C" int SDL_GetCPUCacheLineSize(void) { return 64; }

// Every board this runs on is quad-core, and CORES is what Circle was built
// against - the same number the core split reasons about.
extern "C" int SDL_GetCPUCount(void)
{
    return (int)CORES;
}

extern "C" int SDL_GetSystemRAM(void)
{
    // SDL reports megabytes, and so does Circle. A board that cannot say
    // answers 0, where SDL's contract is to report what is known; passing
    // the 0 straight on is that.
    return (int)CMachineInfo::Get()->GetRAMSize();
}

extern "C" size_t SDL_SIMDGetAlignment(void)
{
    return 16;   // NEON's widest vector
}

// ---------------------------------------------------------------------------
// Power, locale and the outside world
// ---------------------------------------------------------------------------

// A board runs from a power supply. SDL has a state for exactly that, and it
// is not "unknown" - there is no battery, so nothing is charging and nothing
// is draining.
extern "C" SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent)
{
    if (seconds) *seconds = -1;   // SDL: not applicable
    if (percent) *percent = -1;
    return SDL_POWERSTATE_NO_BATTERY;
}

// There is no user account and no language setting to read one from. SDL
// returns an array terminated by an entry with a null language, and the
// caller frees it with SDL_free.
extern "C" SDL_Locale *SDL_GetPreferredLocales(void)
{
    SDL_Locale *locales = (SDL_Locale *)SDL_calloc(1, sizeof(SDL_Locale));
    if (!locales)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    locales[0].language = nullptr;   // the terminator, and the whole list
    locales[0].country = nullptr;
    return locales;
}

// There is no browser and no operating system to hand a URL to. Reported as
// a failure, because the alternative is an application waiting for a page
// that will never open and having been told it was fine.
extern "C" int SDL_OpenURL(const char *url)
{
    return SDL_SetError("SDL_OpenURL: cannot open `%s`: there is no browser "
                        "on this machine", url ? url : "");
}

// ---------------------------------------------------------------------------
// Touch
//
// No touch device is attached, and SDL's contract for an empty list is a
// count of zero rather than an error.
// ---------------------------------------------------------------------------

extern "C" int SDL_GetNumTouchDevices(void) { return 0; }

extern "C" SDL_TouchID SDL_GetTouchDevice(int)
{
    return 0;   // SDL: 0 for an index that names no device
}

extern "C" SDL_TouchDeviceType SDL_GetTouchDeviceType(SDL_TouchID)
{
    return SDL_TOUCH_DEVICE_INVALID;
}

extern "C" int SDL_GetNumTouchFingers(SDL_TouchID) { return 0; }

extern "C" SDL_Finger *SDL_GetTouchFinger(SDL_TouchID, int) { return nullptr; }

extern "C" const char *SDL_GetTouchName(int) { return nullptr; }
