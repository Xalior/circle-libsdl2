# What works

| SDL2 subsystem | Circle backing |
|---|---|
| Video: fullscreen window, software `SDL_Renderer`, streaming ARGB8888 textures, alpha blending, scaled `SDL_RenderCopy`, render targets | `CBcmFrameBuffer` - double-buffered, vsync page flip. Where the firmware grants one screen instead of two, the finished frame is scaled onto it, on a core of the host's choosing |
| Display/renderer queries (modes, bounds, formats, masks) | single HDMI panel, or the virtual device the application declared for itself |
| Keyboard → SDL events, `SDL_GetKeyboardState`, modifiers | Circle USB HID (raw reports; SDL scancodes *are* USB usage codes). Scancodes are physical positions and do not move with the keyboard layout. Off core 0: USB stays on core 0, events cross by ring |
| Mouse → SDL events, `SDL_GetMouseState`, relative mode, warping | Circle USB mouse (raw reports - no Circle-drawn cursor). A mouse says how far it moved and never where it is, so the library keeps the position itself and clamps it to the window. Off core 0: USB and event synthesis stay on core 0; the position and buttons are held in memory both cores see |
| Joysticks, gamepads and wheels: enumeration, hot-plug, axes, hats, buttons, GUIDs, coarse rumble | Circle's USB gamepad drivers - the generic HID one and the console-specific ones. Off core 0: USB and event synthesis stay on core 0; the readings are held in memory both cores see |
| Game controllers: `gamecontrollerdb.txt` mappings, `SDL_IsGameController`, mapped axes and buttons, controller events | the mapping text is read the way SDL2 reads it, and found by the same joystick GUID SDL2 builds |
| Files as `SDL_RWops` streams, and streams over memory | the I/O service, so an application off core 0 opens a file with the ordinary SDL call |
| Audio: `SDL_OpenAudioDevice` callback API | `CHDMISoundBaseDevice`, ~100 ms hardware queue. The device plays 16-bit signed stereo; an application that asks for something else and does not permit a change gets **what it asked for**, converted at the device boundary, and `obtained` reports its own spec. Permit a change and `obtained` reports the device's instead. Off core 0: your callback fills a ring, core 0's servo task feeds the device |
| Events: queue, `SDL_PumpEvents`, window focus | the per-frame service point: USB pump and scheduler yield on core 0, ring drain and liveness signal off it |
| Timers: `SDL_GetTicks64`, performance counter, `SDL_Delay` | Circle system timer (µs). `SDL_Delay` runs the audio callback while it waits, as every wait in this library does; on core 0 it also yields to the scheduler, and off it spins to a µs-exact deadline, occupying the core for the duration |
| Files: an I/O service callable from any core | FatFs on core 0, marshalled (`SDL2Circle_IO*`) - for applications whose own file layer must not touch the SD card directly |
| Surfaces and pixel formats: `SDL_CreateRGBSurface*`, `SDL_ConvertSurface`, `SDL_ConvertSurfaceFormat`, `SDL_MapRGB`/`SDL_MapRGBA`, `SDL_GetRGB`/`SDL_GetRGBA`, palettes and `SDL_SetPaletteColors`, `SDL_FillRect`, blitting, `SDL_SetColorKey`, lock/unlock, `SDL_ConvertPixels` | in memory. 8-bit paletted surfaces are first-class: a game that renders through a palette does so here without converting anything itself |
| Threads and synchronisation: `SDL_CreateThread`, mutexes, condition variables, semaphores, atomics, thread-local storage | Circle's scheduler and the AArch64 atomics |
| C++ standard library threading: `std::mutex`, `std::recursive_mutex`, `std::condition_variable`, `std::call_once`, `std::thread`, `thread_local` | this library's own implementation of the interface libc++ asks its platform for, built from processor atomics so it is valid on every core. See [C++ threading](THREADING.md) |
| Timers: `SDL_AddTimer`/`SDL_RemoveTimer` | the system timer, serviced from the frame's service point |
| `SDL_image`: `IMG_Load` and its family | PNG decoded here - a DEFLATE decompressor and a PNG reader, because there is no libpng and no zlib. BMP goes to `SDL_LoadBMP_RW` |
| `SDL_mixer`: several sounds at once, channels, volumes, panning, music | above the audio device, mixing into it. Chunks are converted at load, never in the callback |
| Audio conversion: `SDL_BuildAudioCVT`, `SDL_ConvertAudio`, `SDL_LoadWAV_RW`, `SDL_MixAudioFormat` | in memory, through float - every width, either byte order, one or two channels, any rate ratio |
| `SDL_Log` and its family, message boxes | the same ring every other line takes, so an application's own diagnostics are safe to write from its own core |
| `SDL_GetBasePath` / `SDL_GetPrefPath`, clipboard, key names, `SDL_stdinc` strings and maths | in memory, and the card. See [Declaring the base path](DISPLAY.md#declaring-the-base-path) |
| Typed text: `SDL_TEXTINPUT` events, `SDL_StartTextInput`/`SDL_StopTextInput`/`SDL_IsTextInputActive` | the same USB keyboard, read through Circle's keyboard layout - the one the board's `cmdline.txt` names with `keymap=`. A key press sends the key event first and the text second, as SDL does. See [Keyboard layout](INPUT.md#keyboard-layout) |
| Init/error/version/hints | - |

**The framebuffer is 32-bit; the formats an application works in are its own.** The panel is always allocated at 32 bits per pixel and a texture is always stored as ARGB8888, because the presentation path reads a texture's pixels directly and converting during presentation would put that work on the core that must not be delayed.

The application's format is honoured at the edge instead. `SDL_CreateTexture` accepts any format SDL names; `SDL_QueryTexture` answers with the one that was asked for; pixels handed in through `SDL_UpdateTexture` are converted on the way in, and `SDL_LockTexture` hands back a staging buffer in the application's own format which `SDL_UnlockTexture` converts. So an application writes the pixels it believes it is writing, and the cost falls on its own core. Where the format is ARGB8888 there is no staging buffer and no conversion at all.

Surfaces carry no such restriction: any depth, any masks, and 8-bit paletted throughout.

**A texture can be drawn into as well as read from.** `SDL_CreateTexture` accepts `SDL_TEXTUREACCESS_TARGET`, `SDL_SetRenderTarget` aims the renderer at such a texture, and every drawing call then lands in it instead of in the frame; aiming back at the frame is passing null. `SDL_RenderTargetSupported` answers true, and `SDL_GetRendererInfo` reports `SDL_RENDERER_TARGETTEXTURE`. It is the frame's own composition machinery pointed at a different buffer - see [Render targets](DISPLAY.md#render-targets).

## Not yet

Each of these fails with a message saying so rather than quietly doing something else:

- **No MIDI synthesiser.** `Mix_LoadMUS` reads WAV. A MIDI file is a score rather than a recording, and performing one is a sound engine in its own right.
- **No fades in the mixer.** `Mix_FadeInMusic` and its relatives start and stop at volume.
- **No rotation in `SDL_RenderCopyEx`.** Mirroring works, in all three combinations; an angle is refused.
- **No force-feedback device.** The `SDL_Haptic` calls exist and report that there is none, so an application's "rumble if it can" path takes its other branch.
- **No OpenGL, Vulkan or Metal.** The Pi has no bare-metal GPU driver; software rendering is the design, not a temporary measure. The entry points all exist: `SDL_GL_CreateContext` returns null with an error, so a program with an optional accelerated renderer takes its software path, and `SDL_GL_DeleteContext` is a no-op so its shutdown path still runs. They are here because that shutdown path is usually written as `if (ctx) SDL_GL_DeleteContext(ctx)` - dead code that must still link.

  **A window never reports `SDL_WINDOW_OPENGL`, `SDL_WINDOW_VULKAN` or `SDL_WINDOW_METAL`, whatever was asked for**, because there is no such renderer to report. A program that tests the window flag therefore takes its software path straight away, which is the path that works here. Upstream SDL reaches the same outcome by refusing to create the window at all; the window is worth having, so the bit goes instead of the window.
- Controller motion sensors and touchpads, virtual joysticks.
