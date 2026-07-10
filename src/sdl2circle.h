//
// sdl2circle.h — internal glue between the SDL surface and Circle backends.
// Not installed; consumers see only <SDL2/*.h>.
//
// Contract with the host kernel: CInterruptSystem and CTimer are initialized
// before SDL_Init. Everything else (USB, framebuffer, ...) is owned by the
// shim, so an SDL app is self-contained whether chainloaded or SD-booted.
//
#ifndef _sdl2circle_h
#define _sdl2circle_h

void SDL2Circle_InputInit(void);   // bring up USB (idempotent)
void SDL2Circle_InputPump(void);   // PnP + translate HID reports to events
void SDL2Circle_AudioPump(void);   // run app audio callback into the queue

#endif
