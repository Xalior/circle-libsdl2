# circle-libsdl2

An SDL2-compatible shim over the [Circle](https://github.com/rsta2/circle)
bare-metal Raspberry Pi framework. **Not a port of SDL** — a from-scratch
implementation of the subset of the SDL2 API that our consumers actually
call, mapped directly onto Circle devices. Built as the porting layer for
running MAME bare-metal on the Raspberry Pi 4, designed to be reusable for
other bare-metal ports.

| SDL2 subsystem | Circle backing | Status |
|---|---|---|
| Init/error/version/hints | — | done |
| Timer (`SDL_GetTicks64`, perf counter) | `CTimer` system timer (µs) | done |
| Video: window + software renderer + streaming ARGB8888 textures | `CBcmFrameBuffer`, double-buffered, vsync page flip | working |
| Keyboard | Circle USB HID (raw reports → SDL events; scancodes are USB usage codes) | working |
| Events | queue + `SDL_PumpEvents` (also the cooperative-scheduler yield point) | working |
| Audio (callback API) | `CHDMISoundBaseDevice` — app callback runs on the main loop out of `SDL_PumpEvents`, feeding a ~100ms hardware queue | working |
| Mouse/gamepad | Circle USB HID | TBD |

Pi 4 note: HDMI audio exists on **HDMI0 only** (the port next to USB-C);
`config.txt` should carry `hdmi_drive=2` (and `hdmi_force_edid_audio=1` for
displays with shy EDIDs).

Headers in `include/SDL2/` are the official SDL2 2.32.4 headers (zlib
license, `SDL2-LICENSE.txt`) with one substitution: `SDL_config.h` is ours,
configured for AArch64/newlib/Circle.

Host-kernel contract: initialize `CInterruptSystem` and `CTimer` before
calling `SDL_Init`; the shim owns everything else it needs (USB host
controller, framebuffer), so an SDL app is self-contained however it is
booted. Apps that use `std::thread` should run a `CScheduler`;
`SDL_PumpEvents`/`SDL_Delay` yield to it when present.

Build: `make` against a configured
[circle-stdlib](https://github.com/smuehlst/circle-stdlib) tree (see the
Makefile's include). Consumers link `libSDL2.a` ahead of the circle-stdlib
libraries. Test apps live in `test/`.
