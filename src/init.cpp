//
// init.cpp — subsystem bookkeeping, version, platform
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

static Uint32 s_initialized = 0;

extern "C" int SDL_InitSubSystem(Uint32 flags)
{
    // Video/window devices come up lazily in SDL_CreateWindow; USB comes up
    // here so keyboards enumerate while the app is still initializing.
    if (flags & (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK
                 | SDL_INIT_GAMECONTROLLER))
        SDL2Circle_InputInit();

    s_initialized |= flags;
    return 0;
}

extern "C" int SDL_Init(Uint32 flags)
{
    return SDL_InitSubSystem(flags);
}

extern "C" void SDL_QuitSubSystem(Uint32 flags)
{
    s_initialized &= ~flags;
}

extern "C" Uint32 SDL_WasInit(Uint32 flags)
{
    return s_initialized & (flags ? flags : ~0u);
}

extern "C" void SDL_Quit(void)
{
    s_initialized = 0;
}

extern "C" void SDL_GetVersion(SDL_version *ver)
{
    ver->major = SDL_MAJOR_VERSION;
    ver->minor = SDL_MINOR_VERSION;
    ver->patch = SDL_PATCHLEVEL;
}

extern "C" const char *SDL_GetRevision(void)
{
    return "circle-libsdl2";
}

extern "C" const char *SDL_GetPlatform(void)
{
    return "Circle";
}
