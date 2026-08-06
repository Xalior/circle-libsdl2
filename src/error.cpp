//
// error.cpp — SDL_GetError / SDL_SetError / SDL_ClearError
//
#include <SDL2/SDL.h>
#include <cstdarg>
#include <cstdio>

static char s_error[512];

extern "C" int SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
    return -1;
}

extern "C" const char *SDL_GetError(void)
{
    return s_error;
}

extern "C" void SDL_ClearError(void)
{
    s_error[0] = '\0';
}

extern "C" char *SDL_GetErrorMsg(char *errstr, int maxlen)
{
    snprintf(errstr, maxlen, "%s", s_error);
    return errstr;
}

// SDL's internal error reporter, and the one every SDL_OutOfMemory() and
// SDL_Unsupported() in the headers expands into. Those are macros, so a call
// site can be anywhere — including inside another library's headers — and
// this has to exist for the archive to be self-contained.
//
// It unconditionally returns -1, which is what lets a caller write
// `return SDL_OutOfMemory();` from a function returning int.
extern "C" int SDL_Error(SDL_errorcode code)
{
    switch (code)
    {
    case SDL_ENOMEM:      SDL_SetError("Out of memory"); break;
    case SDL_EFREAD:      SDL_SetError("Error reading from datastream"); break;
    case SDL_EFWRITE:     SDL_SetError("Error writing to datastream"); break;
    case SDL_EFSEEK:      SDL_SetError("Error seeking in datastream"); break;
    case SDL_UNSUPPORTED: SDL_SetError("That operation is not supported"); break;
    default:              SDL_SetError("Unknown SDL error"); break;
    }
    return -1;
}
