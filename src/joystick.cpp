//
// joystick.cpp — Circle USB gamepads behind the SDL joystick API.
//
// Circle publishes every gamepad it binds — the generic HID driver and the
// five vendor drivers alike — as a character device named "upadN", N from 1.
// This file turns that into SDL's two-level identity scheme:
//
//   a DEVICE INDEX is a position in the list of currently attached pads,
//   0..SDL_NumJoysticks()-1, and it renumbers whenever a pad comes or goes;
//
//   an INSTANCE ID is handed out once at attach, never reused, and is what
//   every SDL event carries.
//
// A pad's slot in the table below is neither: it is the Circle device number
// minus one, so a pad keeps its slot for as long as it is plugged in.
//
// WHICH CORE DOES WHAT. USB belongs to core 0, so attach, detach, report
// decoding and event synthesis all happen there — from SDL2Circle_InputPump,
// which is the core-0 servo's job under the split and the application's own
// pump without it. Everything an application asks for afterwards (how many
// pads, what is this axis reading) is answered from the shared table by
// whichever core asks, with no call to core 0 at all: the state fields are
// atomics written on core 0 and read anywhere. The one exception is rumble,
// which is a USB control transfer, so it is marshalled.
//
// Circle's report callback runs in interrupt context. Like the keyboard path
// it only snapshots, behind a sequence counter; the pump does the diffing.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "shim_internal.h"

#include <circle/devicenameservice.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbdevice.h>
#include <circle/timer.h>
#include <atomic>
#include <cstring>
#include <cstdio>

namespace
{

// Circle's own limits (circle/usb/usbgamepad.h), plus the button count that
// its 32-bit button mask can actually carry.
const unsigned MAX_PADS    = 8;
const unsigned PAD_AXES    = MAX_AXIS;
const unsigned PAD_HATS    = MAX_HATS;
const unsigned PAD_BUTTONS = 32;

// ---------------------------------------------------------------------------
// The pad table: written on core 0, read from any core.
// ---------------------------------------------------------------------------

struct PadSlot
{
    // Descriptor. Written at attach and published by the release-store to
    // `present`; a reader that has acquired `present` sees all of it.
    char             name[128];
    char             path[16];          // the Circle device name, "upadN"
    SDL_JoystickGUID guid;
    Uint16           vendor, product, version;
    int              naxes, nhats, nbuttons;
    int              axismin[PAD_AXES], axismax[PAD_AXES];
    unsigned         properties;        // Circle's TGamePadProperty mask
    SDL_JoystickID   instance;

    // Live state. Written by the core-0 pump, read from any core.
    std::atomic<Sint16> axis[PAD_AXES];
    std::atomic<Uint8>  hat[PAD_HATS];
    std::atomic<Uint32> buttons;

    std::atomic<bool> present;

    // Core 0 only.
    CUSBGamePadDevice *dev;
    CDevice           *devbase;         // same object, upcast at attach, so the
                                        // removal path never downcasts a
                                        // half-destroyed object
    u64                rumble_until;    // clock ticks; 0 = no deadline
};

PadSlot s_pad[MAX_PADS];

// Instance IDs are never reused. Core 0 only.
SDL_JoystickID s_nextInstance = 0;

// ---------------------------------------------------------------------------
// Interrupt-side report snapshot (one per pad).
//
// Circle hands the callback a pointer to the driver's live state, which the
// next report overwrites, so the callback copies. The counter is odd while a
// copy is in progress: the pump retries a snapshot it caught mid-write, and
// gives up after a few tries rather than spinning against a device that is
// reporting faster than the pump runs.
// ---------------------------------------------------------------------------

struct RawPad
{
    TGamePadState        state;
    std::atomic<unsigned> seq;
};

RawPad   s_raw[MAX_PADS];
unsigned s_rawSeen[MAX_PADS];           // last sequence the pump translated

// Set from the device-removed callback, drained by the pump. Doing the SDL
// work here rather than in the callback keeps it out of a destructor.
std::atomic<unsigned> s_removedMask{0};

void PadStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState)
{
    if (nDeviceIndex >= MAX_PADS || pState == nullptr)
        return;

    RawPad &r = s_raw[nDeviceIndex];
    r.seq.fetch_add(1, std::memory_order_release);
    r.state = *pState;
    r.seq.fetch_add(1, std::memory_order_release);
}

void PadRemovedHandler(CDevice *, void *pContext)
{
    unsigned slot = (unsigned)(uintptr_t)pContext;
    if (slot < MAX_PADS)
        s_removedMask.fetch_or(1u << slot, std::memory_order_release);
}

// Copy a pad's latest report out from under the interrupt. Returns false if
// no new report has arrived, or if the snapshot could not be taken cleanly.
bool SnapshotPad(unsigned slot, TGamePadState *out)
{
    RawPad &r = s_raw[slot];

    for (int tries = 0; tries < 8; tries++)
    {
        unsigned before = r.seq.load(std::memory_order_acquire);
        if (before & 1)
            continue;                   // a copy is in flight
        if (before == s_rawSeen[slot])
            return false;               // nothing new

        *out = r.state;

        if (r.seq.load(std::memory_order_acquire) == before)
        {
            s_rawSeen[slot] = before;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SDL joystick GUIDs
//
// Byte-for-byte the layout SDL2 builds (SDL_CreateJoystickGUID), because a
// gamecontrollerdb.txt lookup is a memcmp and nothing less will match:
//
//   0..1   bus type, little-endian   (3 = USB)
//   2..3   CRC16 of the device name, little-endian
//   4..5   vendor ID, little-endian
//   6..7   zero
//   8..9   product ID, little-endian
//  10..11  zero
//  12..13  version, little-endian
//     14   driver signature (0: the plain USB HID path, as on Linux)
//     15   driver data
//
// The CRC is CRC-16/ARC over the device name with no terminator. Mappings in
// the database never carry it, so a lookup zeroes it before comparing; it
// survives only to disambiguate two devices that share vendor and product,
// through a mapping's optional `crc:` field.
// ---------------------------------------------------------------------------

const Uint16 SDL_BUS_USB = 0x03;

Uint16 Crc16Byte(Uint8 r)
{
    Uint16 crc = 0;
    for (int i = 0; i < 8; i++)
    {
        crc = ((crc ^ r) & 1 ? 0xA001 : 0) ^ (Uint16)(crc >> 1);
        r >>= 1;
    }
    return crc;
}

void PutLE16(Uint8 *p, Uint16 v)
{
    p[0] = (Uint8)(v & 0xFF);
    p[1] = (Uint8)(v >> 8);
}

Uint16 GetLE16(const Uint8 *p)
{
    return (Uint16)(p[0] | ((Uint16)p[1] << 8));
}

// ---------------------------------------------------------------------------
// Value conversion
// ---------------------------------------------------------------------------

// Circle reports an axis in the range the HID report descriptor declared.
// SDL always reports -32768..32767.
Sint16 ScaleAxis(int value, int minimum, int maximum)
{
    if (maximum <= minimum)
        return 0;
    if (value <= minimum)
        return -32768;
    if (value >= maximum)
        return 32767;

    long span = (long)maximum - (long)minimum;
    long scaled = ((long)(value - minimum) * 65535L) / span - 32768L;
    return (Sint16)scaled;
}

// Circle stores the hat's raw HID field: a DIRECTION INDEX, clockwise from
// up, plus whatever out-of-range value the device uses for centred (15 and 8
// are both common). SDL wants a bitmask. Circle keeps no record of the hat's
// declared logical range, so anything outside 0..7 is read as centred — which
// is right for every hat whose logical minimum is zero, and that is the
// overwhelming majority.
Uint8 HatToSDL(int value)
{
    static const Uint8 kMask[8] = {
        SDL_HAT_UP,   SDL_HAT_RIGHTUP,   SDL_HAT_RIGHT, SDL_HAT_RIGHTDOWN,
        SDL_HAT_DOWN, SDL_HAT_LEFTDOWN,  SDL_HAT_LEFT,  SDL_HAT_LEFTUP
    };
    if (value < 0 || value > 7)
        return SDL_HAT_CENTERED;
    return kMask[value];
}

// ---------------------------------------------------------------------------
// Device index <-> slot
// ---------------------------------------------------------------------------

int DeviceIndexOfSlot(unsigned slot)
{
    if (slot >= MAX_PADS || !s_pad[slot].present.load(std::memory_order_acquire))
        return -1;

    int index = 0;
    for (unsigned i = 0; i < slot; i++)
        if (s_pad[i].present.load(std::memory_order_acquire))
            index++;
    return index;
}

// ---------------------------------------------------------------------------
// Open joystick handles
// ---------------------------------------------------------------------------

} // namespace

// SDL's opaque joystick handle. One per slot: a pad cannot be open twice
// under two different objects, and SDL_JoystickOpen on an already-open device
// is defined to return the same handle.
struct _SDL_Joystick
{
    int            slot;
    SDL_JoystickID instance;
    int            refcount;
    int            player_index;
};

namespace
{

SDL_Joystick s_joy[MAX_PADS];

// Resolve a handle to a slot that is still plugged in; -1 otherwise.
int LiveSlot(SDL_Joystick *joystick)
{
    if (!joystick || joystick->slot < 0)
        return -1;
    const PadSlot &p = s_pad[joystick->slot];
    if (!p.present.load(std::memory_order_acquire) || p.instance != joystick->instance)
        return -1;
    return joystick->slot;
}

// ---------------------------------------------------------------------------
// Attach / detach (core 0)
// ---------------------------------------------------------------------------

void PushEvent(SDL_Event &ev)
{
    ev.common.timestamp = SDL_GetTicks();
    SDL_PushEvent(&ev);
}

void AttachSlot(unsigned slot, CUSBGamePadDevice *pPad)
{
    PadSlot &p = s_pad[slot];

    memset(p.name, 0, sizeof p.name);
    memset(p.path, 0, sizeof p.path);
    memset(&p.guid, 0, sizeof p.guid);

    snprintf(p.path, sizeof p.path, "upad%u", slot + 1);

    // Identity from the USB device descriptor, which the gamepad object
    // reaches through the function it is.
    const TUSBDeviceDescriptor *pDesc = pPad->GetDevice()->GetDeviceDescriptor();
    p.vendor  = pDesc ? pDesc->idVendor  : 0;
    p.product = pDesc ? pDesc->idProduct : 0;
    p.version = pDesc ? pDesc->bcdDevice : 0;

    // The name. Circle stashes the USB manufacturer and product strings on
    // the driver object as device properties. Joining them with one space is
    // what Linux's USB HID layer does too, so the name — and therefore the
    // CRC inside the GUID — matches what a mapping database was generated
    // against. Empty strings are normal on cheap pads; fall back to the IDs.
    const char *pVendorStr  = pPad->GetProperty(CDevice::PropertyVendor);
    const char *pProductStr = pPad->GetProperty(CDevice::PropertyProduct);
    if (!pVendorStr)  pVendorStr  = "";
    if (!pProductStr) pProductStr = "";

    if (*pVendorStr && *pProductStr)
        snprintf(p.name, sizeof p.name, "%s %s", pVendorStr, pProductStr);
    else if (*pProductStr)
        snprintf(p.name, sizeof p.name, "%s", pProductStr);
    else if (*pVendorStr)
        snprintf(p.name, sizeof p.name, "%s", pVendorStr);
    else
        snprintf(p.name, sizeof p.name, "USB Gamepad %04x:%04x",
                 p.vendor, p.product);

    // The GUID, in SDL's layout (see above).
    Uint16 crc = 0;
    for (const char *s = p.name; *s; s++)
        crc = Crc16Byte((Uint8)((Uint8)crc ^ (Uint8)*s)) ^ (Uint16)(crc >> 8);

    PutLE16(&p.guid.data[0],  SDL_BUS_USB);
    PutLE16(&p.guid.data[2],  crc);
    PutLE16(&p.guid.data[4],  p.vendor);
    PutLE16(&p.guid.data[8],  p.product);
    PutLE16(&p.guid.data[12], p.version);
    p.guid.data[14] = 0;
    p.guid.data[15] = 0;

    // Control counts and per-axis ranges. Circle's generic driver decodes the
    // report descriptor once while configuring, so these are already right
    // before the first report arrives.
    const TGamePadState *pInit = pPad->GetInitialState();
    p.naxes    = 0;
    p.nhats    = 0;
    p.nbuttons = 0;
    if (pInit)
    {
        p.naxes    = pInit->naxes    < (int)PAD_AXES    ? pInit->naxes    : (int)PAD_AXES;
        p.nhats    = pInit->nhats    < (int)PAD_HATS    ? pInit->nhats    : (int)PAD_HATS;
        p.nbuttons = pInit->nbuttons < (int)PAD_BUTTONS ? pInit->nbuttons : (int)PAD_BUTTONS;
        if (p.naxes    < 0) p.naxes    = 0;
        if (p.nhats    < 0) p.nhats    = 0;
        if (p.nbuttons < 0) p.nbuttons = 0;

        for (int i = 0; i < p.naxes; i++)
        {
            p.axismin[i] = pInit->axes[i].minimum;
            p.axismax[i] = pInit->axes[i].maximum;
        }
    }
    for (int i = p.naxes; i < (int)PAD_AXES; i++)
    {
        p.axismin[i] = 0;
        p.axismax[i] = 0;
    }

    p.properties = pPad->GetProperties();

    // Rest position: an axis with a symmetric range rests at zero, and one
    // that only goes up (a trigger) rests at its minimum. Publishing this
    // before the first report keeps an untouched control from looking held.
    for (unsigned i = 0; i < PAD_AXES; i++)
    {
        Sint16 rest = 0;
        if (i < (unsigned)p.naxes && p.axismin[i] >= 0)
            rest = -32768;
        p.axis[i].store(rest, std::memory_order_relaxed);
    }
    for (unsigned i = 0; i < PAD_HATS; i++)
        p.hat[i].store(SDL_HAT_CENTERED, std::memory_order_relaxed);
    p.buttons.store(0, std::memory_order_relaxed);

    p.instance     = s_nextInstance++;
    p.dev          = pPad;
    p.devbase      = pPad;
    p.rumble_until = 0;

    s_raw[slot].seq.store(0, std::memory_order_relaxed);
    s_rawSeen[slot] = 0;

    pPad->RegisterRemovedHandler(PadRemovedHandler, (void *)(uintptr_t)slot);
    pPad->RegisterStatusHandler(PadStatusHandler);

    p.present.store(true, std::memory_order_release);

    int device_index = DeviceIndexOfSlot(slot);

    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_NOTICE,
                   "%s attached: \"%s\" %04x:%04x %d axes %d hats %d buttons",
                   p.path, p.name, p.vendor, p.product,
                   p.naxes, p.nhats, p.nbuttons);

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type          = SDL_JOYDEVICEADDED;
    ev.jdevice.which = device_index;      // ADDED carries a device index
    PushEvent(ev);

    SDL2Circle_ControllerDeviceAdded(device_index);
}

void DetachSlot(unsigned slot)
{
    PadSlot &p = s_pad[slot];
    if (!p.present.load(std::memory_order_acquire))
        return;

    SDL_JoystickID instance = p.instance;

    SDL2Circle_ControllerDeviceRemoved(instance);

    p.present.store(false, std::memory_order_release);
    p.dev     = nullptr;
    p.devbase = nullptr;

    // Nothing stays held on a pad that is no longer there.
    for (unsigned i = 0; i < PAD_AXES; i++)
        p.axis[i].store(0, std::memory_order_relaxed);
    for (unsigned i = 0; i < PAD_HATS; i++)
        p.hat[i].store(SDL_HAT_CENTERED, std::memory_order_relaxed);
    p.buttons.store(0, std::memory_order_relaxed);

    SDL2Circle_Log("sdl2", SDL2CIRCLE_LOG_NOTICE, "%s detached", p.path);

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type          = SDL_JOYDEVICEREMOVED;
    ev.jdevice.which = instance;          // REMOVED carries an instance ID
    PushEvent(ev);
}

// One pad's newest report, turned into events.
void PumpSlot(unsigned slot)
{
    PadSlot &p = s_pad[slot];

    TGamePadState now;
    if (!SnapshotPad(slot, &now))
        return;

    SDL_JoystickID which = p.instance;

    for (int i = 0; i < p.naxes; i++)
    {
        Sint16 value = ScaleAxis(now.axes[i].value, p.axismin[i], p.axismax[i]);
        if (p.axis[i].load(std::memory_order_relaxed) == value)
            continue;
        p.axis[i].store(value, std::memory_order_release);

        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type        = SDL_JOYAXISMOTION;
        ev.jaxis.which = which;
        ev.jaxis.axis  = (Uint8)i;
        ev.jaxis.value = value;
        PushEvent(ev);

        SDL2Circle_ControllerJoyAxis(which, i, value);
    }

    for (int i = 0; i < p.nhats; i++)
    {
        Uint8 value = HatToSDL(now.hats[i]);
        if (p.hat[i].load(std::memory_order_relaxed) == value)
            continue;
        p.hat[i].store(value, std::memory_order_release);

        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type       = SDL_JOYHATMOTION;
        ev.jhat.which = which;
        ev.jhat.hat   = (Uint8)i;
        ev.jhat.value = value;
        PushEvent(ev);

        SDL2Circle_ControllerJoyHat(which, i, value);
    }

    Uint32 mask = p.nbuttons >= 32 ? 0xFFFFFFFFu : ((1u << p.nbuttons) - 1u);
    Uint32 buttons = now.buttons & mask;
    Uint32 diff = buttons ^ p.buttons.load(std::memory_order_relaxed);
    if (diff)
    {
        p.buttons.store(buttons, std::memory_order_release);

        for (int i = 0; i < p.nbuttons; i++)
        {
            if (!(diff & (1u << i)))
                continue;
            bool down = (buttons & (1u << i)) != 0;

            SDL_Event ev;
            memset(&ev, 0, sizeof ev);
            ev.type          = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
            ev.jbutton.which = which;
            ev.jbutton.button = (Uint8)i;
            ev.jbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
            PushEvent(ev);

            SDL2Circle_ControllerJoyButton(which, i, down);
        }
    }
}

// ---------------------------------------------------------------------------
// Rumble, marshalled to core 0
// ---------------------------------------------------------------------------

struct RumbleArgs
{
    unsigned slot;
    TGamePadRumbleMode mode;
    Uint32 duration_ms;
};

void rumble_on0(void *arg)
{
    RumbleArgs *a = (RumbleArgs *)arg;
    PadSlot &p = s_pad[a->slot];
    if (!p.present.load(std::memory_order_acquire) || !p.dev)
        return;

    p.dev->SetRumbleMode(a->mode);
    p.rumble_until = (a->mode != GamePadRumbleModeOff && a->duration_ms)
                         ? CTimer::GetClockTicks64() + (u64)a->duration_ms * 1000
                         : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Producer entry points (core 0)
// ---------------------------------------------------------------------------

void SDL2Circle_JoystickPump(bool bPlugAndPlayChanged)
{
    unsigned removed = s_removedMask.exchange(0, std::memory_order_acq_rel);
    for (unsigned i = 0; i < MAX_PADS; i++)
        if (removed & (1u << i))
            DetachSlot(i);

    // Circle names a pad the moment it has configured it, and a pad can turn
    // up at any time — hours into a session. UpdatePlugAndPlay saying
    // "something changed" is the only attach signal there is, so a scan of
    // the free slots follows every one of them.
    if (bPlugAndPlayChanged)
    {
        for (unsigned i = 0; i < MAX_PADS; i++)
        {
            if (s_pad[i].present.load(std::memory_order_relaxed))
                continue;
            CDevice *pDevice =
                CDeviceNameService::Get()->GetDevice("upad", i + 1, FALSE);
            if (pDevice)
                AttachSlot(i, (CUSBGamePadDevice *)pDevice);
        }
    }

    u64 now = 0;
    for (unsigned i = 0; i < MAX_PADS; i++)
    {
        if (!s_pad[i].present.load(std::memory_order_relaxed))
            continue;

        PumpSlot(i);

        if (s_pad[i].rumble_until)
        {
            if (!now)
                now = CTimer::GetClockTicks64();
            if (now >= s_pad[i].rumble_until)
            {
                s_pad[i].rumble_until = 0;
                if (s_pad[i].dev)
                    s_pad[i].dev->SetRumbleMode(GamePadRumbleModeOff);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Services for the game-controller layer (src/gamecontroller.cpp)
// ---------------------------------------------------------------------------

int SDL2Circle_JoySlotForDeviceIndex(int device_index)
{
    if (device_index < 0)
        return -1;
    int index = 0;
    for (unsigned i = 0; i < MAX_PADS; i++)
    {
        if (!s_pad[i].present.load(std::memory_order_acquire))
            continue;
        if (index == device_index)
            return (int)i;
        index++;
    }
    return -1;
}

int SDL2Circle_JoySlotForInstance(SDL_JoystickID instance)
{
    for (unsigned i = 0; i < MAX_PADS; i++)
        if (s_pad[i].present.load(std::memory_order_acquire)
            && s_pad[i].instance == instance)
            return (int)i;
    return -1;
}

int SDL2Circle_JoySlotOf(SDL_Joystick *joystick)
{
    return LiveSlot(joystick);
}

int SDL2Circle_JoyInfo(int slot, SDL2CircleJoyInfo *out)
{
    if (slot < 0 || slot >= (int)MAX_PADS)
        return 0;
    const PadSlot &p = s_pad[slot];
    if (!p.present.load(std::memory_order_acquire))
        return 0;

    out->name     = p.name;
    out->path     = p.path;
    out->guid     = p.guid;
    out->vendor   = p.vendor;
    out->product  = p.product;
    out->version  = p.version;
    out->naxes    = p.naxes;
    out->nhats    = p.nhats;
    out->nbuttons = p.nbuttons;
    out->instance = p.instance;
    out->properties = p.properties;
    return 1;
}

Sint16 SDL2Circle_JoySlotAxis(int slot, int axis)
{
    if (slot < 0 || slot >= (int)MAX_PADS || axis < 0 || axis >= (int)PAD_AXES)
        return 0;
    return s_pad[slot].axis[axis].load(std::memory_order_acquire);
}

Uint8 SDL2Circle_JoySlotHat(int slot, int hat)
{
    if (slot < 0 || slot >= (int)MAX_PADS || hat < 0 || hat >= (int)PAD_HATS)
        return SDL_HAT_CENTERED;
    return s_pad[slot].hat[hat].load(std::memory_order_acquire);
}

Uint8 SDL2Circle_JoySlotButton(int slot, int button)
{
    if (slot < 0 || slot >= (int)MAX_PADS || button < 0 || button >= (int)PAD_BUTTONS)
        return 0;
    return (s_pad[slot].buttons.load(std::memory_order_acquire) & (1u << button))
               ? 1 : 0;
}

// Handles come from a fixed pool, and an entry is only ever reused once the
// application has closed it. A handle the application still holds therefore
// never turns into some other device that happened to arrive in the same
// place: it keeps reporting the instance it was opened for, which is gone,
// and SDL_JoystickGetAttached keeps saying so.
SDL_Joystick *SDL2Circle_JoyOpenSlot(int slot)
{
    if (slot < 0 || slot >= (int)MAX_PADS
        || !s_pad[slot].present.load(std::memory_order_acquire))
        return nullptr;

    SDL_JoystickID instance = s_pad[slot].instance;

    for (unsigned i = 0; i < MAX_PADS; i++)
        if (s_joy[i].refcount > 0 && s_joy[i].instance == instance)
        {
            s_joy[i].refcount++;
            return &s_joy[i];
        }

    for (unsigned i = 0; i < MAX_PADS; i++)
    {
        if (s_joy[i].refcount > 0)
            continue;
        s_joy[i].slot         = slot;
        s_joy[i].instance     = instance;
        s_joy[i].refcount     = 1;
        s_joy[i].player_index = -1;
        return &s_joy[i];
    }

    SDL_SetError("every joystick handle is in use");
    return nullptr;
}

// ---------------------------------------------------------------------------
// The SDL joystick API
// ---------------------------------------------------------------------------

extern "C" void SDL_LockJoysticks(void) {}
extern "C" void SDL_UnlockJoysticks(void) {}

extern "C" int SDL_NumJoysticks(void)
{
    int n = 0;
    for (unsigned i = 0; i < MAX_PADS; i++)
        if (s_pad[i].present.load(std::memory_order_acquire))
            n++;
    return n;
}

extern "C" const char *SDL_JoystickNameForIndex(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    if (slot < 0)
    {
        SDL_SetError("no joystick at device index %d", device_index);
        return nullptr;
    }
    return s_pad[slot].name;
}

extern "C" const char *SDL_JoystickPathForIndex(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    if (slot < 0)
    {
        SDL_SetError("no joystick at device index %d", device_index);
        return nullptr;
    }
    return s_pad[slot].path;
}

extern "C" SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index)
{
    SDL_JoystickGUID guid;
    memset(&guid, 0, sizeof guid);
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    if (slot >= 0)
        guid = s_pad[slot].guid;
    return guid;
}

extern "C" Uint16 SDL_JoystickGetDeviceVendor(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    return slot < 0 ? 0 : s_pad[slot].vendor;
}

extern "C" Uint16 SDL_JoystickGetDeviceProduct(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    return slot < 0 ? 0 : s_pad[slot].product;
}

extern "C" Uint16 SDL_JoystickGetDeviceProductVersion(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    return slot < 0 ? 0 : s_pad[slot].version;
}

extern "C" SDL_JoystickType SDL_JoystickGetDeviceType(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    if (slot < 0)
        return SDL_JOYSTICK_TYPE_UNKNOWN;
    return SDL_IsGameController(device_index) ? SDL_JOYSTICK_TYPE_GAMECONTROLLER
                                              : SDL_JOYSTICK_TYPE_UNKNOWN;
}

extern "C" SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    return slot < 0 ? -1 : s_pad[slot].instance;
}

extern "C" int SDL_JoystickGetDevicePlayerIndex(int device_index)
{
    (void) device_index;
    return -1;
}

extern "C" SDL_Joystick *SDL_JoystickOpen(int device_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(device_index);
    if (slot < 0)
    {
        SDL_SetError("no joystick at device index %d", device_index);
        return nullptr;
    }
    return SDL2Circle_JoyOpenSlot(slot);
}

extern "C" SDL_Joystick *SDL_JoystickFromInstanceID(SDL_JoystickID instance_id)
{
    for (unsigned i = 0; i < MAX_PADS; i++)
        if (s_joy[i].refcount > 0 && s_joy[i].instance == instance_id)
            return &s_joy[i];
    return nullptr;
}

extern "C" SDL_Joystick *SDL_JoystickFromPlayerIndex(int player_index)
{
    for (unsigned i = 0; i < MAX_PADS; i++)
        if (s_joy[i].refcount > 0 && s_joy[i].player_index == player_index)
            return &s_joy[i];
    return nullptr;
}

extern "C" void SDL_JoystickClose(SDL_Joystick *joystick)
{
    if (joystick && joystick->refcount > 0)
        joystick->refcount--;
}

extern "C" const char *SDL_JoystickName(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? nullptr : s_pad[slot].name;
}

extern "C" const char *SDL_JoystickPath(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? nullptr : s_pad[slot].path;
}

extern "C" int SDL_JoystickGetPlayerIndex(SDL_Joystick *joystick)
{
    return joystick ? joystick->player_index : -1;
}

extern "C" void SDL_JoystickSetPlayerIndex(SDL_Joystick *joystick, int player_index)
{
    if (joystick)
        joystick->player_index = player_index;
}

extern "C" SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joystick)
{
    SDL_JoystickGUID guid;
    memset(&guid, 0, sizeof guid);
    int slot = LiveSlot(joystick);
    if (slot >= 0)
        guid = s_pad[slot].guid;
    return guid;
}

extern "C" Uint16 SDL_JoystickGetVendor(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? 0 : s_pad[slot].vendor;
}

extern "C" Uint16 SDL_JoystickGetProduct(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? 0 : s_pad[slot].product;
}

extern "C" Uint16 SDL_JoystickGetProductVersion(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? 0 : s_pad[slot].version;
}

extern "C" Uint16 SDL_JoystickGetFirmwareVersion(SDL_Joystick *joystick)
{
    (void) joystick;
    return 0;
}

extern "C" const char *SDL_JoystickGetSerial(SDL_Joystick *joystick)
{
    (void) joystick;
    return nullptr;
}

extern "C" SDL_JoystickType SDL_JoystickGetType(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    if (slot < 0)
        return SDL_JOYSTICK_TYPE_UNKNOWN;
    return SDL_JoystickGetDeviceType(DeviceIndexOfSlot(slot));
}

extern "C" void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *pszGUID,
                                          int cbGUID)
{
    static const char hex[] = "0123456789abcdef";
    if (!pszGUID || cbGUID <= 0)
        return;
    int i;
    for (i = 0; i < (int)sizeof guid.data && i < (cbGUID - 1) / 2; i++)
    {
        *pszGUID++ = hex[guid.data[i] >> 4];
        *pszGUID++ = hex[guid.data[i] & 0x0F];
    }
    *pszGUID = '\0';
}

extern "C" SDL_JoystickGUID SDL_JoystickGetGUIDFromString(const char *pchGUID)
{
    SDL_JoystickGUID guid;
    memset(&guid, 0, sizeof guid);
    if (!pchGUID)
        return guid;

    size_t len = strlen(pchGUID) & ~(size_t)1;
    if (len > sizeof(guid.data) * 2)
        len = sizeof(guid.data) * 2;

    for (size_t i = 0; i < len; i += 2)
    {
        Uint8 byte = 0;
        for (int n = 0; n < 2; n++)
        {
            char c = pchGUID[i + n];
            Uint8 nibble = 0;
            if      (c >= '0' && c <= '9') nibble = (Uint8)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (Uint8)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = (Uint8)(c - 'A' + 10);
            byte = (Uint8)((byte << 4) | nibble);
        }
        guid.data[i / 2] = byte;
    }
    return guid;
}

extern "C" void SDL_GetJoystickGUIDInfo(SDL_JoystickGUID guid, Uint16 *vendor,
                                        Uint16 *product, Uint16 *version,
                                        Uint16 *crc16)
{
    Uint16 bus = GetLE16(&guid.data[0]);

    // The standard form: a small bus number, and the two padding words that
    // sit either side of the product ID are zero. Anything else carries the
    // device name in place of the IDs, and has no vendor or product to give.
    if (bus < ' '
        && GetLE16(&guid.data[6]) == 0
        && GetLE16(&guid.data[10]) == 0)
    {
        if (vendor)  *vendor  = GetLE16(&guid.data[4]);
        if (product) *product = GetLE16(&guid.data[8]);
        if (version) *version = GetLE16(&guid.data[12]);
        if (crc16)   *crc16   = GetLE16(&guid.data[2]);
        return;
    }

    if (vendor)  *vendor  = 0;
    if (product) *product = 0;
    if (version) *version = 0;
    if (crc16)   *crc16   = (bus < ' ') ? GetLE16(&guid.data[2]) : 0;
}

extern "C" SDL_bool SDL_JoystickGetAttached(SDL_Joystick *joystick)
{
    return LiveSlot(joystick) >= 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick)
{
    return joystick ? joystick->instance : -1;
}

extern "C" int SDL_JoystickNumAxes(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? -1 : s_pad[slot].naxes;
}

extern "C" int SDL_JoystickNumHats(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? -1 : s_pad[slot].nhats;
}

extern "C" int SDL_JoystickNumButtons(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    return slot < 0 ? -1 : s_pad[slot].nbuttons;
}

// Circle's gamepad state has no trackball, and no USB gamepad driver in it
// reports one.
extern "C" int SDL_JoystickNumBalls(SDL_Joystick *joystick)
{
    return LiveSlot(joystick) < 0 ? -1 : 0;
}

extern "C" int SDL_JoystickGetBall(SDL_Joystick *joystick, int ball, int *dx, int *dy)
{
    (void) joystick; (void) ball;
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return SDL_SetError("joystick has no balls");
}

extern "C" Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis)
{
    return SDL2Circle_JoySlotAxis(LiveSlot(joystick), axis);
}

extern "C" SDL_bool SDL_JoystickGetAxisInitialState(SDL_Joystick *joystick,
                                                    int axis, Sint16 *state)
{
    int slot = LiveSlot(joystick);
    if (slot < 0 || axis < 0 || axis >= s_pad[slot].naxes)
        return SDL_FALSE;
    if (state)
        *state = (Sint16)(s_pad[slot].axismin[axis] >= 0 ? -32768 : 0);
    return SDL_TRUE;
}

extern "C" Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat)
{
    return SDL2Circle_JoySlotHat(LiveSlot(joystick), hat);
}

extern "C" Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button)
{
    return SDL2Circle_JoySlotButton(LiveSlot(joystick), button);
}

// State lives in memory both cores can see and is refreshed by the pump, so
// this exists to make sure the pump has run for an application that never
// polls the event queue.
extern "C" void SDL_JoystickUpdate(void)
{
    SDL_PumpEvents();
}

// Joystick and controller events are always delivered; there is no queue to
// switch off.
extern "C" int SDL_JoystickEventState(int state)
{
    (void) state;
    return SDL_ENABLE;
}

extern "C" SDL_JoystickPowerLevel SDL_JoystickCurrentPowerLevel(SDL_Joystick *joystick)
{
    return LiveSlot(joystick) < 0 ? SDL_JOYSTICK_POWER_UNKNOWN
                                  : SDL_JOYSTICK_POWER_WIRED;
}

extern "C" SDL_bool SDL_JoystickHasRumble(SDL_Joystick *joystick)
{
    int slot = LiveSlot(joystick);
    if (slot < 0)
        return SDL_FALSE;
    return (s_pad[slot].properties & GamePadPropertyHasRumble) ? SDL_TRUE
                                                               : SDL_FALSE;
}

// Circle's rumble control is three states — off, low, high — so that is
// exactly what this offers. SDL's two magnitudes are reduced to whichever is
// stronger, and anything past halfway is "high". There is no per-motor
// control underneath to expose, and no envelope: pretending otherwise would
// make an application think it had shaped a effect it had not.
extern "C" int SDL_JoystickRumble(SDL_Joystick *joystick,
                                  Uint16 low_frequency_rumble,
                                  Uint16 high_frequency_rumble,
                                  Uint32 duration_ms)
{
    int slot = LiveSlot(joystick);
    if (slot < 0)
        return SDL_SetError("joystick is not attached");
    if (!(s_pad[slot].properties & GamePadPropertyHasRumble))
        return SDL_SetError("gamepad has no rumble motor");

    Uint16 strength = low_frequency_rumble > high_frequency_rumble
                          ? low_frequency_rumble : high_frequency_rumble;

    RumbleArgs args;
    args.slot        = (unsigned)slot;
    args.duration_ms = duration_ms;
    args.mode        = strength == 0        ? GamePadRumbleModeOff
                       : strength < 0x8000  ? GamePadRumbleModeLow
                                            : GamePadRumbleModeHigh;

    SDL2Circle_CallOn0(rumble_on0, &args);
    return 0;
}

// Trigger motors, an LED the shim does not drive, and vendor effect packets:
// no Circle gamepad driver offers any of them through an interface this shim
// could reach honestly, so each one says no rather than returning success and
// doing nothing.
extern "C" SDL_bool SDL_JoystickHasRumbleTriggers(SDL_Joystick *joystick)
{
    (void) joystick;
    return SDL_FALSE;
}

extern "C" int SDL_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left,
                                          Uint16 right, Uint32 duration_ms)
{
    (void) joystick; (void) left; (void) right; (void) duration_ms;
    return SDL_SetError("trigger rumble is not supported");
}

extern "C" SDL_bool SDL_JoystickHasLED(SDL_Joystick *joystick)
{
    (void) joystick;
    return SDL_FALSE;
}

extern "C" int SDL_JoystickSetLED(SDL_Joystick *joystick, Uint8 r, Uint8 g, Uint8 b)
{
    (void) joystick; (void) r; (void) g; (void) b;
    return SDL_SetError("LED control is not supported");
}

extern "C" int SDL_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void) joystick; (void) data; (void) size;
    return SDL_SetError("effect packets are not supported");
}

// Virtual joysticks have no place in a bare-metal shim: there is no software
// device layer for them to plug into.
extern "C" int SDL_JoystickAttachVirtual(SDL_JoystickType type, int naxes,
                                         int nbuttons, int nhats)
{
    (void) type; (void) naxes; (void) nbuttons; (void) nhats;
    return SDL_SetError("virtual joysticks are not supported");
}

extern "C" int SDL_JoystickDetachVirtual(int device_index)
{
    (void) device_index;
    return SDL_SetError("virtual joysticks are not supported");
}

extern "C" SDL_bool SDL_JoystickIsVirtual(int device_index)
{
    (void) device_index;
    return SDL_FALSE;
}

extern "C" int SDL_JoystickSetVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value)
{
    (void) joystick; (void) axis; (void) value;
    return SDL_SetError("virtual joysticks are not supported");
}

extern "C" int SDL_JoystickSetVirtualButton(SDL_Joystick *joystick, int button, Uint8 value)
{
    (void) joystick; (void) button; (void) value;
    return SDL_SetError("virtual joysticks are not supported");
}

extern "C" int SDL_JoystickSetVirtualHat(SDL_Joystick *joystick, int hat, Uint8 value)
{
    (void) joystick; (void) hat; (void) value;
    return SDL_SetError("virtual joysticks are not supported");
}
