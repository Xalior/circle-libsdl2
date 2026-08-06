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

// How a surface reads as a blit source, for a surface that may not have one.
//
// `map` is private to SDL2 and to this library, which means code OUTSIDE the
// library cannot populate it even when it is holding a perfectly good
// SDL_Surface — a port that allocates one itself, to wrap a buffer or to
// stand in for an upstream symbol, has no way to build the struct because
// the definition is here. Such a surface arrives with `map` null.
//
// It must not fault. A null pointer dereferenced inside a blit runs on the
// application core with nothing underneath to catch it, and takes the board
// down with no message. A surface with no blit state simply has no colour
// key, no blending and no modulation, which is the state every surface
// starts in, so that is what it reads as.
//
// The library never sets a null map on a surface it made itself; this is for
// the ones it did not make.
inline const SDL_BlitMap *SDL2Circle_BlitState(const SDL_Surface *surface)
{
    static const SDL_BlitMap kDefaults =
    {
        0,                      // colorkey
        SDL_FALSE,              // colorkey_set
        SDL_BLENDMODE_NONE,     // blend
        255,                    // alphamod
        255, 255, 255           // rmod, gmod, bmod
    };

    if (surface == nullptr || surface->map == nullptr)
        return &kDefaults;
    return (const SDL_BlitMap *)surface->map;
}

#endif
