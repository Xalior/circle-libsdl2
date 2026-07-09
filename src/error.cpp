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
