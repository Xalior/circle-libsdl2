//
// surface.cpp - SDL_Surface: memory-backed images in any pixel format
//
// A surface is a block of pixels with a width, a height, a pitch and a
// format record saying what a pixel means. SDL2 applications use them for
// two quite different jobs and this file serves both:
//
//   - as a staging buffer for a texture upload, which is all this library's
//     renderer ever needs, and
//   - as an image the application draws on: filling rectangles, blitting
//     one into another, converting between formats, and - for the many
//     games that render the way a VGA card did - an 8-bit surface of
//     palette indices with the palette changed per frame.
//
// Every format described in pixels.cpp can be a surface. Indexed surfaces
// carry their own palette, and a paletted source blitted or converted into a
// true-colour destination is resolved through that palette, which is what
// makes a 256-colour game's frame arrive on a 32-bit display looking like
// itself.
//
// Blitting itself is in blit.cpp; this file owns the object, its lifetime,
// its clip rectangle, its colour key and its blend state.
//
#include "pixels.h"
#include "blit.h"
#include "sdl2circle.h"

#include <cstdlib>
#include <cstring>

static SDL_Surface *surface_alloc(int width, int height, SDL_PixelFormat *format,
                                  void *pixels, int pitch, bool owns_pixels)
{
    SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (surface == nullptr)
    {
        SDL_FreeFormat(format);
        SDL_SetError("out of memory allocating surface");
        return nullptr;
    }
    SDL_BlitMap *state = (SDL_BlitMap *)calloc(1, sizeof(SDL_BlitMap));
    if (state == nullptr)
    {
        free(surface);
        SDL_FreeFormat(format);
        SDL_SetError("out of memory allocating surface state");
        return nullptr;
    }
    // A surface with an alpha channel starts out blending. SDL2 does this
    // at creation, and applications rely on it without ever calling
    // SDL_SetSurfaceBlendMode: they build an ARGB surface, write per-pixel
    // alpha into it, and blit it with the plain SDL_BlitSurface, expecting
    // the alpha to decide what lands. Started at NONE instead, that same
    // blit would copy every pixel including the transparent ones, losing
    // the shape in the alpha channel and leaving a solid rectangle of the
    // colour underneath it - most visibly with text, where a glyph is
    // exactly that: a rectangle whose alpha channel is the letter.
    state->blend    = (format != nullptr && format->Amask != 0)
                          ? SDL_BLENDMODE_BLEND
                          : SDL_BLENDMODE_NONE;
    state->alphamod = 255;
    state->rmod = state->gmod = state->bmod = 255;

    surface->flags     = owns_pixels ? 0 : SDL_PREALLOC;
    surface->format    = format;
    surface->w         = width;
    surface->h         = height;
    surface->pitch     = pitch;
    surface->pixels    = pixels;
    surface->clip_rect = { 0, 0, width, height };
    surface->map       = (SDL_BlitMap *)state;
    surface->refcount  = 1;
    return surface;
}

extern "C" SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width,
                                                       int height, int depth,
                                                       Uint32 format_enum)
{
    (void)flags;
    (void)depth;   // SDL2 ignores the depth when a format is named

    if (width < 0 || height < 0)
    {
        SDL_SetError("surface dimensions must not be negative");
        return nullptr;
    }
    const int bpp = SDL2Circle_BytesPerPixel(format_enum);
    if (bpp == 0)
    {
        SDL_SetError("cannot create a surface in pixel format 0x%08x",
                     (unsigned)format_enum);
        return nullptr;
    }

    SDL_PixelFormat *format = SDL_AllocFormat(format_enum);
    if (format == nullptr)
        return nullptr;

    // SDL2 pitches rows to a four-byte boundary, and applications that hand
    // a surface's pixels to code expecting word-aligned rows depend on it.
    const int pitch = ((width * bpp) + 3) & ~3;

    void *pixels = nullptr;
    if (width > 0 && height > 0)
    {
        pixels = calloc(1, (size_t)pitch * (size_t)height);
        if (pixels == nullptr)
        {
            SDL_FreeFormat(format);
            SDL_SetError("out of memory allocating surface pixels");
            return nullptr;
        }
    }

    SDL_Surface *surface = surface_alloc(width, height, format, pixels, pitch, true);
    if (surface == nullptr)
    {
        free(pixels);
        return nullptr;
    }

    // An indexed surface gets a palette of its own straight away, as SDL2
    // does, so an application may set colours without allocating one first.
    if (format->BitsPerPixel <= 8 && format->Rmask == 0)
    {
        SDL_Palette *palette = SDL_AllocPalette(1 << format->BitsPerPixel);
        if (palette == nullptr)
        {
            SDL_FreeSurface(surface);
            return nullptr;
        }
        // SDL2 starts an 8-bit palette black rather than white - a freshly
        // created indexed surface is all index 0, and a white index 0 would
        // make an untouched surface a white rectangle.
        memset(palette->colors, 0, (size_t)palette->ncolors * sizeof(SDL_Color));
        for (int i = 0; i < palette->ncolors; i++)
            palette->colors[i].a = SDL_ALPHA_OPAQUE;
        SDL_SetPixelFormatPalette(format, palette);
        SDL_FreePalette(palette);
    }
    return surface;
}

extern "C" SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                             int depth, Uint32 Rmask, Uint32 Gmask,
                                             Uint32 Bmask, Uint32 Amask)
{
    const Uint32 format = SDL_MasksToPixelFormatEnum(depth, Rmask, Gmask, Bmask, Amask);
    if (format == SDL_PIXELFORMAT_UNKNOWN)
    {
        SDL_SetError("no pixel format matches %d bits with masks "
                     "%08x/%08x/%08x/%08x",
                     depth, (unsigned)Rmask, (unsigned)Gmask,
                     (unsigned)Bmask, (unsigned)Amask);
        return nullptr;
    }
    return SDL_CreateRGBSurfaceWithFormat(flags, width, height, depth, format);
}

extern "C" SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width,
                                                           int height, int depth,
                                                           int pitch,
                                                           Uint32 format_enum)
{
    (void)depth;
    const int bpp = SDL2Circle_BytesPerPixel(format_enum);
    if (bpp == 0)
    {
        SDL_SetError("cannot wrap pixels in format 0x%08x", (unsigned)format_enum);
        return nullptr;
    }
    if (width < 0 || height < 0)
    {
        SDL_SetError("surface dimensions must not be negative");
        return nullptr;
    }
    // A pitch of zero means the caller has not got one yet: it is about to
    // point this surface at memory it locks elsewhere, and will write the
    // pitch in at the same time. SDL2 accepts that, so a zero is filled in
    // with the natural row length rather than refused.
    if (pitch == 0)
        pitch = width * bpp;
    else if (pitch < width * bpp)
    {
        SDL_SetError("pitch %d is too small for %d pixels of %d bytes",
                     pitch, width, bpp);
        return nullptr;
    }

    SDL_PixelFormat *format = SDL_AllocFormat(format_enum);
    if (format == nullptr)
        return nullptr;

    SDL_Surface *surface = surface_alloc(width, height, format, pixels, pitch, false);
    if (surface == nullptr)
        return nullptr;

    if (format->BitsPerPixel <= 8 && format->Rmask == 0)
    {
        SDL_Palette *palette = SDL_AllocPalette(1 << format->BitsPerPixel);
        if (palette != nullptr)
        {
            memset(palette->colors, 0, (size_t)palette->ncolors * sizeof(SDL_Color));
            for (int i = 0; i < palette->ncolors; i++)
                palette->colors[i].a = SDL_ALPHA_OPAQUE;
            SDL_SetPixelFormatPalette(format, palette);
            SDL_FreePalette(palette);
        }
    }
    return surface;
}

extern "C" SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height,
                                                 int depth, int pitch, Uint32 Rmask,
                                                 Uint32 Gmask, Uint32 Bmask,
                                                 Uint32 Amask)
{
    const Uint32 format = SDL_MasksToPixelFormatEnum(depth, Rmask, Gmask, Bmask, Amask);
    if (format == SDL_PIXELFORMAT_UNKNOWN)
    {
        SDL_SetError("no pixel format matches %d bits with masks "
                     "%08x/%08x/%08x/%08x",
                     depth, (unsigned)Rmask, (unsigned)Gmask,
                     (unsigned)Bmask, (unsigned)Amask);
        return nullptr;
    }
    return SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height, depth, pitch,
                                              format);
}

extern "C" void SDL_FreeSurface(SDL_Surface *surface)
{
    if (surface == nullptr)
        return;
    if (surface->flags & SDL_DONTFREE)
        return;
    if (--surface->refcount > 0)
        return;

    if (surface->format != nullptr)
        SDL_FreeFormat(surface->format);
    if (!(surface->flags & SDL_PREALLOC))
        free(surface->pixels);
    free(surface->map);
    free(surface);
}

// ---------------------------------------------------------------------------
// Locking
//
// Surfaces here are plain memory and are never held by hardware, so a lock
// has nothing to arrange. It is still counted and still checked, because
// SDL2 requires the pairing and an application that leaks one is telling
// you about a bug in itself.
// ---------------------------------------------------------------------------

extern "C" int SDL_LockSurface(SDL_Surface *surface)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    surface->locked++;
    return 0;
}

extern "C" void SDL_UnlockSurface(SDL_Surface *surface)
{
    if (surface == nullptr || surface->locked == 0)
        return;
    surface->locked--;
}

// ---------------------------------------------------------------------------
// Palette, colour key, modulation, blend mode
// ---------------------------------------------------------------------------

extern "C" int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    return SDL_SetPixelFormatPalette(surface->format, palette);
}

extern "C" int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (surface->map == nullptr)
        return SDL_SetError("SDL_SetColorKey: this surface has no blit state "
                            "to set (it was not made by this library)");
    SDL_BlitMap *state = (SDL_BlitMap *)surface->map;
    if (flag)
    {
        state->colorkey     = key;
        state->colorkey_set = SDL_TRUE;
    }
    else
    {
        state->colorkey_set = SDL_FALSE;
    }
    return 0;
}

extern "C" SDL_bool SDL_HasColorKey(SDL_Surface *surface)
{
    if (surface == nullptr)
        return SDL_FALSE;
    return SDL2Circle_BlitState(surface)->colorkey_set;
}

extern "C" int SDL_GetColorKey(SDL_Surface *surface, Uint32 *key)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    const SDL_BlitMap *state = SDL2Circle_BlitState(surface);
    if (!state->colorkey_set)
        return SDL_SetError("surface has no colour key");
    if (key)
        *key = state->colorkey;
    return 0;
}

extern "C" int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (surface->map == nullptr)
        return SDL_SetError("SDL_SetSurfaceBlendMode: this surface has no "
                            "blit state to set (it was not made by this "
                            "library)");
    ((SDL_BlitMap *)surface->map)->blend = blendMode;
    return 0;
}

extern "C" int SDL_GetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode *blendMode)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (blendMode)
        *blendMode = SDL2Circle_BlitState(surface)->blend;
    return 0;
}

extern "C" int SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (surface->map == nullptr)
        return SDL_SetError("SDL_SetSurfaceAlphaMod: this surface has no "
                            "blit state to set (it was not made by this "
                            "library)");
    ((SDL_BlitMap *)surface->map)->alphamod = alpha;
    return 0;
}

extern "C" int SDL_GetSurfaceAlphaMod(SDL_Surface *surface, Uint8 *alpha)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (alpha)
        *alpha = SDL2Circle_BlitState(surface)->alphamod;
    return 0;
}

extern "C" int SDL_SetSurfaceColorMod(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    if (surface->map == nullptr)
        return SDL_SetError("SDL_SetSurfaceColorMod: this surface has no "
                            "blit state to set (it was not made by this "
                            "library)");
    SDL_BlitMap *state = (SDL_BlitMap *)surface->map;
    state->rmod = r;
    state->gmod = g;
    state->bmod = b;
    return 0;
}

extern "C" int SDL_GetSurfaceColorMod(SDL_Surface *surface, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    const SDL_BlitMap *state = SDL2Circle_BlitState(surface);
    if (r) *r = state->rmod;
    if (g) *g = state->gmod;
    if (b) *b = state->bmod;
    return 0;
}

// RLE is an internal encoding SDL2 may apply to a colour-keyed surface. This
// library never encodes, so the request is accepted and the surface stays
// plain - which is a valid SDL2 state, since SDL_RLEACCEL is only ever a
// hint about how the same pixels are stored.
extern "C" int SDL_SetSurfaceRLE(SDL_Surface *surface, int flag)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    (void)flag;
    return 0;
}

extern "C" SDL_bool SDL_HasSurfaceRLE(SDL_Surface *surface)
{
    (void)surface;
    return SDL_FALSE;
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

extern "C" SDL_bool SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect)
{
    if (surface == nullptr)
        return SDL_FALSE;
    const SDL_Rect full = { 0, 0, surface->w, surface->h };
    if (rect == nullptr)
    {
        surface->clip_rect = full;
        return SDL_TRUE;
    }
    // SDL2 answers false when the requested rectangle lies wholly outside
    // the surface - the clip is still set (to the empty intersection), and
    // the answer is what tells the caller that nothing will draw.
    return SDL_IntersectRect(rect, &full, &surface->clip_rect);
}

extern "C" void SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect)
{
    if (surface == nullptr || rect == nullptr)
        return;
    *rect = surface->clip_rect;
}

// ---------------------------------------------------------------------------
// Filling
// ---------------------------------------------------------------------------

extern "C" int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    if (dst == nullptr)
        return SDL_InvalidParamError("dst");
    if (dst->pixels == nullptr)
        return SDL_SetError("surface has no pixels");

    const int bpp = dst->format->BytesPerPixel;
    if (bpp == 0)
        return SDL_SetError("cannot fill a sub-byte indexed surface");

    SDL_Rect area;
    if (rect == nullptr)
    {
        area = dst->clip_rect;
    }
    else if (!SDL_IntersectRect(rect, &dst->clip_rect, &area))
    {
        return 0;   // entirely clipped: a no-op, and not an error
    }

    Uint8 *row = (Uint8 *)dst->pixels + (size_t)area.y * dst->pitch
               + (size_t)area.x * bpp;

    // A one-byte fill and a four-byte fill of an already-replicated word are
    // the two cases worth having: between them they cover every paletted and
    // every 32-bit surface, which is nearly all of them.
    if (bpp == 1)
    {
        for (int y = 0; y < area.h; y++)
        {
            memset(row, (int)(color & 0xFF), (size_t)area.w);
            row += dst->pitch;
        }
        return 0;
    }
    if (bpp == 4)
    {
        for (int y = 0; y < area.h; y++)
        {
            Uint32 *p = (Uint32 *)row;
            for (int x = 0; x < area.w; x++)
                p[x] = color;
            row += dst->pitch;
        }
        return 0;
    }
    for (int y = 0; y < area.h; y++)
    {
        Uint8 *p = row;
        for (int x = 0; x < area.w; x++)
        {
            SDL2Circle_PutPixel(p, bpp, color);
            p += bpp;
        }
        row += dst->pitch;
    }
    return 0;
}

extern "C" int SDL_FillRects(SDL_Surface *dst, const SDL_Rect *rects, int count,
                             Uint32 color)
{
    if (rects == nullptr)
        return SDL_InvalidParamError("rects");
    for (int i = 0; i < count; i++)
    {
        if (SDL_FillRect(dst, &rects[i], color) < 0)
            return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

extern "C" SDL_Surface *SDL_ConvertSurface(SDL_Surface *src,
                                           const SDL_PixelFormat *fmt, Uint32 flags)
{
    (void)flags;
    if (src == nullptr)
    {
        SDL_InvalidParamError("src");
        return nullptr;
    }
    if (fmt == nullptr)
    {
        SDL_InvalidParamError("fmt");
        return nullptr;
    }

    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h,
                                                      fmt->BitsPerPixel, fmt->format);
    if (dst == nullptr)
        return nullptr;

    // Converting to an indexed format needs the destination's palette, and
    // the only palette that can be meant is the one the caller put on the
    // format they handed in.
    if (fmt->palette != nullptr)
        SDL_SetPixelFormatPalette(dst->format, fmt->palette);

    // The conversion is a blit with blending off: SDL2 defines
    // SDL_ConvertSurface as copying pixels, not compositing them, so a
    // source with per-pixel alpha keeps its alpha rather than being
    // flattened against the new surface's blank background.
    SDL_BlendMode saved;
    SDL_GetSurfaceBlendMode(src, &saved);
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    const int rc = SDL_UpperBlit(src, nullptr, dst, nullptr);
    SDL_SetSurfaceBlendMode(src, saved);

    if (rc < 0)
    {
        SDL_FreeSurface(dst);
        return nullptr;
    }

    // The colour key travels, remapped into the new format - a key is a
    // colour, not a bit pattern, and carrying the raw value across a format
    // change is the classic way transparency turns into a coloured rectangle.
    Uint32 key = 0;
    if (SDL_GetColorKey(src, &key) == 0)
    {
        Uint8 r, g, b, a;
        SDL_GetRGBA(key, src->format, &r, &g, &b, &a);
        SDL_SetColorKey(dst, SDL_TRUE, SDL_MapRGBA(dst->format, r, g, b, a));
    }

    SDL_BlendMode blend;
    SDL_GetSurfaceBlendMode(src, &blend);
    SDL_SetSurfaceBlendMode(dst, blend);
    Uint8 amod;
    SDL_GetSurfaceAlphaMod(src, &amod);
    SDL_SetSurfaceAlphaMod(dst, amod);
    return dst;
}

extern "C" SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src,
                                                 Uint32 pixel_format, Uint32 flags)
{
    SDL_PixelFormat *fmt = SDL_AllocFormat(pixel_format);
    if (fmt == nullptr)
        return nullptr;
    SDL_Surface *dst = SDL_ConvertSurface(src, fmt, flags);
    SDL_FreeFormat(fmt);
    return dst;
}

extern "C" SDL_Surface *SDL_DuplicateSurface(SDL_Surface *surface)
{
    if (surface == nullptr)
    {
        SDL_InvalidParamError("surface");
        return nullptr;
    }
    return SDL_ConvertSurface(surface, surface->format, 0);
}
