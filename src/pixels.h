//
// pixels.h — the pixel-format machinery, shared inside this library
//
// Not installed. The public surface of all this is SDL2's own
// SDL_pixels.h and SDL_surface.h; these are the pieces the surface, blit,
// BMP and texture code reach for directly rather than going the long way
// round through the public API.
//
#ifndef _sdl2_circle_pixels_h
#define _sdl2_circle_pixels_h

#include <SDL2/SDL.h>

// Fill in an SDL_PixelFormat from a format enum, in place, without touching
// the shared cache SDL_AllocFormat keeps. The record is left with refcount
// 1 and no palette; an indexed format gets no palette either, which is what
// SDL2 does before SDL_SetPixelFormatPalette is called.
//
// Returns 0, or -1 with SDL_SetError for a format this library cannot
// describe (a FourCC/planar-YUV one).
int SDL2Circle_InitFormat(SDL_PixelFormat *format, Uint32 pixel_format);

// The bytes per pixel a format's rows are measured in. 0 for a format with
// no byte-addressable pixel (the sub-byte indexed formats), which is what
// makes them unusable as a blit source or destination here.
int SDL2Circle_BytesPerPixel(Uint32 pixel_format);

// Read and write one pixel through a format's masks. `bpp` is the format's
// BytesPerPixel; the 3-byte case is byte-ordered, the rest are native.
Uint32 SDL2Circle_GetPixel(const Uint8 *p, int bpp);
void   SDL2Circle_PutPixel(Uint8 *p, int bpp, Uint32 pixel);

// Expand a channel that was stored with `loss` bits discarded back to the
// full 0..255 range, the way SDL2's expansion table does: the kept bits are
// replicated so that all-ones maps to 255 exactly.
Uint8 SDL2Circle_ExpandChannel(Uint32 value, Uint8 loss);

// The nearest entry in a palette to a colour, by squared RGB distance. This
// is what SDL_MapRGB answers for an indexed format.
Uint8 SDL2Circle_FindColor(const SDL_Palette *palette, Uint8 r, Uint8 g, Uint8 b);

#endif
