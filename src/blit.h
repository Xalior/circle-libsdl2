//
// blit.h — the per-surface blit state, shared between surface.cpp and blit.cpp
//
// SDL2's SDL_Surface declares `struct SDL_BlitMap *map` and never defines
// the struct in a public header: it is the surface's private drawing state.
// This is that definition. It holds what governs how a surface is READ when
// it is a blit source — the colour key, the blend mode, the alpha and colour
// modulation — none of which has anywhere else to live, since the public
// struct has no field for any of it.
//
#ifndef _sdl2_circle_blit_h
#define _sdl2_circle_blit_h

#include <SDL2/SDL.h>

struct SDL_BlitMap
{
    Uint32        colorkey;
    SDL_bool      colorkey_set;
    SDL_BlendMode blend;
    Uint8         alphamod;
    Uint8         rmod, gmod, bmod;
};

#endif
