//
// console.cpp — THE output device, and the screen half of it drawn for the
// mode the firmware granted.
//
// ONE DEVICE, MADE ONCE, AND NOTHING IS EVER ATTACHED OR DETACHED. The tee
// below holds the serial device, the screen, and one flag saying whether the
// screen is still ours. Its write puts the bytes on serial always, and draws
// them unless an application has taken the display. Circle's logger is pointed
// at it once and never pointed anywhere else again.
//
// That is how a plain Circle kernel already does it: declare a tee over the
// screen and the serial device, hand it to the logger, and every line reaches
// both places for the rest of the run. This board adds the one thing such a
// kernel never faces — the screen going away when an application takes the
// display — and that one thing is the flag. The display hand-off is not an
// event that rearranges the plumbing; it is a boolean.
//
// WHY THAT SHAPE AND NOT A DESTINATION THAT COMES AND GOES. There is only ever
// one destination object, and what it will do is settled before anything runs.
// So "the console and the game writing the same framebuffer" has no mechanism
// at all, rather than being prevented by a rule somebody has to keep.
//
// THE SCREEN HALF IS THIS LIBRARY'S TO DRAW, because this library owns the
// display. It allocates the framebuffer, it reads the firmware's answers back,
// and it is the only place on the board that knows what the picture really is.
// A console built anywhere else is a second opinion about the same hardware,
// and on a board whose firmware ignores the depth that was asked for, it is
// the wrong one.
//
// WHY CIRCLE'S OWN SCREEN DEVICE CANNOT DO THIS JOB. Its colour depth is a
// compile-time macro, and nothing ever corrects it: CBcmFrameBuffer::Initialize
// reads the granted PITCH back out of the mailbox reply and keeps it, but never
// the granted DEPTH, so GetDepth() goes on echoing its constructor's argument
// for the object's whole life. Circle's terminal sizes every row of pixels from
// that number. Where the firmware hands out a surface at a depth nobody asked
// for — a Pi 5 grants 32 bits per pixel whatever the request was — each glyph
// is then drawn at the wrong stride into the right buffer, and the console
// paints a fraction of each scanline in characters squeezed by the same ratio.
// The fault is structural, not a setting: there is no depth to configure that
// makes a stale number current.
//
// SO EVERY NUMBER HERE IS READ BACK. The pitch and the buffer address are the
// firmware's own reply; the width and height are the firmware's report of the
// display it is scanning; and the bytes per pixel are the pitch divided by that
// width — a granted quantity over a granted quantity, with nothing assumed
// between them. There is no board test anywhere in this file, and there is
// nothing to configure.
//
// THE TEXT IS WHITE AND THE GROUND IS BLACK, and that is a decision about
// evidence rather than taste. Any other colour needs the channel order the
// firmware chose, which is a further question this library does not ask; all
// bits set is white in every pixel format a Pi grants, and all bits clear is
// black in every one of them. So the console is right on a board this library
// has never seen, which is the whole point of it.
//
// WHEN THE SCREEN GOES. The flag is cleared the moment an application
// initialises SDL video, because that is the moment the guest takes the
// display. It is the only transition there is, it happens once, and it cannot
// be undone.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <circle/chargenerator.h>
#include <circle/device.h>
#include <circle/logger.h>
#include <circle/spinlock.h>

#include <string.h>

namespace
{

// The granted surface, and the character cell laid over it. Settled once at
// attach and never changed: the grant is made once and kept for the life of
// the program, so nothing here can drift.
u8      *s_base   = nullptr;
unsigned s_pitch  = 0;          // bytes per row, as granted
unsigned s_bpp    = 0;          // bytes per pixel, granted pitch / granted width
unsigned s_width  = 0;          // pixels across, as the firmware reports them
unsigned s_height = 0;          // pixel rows this console may use

unsigned s_cols = 0, s_rows = 0;        // the text area, in characters
unsigned s_cellw = 0, s_cellh = 0;      // one character cell, in pixels
unsigned s_col = 0, s_row = 0;          // where the next character goes

// The font, and the character cell it implies. Circle's character generator
// is the one half of its console that is worth reusing: it turns a character
// into a bitmap and knows nothing about the screen, the depth or the pitch, so
// none of what is wrong next door reaches it. Constructing it is arithmetic
// over a constant table, which is why it can sit here and be ready before the
// board has brought anything up.
CCharGenerator s_font;

// THE FLAG. True while the screen is a place this library may draw: set once,
// when the tee is built, if the machine has a display and has not asked to be
// left off it — and cleared once, when an application takes the display. There
// is no other transition and no way back.
bool s_screenLive = false;

// The serial device the host kernel gave Circle's logger. The tee holds it and
// writes it on every line for the whole run; it is never replaced.
CDevice *s_serial = nullptr;

// Whether the tee has been built. The build is idempotent because it can be
// asked for at either of two moments — a host kernel wanting the screen during
// its own bring-up, or the arming call every kernel makes — and whichever
// comes first is the one that does it.
bool s_started = false;

// The logger writes to its target without a lock of its own, and a log line
// can be produced by any core when the core split is not carrying it. One
// character at a time is not the granularity that matters; a scroll is.
CSpinLock s_lock;

// One pixel, at whatever depth the firmware granted. All bits set is white in
// every format a Pi hands out, and all bits clear is black in every one.
inline void put_pixel(u8 *p, bool on)
{
    if (s_bpp == 4)
        *(u32 *)p = on ? 0xFFFFFFFFu : 0u;
    else
        *(u16 *)p = on ? 0xFFFFu : 0u;
}

// Clear the text area. The whole of what this console may use, not just the
// rows it will fill, so whatever the firmware left on the screen goes with it.
void clear_screen(void)
{
    for (unsigned y = 0; y < s_height; y++)
        memset(s_base + (size_t)y * s_pitch, 0, (size_t)s_width * s_bpp);
}

void draw_char(unsigned col, unsigned row, char c)
{
    const unsigned x0 = col * s_cellw;
    const unsigned y0 = row * s_cellh;

    for (unsigned dy = 0; dy < s_cellh; dy++)
    {
        u8 *p = s_base + (size_t)(y0 + dy) * s_pitch + (size_t)x0 * s_bpp;
        for (unsigned dx = 0; dx < s_cellw; dx++)
        {
            put_pixel(p, s_font.GetPixel(c, dx, dy) != FALSE);
            p += s_bpp;
        }
    }
}

// Move the text up one cell and blank the row that opens at the bottom.
//
// Row by row rather than one move of the block: the source and the
// destination overlap, and the framebuffer is uncached memory where a single
// copy would have to be trusted to run forwards. A row at a time cannot
// overlap itself.
void scroll(void)
{
    const size_t row_bytes = (size_t)s_width * s_bpp;
    const unsigned text_h = s_rows * s_cellh;

    for (unsigned y = 0; y + s_cellh < text_h; y++)
        memcpy(s_base + (size_t)y * s_pitch,
               s_base + (size_t)(y + s_cellh) * s_pitch, row_bytes);

    for (unsigned y = text_h - s_cellh; y < text_h; y++)
        memset(s_base + (size_t)y * s_pitch, 0, row_bytes);
}

void newline(void)
{
    s_col = 0;
    if (++s_row >= s_rows)
    {
        scroll();
        s_row = s_rows - 1;
    }
}

// Where a control sequence has got to. Circle's logger brackets a line in ANSI
// colour codes — always around a panic, and around every severity where the
// world is configured for colours — so a console that drew the bytes it was
// given would draw those as text. They are recognised in order to be thrown
// away: the picture carries no colour, so there is nothing in them to honour.
enum TEscapeState { EscapeNone, EscapeSeen, EscapeBracket };
TEscapeState s_escape = EscapeNone;

void put_char(char c)
{
    switch (s_escape)
    {
    case EscapeSeen:
        s_escape = (c == '[') ? EscapeBracket : EscapeNone;
        return;

    case EscapeBracket:
        // A control sequence ends at its final byte, which is the first one
        // in the range 0x40..0x7E. Everything before it is parameters.
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E)
            s_escape = EscapeNone;
        return;

    default:
        break;
    }

    switch (c)
    {
    case '\x1b':
        s_escape = EscapeSeen;
        return;

    case '\n':
        newline();
        return;

    case '\r':
        s_col = 0;
        return;

    case '\t':
    {
        // To the next eighth column, in spaces, so that the cells passed over
        // are cleared rather than left holding whatever the previous line put
        // there. Never past the end of the row, which is what keeps this from
        // wrapping in the middle of a tab stop.
        unsigned stop = (s_col + 8) & ~7u;
        if (stop > s_cols)
            stop = s_cols;
        while (s_col < stop)
            put_char(' ');
        return;
    }

    case '\b':
        if (s_col > 0)
            s_col--;
        return;

    default:
        break;
    }

    // Everything else that is not printable is dropped rather than drawn: a
    // glyph for a control code is noise in the middle of the one record of
    // what the board did.
    if ((unsigned char)c < 0x20)
        return;

    if (s_col >= s_cols)
        newline();

    draw_char(s_col, s_row, c);
    s_col++;
}

// THE ONE OUTPUT DEVICE.
//
// Circle's logger writes to a single device, so reaching two places is a
// device that reaches two. The serial port goes first, because it is the
// destination a bench run is settled by and it must not wait on the drawing;
// the screen follows, and only while it is still ours.
class CLogTee : public CDevice
{
public:
    int Write(const void *pBuffer, size_t nCount) override
    {
        const int nResult = s_serial != nullptr
                                ? s_serial->Write(pBuffer, nCount)
                                : (int)nCount;

        if (s_screenLive)
        {
            s_lock.Acquire();
            const char *p = (const char *)pBuffer;
            for (size_t i = 0; i < nCount; i++)
                put_char(p[i]);
            s_lock.Release();
        }

        return nResult;
    }
};

CLogTee s_tee;

// Read the display back and lay a character cell over it. Everything here is
// the firmware's own answer; nothing echoes a request. Returns 0 when the
// screen can be drawn on, -1 with SDL_GetError when it cannot.
int ScreenPrepare(void)
{
    // The framebuffer, and the numbers the firmware granted for it. This is
    // the same one an application's window adopts later — there is one grant
    // on this board and everything that draws shares it.
    SDL2CircleScanout fb;
    if (!SDL2Circle_ScanoutAcquire(&fb))
        return SDL_SetError("there is no display to draw on");

    if (fb.base == nullptr || fb.pitch == 0 || fb.width <= 0 || fb.height <= 0)
        return SDL_SetError("the display grant is not usable "
                            "(pitch %u, %u bytes, %dx%d)",
                            fb.pitch, fb.bytes, fb.width, fb.height);

    // BYTES PER PIXEL, DERIVED FROM TWO THINGS THE FIRMWARE SAID. The pitch is
    // the reply to the allocation; the width is the firmware's report of the
    // display it is scanning. Their ratio is what a row of pixels really costs,
    // whatever depth was asked for and whatever Circle still believes it got.
    const unsigned bpp = fb.pitch / (unsigned)fb.width;
    if (bpp != 2 && bpp != 4)
        return SDL_SetError("%u bytes per pixel (pitch %u over %d pixels) is "
                            "not a format this console can draw",
                            bpp, fb.pitch, fb.width);

    // Rows this console may write. The grant is the ceiling and not the mode:
    // a firmware that hands out one screen where two were asked for (a Pi 5
    // does) leaves fewer rows than the display has, and writing the display's
    // count would write past the allocation.
    unsigned rows = fb.bytes / fb.pitch;
    if (rows > (unsigned)fb.height)
        rows = (unsigned)fb.height;
    if (rows == 0)
        return SDL_SetError("the display grant holds no whole row "
                            "(%u bytes, pitch %u)", fb.bytes, fb.pitch);

    s_base   = fb.base;
    s_pitch  = fb.pitch;
    s_bpp    = bpp;
    s_width  = (unsigned)fb.width;
    s_height = rows;

    s_cellw = s_font.GetCharWidth();
    s_cellh = s_font.GetCharHeight();
    if (s_cellw == 0 || s_cellh == 0)
        return SDL_SetError("the console font has no character cell");

    s_cols = s_width / s_cellw;
    s_rows = s_height / s_cellh;
    if (s_cols == 0 || s_rows == 0)
        return SDL_SetError("%ux%u pixels holds no character cell of %ux%u",
                            s_width, s_height, s_cellw, s_cellh);

    s_col = s_row = 0;
    s_escape = EscapeNone;
    clear_screen();
    return 0;
}

}   // namespace

// ---------------------------------------------------------------------------
// Building the tee (once, whichever moment comes first)
// ---------------------------------------------------------------------------

int SDL2Circle_ConsoleInit(void)
{
    if (s_started)
        return 0;

    // A LOGGER WITH A DESTINATION ALREADY, and the check is on the destination
    // rather than on the logger. Circle's CLogger::Get() never returns
    // nothing — it makes a silent stand-in where no logger exists — so the
    // object is not the question. A logger with no destination has not been
    // initialised yet, and a tee built over nothing would hold nothing to
    // write the serial half to.
    CLogger *pLogger = CLogger::Get();
    if (pLogger->GetTarget() == nullptr)
        return SDL_SetError("SDL2Circle_ConsoleInit: the logger has no "
                            "destination yet — initialise it on the serial "
                            "device first");

    s_started = true;

    // WHAT THE TEE WILL DO IS SETTLED HERE AND NOWHERE ELSE. The serial half
    // is whatever device the host kernel gave the logger, kept for the whole
    // run. The screen half is live if this board has a display it can draw on.
    //
    // THERE IS NOTHING TO CONFIGURE, and that is the rule rather than a
    // default. Output goes to the serial port and to the screen until an
    // application takes the display: a switch turning half of it off would
    // make a machine that prints to one place indistinguishable from a machine
    // whose second destination has quietly failed.
    s_serial = pLogger->GetTarget();
    if (ScreenPrepare() == 0)
        s_screenLive = true;

    // The one pointing. The logger is never pointed anywhere else again — not
    // when the application takes the display, not ever — so there is no moment
    // at which output has no destination and no second object to keep in step.
    pLogger->SetNewTarget(&s_tee);

    if (s_screenLive)
        // Said on both destinations, because it is now on both. Every number
        // in it was read back from the firmware.
        SDL2Circle_Log("sdl2console", SDL2CIRCLE_LOG_NOTICE,
                       "screen log: %ux%u pixels, %u bytes per pixel "
                       "(pitch %u), %ux%u characters of %ux%u",
                       s_width, s_height, s_bpp, s_pitch,
                       s_cols, s_rows, s_cellw, s_cellh);
    else
        // A board with no display is not a fault, so this is a notice: it is a
        // machine with one destination instead of two, and a headless board
        // would otherwise report an error on every boot for working exactly as
        // it is wired.
        SDL2Circle_Log("sdl2console", SDL2CIRCLE_LOG_NOTICE,
                       "no screen log: %s", SDL_GetError());

    return 0;
}

// THE DEVICE, FOR WHOEVER ELSE NEEDS THE SAME ONE. Debug UART key injection
// (src/input.cpp) types serial-RX bytes into the machine, and the device it
// must read from is this one — the console's own, already found and already
// held — not a device looked up again by name and hoped to be the same
// object. Null before SDL2Circle_ConsoleInit has run.
CDevice *SDL2Circle_ConsoleDevice(void)
{
    return s_serial;
}

extern "C" int SDL2Circle_LogAttachScreen(void)
{
    // NOTHING HAS TO CALL THIS. The library builds the tee itself while the
    // machine comes up, so where output goes is settled for every board
    // whether or not a kernel has heard of this.
    //
    // What it is still for is HAVING IT SOONER: a host kernel with bring-up of
    // its own worth watching on the glass — mounting a card, say — makes this
    // call and gets the same one tee, built at its moment instead of at the
    // arming call. There is no second mechanism behind it; this and the arming
    // call are two doors into the same idempotent build.
    return SDL2Circle_ConsoleInit();
}

// ---------------------------------------------------------------------------
// The display hand-off (SDL_Init, when an application starts video)
// ---------------------------------------------------------------------------

void SDL2Circle_ConsoleReleaseScreen(void)
{
    if (!s_screenLive)
        return;

    // Under the same lock a line is drawn under, so a line already part way
    // onto the screen finishes rather than stopping mid-word. Nothing else
    // moves: the logger's target is the tee before this and the tee after it,
    // and the serial half is untouched.
    s_lock.Acquire();
    s_screenLive = false;
    s_lock.Release();

    SDL2Circle_Log("sdl2console", SDL2CIRCLE_LOG_NOTICE,
                   "the application has the display; output is on the serial "
                   "port alone from here");
}
