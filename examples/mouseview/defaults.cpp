//
// defaults.cpp — carrying and consuming the patchable-defaults block.
//
// Three jobs, all against the shared interface in defaultsblock.h:
//
//  1. CARRY the block. The image's one copy lives in its own section, which
//     mouseview-defaults.ld pins to image offset 0x800. It ships with the
//     magic, the capacity, and an empty string, so an image nobody has
//     written to boots through exactly the same code as one that has been.
//
//  2. OPEN the image. The first four bytes of the image are a branch over
//     the reserved space and the block, to Circle's own startup. Without it
//     the processor would begin executing the block.
//
//  3. CONSUME the text. Check the magic first, split the text into
//     arguments, act on the kernel's own switches, and report the rest.
//
// The magic check is a seatbelt, and it is deliberately made at the fixed
// offset rather than through the symbol: if a future link ever moved the
// block, the magic would not be where it belongs, and the block is then
// ignored rather than trusted. A misplaced block must read as absent, never
// as whatever bytes happen to be sitting there.
//
#include "defaults.h"
#include "defaultsblock.h"

#include <SDL2/SDL_circle.h>
#include <circle/memorymap.h>
#include <cstring>

static const char From[] = "defaults";

// ---------------------------------------------------------------------------
// 2. The trampoline: the image's first four bytes.
//
// `b _start` is PC-relative and reaches far further than it needs to, so the
// entry stays one instruction wherever Circle's startup ends up.
// ---------------------------------------------------------------------------
__asm__ (
    "\t.section .mouseview.entry, \"ax\", %progbits\n"
    "\t.globl _mouseview_entry\n"
    "_mouseview_entry:\n"
    "\tb _start\n"
    "\t.previous\n"
);

// ---------------------------------------------------------------------------
// 1. The block itself. `used` and the linker script's KEEP() stop it being
// discarded for having no callers; the script's ASSERTs refuse any link that
// puts it anywhere but 0x800.
// ---------------------------------------------------------------------------
extern "C"
{

__attribute__ ((section (".mouseview.defaults"), used, aligned (8)))
TDefaultsBlock _mouseview_defaults =
{
    {DEFAULTS_MAGIC0, DEFAULTS_MAGIC1, DEFAULTS_MAGIC2, DEFAULTS_MAGIC3},
    DEFAULTS_BUFFER_BYTES,
    0,          // Length: empty
    {0}         // Text: nothing asked for
};

unsigned rapi_perf_seconds = 0;

}

// Every argument starting `--rapi-` is the kernel's.
//
// --rapi-debug-uart is accepted and reported rather than acted on: this
// example arms serial injection unconditionally in its Run(), because a
// harness whose whole purpose is to be driven from a terminal should not
// also require the terminal to have stamped it first. Saying so on the log
// is what stops that reading as the switch having been ignored.
static void DispatchKernelSwitch(const char *pSwitch)
{
    if (strcmp(pSwitch, "--rapi-debug-uart") == 0)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "--rapi-debug-uart consumed: injection is already on "
                       "in this example, so the switch changes nothing");
        return;
    }

    if (strncmp(pSwitch, "--rapi-perf=", 12) == 0)
    {
        // Seconds between performance reports. Nothing but digits is an
        // answer: anything else falls through to the unrecognised branch
        // rather than arming an interval nobody asked for.
        const char *pValue = pSwitch + 12;
        unsigned nSeconds = 0;
        bool bDigits = *pValue != '\0';
        for (const char *p = pValue; *p != '\0'; p++)
        {
            if (*p < '0' || *p > '9')
            {
                bDigits = false;
                break;
            }
            nSeconds = nSeconds * 10 + (unsigned)(*p - '0');
        }

        if (bDigits)
        {
            rapi_perf_seconds = nSeconds;
            SDL2Circle_SetPerfInterval(nSeconds);
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                           "--rapi-perf consumed: performance reports every %u s",
                           nSeconds);
            return;
        }
    }

    SDL2Circle_Log(From, SDL2CIRCLE_LOG_WARNING,
                   "unrecognised kernel switch \"%s\" ignored", pSwitch);
}

// The text is copied out before it is split, because splitting writes into
// it. The block itself stays as the writer left it, which matters when the
// bench is trying to work out what was actually stamped.
static char s_TokenText[DEFAULTS_BUFFER_BYTES];

void DefaultsApply(void)
{
    const TDefaultsBlock *pBlock =
        (const TDefaultsBlock *)(MEM_KERNEL_START + DEFAULTS_BLOCK_OFFSET);
    if (   pBlock->Magic[0] != DEFAULTS_MAGIC0
        || pBlock->Magic[1] != DEFAULTS_MAGIC1
        || pBlock->Magic[2] != DEFAULTS_MAGIC2
        || pBlock->Magic[3] != DEFAULTS_MAGIC3)
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_WARNING,
                       "no block magic at 0x%lX — nothing to apply",
                       (unsigned long)(MEM_KERNEL_START + DEFAULTS_BLOCK_OFFSET));
        return;
    }

    // Bounded by the block's own capacity, never past this build's buffer,
    // and terminated whatever the writer claimed: Length is the writer's
    // convenience, not something to trust.
    unsigned nBound = pBlock->Capacity;
    if (nBound > DEFAULTS_BUFFER_BYTES)
        nBound = DEFAULTS_BUFFER_BYTES;
    memcpy(s_TokenText, pBlock->Text, nBound);
    s_TokenText[nBound - 1] = '\0';

    if (s_TokenText[0] == '\0')
    {
        SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                       "defaults block present and empty");
        return;
    }

    SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE, "stamped: \"%s\"", s_TokenText);

    // Split on whitespace, in place. Double quotes group whitespace into one
    // argument and are removed from it. Quotes only ever shorten an argument,
    // so the write position never overtakes the read position and one buffer
    // is enough.
    char *p = s_TokenText;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        char *pToken = p;       // where the argument starts
        char *pWrite = p;       // where the next kept character goes
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
            p++;                // step past the separator
        *pWrite = '\0';

        if (strncmp(pToken, "--rapi-", 7) == 0)
            DispatchKernelSwitch(pToken);
        else
            SDL2Circle_Log(From, SDL2CIRCLE_LOG_NOTICE,
                           "\"%s\" is not a kernel switch and this example has "
                           "no program to pass it to — ignored", pToken);
    }
}
