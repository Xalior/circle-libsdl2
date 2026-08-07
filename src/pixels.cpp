//
// pixels.cpp — SDL2 pixel formats, palettes and colour packing
//
// A pixel format in SDL2 is two things at once: a name (the
// SDL_PIXELFORMAT_* enum) and a description (bits per pixel, one mask and
// one shift per channel, optionally a palette). Everything that reads or
// writes a pixel anywhere in this library goes through the description, so
// adding a format here is what makes surfaces, blits, conversions and
// texture uploads understand it — there is no second place to teach.
//
// Every packed RGB format SDL2 names is described here, from 8-bit indexed
// and RGB332 through the 16-bit 4444/1555/565 families and 24-bit RGB/BGR
// to the 32-bit 8888 family and ARGB2101010. What is deliberately absent is
// the planar and packed YUV formats (SDL_PIXELFORMAT_YV12 and its
// relatives): they are not laid out as one mask-addressable pixel, nothing
// in this library produces or consumes them, and describing them here would
// be describing something no code path can act on.
//
// Format records for the PACKED formats are shared and reference-counted,
// exactly as SDL2 does it: SDL_AllocFormat returns the one record for a
// given format enum with its count raised, and SDL_FreeFormat lowers it.
// Surfaces therefore cost no format storage of their own, and an
// application that allocates the same format in a loop is not allocating
// anything.
//
// AN INDEXED FORMAT IS NEVER SHARED, because the palette lives in the format
// record and a palette belongs to ONE picture. Two 8-bit surfaces sharing a
// record share a palette, so making the second one — which installs a fresh
// palette of its own — silently repaints the first in whatever colours the
// new palette holds. Nothing warns and nothing fails; the picture simply
// comes out in the wrong colours, or in none at all.
//
#include "pixels.h"
#include "sdl2circle.h"

#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// The format table
//
// One row per format this library can describe. It is the single source for
// both directions of the enum/masks translation, so the two can never drift
// apart — which they do when each is written as its own switch.
// ---------------------------------------------------------------------------
namespace
{
// A row holds only what the format enum cannot say for itself: the channel
// masks and the name. WIDTH IS NEVER WRITTEN HERE. Both widths a format has —
// its significant bits and the bytes one pixel occupies — are encoded in the
// enum value and are read back out of it below, so a row cannot disagree with
// the format it names.
struct FormatDesc
{
    Uint32 format;
    Uint32 rmask, gmask, bmask, amask;
    const char *name;
};

// Masks are written for a LITTLE-ENDIAN machine, which every AArch64 Pi is.
// The 24-bit rows are the ones this actually matters for: RGB24 means "red
// byte first", which reads back as mask 0x0000FF, while the 32-bit rows name
// a whole word and are endianness-independent as written.
const FormatDesc s_formats[] = {
    { SDL_PIXELFORMAT_INDEX1LSB,  0,0,0,0,                                     "SDL_PIXELFORMAT_INDEX1LSB"  },
    { SDL_PIXELFORMAT_INDEX1MSB,  0,0,0,0,                                     "SDL_PIXELFORMAT_INDEX1MSB"  },
    { SDL_PIXELFORMAT_INDEX4LSB,  0,0,0,0,                                     "SDL_PIXELFORMAT_INDEX4LSB"  },
    { SDL_PIXELFORMAT_INDEX4MSB,  0,0,0,0,                                     "SDL_PIXELFORMAT_INDEX4MSB"  },
    { SDL_PIXELFORMAT_INDEX8,     0,0,0,0,                                     "SDL_PIXELFORMAT_INDEX8"     },
    { SDL_PIXELFORMAT_RGB332,     0xE0,0x1C,0x03,0x00,                         "SDL_PIXELFORMAT_RGB332"     },

    { SDL_PIXELFORMAT_XRGB4444,   0x0F00,0x00F0,0x000F,0x0000,                 "SDL_PIXELFORMAT_RGB444"     },
    { SDL_PIXELFORMAT_XBGR4444,   0x000F,0x00F0,0x0F00,0x0000,                 "SDL_PIXELFORMAT_BGR444"     },
    { SDL_PIXELFORMAT_ARGB4444,   0x0F00,0x00F0,0x000F,0xF000,                 "SDL_PIXELFORMAT_ARGB4444"   },
    { SDL_PIXELFORMAT_RGBA4444,   0xF000,0x0F00,0x00F0,0x000F,                 "SDL_PIXELFORMAT_RGBA4444"   },
    { SDL_PIXELFORMAT_ABGR4444,   0x000F,0x00F0,0x0F00,0xF000,                 "SDL_PIXELFORMAT_ABGR4444"   },
    { SDL_PIXELFORMAT_BGRA4444,   0x00F0,0x0F00,0xF000,0x000F,                 "SDL_PIXELFORMAT_BGRA4444"   },

    { SDL_PIXELFORMAT_XRGB1555,   0x7C00,0x03E0,0x001F,0x0000,                 "SDL_PIXELFORMAT_RGB555"     },
    { SDL_PIXELFORMAT_XBGR1555,   0x001F,0x03E0,0x7C00,0x0000,                 "SDL_PIXELFORMAT_BGR555"     },
    { SDL_PIXELFORMAT_ARGB1555,   0x7C00,0x03E0,0x001F,0x8000,                 "SDL_PIXELFORMAT_ARGB1555"   },
    { SDL_PIXELFORMAT_RGBA5551,   0xF800,0x07C0,0x003E,0x0001,                 "SDL_PIXELFORMAT_RGBA5551"   },
    { SDL_PIXELFORMAT_ABGR1555,   0x001F,0x03E0,0x7C00,0x8000,                 "SDL_PIXELFORMAT_ABGR1555"   },
    { SDL_PIXELFORMAT_BGRA5551,   0x003E,0x07C0,0xF800,0x0001,                 "SDL_PIXELFORMAT_BGRA5551"   },

    { SDL_PIXELFORMAT_RGB565,     0xF800,0x07E0,0x001F,0x0000,                 "SDL_PIXELFORMAT_RGB565"     },
    { SDL_PIXELFORMAT_BGR565,     0x001F,0x07E0,0xF800,0x0000,                 "SDL_PIXELFORMAT_BGR565"     },

    { SDL_PIXELFORMAT_RGB24,      0x0000FF,0x00FF00,0xFF0000,0x000000,         "SDL_PIXELFORMAT_RGB24"      },
    { SDL_PIXELFORMAT_BGR24,      0xFF0000,0x00FF00,0x0000FF,0x000000,         "SDL_PIXELFORMAT_BGR24"      },

    { SDL_PIXELFORMAT_XRGB8888,   0x00FF0000,0x0000FF00,0x000000FF,0x00000000, "SDL_PIXELFORMAT_RGB888"     },
    { SDL_PIXELFORMAT_RGBX8888,   0xFF000000,0x00FF0000,0x0000FF00,0x00000000, "SDL_PIXELFORMAT_RGBX8888"   },
    { SDL_PIXELFORMAT_XBGR8888,   0x000000FF,0x0000FF00,0x00FF0000,0x00000000, "SDL_PIXELFORMAT_BGR888"     },
    { SDL_PIXELFORMAT_BGRX8888,   0x0000FF00,0x00FF0000,0xFF000000,0x00000000, "SDL_PIXELFORMAT_BGRX8888"   },
    { SDL_PIXELFORMAT_ARGB8888,   0x00FF0000,0x0000FF00,0x000000FF,0xFF000000, "SDL_PIXELFORMAT_ARGB8888"   },
    { SDL_PIXELFORMAT_RGBA8888,   0xFF000000,0x00FF0000,0x0000FF00,0x000000FF, "SDL_PIXELFORMAT_RGBA8888"   },
    { SDL_PIXELFORMAT_ABGR8888,   0x000000FF,0x0000FF00,0x00FF0000,0xFF000000, "SDL_PIXELFORMAT_ABGR8888"   },
    { SDL_PIXELFORMAT_BGRA8888,   0x0000FF00,0x00FF0000,0xFF000000,0x000000FF, "SDL_PIXELFORMAT_BGRA8888"   },

    { SDL_PIXELFORMAT_ARGB2101010, 0x3FF00000,0x000FFC00,0x000003FF,0xC0000000,"SDL_PIXELFORMAT_ARGB2101010"},
};

const FormatDesc *find_desc(Uint32 format)
{
    for (const FormatDesc &d : s_formats)
        if (d.format == format)
            return &d;
    return nullptr;
}

// THE BITS A FORMAT REPORTS, which is not always the bits it stores.
//
// SDL2's rule, from its own SDL_PixelFormatEnumToMasks: a format one or two
// bytes wide reports its significant bits; anything wider reports its BYTES
// times eight. The X-channel four-byte formats are where that matters.
// SDL_PIXELFORMAT_RGB888 and its three siblings hold twenty-four significant
// bits — the fourth byte carries nothing, so SDL_BITSPERPIXEL says 24 — but
// a pixel still occupies four bytes, so SDL2 calls them 32-bit formats and
// every buffer, pitch and offset for them is four bytes to the pixel.
//
// Reading 24 as the pixel width is a silent heap overrun: a row sized at
// three bytes a pixel is a quarter short of what an application writing whole
// words puts into it, and nothing reports the overflow.
int format_bits(Uint32 format)
{
    const int bytes = (int)SDL_BYTESPERPIXEL(format);
    return bytes > 2 ? bytes * 8 : (int)SDL_BITSPERPIXEL(format);
}

// A mask lookup matches on the depth the caller named, with ONE pairing that
// is not equality: 15 and 16 answer for each other. XRGB1555 reports fifteen
// bits, and an application asking for a 555 surface names sixteen — real SDL2
// hands back SDL_PIXELFORMAT_RGB555 either way, and refusing it would fail a
// surface every port expects to get.
//
// The twelve-bit formats are deliberately NOT in that pairing, because real
// SDL2 does not put them there: 4444 masks at a depth of sixteen are an
// unknown format to it, and answering with a surface that then reports twelve
// bits is exactly the kind of surprise an application asserts on.
bool depth_matches(int want, int have)
{
    if (want == have)
        return true;
    return (want == 15 && have == 16) || (want == 16 && have == 15);
}

// Derive shift and loss from a channel mask, the way SDL2's own
// SDL_InitFormat does: the shift is the count of low zero bits, the loss is
// eight minus the width of the mask. An absent channel (mask 0) is loss 8,
// shift 0 — which is what makes MapRGBA fold a missing alpha away instead
// of writing a stray bit.
void mask_to_shift_loss(Uint32 mask, Uint8 *shift, Uint8 *loss)
{
    *shift = 0;
    *loss  = 8;
    if (mask == 0)
        return;
    Uint32 m = mask;
    while ((m & 1) == 0)
    {
        m >>= 1;
        (*shift)++;
    }
    while ((m & 1) == 1)
    {
        m >>= 1;
        (*loss)--;
    }
}
} // namespace

// The STRIDE of one pixel: what a pointer walking a row steps by, and what
// every buffer this library allocates is sized from. It comes straight out of
// the format enum, which is the only place that knows it — deriving it from
// the significant bits gets the X-channel formats wrong by a whole byte.
//
// Zero for a sub-byte indexed format, because there is no whole byte to step
// by, and zero for a format this library cannot describe. Callers test for
// zero and refuse the surface or the texture. Note that the FORMAT RECORD's
// BytesPerPixel is a different number for those same formats — it rounds up
// to one, as SDL2's does — because it answers a different question.
int SDL2Circle_BytesPerPixel(Uint32 pixel_format)
{
    if (find_desc(pixel_format) == nullptr)
        return 0;
    return (int)SDL_BYTESPERPIXEL(pixel_format);
}

int SDL2Circle_InitFormat(SDL_PixelFormat *format, Uint32 pixel_format)
{
    const FormatDesc *d = find_desc(pixel_format);
    if (d == nullptr)
        return SDL_SetError("unknown pixel format 0x%08x", (unsigned)pixel_format);

    const int bits = format_bits(pixel_format);

    memset(format, 0, sizeof(*format));
    format->format        = pixel_format;
    format->palette       = nullptr;
    format->BitsPerPixel  = (Uint8)bits;
    format->BytesPerPixel = (Uint8)((bits + 7) / 8);
    format->Rmask         = d->rmask;
    format->Gmask         = d->gmask;
    format->Bmask         = d->bmask;
    format->Amask         = d->amask;
    mask_to_shift_loss(d->rmask, &format->Rshift, &format->Rloss);
    mask_to_shift_loss(d->gmask, &format->Gshift, &format->Gloss);
    mask_to_shift_loss(d->bmask, &format->Bshift, &format->Bloss);
    mask_to_shift_loss(d->amask, &format->Ashift, &format->Aloss);
    format->refcount = 1;
    format->next     = nullptr;
    return 0;
}

Uint8 SDL2Circle_ExpandChannel(Uint32 value, Uint8 loss)
{
    if (loss >= 8)
        return 0;
    if (loss == 0)
        return (Uint8)value;
    // Kept width is 8 - loss bits. Scaling by 255/(2^kept - 1) is the
    // expansion that maps all-ones to 255 exactly, which is what makes a
    // white pixel survive a round trip through a 565 surface.
    const Uint32 kept_max = (1u << (8 - loss)) - 1u;
    return (Uint8)((value * 255u + kept_max / 2u) / kept_max);
}

Uint32 SDL2Circle_GetPixel(const Uint8 *p, int bpp)
{
    switch (bpp)
    {
    case 1: return *p;
    case 2: return *(const Uint16 *)p;
    case 3: return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16);
    case 4: return *(const Uint32 *)p;
    default: return 0;
    }
}

void SDL2Circle_PutPixel(Uint8 *p, int bpp, Uint32 pixel)
{
    switch (bpp)
    {
    case 1: *p = (Uint8)pixel; break;
    case 2: *(Uint16 *)p = (Uint16)pixel; break;
    case 3:
        p[0] = (Uint8)(pixel & 0xFF);
        p[1] = (Uint8)((pixel >> 8) & 0xFF);
        p[2] = (Uint8)((pixel >> 16) & 0xFF);
        break;
    case 4: *(Uint32 *)p = pixel; break;
    default: break;
    }
}

Uint8 SDL2Circle_FindColor(const SDL_Palette *palette, Uint8 r, Uint8 g, Uint8 b)
{
    if (palette == nullptr || palette->ncolors <= 0)
        return 0;
    unsigned best  = 0;
    unsigned bestd = ~0u;
    for (int i = 0; i < palette->ncolors; i++)
    {
        int dr = (int)palette->colors[i].r - (int)r;
        int dg = (int)palette->colors[i].g - (int)g;
        int db = (int)palette->colors[i].b - (int)b;
        unsigned d = (unsigned)(dr * dr + dg * dg + db * db);
        if (d < bestd)
        {
            bestd = d;
            best  = (unsigned)i;
            if (d == 0)
                break;
        }
    }
    return (Uint8)best;
}

// ---------------------------------------------------------------------------
// Enum <-> masks
// ---------------------------------------------------------------------------

extern "C" SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp,
                                               Uint32 *Rmask, Uint32 *Gmask,
                                               Uint32 *Bmask, Uint32 *Amask)
{
    const FormatDesc *d = find_desc(format);
    if (d == nullptr)
    {
        SDL_SetError("unsupported pixel format 0x%08x", (unsigned)format);
        return SDL_FALSE;
    }
    if (bpp)   *bpp   = format_bits(format);
    if (Rmask) *Rmask = d->rmask;
    if (Gmask) *Gmask = d->gmask;
    if (Bmask) *Bmask = d->bmask;
    if (Amask) *Amask = d->amask;
    return SDL_TRUE;
}

extern "C" Uint32 SDL_MasksToPixelFormatEnum(int bpp, Uint32 Rmask, Uint32 Gmask,
                                             Uint32 Bmask, Uint32 Amask)
{
    // All four masks zero is SDL2's "give me the ordinary format for this
    // depth" request, and every application that calls SDL_CreateRGBSurface
    // with zeroes relies on the answer.
    if (Rmask == 0 && Gmask == 0 && Bmask == 0 && Amask == 0)
    {
        switch (bpp)
        {
        case 1:  return SDL_PIXELFORMAT_INDEX1MSB;
        case 4:  return SDL_PIXELFORMAT_INDEX4MSB;
        case 8:  return SDL_PIXELFORMAT_INDEX8;
        case 12: return SDL_PIXELFORMAT_RGB444;
        case 15: return SDL_PIXELFORMAT_RGB555;
        case 16: return SDL_PIXELFORMAT_RGB565;
        case 24: return SDL_PIXELFORMAT_RGB24;

        // ARGB8888 AND NOT RGB888, and this one is a DELIBERATE DIFFERENCE
        // from real SDL2, which answers RGB888 here. Both are four bytes wide
        // and both report 32 bits, so either satisfies the depth the caller
        // named; ARGB8888 is the one that carries an alpha channel, and it is
        // the format the display already works in, so a surface made this way
        // reaches the glass without a conversion.
        case 32: return SDL_PIXELFORMAT_ARGB8888;

        default: return SDL_PIXELFORMAT_UNKNOWN;
        }
    }

    for (const FormatDesc &d : s_formats)
    {
        if (!depth_matches(bpp, format_bits(d.format)))
            continue;
        if (d.rmask == Rmask && d.gmask == Gmask &&
            d.bmask == Bmask && d.amask == Amask)
            return d.format;
    }
    return SDL_PIXELFORMAT_UNKNOWN;
}

extern "C" const char *SDL_GetPixelFormatName(Uint32 format)
{
    if (format == SDL_PIXELFORMAT_UNKNOWN)
        return "SDL_PIXELFORMAT_UNKNOWN";
    const FormatDesc *d = find_desc(format);
    return d ? d->name : "SDL_PIXELFORMAT_UNKNOWN";
}

// ---------------------------------------------------------------------------
// Format records — one shared, reference-counted record per format enum
// ---------------------------------------------------------------------------

static SDL_PixelFormat *s_format_list = nullptr;

extern "C" SDL_PixelFormat *SDL_AllocFormat(Uint32 pixel_format)
{
    // An indexed format carries a palette, and a palette describes one
    // picture. Handing two surfaces the same record would hand them the same
    // palette: creating the second surface installs a new palette in the
    // record they share, and the first surface's colours are gone. So an
    // indexed caller gets a private record, and it never joins the list —
    // which is what SDL2 does too, as any program holding two paletted
    // surfaces at once demonstrates.
    if (!SDL_ISPIXELFORMAT_INDEXED(pixel_format))
    {
        for (SDL_PixelFormat *f = s_format_list; f != nullptr; f = f->next)
        {
            if (f->format == pixel_format)
            {
                f->refcount++;
                return f;
            }
        }
    }

    SDL_PixelFormat *format = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    if (format == nullptr)
    {
        SDL_SetError("out of memory allocating pixel format");
        return nullptr;
    }
    if (SDL2Circle_InitFormat(format, pixel_format) < 0)
    {
        free(format);
        return nullptr;
    }
    // Only a shareable record goes on the list, so a later caller asking for
    // the same indexed format never finds this one.
    if (!SDL_ISPIXELFORMAT_INDEXED(pixel_format))
    {
        format->next  = s_format_list;
        s_format_list = format;
    }
    return format;
}

extern "C" void SDL_FreeFormat(SDL_PixelFormat *format)
{
    if (format == nullptr)
    {
        SDL_InvalidParamError("format");
        return;
    }
    if (--format->refcount > 0)
        return;

    // A private indexed record was never listed, so this walk simply finds
    // nothing and the record is freed on its own.
    SDL_PixelFormat **link = &s_format_list;
    while (*link != nullptr)
    {
        if (*link == format)
        {
            *link = format->next;
            break;
        }
        link = &(*link)->next;
    }
    if (format->palette != nullptr)
        SDL_FreePalette(format->palette);
    free(format);
}

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------

extern "C" SDL_Palette *SDL_AllocPalette(int ncolors)
{
    if (ncolors < 1)
    {
        SDL_InvalidParamError("ncolors");
        return nullptr;
    }
    SDL_Palette *palette = (SDL_Palette *)malloc(sizeof(SDL_Palette));
    if (palette == nullptr)
    {
        SDL_SetError("out of memory allocating palette");
        return nullptr;
    }
    palette->colors = (SDL_Color *)malloc((size_t)ncolors * sizeof(SDL_Color));
    if (palette->colors == nullptr)
    {
        free(palette);
        SDL_SetError("out of memory allocating palette colours");
        return nullptr;
    }
    palette->ncolors  = ncolors;
    palette->version  = 1;
    palette->refcount = 1;
    // SDL2 fills a new palette with white, not black: an application that
    // sets only some entries then draws with an unset one gets something
    // visible rather than a picture that looks like a failed load.
    memset(palette->colors, 0xFF, (size_t)ncolors * sizeof(SDL_Color));
    return palette;
}

extern "C" void SDL_FreePalette(SDL_Palette *palette)
{
    if (palette == nullptr)
    {
        SDL_InvalidParamError("palette");
        return;
    }
    if (--palette->refcount > 0)
        return;
    free(palette->colors);
    free(palette);
}

extern "C" int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                                    int firstcolor, int ncolors)
{
    if (palette == nullptr)
        return SDL_InvalidParamError("palette");

    // SDL2 clamps rather than refusing, and reports the clamp by returning
    // -1 while still writing what fitted. Games that hand over a full 256
    // entries against a smaller palette depend on both halves of that.
    int status = 0;
    int end    = firstcolor + ncolors;
    if (firstcolor < 0 || firstcolor > palette->ncolors || end < firstcolor)
        return SDL_SetError("palette range out of bounds");
    if (end > palette->ncolors)
    {
        ncolors = palette->ncolors - firstcolor;
        status  = -1;
    }
    if (colors != nullptr && ncolors > 0)
        memcpy(&palette->colors[firstcolor], colors,
               (size_t)ncolors * sizeof(SDL_Color));

    // The version is what tells a cached blit or a converted copy that the
    // colours underneath it have moved.
    if (++palette->version == 0)
        palette->version = 1;
    return status;
}

extern "C" int SDL_SetPixelFormatPalette(SDL_PixelFormat *format,
                                         SDL_Palette *palette)
{
    if (format == nullptr)
        return SDL_InvalidParamError("format");
    if (palette != nullptr && palette->ncolors > (1 << format->BitsPerPixel))
        return SDL_SetError("palette has more colours than the format can index");

    if (palette != nullptr)
        palette->refcount++;
    if (format->palette != nullptr)
        SDL_FreePalette(format->palette);
    format->palette = palette;
    return 0;
}

// ---------------------------------------------------------------------------
// Colour packing and unpacking
// ---------------------------------------------------------------------------

extern "C" Uint32 SDL_MapRGB(const SDL_PixelFormat *format,
                             Uint8 r, Uint8 g, Uint8 b)
{
    if (format == nullptr)
        return 0;
    if (format->palette != nullptr)
        return SDL2Circle_FindColor(format->palette, r, g, b);
    // Opaque: the alpha bits are set outright rather than scaled, which is
    // what makes SDL_MapRGB on an ARGB8888 format answer 0xFF......
    return (Uint32)((r >> format->Rloss) << format->Rshift)
         | (Uint32)((g >> format->Gloss) << format->Gshift)
         | (Uint32)((b >> format->Bloss) << format->Bshift)
         | format->Amask;
}

extern "C" Uint32 SDL_MapRGBA(const SDL_PixelFormat *format,
                              Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (format == nullptr)
        return 0;
    if (format->palette != nullptr)
        return SDL2Circle_FindColor(format->palette, r, g, b);
    return (Uint32)((r >> format->Rloss) << format->Rshift)
         | (Uint32)((g >> format->Gloss) << format->Gshift)
         | (Uint32)((b >> format->Bloss) << format->Bshift)
         | (Uint32)(((a >> format->Aloss) << format->Ashift) & format->Amask);
}

extern "C" void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *format,
                           Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (format == nullptr)
        return;
    if (format->palette != nullptr)
    {
        const SDL_Palette *p = format->palette;
        if ((int)pixel < p->ncolors)
        {
            if (r) *r = p->colors[pixel].r;
            if (g) *g = p->colors[pixel].g;
            if (b) *b = p->colors[pixel].b;
        }
        else
        {
            if (r) *r = 0;
            if (g) *g = 0;
            if (b) *b = 0;
        }
        return;
    }
    if (r) *r = SDL2Circle_ExpandChannel((pixel & format->Rmask) >> format->Rshift,
                                         format->Rloss);
    if (g) *g = SDL2Circle_ExpandChannel((pixel & format->Gmask) >> format->Gshift,
                                         format->Gloss);
    if (b) *b = SDL2Circle_ExpandChannel((pixel & format->Bmask) >> format->Bshift,
                                         format->Bloss);
}

extern "C" void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *format,
                            Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
    SDL_GetRGB(pixel, format, r, g, b);
    if (format == nullptr)
        return;
    if (format->palette != nullptr)
    {
        const SDL_Palette *p = format->palette;
        if (a) *a = ((int)pixel < p->ncolors) ? p->colors[pixel].a : SDL_ALPHA_OPAQUE;
        return;
    }
    // A format with no alpha channel reads back fully opaque, which is what
    // lets one code path serve XRGB and ARGB sources alike.
    if (a) *a = (format->Amask == 0)
              ? (Uint8)SDL_ALPHA_OPAQUE
              : SDL2Circle_ExpandChannel((pixel & format->Amask) >> format->Ashift,
                                         format->Aloss);
}

extern "C" void SDL_CalculateGammaRamp(float gamma, Uint16 *ramp)
{
    if (ramp == nullptr)
    {
        SDL_InvalidParamError("ramp");
        return;
    }
    if (gamma <= 0.0f)
    {
        // A gamma of zero is total darkness, and SDL2 says so explicitly
        // rather than dividing by it.
        memset(ramp, 0, 256 * sizeof(Uint16));
        return;
    }
    if (gamma == 1.0f)
    {
        for (int i = 0; i < 256; i++)
            ramp[i] = (Uint16)((i << 8) | i);
        return;
    }
    for (int i = 0; i < 256; i++)
    {
        double value = __builtin_pow((double)i / 256.0, 1.0 / (double)gamma) * 65535.0;
        if (value > 65535.0)
            value = 65535.0;
        ramp[i] = (Uint16)value;
    }
}

// ---------------------------------------------------------------------------
// Bulk conversion
//
// SDL_ConvertPixels is the one entry point every other conversion in this
// library goes through — surface conversion, texture upload in a format the
// present path does not store, and the BMP loader's normalisation all land
// here. So the fast paths belong here and nowhere else.
// ---------------------------------------------------------------------------

extern "C" int SDL_ConvertPixels(int width, int height,
                                 Uint32 src_format, const void *src, int src_pitch,
                                 Uint32 dst_format, void *dst, int dst_pitch)
{
    if (src == nullptr)
        return SDL_InvalidParamError("src");
    if (dst == nullptr)
        return SDL_InvalidParamError("dst");
    if (width <= 0 || height <= 0)
        return 0;

    const int src_bpp = SDL2Circle_BytesPerPixel(src_format);
    const int dst_bpp = SDL2Circle_BytesPerPixel(dst_format);
    if (src_bpp == 0)
        return SDL_SetError("cannot convert from pixel format 0x%08x",
                            (unsigned)src_format);
    if (dst_bpp == 0)
        return SDL_SetError("cannot convert to pixel format 0x%08x",
                            (unsigned)dst_format);

    const Uint8 *srow = (const Uint8 *)src;
    Uint8       *drow = (Uint8 *)dst;

    // Same format: a row copy, and nothing to decide per pixel.
    if (src_format == dst_format)
    {
        const size_t bytes = (size_t)width * (size_t)src_bpp;
        for (int y = 0; y < height; y++)
        {
            memcpy(drow, srow, bytes);
            srow += src_pitch;
            drow += dst_pitch;
        }
        return 0;
    }

    // An indexed source has no palette here — SDL_ConvertPixels takes format
    // enums, not formats, so there is nowhere for one to come from. Surface
    // conversion handles indexed sources itself, through the surface's own
    // palette; this entry point says so rather than inventing grey.
    if (src_format == SDL_PIXELFORMAT_INDEX8 || dst_format == SDL_PIXELFORMAT_INDEX8)
        return SDL_SetError("SDL_ConvertPixels cannot handle indexed formats "
                            "(no palette is passed); convert the surface instead");

    SDL_PixelFormat sf, df;
    if (SDL2Circle_InitFormat(&sf, src_format) < 0)
        return -1;
    if (SDL2Circle_InitFormat(&df, dst_format) < 0)
        return -1;

    // The common case by far: two 32-bit formats differing only in channel
    // order. Whole-pixel shuffling beats a decompose/recompose per channel.
    for (int y = 0; y < height; y++)
    {
        const Uint8 *sp = srow;
        Uint8       *dp = drow;
        for (int x = 0; x < width; x++)
        {
            Uint32 pixel = SDL2Circle_GetPixel(sp, src_bpp);
            Uint8 r, g, b, a;
            SDL_GetRGBA(pixel, &sf, &r, &g, &b, &a);
            SDL2Circle_PutPixel(dp, dst_bpp, SDL_MapRGBA(&df, r, g, b, a));
            sp += src_bpp;
            dp += dst_bpp;
        }
        srow += src_pitch;
        drow += dst_pitch;
    }
    return 0;
}
