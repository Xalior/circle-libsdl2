//
// stdio.cpp — the C library's standard descriptors, on this board's one
// console: a screen and a keyboard, the way a console is on any machine.
//
// WHY THIS EXISTS. Where output goes is a property of the machine: the
// logging destination, whatever it is at the time (src/log.cpp,
// src/console.cpp) — the serial port always, and the screen as well until an
// application takes the display. Ordinary C output had nothing to do with it
// — a printf in a program built on this library reached a descriptor nobody
// had bound, so newlib's _write answered EBADF and the bytes were gone.
// Standard input had nothing to give at all: nothing fed it, so a program
// that read it waited forever. Binding all three descriptors here, to a real
// console, puts every program's ordinary output AND input under the same
// rules as everything else on the board, whatever language it is written in.
//
// OUTPUT ARRIVES ON THE WIRE AS THE PROGRAM WROTE IT. The bytes go through
// SDL2Circle_WriteBytes, which is the same destination the log uses and adds
// no source, no severity, no timestamp and no line discipline of its own. A
// program printing a number gets that number.
//
// INPUT IS THE USB KEYBOARD, COOKED. CConsole's plug-and-play constructor
// finds it by name ("ukbd1") the moment src/input.cpp's pump has enumerated
// one, and turns its key-press strings into the characters a console read
// expects — backspace edits the line, a line ends at Enter, exactly what a
// program reading standard input assumes on any other target. Until a
// keyboard is found, a read reports no data yet, which is CConsole's own
// contract (lib/input/console.cpp, CConsole::Read) and never an error.
//
// NOTHING VENDORED IS TOUCHED. circle-newlib binds its three standard
// descriptors through CGlueStdioInit, which takes a CConsole and asks it to
// read and write; nothing here is a change to that glue, only to which
// CConsole it is given. This library's own CConsole registers under the
// device name "tty1" the way a screen device normally would, so CConsole's
// own plug-and-play machinery — built and tested for exactly this — finds it
// without this library reimplementing any of it.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/device.h>
#include <circle/devicenameservice.h>
#include <circle/input/console.h>
#include <circle_glue.h>

#include <new>

namespace
{

// The router, as a Circle device, because that is the shape CConsole takes
// for its output half. It holds no state of its own: every question about
// where the bytes go is answered on the far side of this call, at the moment
// they are written.
class CRawOutputDevice : public CDevice
{
public:
    int Write(const void *pBuffer, size_t nCount) override
    {
        SDL2Circle_WriteBytes((const char *)pBuffer, (unsigned)nCount);

        // The router does not report how much it took, and it cannot refuse:
        // what it has no room for is dropped and counted, and the drain says
        // so on the console. Reporting a short write instead would turn a
        // dropped diagnostic into an I/O error in the program that printed it.
        return (int)nCount;
    }
};

CRawOutputDevice s_raw;

// THE CONSOLE. Placement-new'd rather than a plain static object: building it
// registers "tty1" and "console" with CDeviceNameService, which needs the
// heap and the rest of the board up first, and a static object at this scope
// would construct before any of that exists.
alignas(CConsole) u8 s_ConsoleStore[sizeof(CConsole)];
CConsole *s_pConsole = nullptr;

bool s_done = false;

}   // namespace

void SDL2Circle_StdioInit(void)
{
    if (s_done)
        return;
    s_done = true;

    // THE OUTPUT HALF, UNDER THE NAME CConsole'S OWN PLUG-AND-PLAY LOOKS FOR.
    // CConsole(nullptr, TRUE) below resolves its screen half by asking
    // CDeviceNameService for "tty1" — the name Circle's own screen device
    // would register under, which this board has none of. Registering the
    // raw router under that name is the one substitution this library makes;
    // everything downstream of it is CConsole's own, unmodified.
    CDeviceNameService::Get()->AddDevice("tty1", &s_raw, FALSE);

    // THE CONSOLE ITSELF. No alternate device: a console with nothing to
    // fall back to and no keyboard found yet answers every read with no data,
    // which is CConsole::Read's own contract, and needs no fallback device to
    // do it. Plug-and-play so it finds "ukbd1" the moment src/input.cpp's
    // pump has enumerated one, exactly as a bare Circle kernel would.
    s_pConsole = new (s_ConsoleStore) CConsole(nullptr, TRUE);
    s_pConsole->Initialize();

    CGlueStdioInit(*s_pConsole);

    SDL2Circle_Log("sdl2stdio", SDL2CIRCLE_LOG_NOTICE,
                   "standard input, output and error are the console: "
                   "keyboard in, whatever the logging destination is out");
}

// Called every input pump, which already walks the device name service for
// "ukbd1" itself (src/input.cpp) — so this is one more idempotent lookup on
// a pass that already makes the expensive one, and a no-op on every pass
// after the console has found its keyboard.
void SDL2Circle_ConsolePumpPlugAndPlay(void)
{
    if (s_pConsole)
        s_pConsole->UpdatePlugAndPlay();
}
