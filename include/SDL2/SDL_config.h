/*
  SDL_config.h — circle-libsdl2

  Configuration for bare-metal Raspberry Pi: Circle framework, AArch64,
  newlib libc (via circle-stdlib). This file replaces the SDL_config.h
  from the SDL2 2.32.4 distribution (altered source, per the zlib license
  — see SDL2-LICENSE.txt).
*/

#ifndef SDL_config_h_
#define SDL_config_h_

#include "SDL_platform.h"

/* 64-bit ARM */
#define SIZEOF_VOIDP 8
#define HAVE_GCC_ATOMICS 1

/* newlib provides a full C library */
#define HAVE_LIBC 1
#define STDC_HEADERS 1
#define HAVE_CTYPE_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_WCHAR_H 1
#define HAVE_SYS_TYPES_H 1

#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_GETENV 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_ABS 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_ATOI 1
#define HAVE_ATOF 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOD 1
#define HAVE_SQRT 1
#define HAVE_SQRTF 1
#define HAVE_SIN 1
#define HAVE_SINF 1
#define HAVE_COS 1
#define HAVE_COSF 1
#define HAVE_FLOOR 1
#define HAVE_FLOORF 1
#define HAVE_CEIL 1
#define HAVE_CEILF 1
#define HAVE_FABS 1
#define HAVE_FABSF 1
#define HAVE_POW 1
#define HAVE_POWF 1
#define HAVE_FMOD 1
#define HAVE_FMODF 1
#define HAVE_M_PI 1

/* Everything platform-specific is provided by the Circle backends in
   circle-libsdl2/src/ — no SDL-internal drivers are built. */
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_VIDEO_DRIVER_DUMMY 1
#define SDL_TIMER_DUMMY 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_HAPTIC_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_LOADSO_DISABLED 1

#endif /* SDL_config_h_ */
