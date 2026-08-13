//
// bootargs.cpp — the library's own switches, read from the boot argument
// block, without the application being involved.
//
// THE BLOCK. A boot-time argument block sits at a fixed offset inside the
// kernel image. A loader writes a plain argument string into it before
// pushing the image, so a setting can ride a boot without anything being
// rebuilt. The layout below is an INTERFACE agreed with whatever wrote it,
// verified by a magic value before a byte is believed; it is reproduced here
// byte-compatibly and must not be changed on one side alone.
//
// The application carries the block — it has to, because placing it at a
// fixed image offset is a matter for the linker script of the program being
// built — and the application splits the string into its own arguments. None
// of that is this library's business.
//
// WHAT IS THIS LIBRARY'S BUSINESS is any switch describing what the LIBRARY
// does: input, video, audio, timing, performance reporting. Those were being
// interpreted by each application in turn, which meant an application that
// had never heard of a switch silently lost the capability — the switch was
// stamped, the loader confirmed it, and nothing happened, with no error to
// explain it.
//
// So the library finds the block ITSELF, at the same fixed offset, and acts
// on its own switches. An application that knows nothing about any of this
// still gets every one of them. The application is never asked, never has to
// forward anything, and cannot fail to.
//
// READING IT TWICE IS THE POINT. The application reads the same block for
// its own arguments and strips every `--rapi-` switch before the program
// sees them; that stays the application's job, because they are its
// arguments. Reading is harmless — nothing here writes to the block.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"

#include <circle/macros.h>
#include <circle/memorymap.h>
#include <cstring>

namespace
{

// The interface. Changing any of it changes what a loader must write.
const unsigned BLOCK_OFFSET = 0x800;
const char     MAGIC[4]     = { 'P', 'M', '8', 'D' };
const unsigned BUFFER_BYTES = 512;

struct TBootArgsBlock
{
    char Magic[4];
    u16  Capacity;              // bytes available in Text
    u16  Length;                // bytes used in Text, excluding the NUL
    char Text[BUFFER_BYTES];    // NUL-terminated argument string
} PACKED;

const char From[] = "bootargs";
} // namespace

// ---------------------------------------------------------------------------
// CARRYING THE BLOCK. Placing a block at a fixed image offset is a linker's
// job, not this function's, and it used to be asked of every application in
// turn: a linker script of its own, a trampoline over the reserved space, a
// copy of the struct above to instantiate. Every one of those was the same
// eleven bytes of magic and capacity, written out again per consumer, and an
// application that forgot the linker script silently got no block at all —
// which is exactly the failure this replaces. The library places its own
// copy instead, in a section sdl-app.ld reserves at image offset 0x800
// unconditionally, so every application linked against this library carries
// a valid block whether or not it has ever heard of one.
//
// THE TRAMPOLINE. The image's first instruction must not be the block, or
// the processor would start executing magic bytes. `b _start` steps over the
// reserved region to Circle's own startup, which sdl-app.ld places right
// after the block. sdl-app.ld makes this symbol the image's entry point.
// ---------------------------------------------------------------------------
extern "C"
{

__asm__ (
    "\t.section .rapiboot.entry, \"ax\", %progbits\n"
    "\t.globl _rapiboot_entry\n"
    "_rapiboot_entry:\n"
    "\tb _start\n"
    "\t.previous\n"
);

// A second, external-linkage copy of the struct above: sdl-app.ld places
// this exact object by name, and a variable placed by name needs a name the
// linker script can bind to, which a type from an anonymous namespace
// cannot give an external symbol without a compiler warning about mismatched
// linkage. The two struct definitions must stay byte-identical; both encode
// the same interface, and a static_assert just below checks they agree on
// size.
struct TRapiBootPlacedBlock
{
    char Magic[4];
    u16  Capacity;
    u16  Length;
    char Text[512];
} PACKED;

// `used` and sdl-app.ld's KEEP() stop this being discarded for having no
// callers; its ASSERTs refuse any link that puts it anywhere but 0x800. It
// ships with the magic, the capacity, and an empty string, so an image
// nobody has stamped boots through exactly the same reader code as one that
// has been.
__attribute__ ((section (".rapiboot.defaults"), used, aligned (8)))
TRapiBootPlacedBlock _rapiboot_defaults =
{
    { 'P', 'M', '8', 'D' },    // must match MAGIC above, byte for byte
    512,                        // must match BUFFER_BYTES above
    0,                          // Length: empty
    {0}                         // Text: nothing stamped
};

} // extern "C"

namespace
{
static_assert(sizeof(TRapiBootPlacedBlock) == sizeof(TBootArgsBlock),
              "the placed block and the block this file reads back must be "
              "the same shape");

bool s_read = false;
bool s_debugUart = false;

// Digits only. Anything else is not an answer, and is refused rather than
// turned into a number nobody asked for.
bool ParseUnsigned(const char *pValue, unsigned &nOut)
{
    if (*pValue == '\0')
        return false;
    unsigned n = 0;
    for (const char *p = pValue; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        n = n * 10 + (unsigned)(*p - '0');
    }
    nOut = n;
    return true;
}

// "WxH": digits, one 'x', digits. Anything else is not an answer.
bool ParseSize(const char *pValue, unsigned &nWidth, unsigned &nHeight)
{
    const char *pX = strchr(pValue, 'x');
    if (pX == nullptr || pX == pValue)
        return false;
    char Width[16];
    size_t nLen = (size_t)(pX - pValue);
    if (nLen >= sizeof(Width))
        return false;
    memcpy(Width, pValue, nLen);
    Width[nLen] = '\0';
    return ParseUnsigned(Width, nWidth) && ParseUnsigned(pX + 1, nHeight);
}

// One switch. Returns true when it was one of this library's, so that a
// switch belonging to the application is passed over in silence — the
// application reports on its own, and two complaints about one token would
// be worse than none.
bool Dispatch(const char *pSwitch)
{
    // Takes NO value. `--rapi-debug-uart=1` is a different string and is not
    // this switch; it belongs to whoever else may claim it, and is passed
    // over here rather than being quietly accepted.
    if (strcmp(pSwitch, "--rapi-debug-uart") == 0)
    {
        s_debugUart = true;
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "--rapi-debug-uart: serial key injection armed");
        return true;
    }

    if (strncmp(pSwitch, "--rapi-perf=", 12) == 0)
    {
        unsigned nSeconds = 0;
        if (!ParseUnsigned(pSwitch + 12, nSeconds))
            return false;       // not a number: not ours to act on
        SDL2Circle_SetPerfInterval(nSeconds);
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "--rapi-perf: performance reports every %u s", nSeconds);
        return true;
    }

    // The virtual framebuffer size. It wins over a window's own size and
    // over SDL2Circle_DeclareVirtualDevice — see src/video.cpp, which holds
    // the value and the precedence between the three.
    if (strncmp(pSwitch, "--rapi-vdisplay=", 16) == 0)
    {
        unsigned nWidth = 0, nHeight = 0;
        if (!ParseSize(pSwitch + 16, nWidth, nHeight))
            return false;       // not WxH: not ours to act on
        if (nWidth == 0 || nHeight == 0)
            return false;       // not a display size
        SDL2Circle_SetVDisplaySwitch((int)nWidth, (int)nHeight);
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "--rapi-vdisplay: virtual framebuffer %ux%u",
                       nWidth, nHeight);
        return true;
    }

    return false;
}

} // namespace

// Idempotent, and called from every point that could be the first to need
// it: the application's start-up order is its own, and this must not depend
// on which of them happens first.
void SDL2Circle_ReadBootArgs(void)
{
    if (s_read)
        return;
    s_read = true;

    const TBootArgsBlock *pBlock =
        (const TBootArgsBlock *)(MEM_KERNEL_START + BLOCK_OFFSET);
    if (memcmp(pBlock->Magic, MAGIC, sizeof(MAGIC)) != 0)
        return;                 // no block in this image; nothing to read

    // Bounded by the block's own capacity, never past this build's buffer,
    // and terminated whatever the writer claimed — Length is the writer's
    // convenience, not something to trust.
    char Text[BUFFER_BYTES];
    unsigned nBound = pBlock->Capacity;
    if (nBound > BUFFER_BYTES)
        nBound = BUFFER_BYTES;
    memcpy(Text, pBlock->Text, nBound);
    Text[nBound - 1] = '\0';
    if (Text[0] == '\0')
        return;

    // Split on whitespace, honouring double quotes the same way the
    // application's own splitter does, so one string cannot mean two things.
    // Quotes only ever shorten a token, so the write position never
    // overtakes the read position and one buffer is enough.
    char *p = Text;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        char *pToken = p;
        char *pWrite = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
        {
            if (*p == '"')
            {
                p++;
                while (*p != '\0' && *p != '"')
                    *pWrite++ = *p++;
                if (*p == '"')
                    p++;
                continue;
            }
            *pWrite++ = *p++;
        }
        if (*p != '\0')
            p++;
        *pWrite = '\0';

        if (strncmp(pToken, "--rapi-", 7) == 0)
            Dispatch(pToken);
    }
}

// Whether serial key injection was asked for. This is the whole switch: the
// injection path finds its own serial device (src/input.cpp), so nothing else
// has to be arranged for a stamped boot to accept typing from the console.
bool SDL2Circle_DebugUartArmed(void)
{
    SDL2Circle_ReadBootArgs();
    return s_debugUart;
}
