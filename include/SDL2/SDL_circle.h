//
// SDL_circle.h — Circle-specific host API, beyond the SDL2 surface.
//
// On bare metal the "desktop" is one framebuffer whose size is fixed for
// the life of the system: Circle's CBcmFrameBuffer never releases its GPU
// allocation, so there is no mode switching after the first window exists.
// The hosting kernel decides the size before the application initializes
// SDL; the VideoCore scaler stretches the framebuffer to the panel.
//
#ifndef SDL_circle_h_
#define SDL_circle_h_

#ifdef __cplusplus
extern "C" {
#endif

// Sets the display size reported to the application (and therefore the
// size of the framebuffer its fullscreen window allocates). Must be
// called before the first SDL_CreateWindow; ignored afterwards.
// Default when never called: 1920x1080.
void SDL2Circle_SetDisplaySize(int w, int h);

// Core-0 time accounting over the PMU cycle counter: every nSeconds the
// pump logs the cycle split between the application's own compute and
// the shim's instrumented sections (render, audio, input, yield). IRQ
// cycles land inside whichever section they preempt (no IRQ-entry hook
// exists on this stack). 0 (the default) disables both the reports and
// the counters.
void SDL2Circle_SetPerfInterval(unsigned nSeconds);

#ifdef __cplusplus
}
#endif

#endif /* SDL_circle_h_ */
