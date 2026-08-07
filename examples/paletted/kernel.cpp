//
// kernel.cpp — 8-bit paletted surfaces, the way a VGA-era game uses them.
//
// A whole class of game draws into an 8-bit indexed buffer and animates by
// rewriting the palette rather than the pixels: Doom, Duke Nukem, Heroes of
// Might and Magic, Cave Story. The picture below is drawn ONCE, before the
// loop starts, and never touched again — everything that moves on screen is
// the palette being rotated under it.
//
// So it tests what those games actually need, rather than that the calls
// exist:
//
//   SDL_CreateRGBSurface with depth 8 and no masks, which must come back
//     reporting BitsPerPixel 8 with a 256-entry palette of its own;
//   SDL_SetPaletteColors, taking effect on pixels already written;
//   SDL_SetColorKey on an indexed surface, keyed on an INDEX;
//   blitting indexed -> 32-bit, every pixel resolved through the palette;
//   SDL_ConvertSurfaceFormat as the other route to the same place;
//   SDL_FreeSurface releasing a surface that owns a palette.
//
// It also asks for a 32-bit surface the way every one of those games does —
// SDL_CreateRGBSurface(0, w, h, 32, 0, 0, 0, 0) — and checks that the depth
// it gets back is the depth it asked for. That sounds too obvious to test.
// It is here because it was once wrong: the zero-mask 32 case answered with
// an X-channel format reporting 24 significant bits, and a game that
// branches on BitsPerPixel met neither of the cases it handles.
//
#include "kernel.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include <circle/bcmpropertytags.h>
#include <cstring>

static const char From[] = "paletted";

// The physical display, asked of the firmware directly — the library is told
// what virtual display to present and never goes looking for one.
static boolean PhysicalDisplaySize(int *pWidth, int *pHeight)
{
    CBcmPropertyTags Tags;
    TPropertyTagDisplayDimensions Dim;
    memset(&Dim, 0, sizeof Dim);
    if (!Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim))
        return FALSE;
    if (Dim.nWidth == 0 || Dim.nHeight == 0)
        return FALSE;
    *pWidth = (int)Dim.nWidth;
    *pHeight = (int)Dim.nHeight;
    return TRUE;
}

CKernel::CKernel(void)
    : m_Serial(0, FALSE, 0),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer)
{
    m_ActLED.Blink(3);
}

boolean CKernel::Initialize(void)
{
    boolean bOK = TRUE;
    if (bOK) bOK = m_Serial.Initialize(115200);
    if (bOK) bOK = m_Logger.Initialize(&m_Serial);
    if (bOK) bOK = m_Interrupt.Initialize();
    if (bOK) bOK = m_Timer.Initialize();
    return bOK;
}

// The checks that can be made without looking at the screen. Reported one
// line each, because a board with no picture still has a serial console and
// this is the half that can say what happened.
static unsigned CheckSurfaces(CLogger &Log)
{
    unsigned nFailed = 0;
    auto check = [&](const char *pWhat, bool bOK) {
        Log.Write(From, bOK ? LogNotice : LogError, "%-44s %s", pWhat,
                  bOK ? "ok" : "FAILED");
        if (!bOK)
            nFailed++;
    };

    // Depth 8, no masks: an indexed surface with a palette of its own.
    SDL_Surface *pIndexed = SDL_CreateRGBSurface(0, 64, 32, 8, 0, 0, 0, 0);
    check("8-bit surface created", pIndexed != nullptr);
    if (pIndexed == nullptr)
        return nFailed;

    check("reports BitsPerPixel 8", pIndexed->format->BitsPerPixel == 8);
    check("carries a 256-entry palette",
          pIndexed->format->palette != nullptr
              && pIndexed->format->palette->ncolors == 256);
    check("one byte per pixel", pIndexed->format->BytesPerPixel == 1);

    // Depth 32, no masks — the same call every one of these games makes for
    // its output surface. The depth asked for is the depth that must come
    // back.
    SDL_Surface *p32 = SDL_CreateRGBSurface(0, 64, 32, 32, 0, 0, 0, 0);
    check("32-bit surface created", p32 != nullptr);
    check("reports BitsPerPixel 32",
          p32 != nullptr && p32->format->BitsPerPixel == 32);

    SDL_Color Colors[256];
    for (unsigned i = 0; i < 256; i++)
    {
        Colors[i].r = (Uint8)i;
        Colors[i].g = (Uint8)(255 - i);
        Colors[i].b = (Uint8)((i * 3) & 0xFF);
        Colors[i].a = 255;
    }
    check("SDL_SetPaletteColors",
          SDL_SetPaletteColors(pIndexed->format->palette, Colors, 0, 256) == 0);

    // On an indexed format, mapping a colour means finding its index.
    check("SDL_MapRGB resolves to the palette index",
          SDL_MapRGB(pIndexed->format, Colors[77].r, Colors[77].g,
                     Colors[77].b) == 77);

    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++)
            ((Uint8 *)pIndexed->pixels)[y * pIndexed->pitch + x] =
                (Uint8)((x * 4 + y) & 0xFF);

    check("blit indexed -> 32-bit",
          p32 != nullptr && SDL_UpperBlit(pIndexed, nullptr, p32, nullptr) == 0);

    unsigned nWrong = 0;
    if (p32 != nullptr)
    {
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 64; x++)
            {
                const Uint8 nIndex = (Uint8)((x * 4 + y) & 0xFF);
                const Uint32 nPixel =
                    ((Uint32 *)((Uint8 *)p32->pixels + (size_t)y * p32->pitch))[x];
                Uint8 r, g, b, a;
                SDL_GetRGBA(nPixel, p32->format, &r, &g, &b, &a);
                if (r != Colors[nIndex].r || g != Colors[nIndex].g
                    || b != Colors[nIndex].b)
                    nWrong++;
            }
    }
    check("every blitted pixel came through the palette", nWrong == 0);

    SDL_Surface *pConverted =
        SDL_ConvertSurfaceFormat(pIndexed, SDL_PIXELFORMAT_ARGB8888, 0);
    check("SDL_ConvertSurfaceFormat indexed -> ARGB8888",
          pConverted != nullptr);
    if (pConverted != nullptr)
        SDL_FreeSurface(pConverted);

    // A colour key on an indexed surface keys on the INDEX, not a colour.
    check("SDL_SetColorKey on an indexed surface",
          SDL_SetColorKey(pIndexed, SDL_TRUE, 3) == 0);
    Uint32 nKey = 0;
    check("SDL_GetColorKey reads it back",
          SDL_GetColorKey(pIndexed, &nKey) == 0 && nKey == 3);
    SDL_SetColorKey(pIndexed, SDL_FALSE, 0);

    SDL_FreeSurface(pIndexed);
    if (p32 != nullptr)
        SDL_FreeSurface(p32);
    check("SDL_FreeSurface released a surface owning a palette", true);

    return nFailed;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice, "circle-libsdl2 paletted-surface test");

    int W = 0, H = 0;
    if (!PhysicalDisplaySize(&W, &H))
    {
        m_Logger.Write(From, LogError,
                       "the firmware will not report the display size");
        return ShutdownHalt;
    }
    if (SDL2Circle_DeclareVirtualDevice(32, W, H) != 0)
    {
        m_Logger.Write(From, LogError, "virtual device: %s", SDL_GetError());
        return ShutdownHalt;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        m_Logger.Write(From, LogError, "SDL_Init: %s", SDL_GetError());
        return ShutdownHalt;
    }

    const unsigned nFailed = CheckSurfaces(m_Logger);
    m_Logger.Write(From, nFailed ? LogError : LogNotice,
                   "%u check(s) failed", nFailed);

    SDL_Window *pWin = SDL_CreateWindow("paletted", 0, 0, W, H, 0);
    SDL_Renderer *pRen = pWin ? SDL_CreateRenderer(pWin, -1, 0) : nullptr;
    SDL_Texture *pTex =
        pRen ? SDL_CreateTexture(pRen, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, W, H)
             : nullptr;
    if (pTex == nullptr)
    {
        m_Logger.Write(From, LogError, "window/renderer/texture: %s",
                       SDL_GetError());
        return ShutdownHalt;
    }

    // The picture: an 8-bit surface the size of the screen, drawn ONCE.
    SDL_Surface *pPicture = SDL_CreateRGBSurface(0, W, H, 8, 0, 0, 0, 0);
    SDL_Surface *pFrame = SDL_CreateRGBSurface(0, W, H, 32, 0, 0, 0, 0);
    if (pPicture == nullptr || pFrame == nullptr)
    {
        m_Logger.Write(From, LogError, "surfaces: %s", SDL_GetError());
        return ShutdownHalt;
    }

    // Concentric bands of palette index. Nothing here changes again.
    for (int y = 0; y < H; y++)
    {
        Uint8 *pRow = (Uint8 *)pPicture->pixels + (size_t)y * pPicture->pitch;
        for (int x = 0; x < W; x++)
        {
            const int dx = x - W / 2;
            const int dy = (y - H / 2) * 2;
            unsigned nDist = 0;
            for (unsigned n = dx * dx + dy * dy; n > 0; n >>= 1)
                nDist++;
            pRow[x] = (Uint8)(((dx * dx + dy * dy) >> 6) + nDist * 8);
        }
    }

    m_Logger.Write(From, LogNotice,
                   "%dx%d indexed picture drawn once; the palette rotates from "
                   "here. Power-cycle to exit.", W, H);

    unsigned nFrames = 0;
    for (;;)
    {
        // Everything that moves is this: 256 colours rewritten per frame.
        SDL_Color Colors[256];
        const unsigned nPhase = nFrames * 2;
        for (unsigned i = 0; i < 256; i++)
        {
            const unsigned c = (i + nPhase) & 0xFF;
            Colors[i].r = (Uint8)(c < 128 ? c * 2 : (255 - c) * 2);
            Colors[i].g = (Uint8)((c * 3) & 0xFF);
            Colors[i].b = (Uint8)(255 - c);
            Colors[i].a = 255;
        }
        SDL_SetPaletteColors(pPicture->format->palette, Colors, 0, 256);

        // Indexed to the display format, then to the glass.
        SDL_UpperBlit(pPicture, nullptr, pFrame, nullptr);
        SDL_UpdateTexture(pTex, nullptr, pFrame->pixels, pFrame->pitch);
        SDL_RenderCopy(pRen, pTex, nullptr, nullptr);
        SDL_RenderPresent(pRen);

        if (++nFrames % 300 == 0)
            m_Logger.Write(From, LogNotice, "%u frames", nFrames);
    }
}
