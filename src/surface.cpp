//
// surface.cpp — memory-backed SDL_Surface, 32 bits per pixel only.
//
// A surface here is a plain staging buffer: memory the application writes
// pixels into before handing them to a streaming texture. Nothing blits,
// converts, locks or clips through this file, because the shim's renderer
// takes its pixels from textures alone.
//
// The pixel format is fixed at XRGB8888 — red at bit 16, green at bit 8,
// blue at bit 0, no alpha channel — which is byte-for-byte what the
// renderer's ARGB8888 textures expect, so an application can stage into a
// surface and upload it with no conversion step. Desktop SDL2 chooses this
// same format when asked for a 32-bit surface with all four channel masks
// left at zero, so an application written against desktop SDL2 reads back
// the shifts and masks it already expects.
//
#include <SDL2/SDL.h>
#include <cstdlib>

// One shared format record. It is immutable and every surface points at it,
// so surfaces are free to come and go without owning format storage.
static SDL_PixelFormat s_xrgb8888 = {
    SDL_PIXELFORMAT_RGB888,   // SDL2's name for 32-bit XRGB
    nullptr,                  // no palette
    32,                       // BitsPerPixel
    4,                        // BytesPerPixel
    { 0, 0 },                 // padding
    0x00FF0000,               // Rmask
    0x0000FF00,               // Gmask
    0x000000FF,               // Bmask
    0x00000000,               // Amask — no alpha channel
    0, 0, 0, 8,               // Rloss, Gloss, Bloss, Aloss
    16, 8, 0, 0,              // Rshift, Gshift, Bshift, Ashift
    1,                        // refcount
    nullptr                   // next
};

extern "C" SDL_Surface *SDL_CreateRGBSurface(Uint32, int width, int height,
                                             int depth, Uint32, Uint32,
                                             Uint32, Uint32)
{
    if (depth != 32)
    {
        SDL_SetError("only 32-bit surfaces are implemented");
        return nullptr;
    }
    if (width <= 0 || height <= 0)
    {
        SDL_SetError("surface dimensions must be positive");
        return nullptr;
    }

    SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (surface == nullptr)
    {
        SDL_SetError("out of memory allocating surface");
        return nullptr;
    }

    const int pitch = width * 4;
    surface->pixels = calloc(1, (size_t)pitch * height);
    if (surface->pixels == nullptr)
    {
        free(surface);
        SDL_SetError("out of memory allocating surface pixels");
        return nullptr;
    }

    surface->format = &s_xrgb8888;
    surface->w = width;
    surface->h = height;
    surface->pitch = pitch;
    surface->clip_rect = { 0, 0, width, height };
    surface->refcount = 1;
    return surface;
}

extern "C" void SDL_FreeSurface(SDL_Surface *surface)
{
    if (surface == nullptr)
        return;
    if (--surface->refcount > 0)
        return;
    free(surface->pixels);
    free(surface);
}
