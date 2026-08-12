//
// stdio.cpp — the C library's standard output and standard error, on this
// board's one output router.
//
// WHY THIS EXISTS. Where output goes is a property of the machine: the serial
// port always, and the screen as well until an application takes the display.
// That rule already governs the log (src/log.cpp, src/console.cpp). Ordinary
// C output had nothing to do with it — a printf in a program built on this
// library reached a descriptor nobody had bound, so newlib's _write answered
// EBADF and the bytes were gone. Binding those descriptors here puts every
// program's ordinary output under the same rule as everything else on the
// board, whatever language it is written in.
//
// WHAT ARRIVES ON THE WIRE IS WHAT THE PROGRAM WROTE. The bytes go through
// SDL2Circle_WriteBytes, which adds no source, no severity, no timestamp and
// no line discipline. A program printing a number gets that number.
//
// NOTHING VENDORED IS TOUCHED. circle-newlib binds its three standard
// descriptors through CGlueStdioInit, which takes a CConsole and asks it to
// write; CConsole has a constructor that takes the devices to use instead of
// the screen and the USB keyboard. So a device of this library's own, handed
// to that constructor, is the whole of the change: the C library keeps its own
// glue and its own file table, and only the far end of the write moves.
//
// CIRCLE'S OWN CConsole::Initialize IS NOT CALLED, and that is deliberate.
// With both devices given to the constructor its remaining work is input —
// building a line discipline and registering a device named "console" — and
// this console has no input to give. Write needs neither: it goes straight to
// the output device it was constructed with (lib/input/console.cpp,
// CConsole::Write).
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/device.h>
#include <circle/input/console.h>
#include <circle_glue.h>

#include <sys/stat.h>

namespace
{

// The router, as a Circle device, because that is the shape CConsole takes.
// It holds no state of its own: every question about where the bytes go is
// answered on the far side of this call, at the moment they are written.
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

// CConsole's two-device constructor requires both devices, so the same object
// stands in for input as well. Nothing on this board types at it.
CConsole s_console(&s_raw, &s_raw);

bool s_done = false;

}   // namespace

void SDL2Circle_StdioInit(void)
{
    if (s_done)
        return;
    s_done = true;

    // ALL THREE DESCRIPTORS MUST BE FREE BEFORE ASKING. CGlueStdioInit binds
    // 0, 1 and 2 together and asserts that none of them is already open
    // (libgloss/circle/io.cpp, CGlueInitConsole), so calling it over a host
    // kernel's own console — or over a file that has taken a low slot —
    // stops the board inside an assertion rather than failing. fstat answers
    // the question for a descriptor without touching it and without blocking.
    //
    // A kernel that has bound its own console has said where its C output
    // goes, and that decision is left standing.
    struct stat st;
    if (fstat(0, &st) == 0 || fstat(1, &st) == 0 || fstat(2, &st) == 0)
    {
        SDL2Circle_Log("sdl2stdio", SDL2CIRCLE_LOG_NOTICE,
                       "the C standard descriptors are bound already; leaving "
                       "them as they are");
        return;
    }

    CGlueStdioInit(s_console);

    // STANDARD INPUT IS BOUND AND LEFT BOUND, though nothing will ever feed
    // it. Closing it again would be worse than useless: the C library hands
    // out the LOWEST free descriptor (libgloss/circle/filetable.h,
    // FindFreeFileSlot starts at 0), so a freed descriptor 0 goes to the first
    // file a program opens — and a runtime that reads descriptor 0 as the
    // console would then send that file's every write to the console instead
    // of to the card, with nothing saying so. Holding all three is what keeps
    // an ordinary file away from them.
    //
    // A C program that reads standard input waits, because no character ever
    // arrives. That is what circle-newlib's console reader does with a console
    // nobody is typing at (libgloss/circle/io.cpp, CGlueConsole::Read calls
    // until a character comes, yielding in between), and this board has no
    // console input to give it.
    SDL2Circle_Log("sdl2stdio", SDL2CIRCLE_LOG_NOTICE,
                   "C standard output and standard error write to the console "
                   "unlabelled; standard input is bound but never produces a "
                   "character");
}
