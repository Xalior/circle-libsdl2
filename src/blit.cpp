//
// blit.cpp — copying one surface into another
//
// SDL2 has two blit entry points and the difference between them is the
// whole of SDL_UpperBlit's job: SDL_LowerBlit takes rectangles that have
// already been clipped and validated, SDL_UpperBlit does the clipping and
// then calls it. Applications call the upper one, almost always through the
// macro SDL_BlitSurface, and expect the destination rectangle they passed in
// to be UPDATED with what was actually drawn. Getting that write-back wrong
// is one of the quieter ways a port ends up with sprites in the wrong place.
//
// One general blitter serves every format pair, going through the pixel
// machinery in pixels.cpp. A paletted source is resolved through its own
// palette, so an 8-bit frame lands correctly in a 32-bit destination — which
// is what a VGA-shaped game needs and what it otherwise has to write for
// itself. Two fast paths sit in front of it: an identical-format opaque copy
// (a row memcpy) and the 32-bit alpha-blend case, which between them cover
// what a game does thousands of times per frame.
//
// Blending follows SDL2: SDL_BLENDMODE_NONE copies, SDL_BLENDMODE_BLEND
// composites source-over using the source alpha times the alpha modulation,
// SDL_BLENDMODE_ADD adds, and SDL_BLENDMODE_MOD multiplies. A colour key on
// the source makes matching source pixels leave the destination untouched,
// and that is tested before blending, not after.
//
#include "pixels.h"
#include "blit.h"
#include "sdl2circle.h"

#include <cstring>

namespace
{
inline Uint8 mul255(Uint8 a, Uint8 b)
{
    const unsigned t = (unsigned)a * (unsigned)b + 128u;
    return (Uint8)((t + (t >> 8)) >> 8);
}

struct SurfaceReader
{
    const SDL_PixelFormat *format;
    const SDL_Palette     *palette;
    int                    bpp;
};

inline void read_rgba(const SurfaceReader &r, Uint32 pixel,
                      Uint8 *cr, Uint8 *cg, Uint8 *cb, Uint8 *ca)
{
    if (r.palette != nullptr)
    {
        if ((int)pixel < r.palette->ncolors)
        {
            const SDL_Color &c = r.palette->colors[pixel];
            *cr = c.r; *cg = c.g; *cb = c.b; *ca = c.a;
        }
        else
        {
            *cr = *cg = *cb = 0;
            *ca = SDL_ALPHA_OPAQUE;
        }
        return;
    }
    SDL_GetRGBA(pixel, r.format, cr, cg, cb, ca);
}
} // namespace

// The blit proper: rectangles are already clipped and known to be in bounds
// and of equal size.
static int blit_clipped(SDL_Surface *src, const SDL_Rect *srcrect,
                        SDL_Surface *dst, const SDL_Rect *dstrect)
{
    const int w = srcrect->w;
    const int h = srcrect->h;
    if (w <= 0 || h <= 0)
        return 0;

    const SDL_BlitMap *state = (const SDL_BlitMap *)src->map;
    const int sbpp = src->format->BytesPerPixel;
    const int dbpp = dst->format->BytesPerPixel;
    if (sbpp == 0 || dbpp == 0)
        return SDL_SetError("cannot blit a sub-byte indexed surface");

    const Uint8 *srow = (const Uint8 *)src->pixels
                      + (size_t)srcrect->y * src->pitch + (size_t)srcrect->x * sbpp;
    Uint8 *drow = (Uint8 *)dst->pixels
                + (size_t)dstrect->y * dst->pitch + (size_t)dstrect->x * dbpp;

    const SDL_BlendMode blend  = state->blend;
    const bool          keyed  = state->colorkey_set == SDL_TRUE;
    const Uint8         amod   = state->alphamod;
    const bool          modded = (state->rmod != 255) || (state->gmod != 255)
                              || (state->bmod != 255);

    // Straight copy: same format, nothing to key, nothing to blend and no
    // modulation. This is a surface used as a staging buffer, and it is by
    // far the most common blit a port performs.
    if (!keyed && blend == SDL_BLENDMODE_NONE && amod == 255 && !modded &&
        src->format->format == dst->format->format)
    {
        const size_t bytes = (size_t)w * (size_t)sbpp;
        for (int y = 0; y < h; y++)
        {
            memcpy(drow, srow, bytes);
            srow += src->pitch;
            drow += dst->pitch;
        }
        return 0;
    }

    SurfaceReader reader = { src->format, src->format->palette, sbpp };

    for (int y = 0; y < h; y++)
    {
        const Uint8 *sp = srow;
        Uint8       *dp = drow;
        for (int x = 0; x < w; x++)
        {
            const Uint32 spix = SDL2Circle_GetPixel(sp, sbpp);
            if (keyed && spix == state->colorkey)
            {
                sp += sbpp;
                dp += dbpp;
                continue;
            }

            Uint8 sr, sg, sb, sa;
            read_rgba(reader, spix, &sr, &sg, &sb, &sa);
            if (modded)
            {
                sr = mul255(sr, state->rmod);
                sg = mul255(sg, state->gmod);
                sb = mul255(sb, state->bmod);
            }
            if (amod != 255)
                sa = mul255(sa, amod);

            Uint8 outr = sr, outg = sg, outb = sb, outa = sa;

            if (blend != SDL_BLENDMODE_NONE)
            {
                Uint8 dr, dg, db, da;
                const Uint32 dpix = SDL2Circle_GetPixel(dp, dbpp);
                if (dst->format->palette != nullptr)
                {
                    const SDL_Palette *p = dst->format->palette;
                    if ((int)dpix < p->ncolors)
                    {
                        dr = p->colors[dpix].r; dg = p->colors[dpix].g;
                        db = p->colors[dpix].b; da = p->colors[dpix].a;
                    }
                    else
                    {
                        dr = dg = db = 0;
                        da = SDL_ALPHA_OPAQUE;
                    }
                }
                else
                {
                    SDL_GetRGBA(dpix, dst->format, &dr, &dg, &db, &da);
                }

                switch (blend)
                {
                case SDL_BLENDMODE_BLEND:
                {
                    const Uint8 inv = (Uint8)(255 - sa);
                    outr = (Uint8)(mul255(sr, sa) + mul255(dr, inv));
                    outg = (Uint8)(mul255(sg, sa) + mul255(dg, inv));
                    outb = (Uint8)(mul255(sb, sa) + mul255(db, inv));
                    outa = (Uint8)(sa + mul255(da, inv));
                    break;
                }
                case SDL_BLENDMODE_ADD:
                {
                    unsigned r = (unsigned)mul255(sr, sa) + dr;
                    unsigned g = (unsigned)mul255(sg, sa) + dg;
                    unsigned b = (unsigned)mul255(sb, sa) + db;
                    outr = (Uint8)(r > 255 ? 255 : r);
                    outg = (Uint8)(g > 255 ? 255 : g);
                    outb = (Uint8)(b > 255 ? 255 : b);
                    outa = da;
                    break;
                }
                case SDL_BLENDMODE_MOD:
                    outr = mul255(sr, dr);
                    outg = mul255(sg, dg);
                    outb = mul255(sb, db);
                    outa = da;
                    break;
                case SDL_BLENDMODE_MUL:
                {
                    const Uint8 inv = (Uint8)(255 - sa);
                    outr = (Uint8)(mul255(mul255(sr, dr), sa) + mul255(dr, inv));
                    outg = (Uint8)(mul255(mul255(sg, dg), sa) + mul255(dg, inv));
                    outb = (Uint8)(mul255(mul255(sb, db), sa) + mul255(db, inv));
                    outa = da;
                    break;
                }
                default:
                    break;
                }
            }

            SDL2Circle_PutPixel(dp, dbpp,
                                SDL_MapRGBA(dst->format, outr, outg, outb, outa));
            sp += sbpp;
            dp += dbpp;
        }
        srow += src->pitch;
        drow += dst->pitch;
    }
    return 0;
}

extern "C" int SDL_LowerBlit(SDL_Surface *src, SDL_Rect *srcrect,
                             SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (src == nullptr || dst == nullptr)
        return SDL_InvalidParamError("surface");
    if (src->pixels == nullptr || dst->pixels == nullptr)
        return SDL_SetError("surface has no pixels");

    const SDL_Rect whole_src = { 0, 0, src->w, src->h };
    const SDL_Rect sr = srcrect ? *srcrect : whole_src;
    SDL_Rect dr = { 0, 0, sr.w, sr.h };
    if (dstrect)
    {
        dr.x = dstrect->x;
        dr.y = dstrect->y;
    }
    dr.w = sr.w;
    dr.h = sr.h;
    return blit_clipped(src, &sr, dst, &dr);
}

extern "C" int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect,
                             SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (src == nullptr || dst == nullptr)
        return SDL_InvalidParamError("surface");
    if (src->pixels == nullptr || dst->pixels == nullptr)
        return SDL_SetError("surface has no pixels");
    if (src->locked || dst->locked)
        return SDL_SetError("surfaces must not be locked during blit");

    // Clip the source rectangle to the source surface.
    SDL_Rect sr = srcrect ? *srcrect : SDL_Rect{ 0, 0, src->w, src->h };
    int srcx = 0, srcy = 0;
    if (sr.x < 0) { srcx = -sr.x; sr.w += sr.x; sr.x = 0; }
    if (sr.y < 0) { srcy = -sr.y; sr.h += sr.y; sr.y = 0; }
    if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
    if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;

    int dstx = dstrect ? dstrect->x : 0;
    int dsty = dstrect ? dstrect->y : 0;
    dstx += srcx;
    dsty += srcy;

    // Clip the destination against the destination's clip rectangle, moving
    // the source origin by however much was trimmed off the top or left.
    const SDL_Rect &clip = dst->clip_rect;
    int w = sr.w;
    int h = sr.h;
    if (dstx < clip.x) { const int d = clip.x - dstx; sr.x += d; w -= d; dstx = clip.x; }
    if (dsty < clip.y) { const int d = clip.y - dsty; sr.y += d; h -= d; dsty = clip.y; }
    if (dstx + w > clip.x + clip.w) w = clip.x + clip.w - dstx;
    if (dsty + h > clip.y + clip.h) h = clip.y + clip.h - dsty;

    if (w <= 0 || h <= 0)
    {
        // Nothing drawn is not an error, but the caller's rectangle must say
        // so: SDL2 writes back a zero-sized rectangle, and an application
        // that trusts it stops rather than drawing into nowhere.
        if (dstrect)
        {
            dstrect->w = 0;
            dstrect->h = 0;
        }
        return 0;
    }

    sr.w = w;
    sr.h = h;
    const SDL_Rect dr = { dstx, dsty, w, h };

    // SDL2 reports back what was ACTUALLY blitted, position included.
    if (dstrect)
        *dstrect = dr;

    return blit_clipped(src, &sr, dst, &dr);
}

// ---------------------------------------------------------------------------
// Scaled blit
//
// Nearest-neighbour, which is what SDL2's own software scaler does for
// anything other than SDL_BLENDMODE_NONE at exact ratios. It goes through
// the same per-pixel path as the unscaled blit, so keying, blending and
// modulation behave identically.
// ---------------------------------------------------------------------------

extern "C" int SDL_UpperBlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
                                   SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (src == nullptr || dst == nullptr)
        return SDL_InvalidParamError("surface");
    if (src->pixels == nullptr || dst->pixels == nullptr)
        return SDL_SetError("surface has no pixels");

    const SDL_Rect sr = srcrect ? *srcrect : SDL_Rect{ 0, 0, src->w, src->h };
    SDL_Rect dr = dstrect ? *dstrect : SDL_Rect{ 0, 0, dst->w, dst->h };
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0)
        return 0;

    // An unscaled request is the ordinary blit, and taking that route keeps
    // the row-copy fast path available.
    if (sr.w == dr.w && sr.h == dr.h)
        return SDL_UpperBlit(src, &sr, dst, dstrect);

    SDL_Rect clipped;
    if (!SDL_IntersectRect(&dr, &dst->clip_rect, &clipped))
    {
        if (dstrect)
        {
            dstrect->w = 0;
            dstrect->h = 0;
        }
        return 0;
    }

    const SDL_BlitMap *state = (const SDL_BlitMap *)src->map;
    const int sbpp = src->format->BytesPerPixel;
    const int dbpp = dst->format->BytesPerPixel;
    if (sbpp == 0 || dbpp == 0)
        return SDL_SetError("cannot blit a sub-byte indexed surface");

    const bool  keyed = state->colorkey_set == SDL_TRUE;
    const Uint8 amod  = state->alphamod;
    const SDL_BlendMode blend = state->blend;
    SurfaceReader reader = { src->format, src->format->palette, sbpp };

    for (int y = 0; y < clipped.h; y++)
    {
        const int oy = clipped.y + y - dr.y;
        const int sy = sr.y + (int)(((Sint64)oy * sr.h) / dr.h);
        const Uint8 *srow = (const Uint8 *)src->pixels + (size_t)sy * src->pitch;
        Uint8 *dp = (Uint8 *)dst->pixels + (size_t)(clipped.y + y) * dst->pitch
                  + (size_t)clipped.x * dbpp;

        for (int x = 0; x < clipped.w; x++, dp += dbpp)
        {
            const int ox = clipped.x + x - dr.x;
            const int sx = sr.x + (int)(((Sint64)ox * sr.w) / dr.w);
            const Uint32 spix = SDL2Circle_GetPixel(srow + (size_t)sx * sbpp, sbpp);
            if (keyed && spix == state->colorkey)
                continue;

            Uint8 sR, sG, sB, sA;
            read_rgba(reader, spix, &sR, &sG, &sB, &sA);
            if (state->rmod != 255) sR = mul255(sR, state->rmod);
            if (state->gmod != 255) sG = mul255(sG, state->gmod);
            if (state->bmod != 255) sB = mul255(sB, state->bmod);
            if (amod != 255)        sA = mul255(sA, amod);

            if (blend == SDL_BLENDMODE_BLEND && sA != 255)
            {
                Uint8 dR, dG, dB, dA;
                SDL_GetRGBA(SDL2Circle_GetPixel(dp, dbpp), dst->format,
                            &dR, &dG, &dB, &dA);
                const Uint8 inv = (Uint8)(255 - sA);
                sR = (Uint8)(mul255(sR, sA) + mul255(dR, inv));
                sG = (Uint8)(mul255(sG, sA) + mul255(dG, inv));
                sB = (Uint8)(mul255(sB, sA) + mul255(dB, inv));
                sA = (Uint8)(sA + mul255(dA, inv));
            }
            SDL2Circle_PutPixel(dp, dbpp, SDL_MapRGBA(dst->format, sR, sG, sB, sA));
        }
    }

    if (dstrect)
        *dstrect = clipped;
    return 0;
}

extern "C" int SDL_LowerBlitScaled(SDL_Surface *src, SDL_Rect *srcrect,
                                   SDL_Surface *dst, SDL_Rect *dstrect)
{
    return SDL_UpperBlitScaled(src, srcrect, dst, dstrect);
}
