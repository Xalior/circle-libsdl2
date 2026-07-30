//
// hardware.cpp — the board hardware this library manages for its host.
//
// Circle's CCPUThrottle is two things in one class: the CPU clock rate, and
// — when cmdline.txt names a fan pin with `gpiofanpin=` — a case fan. Circle
// creates neither for itself. It requires that a system holds exactly one
// such object, and that something calls Update() on it regularly, or none of
// the management it offers ever happens.
//
// A host kernel has no per-frame loop to call from, and this library already
// runs one, so this library owns the object. There is only ever one, it is
// created once, and it is never destroyed: destroying it puts the clock back
// to the idle rate, which is the wrong thing to do to a machine that is
// still running.
//
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"
#include <circle/cputhrottle.h>
#include <new>

namespace
{

// Whether the object below has been made. This library's own flag, which it
// is the only writer of. Circle offers no safe way to ask the same question:
// CCPUThrottle::Get() asserts instead of returning nothing when no object
// exists, so a null test behind it can never run.
bool s_bUp = false;

CCPUThrottle *s_pThrottle = nullptr;

// Storage for the one throttle, filled in by placement new when the host
// asks for hardware management. It cannot be an ordinary static object: a
// static is constructed before the kernel exists, and this constructor reads
// the kernel's command line options and talks to the firmware.
alignas(CCPUThrottle) u8 s_ThrottleStore[sizeof(CCPUThrottle)];

}

extern "C" void SDL2Circle_HardwareInit(void)
{
    if (s_bUp)
        return;
    s_bUp = true;

    // CPUSpeedMaximum, stated rather than left to the default. Circle boots
    // the board at its idle clock rate and CPUSpeedUnknown would hand the
    // decision to cmdline.txt `fast=`, so a card that does not carry that
    // option would run the application at idle speed. An application asking
    // for a display, a sound device and a frame rate is asking for the
    // machine's throughput.
    //
    // What happens after this point is Circle's policy, chosen on the
    // command line (Circle's doc/cmdline.txt): with no fan pin the clock is
    // pulled back to idle above `socmaxtemp` and restored when the SoC
    // cools; with `gpiofanpin=` set the clock is left where this put it and
    // the fan is switched instead.
    s_pThrottle = new (s_ThrottleStore) CCPUThrottle(CPUSpeedMaximum);
}

void SDL2Circle_HardwareTick(void)
{
    if (!s_bUp)
        return;

    // No interval of this library's own. Update() already does its work at
    // most once every four seconds and costs a clock read the rest of the
    // time — Circle's cputhrottle.h describes it as the form that "can be
    // called as often as you want without checking the calling interval".
    // A second gate here would only beat against that one.
    s_pThrottle->Update();
}

extern "C" unsigned SDL2Circle_SoCTemperature(void)
{
    return s_bUp ? s_pThrottle->GetTemperature() : 0;
}

extern "C" unsigned SDL2Circle_CPUClockRate(void)
{
    return s_bUp ? s_pThrottle->GetClockRate() : 0;
}
