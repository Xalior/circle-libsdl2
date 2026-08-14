//
// gl.cpp - the accelerated-graphics APIs, answering that there are none.
//
// There is no OpenGL here, no Vulkan and no Metal, and there is no plan for
// one: the Pi has no bare-metal GPU driver, so software rendering is the
// design rather than a stage on the way to something else. Nothing in this
// file draws anything.
//
// This exists because a great many SDL2 games are built with an optional
// accelerated renderer and a software fallback, and their shutdown path is
// written to cope with the fallback having been taken:
//
//     if (gl_context)
//         SDL_GL_DeleteContext(gl_context);
//
// That branch is dead - the context is null, because the game never made one
// - but the call still has to resolve for the program to link. Every such
// symbol is answered once, here.
//
// A game must be able to find out there is no accelerated renderer, and must
// never be led to believe there is one:
//
//   - anything that would create a context or surface fails, and says why.
//     A game then takes the path it would take on any machine without
//     acceleration, which is the path that works here.
//   - anything that would destroy one does nothing, and is right to: there
//     was never anything to destroy, and a shutdown path must not fail.
//   - anything that would query one answers "nothing" or fails, so a game
//     testing for support gets a straight no.
//
// None of them succeeds quietly: a game told it has a context and drawing
// through it would produce nothing on screen, with no error anywhere to
// explain a black display.
//
#include <SDL2/SDL.h>

// ---------------------------------------------------------------------------
// OpenGL
// ---------------------------------------------------------------------------

extern "C" int SDL_GL_LoadLibrary(const char *)
{
    return SDL_SetError("SDL_GL_LoadLibrary: there is no OpenGL on this "
                        "machine");
}

extern "C" void SDL_GL_UnloadLibrary(void) {}

extern "C" void *SDL_GL_GetProcAddress(const char *proc)
{
    SDL_SetError("SDL_GL_GetProcAddress: there is no OpenGL on this machine, "
                 "so `%s` cannot be resolved", proc ? proc : "");
    return nullptr;
}

extern "C" SDL_bool SDL_GL_ExtensionSupported(const char *)
{
    return SDL_FALSE;
}

extern "C" void SDL_GL_ResetAttributes(void) {}

// Attributes are the settings a context would be made with. Accepting them
// costs nothing and refusing them would make a game fail before it reaches
// the context call that tells it the real answer - so the refusal happens
// once, at SDL_GL_CreateContext, where a game is looking for it.
extern "C" int SDL_GL_SetAttribute(SDL_GLattr, int) { return 0; }

extern "C" int SDL_GL_GetAttribute(SDL_GLattr, int *value)
{
    if (value)
        *value = 0;
    return 0;
}

// The refusal that matters. A null context with the error set is what every
// SDL2 game's "can I have acceleration?" test is written to read.
extern "C" SDL_GLContext SDL_GL_CreateContext(SDL_Window *)
{
    SDL_SetError("SDL_GL_CreateContext: there is no OpenGL on this machine; "
                 "use the software renderer");
    return nullptr;
}

// A shutdown path must not fail, and there was never a context to delete.
extern "C" void SDL_GL_DeleteContext(SDL_GLContext) {}

extern "C" int SDL_GL_MakeCurrent(SDL_Window *, SDL_GLContext)
{
    return SDL_SetError("SDL_GL_MakeCurrent: there is no OpenGL context");
}

extern "C" SDL_Window *SDL_GL_GetCurrentWindow(void)
{
    SDL_SetError("SDL_GL_GetCurrentWindow: there is no OpenGL context");
    return nullptr;
}

extern "C" SDL_GLContext SDL_GL_GetCurrentContext(void)
{
    SDL_SetError("SDL_GL_GetCurrentContext: there is no OpenGL context");
    return nullptr;
}

extern "C" int SDL_GL_SetSwapInterval(int)
{
    return SDL_SetError("SDL_GL_SetSwapInterval: there is no OpenGL context");
}

extern "C" int SDL_GL_GetSwapInterval(void) { return 0; }

// Nothing was drawn through a context, so there is nothing to put on screen.
// Silent rather than failing: it returns void, so a game cannot be told, and
// a game reaching here has already been told at SDL_GL_CreateContext.
extern "C" void SDL_GL_SwapWindow(SDL_Window *) {}

extern "C" int SDL_GL_BindTexture(SDL_Texture *, float *, float *)
{
    return SDL_SetError("SDL_GL_BindTexture: there is no OpenGL context");
}

extern "C" int SDL_GL_UnbindTexture(SDL_Texture *)
{
    return SDL_SetError("SDL_GL_UnbindTexture: there is no OpenGL context");
}

// ---------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------

extern "C" int SDL_Vulkan_LoadLibrary(const char *)
{
    return SDL_SetError("SDL_Vulkan_LoadLibrary: there is no Vulkan on this "
                        "machine");
}

extern "C" void SDL_Vulkan_UnloadLibrary(void) {}

extern "C" void *SDL_Vulkan_GetVkGetInstanceProcAddr(void)
{
    SDL_SetError("SDL_Vulkan_GetVkGetInstanceProcAddr: there is no Vulkan on "
                 "this machine");
    return nullptr;
}

extern "C" SDL_bool SDL_Vulkan_GetInstanceExtensions(SDL_Window *,
                                                     unsigned int *count,
                                                     const char **)
{
    if (count)
        *count = 0;
    SDL_SetError("SDL_Vulkan_GetInstanceExtensions: there is no Vulkan on "
                 "this machine");
    return SDL_FALSE;
}

// The surface type is Vulkan's own, and naming it would mean including
// Vulkan's headers, which are not here either. It is taken as an opaque
// pointer, which is what it is on every platform.
extern "C" SDL_bool SDL_Vulkan_CreateSurface(SDL_Window *, void *, void *)
{
    SDL_SetError("SDL_Vulkan_CreateSurface: there is no Vulkan on this "
                 "machine");
    return SDL_FALSE;
}

// ---------------------------------------------------------------------------
// Metal
// ---------------------------------------------------------------------------

extern "C" SDL_MetalView SDL_Metal_CreateView(SDL_Window *)
{
    SDL_SetError("SDL_Metal_CreateView: there is no Metal on this machine");
    return nullptr;
}

extern "C" void SDL_Metal_DestroyView(SDL_MetalView) {}

extern "C" void *SDL_Metal_GetLayer(SDL_MetalView)
{
    SDL_SetError("SDL_Metal_GetLayer: there is no Metal on this machine");
    return nullptr;
}
