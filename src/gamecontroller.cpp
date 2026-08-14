//
// gamecontroller.cpp - SDL's game-controller layer over the shim's joysticks.
//
// A game controller is not a device. It is a joystick plus a mapping: a line
// of text, looked up by the joystick's GUID, that says which raw axis, button
// or hat direction plays the part of each named control on a standard pad.
// A pad with no line in the database is not a game controller - it is still a
// perfectly good joystick, and an application that reads raw axes and buttons
// works with it, a steering wheel included.
//
// The mapping text format and its lookup rules are SDL2's, reproduced closely
// enough that an unmodified gamecontrollerdb.txt works: a comma-separated
// line of GUID, name, then key:value pairs whose values name a raw control as
// bN (button), aN (axis), or hN.M (hat N, direction bitmask M), with the
// prefixes and the trailing tilde that select half an axis or invert it.
//
// A database line only loads if its `platform:` field names the platform
// doing the loading. Nothing in any published database says Circle, so lines
// tagged Linux are accepted as well: the GUIDs this shim builds for USB pads
// have exactly the shape Linux's evdev backend builds (USB bus, no driver
// signature), so the Linux table matches byte for byte.
//
// Mappings are added, and controllers opened, by the application. The
// events, though, are derived where the joystick events are made: on core 0,
// straight after each raw event, so a controller event arrives in the same
// order and in the same pump as the joystick event it came from. Opening a
// controller publishes its bindings with a release store, which is what
// makes them safe for core 0 to read.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "shim_internal.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace
{

const int MAX_CONTROLLERS = 8;
const int MAX_BINDINGS    = 64;
const int MAX_JOY_AXES    = 16;

// ---------------------------------------------------------------------------
// A parsed binding: one raw control feeding one named control.
// ---------------------------------------------------------------------------

struct ExtBind
{
    SDL_GameControllerBindType inputType;
    union
    {
        int button;
        struct { int axis, axis_min, axis_max; } axis;
        struct { int hat, hat_mask; } hat;
    } input;

    SDL_GameControllerBindType outputType;
    union
    {
        SDL_GameControllerButton button;
        struct { SDL_GameControllerAxis axis; int axis_min, axis_max; } axis;
    } output;
};

// ---------------------------------------------------------------------------
// The mapping database.
//
// Entries are only ever added, never freed or rewritten in place, and a new
// one goes on the front of the list. That is what lets core 0 walk the list
// while the application is still loading a database: a reader either sees a
// node completely or does not see it at all. Re-adding a GUID supersedes the
// old entry rather than editing it - the old node stays linked, marked dead,
// and lookups skip it.
// ---------------------------------------------------------------------------

struct MappingEntry
{
    SDL_JoystickGUID guid;        // as written in the database
    Uint16           crc;         // from an optional `crc:` field; 0 if absent
    char            *text;        // owned: "<guid>\0<name>\0<bindings...>"
    const char      *name;
    const char      *bindings;
    std::atomic<bool> dead;
    MappingEntry     *next;
};

std::atomic<MappingEntry *> s_mappings{nullptr};
MappingEntry *s_defaultMapping = nullptr;
int s_mappingCount = 0;

const SDL_JoystickGUID s_zeroGUID = {{0}};

bool GuidIsZero(const SDL_JoystickGUID &g)
{
    return memcmp(&g, &s_zeroGUID, sizeof g) == 0;
}

void GuidSetCRC(SDL_JoystickGUID *g, Uint16 crc)
{
    g->data[2] = (Uint8)(crc & 0xFF);
    g->data[3] = (Uint8)(crc >> 8);
}

Uint16 GuidGetCRC(const SDL_JoystickGUID &g)
{
    return (Uint16)(g.data[2] | ((Uint16)g.data[3] << 8));
}

void GuidSetVersion(SDL_JoystickGUID *g, Uint16 version)
{
    g->data[12] = (Uint8)(version & 0xFF);
    g->data[13] = (Uint8)(version >> 8);
}

// ---------------------------------------------------------------------------
// Named controls. The position in each table is the enum value, so these are
// both the parser's vocabulary and SDL_GameControllerGetStringFor*'s answer.
// ---------------------------------------------------------------------------

const char *const kAxisNames[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
};

const char *const kButtonNames[] = {
    "a", "b", "x", "y", "back", "guide", "start",
    "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright",
    "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"
};

bool EqualNoCase(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb)
            return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

} // namespace

// SDL's opaque controller handle. One per joystick slot, from a static pool:
// there is no more than one controller per pad, and the pool keeps the object
// alive for core 0 to read even after the application closes it.
struct _SDL_GameController
{
    int            slot;
    SDL_JoystickID instance;
    int            refcount;
    SDL_Joystick  *joystick;
    char           name[128];
    char           mapping[512];      // the bindings text this was built from

    ExtBind        bindings[MAX_BINDINGS];
    int            nbindings;

    // Derived state, written where the events are made and read anywhere.
    std::atomic<Sint16> axis_value[SDL_CONTROLLER_AXIS_MAX];
    std::atomic<Uint8>  button_state[SDL_CONTROLLER_BUTTON_MAX];

    // Which binding last claimed each raw axis, so an axis leaving one
    // binding's range releases what that binding was driving.
    const ExtBind *last_match_axis[MAX_JOY_AXES];

    // Release-published once the bindings above are complete.
    std::atomic<bool> live;
};

namespace
{

SDL_GameController s_controller[MAX_CONTROLLERS];

// ---------------------------------------------------------------------------
// Mapping-string parsing
// ---------------------------------------------------------------------------

SDL_GameControllerAxis AxisFromString(const char *str)
{
    if (!str || !*str)
        return SDL_CONTROLLER_AXIS_INVALID;
    if (*str == '+' || *str == '-')
        str++;
    for (int i = 0; i < (int)(sizeof kAxisNames / sizeof kAxisNames[0]); i++)
        if (EqualNoCase(str, kAxisNames[i]))
            return (SDL_GameControllerAxis)i;
    return SDL_CONTROLLER_AXIS_INVALID;
}

SDL_GameControllerButton ButtonFromString(const char *str)
{
    if (!str || !*str)
        return SDL_CONTROLLER_BUTTON_INVALID;
    for (int i = 0; i < (int)(sizeof kButtonNames / sizeof kButtonNames[0]); i++)
        if (EqualNoCase(str, kButtonNames[i]))
            return (SDL_GameControllerButton)i;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// One "output:input" pair, appended to the controller's binding list.
void ParseElement(SDL_GameController *gc, const char *szOut, const char *szIn)
{
    if (gc->nbindings >= MAX_BINDINGS || !*szOut || !*szIn)
        return;

    ExtBind bind;
    memset(&bind, 0, sizeof bind);

    char half_out = 0;
    if (*szOut == '+' || *szOut == '-')
        half_out = *szOut++;

    SDL_GameControllerAxis axis = AxisFromString(szOut);
    SDL_GameControllerButton button = ButtonFromString(szOut);

    if (axis != SDL_CONTROLLER_AXIS_INVALID)
    {
        bind.outputType = SDL_CONTROLLER_BINDTYPE_AXIS;
        bind.output.axis.axis = axis;
        if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT
            || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        {
            // A trigger only ever travels one way, whatever the line says.
            bind.output.axis.axis_min = 0;
            bind.output.axis.axis_max = 32767;
        }
        else if (half_out == '+')
        {
            bind.output.axis.axis_min = 0;
            bind.output.axis.axis_max = 32767;
        }
        else if (half_out == '-')
        {
            bind.output.axis.axis_min = 0;
            bind.output.axis.axis_max = -32768;
        }
        else
        {
            bind.output.axis.axis_min = -32768;
            bind.output.axis.axis_max = 32767;
        }
    }
    else if (button != SDL_CONTROLLER_BUTTON_INVALID)
    {
        bind.outputType = SDL_CONTROLLER_BINDTYPE_BUTTON;
        bind.output.button = button;
    }
    else
    {
        return;   // a metadata field, or a control this SDL version has no name for
    }

    char half_in = 0;
    if (*szIn == '+' || *szIn == '-')
        half_in = *szIn++;

    size_t inlen = strlen(szIn);
    if (inlen == 0)
        return;
    bool invert = szIn[inlen - 1] == '~';

    if (szIn[0] == 'a' && IsDigit(szIn[1]))
    {
        bind.inputType = SDL_CONTROLLER_BINDTYPE_AXIS;
        bind.input.axis.axis = atoi(&szIn[1]);
        if (half_in == '+')
        {
            bind.input.axis.axis_min = 0;
            bind.input.axis.axis_max = 32767;
        }
        else if (half_in == '-')
        {
            bind.input.axis.axis_min = 0;
            bind.input.axis.axis_max = -32768;
        }
        else
        {
            bind.input.axis.axis_min = -32768;
            bind.input.axis.axis_max = 32767;
        }
        if (invert)
        {
            int tmp = bind.input.axis.axis_min;
            bind.input.axis.axis_min = bind.input.axis.axis_max;
            bind.input.axis.axis_max = tmp;
        }
    }
    else if (szIn[0] == 'b' && IsDigit(szIn[1]))
    {
        bind.inputType = SDL_CONTROLLER_BINDTYPE_BUTTON;
        bind.input.button = atoi(&szIn[1]);
    }
    else if (szIn[0] == 'h' && IsDigit(szIn[1]) && szIn[2] == '.' && IsDigit(szIn[3]))
    {
        bind.inputType = SDL_CONTROLLER_BINDTYPE_HAT;
        bind.input.hat.hat = atoi(&szIn[1]);
        bind.input.hat.hat_mask = atoi(&szIn[3]);
    }
    else
    {
        return;
    }

    gc->bindings[gc->nbindings++] = bind;
}

// Walk the key:value part of a mapping line. Spaces are dropped wherever they
// appear, a comma ends an element and a colon separates the two halves.
void ParseBindings(SDL_GameController *gc, const char *text)
{
    char out[24], in[24];
    unsigned i = 0;
    bool inValue = false;

    out[0] = in[0] = 0;

    for (const char *p = text; ; p++)
    {
        if (*p == ':')
        {
            i = 0;
            inValue = true;
        }
        else if (*p == ' ')
        {
            continue;
        }
        else if (*p == ',' || *p == 0)
        {
            if (out[0] || in[0])
                ParseElement(gc, out, in);
            out[0] = in[0] = 0;
            i = 0;
            inValue = false;
            if (*p == 0)
                return;
        }
        else
        {
            char *buf = inValue ? in : out;
            if (i < sizeof out - 1)
            {
                buf[i++] = *p;
                buf[i] = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Mapping lookup
// ---------------------------------------------------------------------------

MappingEntry *MatchGUID(SDL_JoystickGUID guid, bool match_version, bool exact_crc)
{
    Uint16 crc = GuidGetCRC(guid);
    GuidSetCRC(&guid, 0);
    if (!match_version)
        GuidSetVersion(&guid, 0);

    MappingEntry *best = nullptr;

    for (MappingEntry *m = s_mappings.load(std::memory_order_acquire);
         m != nullptr; m = m->next)
    {
        if (m->dead.load(std::memory_order_acquire) || GuidIsZero(m->guid))
            continue;

        SDL_JoystickGUID mg = m->guid;
        if (!match_version)
            GuidSetVersion(&mg, 0);

        if (memcmp(&guid, &mg, sizeof guid) != 0)
            continue;

        if (m->crc)
        {
            if (m->crc != crc)
                continue;      // the line named a different device of this model
            return m;          // an exact match, CRC included
        }
        if (crc && exact_crc)
            return nullptr;
        if (!best)
            best = m;
    }
    return best;
}

MappingEntry *FindMappingForGUID(SDL_JoystickGUID guid)
{
    MappingEntry *m = MatchGUID(guid, true, true);
    if (!m) m = MatchGUID(guid, false, true);
    if (!m) m = MatchGUID(guid, true, false);
    if (!m) m = MatchGUID(guid, false, false);
    return m ? m : s_defaultMapping;
}

MappingEntry *FindMappingForSlot(int slot)
{
    SDL2CircleJoyInfo info;
    if (!SDL2Circle_JoyInfo(slot, &info))
        return nullptr;
    return FindMappingForGUID(info.guid);
}

// ---------------------------------------------------------------------------
// Adding mappings
// ---------------------------------------------------------------------------

// Split one line into its three parts, in a copy this entry then owns.
// Returns nullptr if the line is not a mapping.
MappingEntry *BuildEntry(const char *line)
{
    const char *c1 = strchr(line, ',');
    if (!c1)
        return nullptr;
    const char *c2 = strchr(c1 + 1, ',');
    if (!c2)
        return nullptr;

    size_t len = strlen(line);
    char *text = (char *)malloc(len + 1);
    if (!text)
        return nullptr;
    memcpy(text, line, len + 1);

    size_t off1 = (size_t)(c1 - line);
    size_t off2 = (size_t)(c2 - line);
    text[off1] = 0;
    text[off2] = 0;

    MappingEntry *m = (MappingEntry *)malloc(sizeof(MappingEntry));
    if (!m)
    {
        free(text);
        return nullptr;
    }

    m->text     = text;
    m->name     = text + off1 + 1;
    m->bindings = text + off2 + 1;
    m->next     = nullptr;
    m->crc      = 0;
    m->dead.store(false, std::memory_order_relaxed);

    // "default" is SDL's catch-all: a mapping for every pad with no line of
    // its own. A database that carries one is opting every unknown device in.
    if (EqualNoCase(text, "default"))
        m->guid = s_zeroGUID;
    else
        m->guid = SDL_JoystickGetGUIDFromString(text);

    // A `crc:` field pins the line to one device among several sharing a
    // vendor and product ID.
    const char *crcField = strstr(m->bindings, "crc:");
    if (crcField)
        m->crc = (Uint16)strtol(crcField + 4, nullptr, 16);

    return m;
}

int AddMappingLine(const char *line)
{
    if (!line || !*line)
        return SDL_SetError("mapping string is empty");

    MappingEntry *m = BuildEntry(line);
    if (!m)
        return SDL_SetError("mapping string is malformed");

    bool isDefault = GuidIsZero(m->guid) && EqualNoCase(m->text, "default");

    // Supersede any live entry for the same GUID rather than editing it, so a
    // reader on another core never sees a half-replaced one.
    int replaced = 0;
    if (!GuidIsZero(m->guid))
    {
        for (MappingEntry *old = s_mappings.load(std::memory_order_acquire);
             old != nullptr; old = old->next)
        {
            if (old->dead.load(std::memory_order_relaxed))
                continue;
            if (memcmp(&old->guid, &m->guid, sizeof m->guid) == 0
                && old->crc == m->crc)
            {
                old->dead.store(true, std::memory_order_release);
                replaced = 1;
            }
        }
    }

    m->next = s_mappings.load(std::memory_order_relaxed);
    s_mappings.store(m, std::memory_order_release);

    if (isDefault)
        s_defaultMapping = m;
    if (!replaced)
        s_mappingCount++;

    return replaced ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Deriving controller events from joystick events (core 0)
// ---------------------------------------------------------------------------

SDL_GameController *LiveControllerFor(SDL_JoystickID instance)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (s_controller[i].live.load(std::memory_order_acquire)
            && s_controller[i].instance == instance)
            return &s_controller[i];
    return nullptr;
}

void PushEvent(SDL_Event &ev)
{
    ev.common.timestamp = SDL_GetTicks();
    SDL_PushEvent(&ev);
}

// `report` false seeds the state at open time without waking the event queue.
void SetAxis(SDL_GameController *gc, SDL_GameControllerAxis axis, Sint16 value,
             bool report)
{
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
        return;
    if (gc->axis_value[axis].load(std::memory_order_relaxed) == value)
        return;
    gc->axis_value[axis].store(value, std::memory_order_release);
    if (!report)
        return;

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type         = SDL_CONTROLLERAXISMOTION;
    ev.caxis.which  = gc->instance;
    ev.caxis.axis   = (Uint8)axis;
    ev.caxis.value  = value;
    PushEvent(ev);
}

void SetButton(SDL_GameController *gc, SDL_GameControllerButton button,
               bool down, bool report)
{
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return;
    Uint8 state = down ? SDL_PRESSED : SDL_RELEASED;
    if (gc->button_state[button].load(std::memory_order_relaxed) == state)
        return;
    gc->button_state[button].store(state, std::memory_order_release);
    if (!report)
        return;

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type           = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.which  = gc->instance;
    ev.cbutton.button = (Uint8)button;
    ev.cbutton.state  = state;
    PushEvent(ev);
}

bool SameOutput(const ExtBind *a, const ExtBind *b)
{
    if (a->outputType != b->outputType)
        return false;
    if (a->outputType == SDL_CONTROLLER_BINDTYPE_AXIS)
        return a->output.axis.axis == b->output.axis.axis;
    return a->output.button == b->output.button;
}

void ResetOutput(SDL_GameController *gc, const ExtBind *bind, bool report)
{
    if (bind->outputType == SDL_CONTROLLER_BINDTYPE_AXIS)
        SetAxis(gc, bind->output.axis.axis, 0, report);
    else
        SetButton(gc, bind->output.button, false, report);
}

void HandleAxis(SDL_GameController *gc, int axis, Sint16 raw, bool report)
{
    if (axis < 0 || axis >= MAX_JOY_AXES)
        return;

    int value = raw;
    const ExtBind *match = nullptr;

    // A half-axis binding only claims the half it covers, and an inverted one
    // stores its endpoints the other way round, so the test works both ways.
    for (int i = 0; i < gc->nbindings; i++)
    {
        const ExtBind *b = &gc->bindings[i];
        if (b->inputType != SDL_CONTROLLER_BINDTYPE_AXIS
            || b->input.axis.axis != axis)
            continue;

        if (b->input.axis.axis_min < b->input.axis.axis_max)
        {
            if (value >= b->input.axis.axis_min && value <= b->input.axis.axis_max)
            {
                match = b;
                break;
            }
        }
        else if (value >= b->input.axis.axis_max && value <= b->input.axis.axis_min)
        {
            match = b;
            break;
        }
    }

    const ExtBind *last = gc->last_match_axis[axis];
    if (last && (!match || !SameOutput(last, match)))
        ResetOutput(gc, last, report);

    if (match)
    {
        if (match->outputType == SDL_CONTROLLER_BINDTYPE_AXIS)
        {
            int out = value;
            if (match->input.axis.axis_min != match->output.axis.axis_min
                || match->input.axis.axis_max != match->output.axis.axis_max)
            {
                float t = (float)(value - match->input.axis.axis_min)
                          / (float)(match->input.axis.axis_max
                                    - match->input.axis.axis_min);
                out = match->output.axis.axis_min
                      + (int)(t * (float)(match->output.axis.axis_max
                                          - match->output.axis.axis_min));
            }
            SetAxis(gc, match->output.axis.axis, (Sint16)out, report);
        }
        else
        {
            int threshold = match->input.axis.axis_min
                            + (match->input.axis.axis_max
                               - match->input.axis.axis_min) / 2;
            bool down = (match->input.axis.axis_max < match->input.axis.axis_min)
                            ? (value <= threshold) : (value >= threshold);
            SetButton(gc, match->output.button, down, report);
        }
    }

    gc->last_match_axis[axis] = match;
}

void HandleButton(SDL_GameController *gc, int button, bool down, bool report)
{
    for (int i = 0; i < gc->nbindings; i++)
    {
        const ExtBind *b = &gc->bindings[i];
        if (b->inputType != SDL_CONTROLLER_BINDTYPE_BUTTON
            || b->input.button != button)
            continue;

        if (b->outputType == SDL_CONTROLLER_BINDTYPE_AXIS)
            SetAxis(gc, b->output.axis.axis,
                    (Sint16)(down ? b->output.axis.axis_max
                                  : b->output.axis.axis_min), report);
        else
            SetButton(gc, b->output.button, down, report);
    }
}

void HandleHat(SDL_GameController *gc, int hat, Uint8 value, bool report)
{
    for (int i = 0; i < gc->nbindings; i++)
    {
        const ExtBind *b = &gc->bindings[i];
        if (b->inputType != SDL_CONTROLLER_BINDTYPE_HAT || b->input.hat.hat != hat)
            continue;

        bool down = (value & b->input.hat.hat_mask) != 0;
        if (b->outputType == SDL_CONTROLLER_BINDTYPE_AXIS)
            SetAxis(gc, b->output.axis.axis,
                    (Sint16)(down ? b->output.axis.axis_max : 0), report);
        else
            SetButton(gc, b->output.button, down, report);
    }
}

// Run the whole current joystick state through the bindings, quietly, so a
// controller opened while a control is already deflected starts out right.
void SeedState(SDL_GameController *gc)
{
    SDL2CircleJoyInfo info;
    if (!SDL2Circle_JoyInfo(gc->slot, &info))
        return;

    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
        gc->axis_value[i].store(0, std::memory_order_relaxed);
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        gc->button_state[i].store(SDL_RELEASED, std::memory_order_relaxed);
    for (int i = 0; i < MAX_JOY_AXES; i++)
        gc->last_match_axis[i] = nullptr;

    for (int i = 0; i < info.naxes; i++)
        HandleAxis(gc, i, SDL2Circle_JoySlotAxis(gc->slot, i), false);
    for (int i = 0; i < info.nhats; i++)
        HandleHat(gc, i, SDL2Circle_JoySlotHat(gc->slot, i), false);
    for (int i = 0; i < info.nbuttons; i++)
        HandleButton(gc, i, SDL2Circle_JoySlotButton(gc->slot, i) != 0, false);
}

} // namespace

// ---------------------------------------------------------------------------
// Producer hooks (called from the joystick pump on core 0)
// ---------------------------------------------------------------------------

void SDL2Circle_ControllerJoyAxis(SDL_JoystickID which, int axis, Sint16 value)
{
    SDL_GameController *gc = LiveControllerFor(which);
    if (gc)
        HandleAxis(gc, axis, value, true);
}

void SDL2Circle_ControllerJoyButton(SDL_JoystickID which, int button, bool down)
{
    SDL_GameController *gc = LiveControllerFor(which);
    if (gc)
        HandleButton(gc, button, down, true);
}

void SDL2Circle_ControllerJoyHat(SDL_JoystickID which, int hat, Uint8 value)
{
    SDL_GameController *gc = LiveControllerFor(which);
    if (gc)
        HandleHat(gc, hat, value, true);
}

void SDL2Circle_ControllerDeviceAdded(int device_index)
{
    if (!SDL_IsGameController(device_index))
        return;

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type           = SDL_CONTROLLERDEVICEADDED;
    ev.cdevice.which  = device_index;    // ADDED carries a device index
    PushEvent(ev);
}

void SDL2Circle_ControllerDeviceRemoved(SDL_JoystickID instance)
{
    SDL_GameController *gc = LiveControllerFor(instance);
    if (gc)
        gc->live.store(false, std::memory_order_release);
    else if (!FindMappingForSlot(SDL2Circle_JoySlotForInstance(instance)))
        return;   // never was a controller; the joystick event is the whole story

    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type          = SDL_CONTROLLERDEVICEREMOVED;
    ev.cdevice.which = instance;         // REMOVED carries an instance ID
    PushEvent(ev);
}

// ---------------------------------------------------------------------------
// The SDL game-controller API
// ---------------------------------------------------------------------------

extern "C" int SDL_GameControllerAddMapping(const char *mappingString)
{
    return AddMappingLine(mappingString);
}

extern "C" int SDL_GameControllerAddMappingsFromRW(SDL_RWops *rw, int freerw)
{
    if (!rw)
        return SDL_SetError("no mapping database to read");

    Sint64 size = rw->size ? rw->size(rw) : -1;
    if (size <= 0 || size > 16 * 1024 * 1024)
    {
        if (freerw && rw->close)
            rw->close(rw);
        return SDL_SetError("mapping database is empty or too large");
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf)
    {
        if (freerw && rw->close)
            rw->close(rw);
        return SDL_SetError("out of memory reading the mapping database");
    }

    size_t got = rw->read ? rw->read(rw, buf, 1, (size_t)size) : 0;
    buf[got] = 0;

    if (freerw && rw->close)
        rw->close(rw);

    const char *platform = SDL_GetPlatform();
    int added = 0;

    char *line = buf;
    char *end = buf + got;
    while (line < end)
    {
        char *nl = (char *)memchr(line, '\n', (size_t)(end - line));
        char *stop = nl ? nl : end;
        if (nl)
            *nl = 0;
        else
            *end = 0;
        if (stop > line && stop[-1] == '\r')
            stop[-1] = 0;    // a database written on Windows

        // A line loads only if it names a platform, and that platform is one
        // this build answers to. Nothing published says Circle, and a Linux
        // line's GUIDs have exactly the shape this shim builds, so both are
        // accepted.
        const char *tag = strstr(line, "platform:");
        if (tag)
        {
            tag += 9;
            const char *comma = strchr(tag, ',');
            if (comma)
            {
                size_t len = (size_t)(comma - tag);
                char name[32];
                if (len < sizeof name)
                {
                    memcpy(name, tag, len);
                    name[len] = 0;
                    if ((EqualNoCase(name, platform) || EqualNoCase(name, "Linux"))
                        && AddMappingLine(line) > 0)
                        added++;
                }
            }
        }

        line = stop + 1;
    }

    free(buf);
    return added;
}

extern "C" int SDL_GameControllerNumMappings(void)
{
    return s_mappingCount;
}

namespace
{
// Rebuild a whole mapping line from the parts the entry keeps. Ownership
// passes to the caller, as SDL's does.
char *CopyMappingText(const MappingEntry *m)
{
    if (!m)
        return nullptr;
    size_t n = strlen(m->text) + strlen(m->name) + strlen(m->bindings) + 3;
    char *s = (char *)malloc(n);
    if (s)
        snprintf(s, n, "%s,%s,%s", m->text, m->name, m->bindings);
    return s;
}
} // namespace

extern "C" char *SDL_GameControllerMappingForIndex(int mapping_index)
{
    int i = 0;
    for (MappingEntry *m = s_mappings.load(std::memory_order_acquire);
         m != nullptr; m = m->next)
    {
        if (m->dead.load(std::memory_order_relaxed))
            continue;
        if (i == mapping_index)
            return CopyMappingText(m);
        i++;
    }
    return nullptr;
}

extern "C" char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid)
{
    return CopyMappingText(FindMappingForGUID(guid));
}

extern "C" char *SDL_GameControllerMappingForDeviceIndex(int joystick_index)
{
    return CopyMappingText(
        FindMappingForSlot(SDL2Circle_JoySlotForDeviceIndex(joystick_index)));
}

extern "C" char *SDL_GameControllerMapping(SDL_GameController *gamecontroller)
{
    if (!gamecontroller || gamecontroller->refcount <= 0)
        return nullptr;
    return CopyMappingText(FindMappingForSlot(gamecontroller->slot));
}

extern "C" SDL_bool SDL_IsGameController(int joystick_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(joystick_index);
    if (slot < 0)
        return SDL_FALSE;
    return FindMappingForSlot(slot) ? SDL_TRUE : SDL_FALSE;
}

extern "C" const char *SDL_GameControllerNameForIndex(int joystick_index)
{
    MappingEntry *m =
        FindMappingForSlot(SDL2Circle_JoySlotForDeviceIndex(joystick_index));
    if (m)
        return m->name;
    return SDL_JoystickNameForIndex(joystick_index);
}

extern "C" const char *SDL_GameControllerPathForIndex(int joystick_index)
{
    return SDL_JoystickPathForIndex(joystick_index);
}

// Circle's drivers do say which console family a pad belongs to, but not in a
// form that maps onto SDL's list without guessing, so this reports what it
// knows for certain: nothing.
extern "C" SDL_GameControllerType SDL_GameControllerTypeForIndex(int joystick_index)
{
    (void) joystick_index;
    return SDL_CONTROLLER_TYPE_UNKNOWN;
}

extern "C" SDL_GameController *SDL_GameControllerOpen(int joystick_index)
{
    int slot = SDL2Circle_JoySlotForDeviceIndex(joystick_index);
    if (slot < 0)
    {
        SDL_SetError("no joystick at device index %d", joystick_index);
        return nullptr;
    }

    SDL2CircleJoyInfo info;
    if (!SDL2Circle_JoyInfo(slot, &info))
    {
        SDL_SetError("joystick is not attached");
        return nullptr;
    }

    MappingEntry *m = FindMappingForSlot(slot);
    if (!m)
    {
        SDL_SetError("no game-controller mapping for this device");
        return nullptr;
    }

    // As with joystick handles: an entry is reused only after the application
    // has closed it, so a handle still held never becomes a different device.
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (s_controller[i].refcount > 0 && s_controller[i].instance == info.instance)
        {
            s_controller[i].refcount++;
            return &s_controller[i];
        }

    SDL_GameController *gc = nullptr;
    for (int i = 0; i < MAX_CONTROLLERS && !gc; i++)
        if (s_controller[i].refcount == 0)
            gc = &s_controller[i];
    if (!gc)
    {
        SDL_SetError("every game-controller handle is in use");
        return nullptr;
    }

    gc->live.store(false, std::memory_order_release);

    gc->slot     = slot;
    gc->instance = info.instance;
    gc->refcount = 1;
    gc->joystick = SDL2Circle_JoyOpenSlot(slot);
    snprintf(gc->name, sizeof gc->name, "%s", m->name);
    snprintf(gc->mapping, sizeof gc->mapping, "%s", m->bindings);
    gc->nbindings = 0;

    ParseBindings(gc, gc->mapping);
    SeedState(gc);

    gc->live.store(true, std::memory_order_release);
    return gc;
}

extern "C" void SDL_GameControllerClose(SDL_GameController *gamecontroller)
{
    if (!gamecontroller || gamecontroller->refcount <= 0)
        return;
    if (--gamecontroller->refcount > 0)
        return;

    gamecontroller->live.store(false, std::memory_order_release);
    if (gamecontroller->joystick)
    {
        SDL_JoystickClose(gamecontroller->joystick);
        gamecontroller->joystick = nullptr;
    }
}

extern "C" SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID joyid)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (s_controller[i].refcount > 0 && s_controller[i].instance == joyid)
            return &s_controller[i];
    return nullptr;
}

extern "C" SDL_GameController *SDL_GameControllerFromPlayerIndex(int player_index)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (s_controller[i].refcount > 0 && s_controller[i].joystick
            && SDL_JoystickGetPlayerIndex(s_controller[i].joystick) == player_index)
            return &s_controller[i];
    return nullptr;
}

extern "C" const char *SDL_GameControllerName(SDL_GameController *gamecontroller)
{
    return gamecontroller ? gamecontroller->name : nullptr;
}

extern "C" const char *SDL_GameControllerPath(SDL_GameController *gamecontroller)
{
    return gamecontroller ? SDL_JoystickPath(gamecontroller->joystick) : nullptr;
}

extern "C" SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *gc)
{
    (void) gc;
    return SDL_CONTROLLER_TYPE_UNKNOWN;
}

extern "C" int SDL_GameControllerGetPlayerIndex(SDL_GameController *gc)
{
    return gc ? SDL_JoystickGetPlayerIndex(gc->joystick) : -1;
}

extern "C" void SDL_GameControllerSetPlayerIndex(SDL_GameController *gc, int player_index)
{
    if (gc)
        SDL_JoystickSetPlayerIndex(gc->joystick, player_index);
}

extern "C" Uint16 SDL_GameControllerGetVendor(SDL_GameController *gc)
{
    return gc ? SDL_JoystickGetVendor(gc->joystick) : 0;
}

extern "C" Uint16 SDL_GameControllerGetProduct(SDL_GameController *gc)
{
    return gc ? SDL_JoystickGetProduct(gc->joystick) : 0;
}

extern "C" Uint16 SDL_GameControllerGetProductVersion(SDL_GameController *gc)
{
    return gc ? SDL_JoystickGetProductVersion(gc->joystick) : 0;
}

extern "C" Uint16 SDL_GameControllerGetFirmwareVersion(SDL_GameController *gc)
{
    (void) gc;
    return 0;
}

extern "C" const char *SDL_GameControllerGetSerial(SDL_GameController *gc)
{
    (void) gc;
    return nullptr;
}

extern "C" Uint64 SDL_GameControllerGetSteamHandle(SDL_GameController *gc)
{
    (void) gc;
    return 0;
}

extern "C" SDL_bool SDL_GameControllerGetAttached(SDL_GameController *gc)
{
    if (!gc || gc->refcount <= 0)
        return SDL_FALSE;
    return SDL2Circle_JoySlotForInstance(gc->instance) >= 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc)
{
    return gc ? gc->joystick : nullptr;
}

extern "C" int SDL_GameControllerEventState(int state)
{
    (void) state;
    return SDL_ENABLE;
}

extern "C" void SDL_GameControllerUpdate(void)
{
    SDL_PumpEvents();
}

extern "C" SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *str)
{
    return AxisFromString(str);
}

extern "C" const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis axis)
{
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
        return nullptr;
    return kAxisNames[axis];
}

extern "C" SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *str)
{
    return ButtonFromString(str);
}

extern "C" const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton button)
{
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return nullptr;
    return kButtonNames[button];
}

namespace
{
SDL_GameControllerButtonBind BindFor(SDL_GameController *gc,
                                     SDL_GameControllerBindType outType,
                                     int output)
{
    SDL_GameControllerButtonBind bind;
    memset(&bind, 0, sizeof bind);
    bind.bindType = SDL_CONTROLLER_BINDTYPE_NONE;

    if (!gc || gc->refcount <= 0)
        return bind;

    for (int i = 0; i < gc->nbindings; i++)
    {
        const ExtBind *b = &gc->bindings[i];
        if (b->outputType != outType)
            continue;
        int got = (outType == SDL_CONTROLLER_BINDTYPE_AXIS)
                      ? (int)b->output.axis.axis : (int)b->output.button;
        if (got != output)
            continue;

        bind.bindType = b->inputType;
        switch (b->inputType)
        {
        case SDL_CONTROLLER_BINDTYPE_BUTTON:
            bind.value.button = b->input.button;
            break;
        case SDL_CONTROLLER_BINDTYPE_AXIS:
            bind.value.axis = b->input.axis.axis;
            break;
        case SDL_CONTROLLER_BINDTYPE_HAT:
            bind.value.hat.hat = b->input.hat.hat;
            bind.value.hat.hat_mask = b->input.hat.hat_mask;
            break;
        default:
            break;
        }
        return bind;
    }
    return bind;
}
} // namespace

extern "C" SDL_GameControllerButtonBind
SDL_GameControllerGetBindForAxis(SDL_GameController *gc, SDL_GameControllerAxis axis)
{
    return BindFor(gc, SDL_CONTROLLER_BINDTYPE_AXIS, (int)axis);
}

extern "C" SDL_GameControllerButtonBind
SDL_GameControllerGetBindForButton(SDL_GameController *gc, SDL_GameControllerButton button)
{
    return BindFor(gc, SDL_CONTROLLER_BINDTYPE_BUTTON, (int)button);
}

extern "C" SDL_bool SDL_GameControllerHasAxis(SDL_GameController *gc,
                                              SDL_GameControllerAxis axis)
{
    return BindFor(gc, SDL_CONTROLLER_BINDTYPE_AXIS, (int)axis).bindType
                   != SDL_CONTROLLER_BINDTYPE_NONE
               ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_bool SDL_GameControllerHasButton(SDL_GameController *gc,
                                                SDL_GameControllerButton button)
{
    return BindFor(gc, SDL_CONTROLLER_BINDTYPE_BUTTON, (int)button).bindType
                   != SDL_CONTROLLER_BINDTYPE_NONE
               ? SDL_TRUE : SDL_FALSE;
}

extern "C" Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc,
                                            SDL_GameControllerAxis axis)
{
    if (!gc || gc->refcount <= 0 || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
        return 0;
    return gc->axis_value[axis].load(std::memory_order_acquire);
}

extern "C" Uint8 SDL_GameControllerGetButton(SDL_GameController *gc,
                                             SDL_GameControllerButton button)
{
    if (!gc || gc->refcount <= 0 || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return 0;
    return gc->button_state[button].load(std::memory_order_acquire);
}

extern "C" SDL_bool SDL_GameControllerHasRumble(SDL_GameController *gc)
{
    return gc ? SDL_JoystickHasRumble(gc->joystick) : SDL_FALSE;
}

extern "C" int SDL_GameControllerRumble(SDL_GameController *gc, Uint16 low,
                                        Uint16 high, Uint32 duration_ms)
{
    if (!gc)
        return SDL_SetError("controller is not open");
    return SDL_JoystickRumble(gc->joystick, low, high, duration_ms);
}

extern "C" SDL_bool SDL_GameControllerHasRumbleTriggers(SDL_GameController *gc)
{
    (void) gc;
    return SDL_FALSE;
}

extern "C" int SDL_GameControllerRumbleTriggers(SDL_GameController *gc, Uint16 left,
                                                Uint16 right, Uint32 duration_ms)
{
    (void) gc; (void) left; (void) right; (void) duration_ms;
    return SDL_SetError("trigger rumble is not supported");
}

extern "C" SDL_bool SDL_GameControllerHasLED(SDL_GameController *gc)
{
    (void) gc;
    return SDL_FALSE;
}

extern "C" int SDL_GameControllerSetLED(SDL_GameController *gc, Uint8 r, Uint8 g, Uint8 b)
{
    (void) gc; (void) r; (void) g; (void) b;
    return SDL_SetError("LED control is not supported");
}

extern "C" int SDL_GameControllerSendEffect(SDL_GameController *gc, const void *data, int size)
{
    (void) gc; (void) data; (void) size;
    return SDL_SetError("effect packets are not supported");
}

// Touchpads and motion sensors: Circle's PS4 driver reports both, but the
// shim has no sensor subsystem to publish them through, so these say no
// rather than reporting a device that never sends anything.
extern "C" int SDL_GameControllerGetNumTouchpads(SDL_GameController *gc)
{
    (void) gc;
    return 0;
}

extern "C" int SDL_GameControllerGetNumTouchpadFingers(SDL_GameController *gc, int touchpad)
{
    (void) gc; (void) touchpad;
    return 0;
}

extern "C" int SDL_GameControllerGetTouchpadFinger(SDL_GameController *gc, int touchpad,
                                                   int finger, Uint8 *state, float *x,
                                                   float *y, float *pressure)
{
    (void) gc; (void) touchpad; (void) finger;
    if (state)    *state = 0;
    if (x)        *x = 0.0f;
    if (y)        *y = 0.0f;
    if (pressure) *pressure = 0.0f;
    return SDL_SetError("controller has no touchpad");
}

extern "C" SDL_bool SDL_GameControllerHasSensor(SDL_GameController *gc, SDL_SensorType type)
{
    (void) gc; (void) type;
    return SDL_FALSE;
}

extern "C" int SDL_GameControllerSetSensorEnabled(SDL_GameController *gc,
                                                  SDL_SensorType type, SDL_bool enabled)
{
    (void) gc; (void) type; (void) enabled;
    return SDL_SetError("controller sensors are not supported");
}

extern "C" SDL_bool SDL_GameControllerIsSensorEnabled(SDL_GameController *gc, SDL_SensorType type)
{
    (void) gc; (void) type;
    return SDL_FALSE;
}

extern "C" float SDL_GameControllerGetSensorDataRate(SDL_GameController *gc, SDL_SensorType type)
{
    (void) gc; (void) type;
    return 0.0f;
}

extern "C" int SDL_GameControllerGetSensorData(SDL_GameController *gc, SDL_SensorType type,
                                               float *data, int num_values)
{
    (void) gc; (void) type; (void) data; (void) num_values;
    return SDL_SetError("controller sensors are not supported");
}

extern "C" int SDL_GameControllerGetSensorDataWithTimestamp(SDL_GameController *gc,
                                                            SDL_SensorType type,
                                                            Uint64 *timestamp,
                                                            float *data, int num_values)
{
    (void) gc; (void) type; (void) timestamp; (void) data; (void) num_values;
    return SDL_SetError("controller sensors are not supported");
}

extern "C" const char *
SDL_GameControllerGetAppleSFSymbolsNameForButton(SDL_GameController *gc,
                                                 SDL_GameControllerButton button)
{
    (void) gc; (void) button;
    return nullptr;
}

extern "C" const char *
SDL_GameControllerGetAppleSFSymbolsNameForAxis(SDL_GameController *gc,
                                               SDL_GameControllerAxis axis)
{
    (void) gc; (void) axis;
    return nullptr;
}
