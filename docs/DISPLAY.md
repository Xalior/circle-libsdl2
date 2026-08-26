# Display and video

## Presentation geometry

Resolutions are always involved on a bare-metal Pi, and the library names each of them rather than treating them as one.

**The scanout is the physical display** - what the hardware actually sends over the display cable. `width=` and `height=` in `cmdline.txt` ask the firmware for a display mode, allocating the framebuffer is what sets it, and **the firmware then reports the mode it actually set. That report is the scanout.** It is read from the firmware, never calculated: not from the framebuffer's pitch, not from its size, and not from the width and height Circle returns, which are only the arguments it was constructed with.

**Set neither and the panel keeps its own mode.** The library then asks the firmware for no particular size, which is how you say "whatever the display is already doing", and the firmware allocates the display's own mode. There is no default resolution anywhere in the library - a default would not be a preference, it would be an instruction, because asking for a mode is what sets one. A card whose configuration asks for nothing gets whatever mode the attached display is already using.

**On a Pi 5, either set no display mode at all or set exactly the one the screen is already using.** That board's firmware chooses its display mode before any kernel starts and does not change it afterwards. It still accepts a `width=`/`height=` request, and then reports the requested mode back as though it had applied it, while continuing to send the screen's own mode to the display. This library takes the firmware's report as the truth, so it would describe a mode that is not being displayed, and every measurement derived from it would be wrong.

A Pi 3 or a Pi 4 applies the requested mode and reports it correctly, so neither has this problem.

**The canvas is the virtual display** - the display area the application is given, and the relation between its shape and the scanout's decides the letterboxing. It is **first** settled at the earliest of these moments to occur, in this order - see [Declaring the display](#declaring-the-display):

1. The `--rapi-vfb=WxH` boot switch.
2. `SDL2Circle_DeclareVirtualDevice`, called by the application before `SDL_Init`.
3. The application's first `SDL_CreateWindow` - the window and the canvas are the same rectangle when neither of the above was used.
4. The physical panel size, read from the firmware, when none of the above ever gave one. Last resort only: it is reached exclusively where the library would otherwise refuse to start.

**After that the canvas follows the application.** It is memory rather than hardware, so any size the application sets becomes the canvas, and it may be set again as often as the application likes - see [Changing the canvas](#changing-the-canvas). The panel is the ceiling and it is enforced: the library scales the canvas up onto the glass, so nothing above the panel is offered and nothing above it is allowed.

**`width=`/`height=` and the canvas are settings doing different jobs, and they coexist.** Neither is a fallback for the other. One is asked of the firmware by the operator; the other is settled as above. `width=` and `height=` never set the virtual display, and nothing that sets the virtual display ever sets the physical one.

**The application's own resolution** is whatever it renders at - the canvas, always, whatever `SDL_CreateWindow` reports back (see [Declaring the display](#declaring-the-display) for the one case those differ). It calls `SDL_CreateWindow` and `SDL_RenderCopy` as usual, and the rectangles it passes are canvas coordinates. An application never learns what the physical display is doing.

A frame therefore travels application → canvas → scanout, and **the library composes both steps into a single resampling pass** when the frame is presented - on the presentation core when the core split is active, inline otherwise. The canvas contributes arithmetic, never an intermediate copy.

- **`SDL_RenderCopy` honours its rectangles.** A destination the same size as the source, on a canvas that is the scanout, is still an unscaled blit - the same bytes, on the same path. Anything else resamples.
- **Nearest neighbour, and only nearest neighbour.** Per-axis index tables are built once per geometry and reused; an exact integer ratio skips the tables and replicates. `SDL_HINT_RENDER_SCALE_QUALITY` is stored like any other hint and **has no effect** - `"linear"` is a later phase, not a silent fallback.
- **Fit is the default placement.** The canvas is scaled up as far as it fits, centered, and the remainder of the scanout stays black. Put `canvas=stretch` in `cmdline.txt` to fill the scanout instead, without preserving the aspect ratio.
- **A canvas the same size as the scanout is mapped through untouched.** The placement is then the identity, and a rectangle crosses to the panel with no arithmetic done to it at all. An application that takes the largest entry of [the mode list](#the-mode-list) gets exactly this, which is why the list is ordered largest first.
- **A frame is composed in ordinary memory, never directly in the framebuffer.** The framebuffer is uncached, and a scaler writing it directly pays that cost once per pixel: on a Pi 4, 26.1 ms for a 1280x720 frame against 1.4 ms into ordinary memory. So a present is composed off-screen and the finished frame is copied to the framebuffer in whole rows - 6.0 ms for the same frame - which is also what keeps the picture whole, because the screen is written by that one copy and is never visible mid-composition.
- **The copy to the screen runs on the DMA engine where it can.** When the firmware grants enough memory for two screens, presenting is a page flip and the copy goes to the half being panned to. When it grants only one - a Pi 5 does - the finished frame goes to the granted surface itself, which is the most expensive thing the presentation core does. So the library gives that copy to a DMA channel and returns without waiting for it, scaling the next frame into a second buffer while the transfer runs. One frame is in flight at a time. If no DMA channel is free, the CPU does the copy exactly as before.
- **There is one framebuffer grant on the board, and everything that draws shares it.** Asking the firmware for it is what sets the display mode, so two askers with two different requests would be two modes. Output is on the screen from boot on every board ([LOGGING.md](LOGGING.md)), which makes that grant during the machine's own bring-up, with the same request an application's first window would have made; the window then adopts it. The console stops drawing the moment an application **creates its window** - not merely when it calls `SDL_Init`, which brings video up but does not yet take the display - so the console and the application never hold the framebuffer at the same time. Destroying that window gives the screen back.
- **The present path is built once and reused.** Its buffers and its DMA channel are sized by the framebuffer the firmware granted, and that grant is made once and kept for as long as the machine runs, so a second window adopts the same one. An application may destroy its window, renderer and textures and create new ones as often as it likes - which is what a settings menu does whenever a video setting changes - and none of it is allocated again. There are only a few DMA channels on the board and the sound device needs one too.

The library logs the whole chain once at startup and once per distinct geometry, so a serial console tells you what happened without guessing:

```
sdl2video: scanout 1920x1080 (firmware reported), canvas 720x576 (declared virtual device)
sdl2video: canvas 720x576 on scanout 1920x1080: fit -> 1350x1080+285+0
sdl2video: granted 1080 rows < 2160: shadow-buffered present
sdl2video: present: dma copy, channel 11, 8294400 bytes, double-shadowed
sdl2video: copy src 320x224 -> canvas 720x504+0+36 -> scanout 1350x945+285+67 (nearest)
```

The `(declared virtual device)` on the first line names which of the four sources in [Declaring the display](#declaring-the-display) settled the canvas - `--rapi-vfb switch`, `declared virtual device`, `first window created` or `physical panel size`.

The `present:` line names the path that is actually in use - `dma copy` or `cpu copy`, and the reason when it is the latter.

**Making a renderer is logged too**, always, because which flags a caller asked for and what it was given is the first thing anyone asks when the picture is wrong:

```
sdl2video: renderer created: 1280x720, flags 0x6, vsync on
```

**Every window and mode call puts a line on the log at debug level**, so `--rapi-debug-uart` shows what an application asked for and what it got, in the order it asked:

```
sdl2video: window created: asked 1280x720, given 1280x720, canvas 1280x720
sdl2video: SetWindowDisplayMode 640x480, canvas 1280x720
sdl2video: SetWindowFullscreen flags 0x1, canvas 640x480
sdl2video: SetWindowSize 800x600, canvas 640x480
sdl2video: window destroyed: 800x600, canvas 800x600
```

Each line names the canvas **as it was when the call arrived**, so reading down the list shows the size the previous call left behind and the size this one is asking for.

**The answers are metered too**, on the same switch. What an application is told is what it sizes itself from, so a program that asks for a strange size is usually answering one of these:

```
sdl2video: GetDesktopDisplayMode -> 640x480
sdl2video: GetCurrentDisplayMode -> 640x400
sdl2video: GetDisplayBounds -> 640x480 (canvas 640x400)
sdl2video: GetNumDisplayModes -> 24 (scanout 1920x1080)
sdl2video: GetDisplayMode[0] -> 1920x1080
sdl2video: GetClosestDisplayMode 800x600 -> 800x600
sdl2video: GetWindowSize -> 640x400 (canvas 640x400)
sdl2video: GetRendererOutputSize -> 640x400
sdl2video: SetWindowMouseRect 544x332+0+0, canvas 640x400 (not applied)
sdl2video: SetWindowGrab on
```

A query is asked many times a frame, so each site remembers what it last answered and writes only when the answer changes. `SDL_SetWindowMouseRect` is in the list although it is not a query: it is accepted and not acted on, because the pointer is already confined to the one screen, and the line is what makes visible that a program believes the pointer is confined to something smaller.

### Window flags

`SDL_GetWindowFlags` describes **the machine, not the request**. A flag an application passed to `SDL_CreateWindow` is reported back only where this window can honour it; reporting it otherwise tells the asker its own question back, and a game branches on the answer.

- **`SDL_WINDOW_INPUT_FOCUS` is always set.** There is one window and no window manager to take focus away from it. A flag that is never set reads exactly like a flag that is false, and a game that believes it has lost focus pauses, stops drawing or drops input - a black screen with a clean log.
- **`SDL_WINDOW_SHOWN` is always set, and `SDL_HideWindow` does not clear it.** The surface is on the glass and cannot leave it, so `SHOWN` is the truth. `SDL_WINDOW_HIDDEN` and `SDL_WINDOW_MINIMIZED` are never reported.
- **`SDL_WINDOW_OPENGL`, `SDL_WINDOW_VULKAN` and `SDL_WINDOW_METAL` are never reported.** See the accelerated-graphics note in [FEATURES.md](FEATURES.md).
- **`SDL_WINDOW_MOUSE_FOCUS` is asked, not stored**, because a USB mouse can arrive or leave long after the window is made. It answers the same question `SDL_GetMouseFocus` answers, so the flag and the function cannot drift apart - which is the rule for any flag whose truth can change after the window exists.

## Render targets

A texture created with `SDL_TEXTUREACCESS_TARGET` can be drawn into. `SDL_SetRenderTarget` aims the renderer at it, every drawing call then lands in that texture's pixels instead of in the frame, and passing null aims the renderer back at the frame. The texture is then a source for `SDL_RenderCopy` like any other. That is how a game composes its picture once, at the fixed low resolution its artwork was made for, and magnifies the finished image in a single step - the whole display chain above still applies to that one copy.

**It is the same machinery.** A frame is composed by one executor writing fills and blits into a buffer, and a render target is that same executor writing into the texture instead, at the texture's own pitch: the same blending, the same nearest-neighbour scaling, the same clipping.

- **A target is drawn into as each call is made.** Drawing aimed at the frame may be held back, to cross to the presentation core as a list of commands rather than as pixels; nothing aimed at a target ever is, because a target's pixels are not a frame on its way anywhere - the application may copy from them on its very next call, so they have to be there.
- **Nothing a target does reaches the panel by itself.** `SDL_RenderPresent` presents the frame whatever the renderer is aimed at, and a target's pixels arrive on the screen only when something copies that texture into the frame.
- **Every rectangle is placed against whatever is being drawn into.** While a target is set, `SDL_GetRendererOutputSize` answers with the texture's size, an absent destination rectangle means the whole texture, `SDL_RenderClear` clears the whole texture, and nothing is drawn outside it. `SDL_RenderReadPixels` reads back from the target as well.
- **Each target keeps its own coordinate state** - viewport, clip rectangle, logical size, integer scale and render scale - as SDL2 does. A viewport set while a texture is the target belongs to that texture and is still there the next time it is the target; the window's own is untouched by it, and comes back when the renderer is aimed at the frame again.
- **A blended draw into a target composes the destination alpha too**, as desktop SDL does: a clear with an alpha of zero stays transparent, an unblended copy carries its source's alpha through unchanged, and a `SDL_BLENDMODE_BLEND` copy leaves each pixel it touches at `srcA + dstA * (1 - srcA)` rather than discarding what was there. A target used as a translucent overlay keeps its transparency wherever the drawing onto it means it should.
- **A target cannot be copied into itself.** `SDL_RenderCopy` and `SDL_RenderCopyEx` refuse it and say so, because the source and the destination would be one buffer that the blitter reads as it writes. Upstream SDL does not define the result either; refusing is so that the reason is on the log rather than on the screen.
- **A target texture cannot be locked.** `SDL_LockTexture` is for streaming textures, here as upstream. `SDL_UpdateTexture` works on a target, and so does drawing into it.
- **`SDL_SetRenderTarget` refuses a texture that was not created with `SDL_TEXTUREACCESS_TARGET`**, and destroying the texture the renderer is aimed at aims it back at the frame. Both match upstream SDL.
- **Under the core split, drawing into a target the presentation core is still reading copies it aside first.** A texture keeps several stores so that the application never waits for the presentation core, and the rule that decides which one it may write is the same for a target as for any other texture. The difference is that a target's existing content matters, so the content moves with it - one texture-sized copy, and only when a frame in flight still names the store.

`examples/rendertarget` is a bootable example of all of this - see [Examples](EXAMPLES.md).

## Declaring the display

**Nothing is required.** An application that calls `SDL_Init` and then `SDL_CreateWindow` with no further ceremony gets a canvas sized from that window's own width and height - the window and the canvas are the same rectangle. `SDL_Init` itself never refuses for want of a display size; a program may bring video up and never create a window at all.

A display query that arrives before any window exists, or before the switch or a declaration has said anything, is not refused either: the library falls back to the physical panel size, read from the firmware - the same figure the scanout log line above comes from, asked for the same way. Only when the firmware itself has nothing to report does the original refusal stand.

These can override that default, and they are settled in this order - the first one present wins outright, and neither is required for the other to work:

1. **The `--rapi-vfb=WxH` boot switch**, handled by the library like every other `--rapi-` switch - see [Boot switches](#boot-switches). It is read from the boot argument block before `SDL_Init` runs, so it is known before any window can exist, and it wins over a declaration as well as over a window's own size.
2. **`SDL2Circle_DeclareVirtualDevice`**, called by the application before `SDL_Init`:

   ```c
   #include <SDL2/SDL_circle.h>

   if (SDL2Circle_DeclareVirtualDevice(32, 800, 450) != 0)
       fprintf(stderr, "%s\n", SDL_GetError());
   ```

   This call is unchanged from before and remains entirely optional. It loses to the switch when both are present.

Whichever of the four settles it, that size is the **vFB**: the virtual display this machine gave the application. It is recorded once and never moves again, whatever the application later does to its canvas.

SDL keeps two display sizes apart on every platform, and so does this library:

| call | answers with | because |
|---|---|---|
| `SDL_GetDesktopDisplayMode` | the vFB | the desktop is the display an application was given, and a game going fullscreen at 640x400 does not shrink the monitor it did it on |
| `SDL_GetDisplayBounds` | the vFB | SDL's other spelling of the same question |
| `SDL_GetDisplayUsableBounds` | the vFB | the usable area is the display minus what the system reserves - a menu bar, a dock, a taskbar. There is no window manager here and nothing reserves any of the panel, so it is the whole of it |
| `SDL_GetCurrentDisplayMode` | the canvas | that genuinely is the mode in effect |

**Neither is the panel.** The application never learns the real output resolution from either of them - it draws in the canvas, and the placement rules above put that onto the physical screen. What it does learn is the range of sizes it may ask for, which is [the mode list](#the-mode-list).

**Answering the desktop with the canvas is a ratchet, and it is why these are two numbers.** An application that sets a small mode would afterwards be told its desktop was that small, so it could never ask for anything larger again for the rest of the run. A program that reads the desktop, sets a mode inside it, and reads the desktop again later - which is ordinary SDL - would find the screen had shrunk underneath it every time.

**The switch and the window can disagree on purpose.** With `--rapi-vfb=WxH` set, `SDL_CreateWindow` still returns a window of the size the application asked for - `SDL_GetWindowSize` answers honestly - but the application draws into a canvas of the switch's size regardless, exactly as `SDL_GetRendererOutputSize` and `SDL_GetWindowSizeInPixels` report it. The two sizes are then deliberately different, and the scanout scaling above is what reconciles the canvas with the panel; nothing reconciles the window's reported size with the canvas, so a program meant to run under the switch should read its drawing size from the renderer or the display mode, not from `SDL_GetWindowSize`. Under a declaration, or with neither override, the window and the canvas are always the same rectangle, so this distinction does not arise.

- **A declaration is settled once; the canvas is not.** A `SDL2Circle_DeclareVirtualDevice` call arriving after the canvas has settled is refused, and so is a second call. The canvas itself goes on following the application for as long as it runs - see [Changing the canvas](#changing-the-canvas).
- **32 bits per pixel, and nothing else.** The framebuffer is allocated at 32 bits and streaming ARGB8888 is the only texture format, so another depth is refused rather than quietly rounded to this one. Width and height must both be above zero, for the switch as well as for the declaration.
- **`SDL2Circle_DeclareVirtualDevice`'s return value reports the outcome.** Zero means accepted; -1 means refused, with `SDL_GetError` saying which of the above was not met. A refused declaration changes nothing, and an earlier accepted one still stands.
- **Either states the virtual display, not the physical one.** The mode the panel is driven at remains the operator's decision, asked for in `config.txt` and `cmdline.txt` and granted, or not, by the firmware. Neither the switch nor the declaration asks the firmware for anything; each says what the application is to be shown, and the library scales.

### The mode list

**The canvas is memory, not hardware.** Any size up to the panel can be allocated, and the presentation core scales whatever it is handed onto the glass. The panel is the ceiling, because this library is built to scale a canvas **up**: a canvas larger than the glass is being scaled the other way, nearest neighbour then discards source pixels rather than resampling them, and the application core pays for every one of those pixels in the canvas-sized copy it makes each frame before they are thrown away.

That is what an application is told:

- **Index 0 is always the panel's own mode**, whether or not the table names it. The table lists sizes that were standard on desktop hardware, and plenty of panels are not one of them: on an 800x480 screen the widest table entry that fits is 720x480, because 856x480 is too wide and 800x600 too tall, so the display's own resolution would never be offered at all. An application taking the top of the list would then choose a size that has to be scaled, while the one size needing no scaling was the panel's and was never shown.
- **`SDL_GetNumDisplayModes` and `SDL_GetDisplayMode` then enumerate the standard sizes that fit inside the panel**, largest first as SDL orders them. The table runs from 1920x1080 down to 160x120 and carries the sizes a game is likely to look for - 1280x720, 1024x768, 800x600, 720x576, 640x480, 320x240, 320x200 and the rest. Where the panel's own mode is in the table it is not listed twice. Every mode is reported at 32 bits per pixel in `SDL_PIXELFORMAT_ARGB8888`, which is the only format there is.
- **The list is filtered against the scanout**, so a panel running at 1280x720 does not offer 1920x1080. Until the scanout is known there is nothing to measure against and the count is zero.
- **`SDL_GetClosestDisplayMode` answers a request that fits with itself.** The library would allocate exactly that canvas if the application went on to set it, so offering something else would refuse a mode that works. A request larger than the panel comes back as the panel.
- **A size that fits but is not in the table is still honoured if the application simply sets it.** The table is what an application finds when it enumerates; it is not a list of the only sizes that work.
- **A canvas larger than the panel is clamped to it**, whether it was asked for through the list or set directly. The ceiling is not just an absence from the list, it is enforced on the one path every size change takes. Clamped rather than refused, because `SDL_SetWindowSize` returns nothing and an application cannot be told: it reads the size back through `SDL_GetWindowSize`, `SDL_GetRendererOutputSize` or the current display mode, all of which report what it actually got. That is what SDL does on a desktop when a mode is not available. The clamp is stated on the log at notice.

This replaces reporting one mode - the canvas - which is what the library did before the canvas followed the application. That made every consumer name a size in its own kernel just to put that size in the list.

### Changing the canvas

**The canvas is the application's drawing area, so a program that changes resolution has changed exactly that.** Four calls move it, and all four take the same path:

| call | what moves the canvas |
|---|---|
| `SDL_SetWindowSize` | the size passed |
| `SDL_SetWindowDisplayMode` | the mode's size, which is also recorded against the window |
| `SDL_SetWindowFullscreen` | the recorded mode, for `SDL_WINDOW_FULLSCREEN` only |
| `SDL_CreateWindow` | the new window's own size, when a canvas already exists |

`SDL_WINDOW_FULLSCREEN` means "drive the display at the mode set for this window", so it is a size change. `SDL_WINDOW_FULLSCREEN_DESKTOP` means "cover the desktop at the size you have", which changes nothing here, because this display is only ever fullscreen and the canvas already covers it. The mode is applied at `SDL_SetWindowFullscreen` as well as at `SDL_SetWindowDisplayMode`, so the order a program calls them in does not matter.

An emulator whose guest changes mode - DOS going from text to VGA and back - is the case this exists for. The alternative is a canvas fixed at whatever the first mode happened to be, with every later mode letterboxed inside it.

**A program may destroy the window and build another.** The canvas then takes the new window's size, which is what a settings menu does when a video option changes: destroy the renderer, the textures and the window, and create them again at the new resolution. The framebuffer grant, the present path, its buffers and its DMA channel all belong to the grant rather than to the window, so none of it is allocated again and nothing is stranded.

**Under `--rapi-vfb` a later `SDL_CreateWindow` does not move the canvas.** The switch fixes it, and the window is answered honestly about its own size instead. The other three calls in the table are not guarded that way, so a program running under the switch can still move the canvas by setting a size or a mode.

What a change of size does, in order:

1. **It waits for the presentation core.** The canvas size and the placement are read by that core as it executes a frame, and `SDL2Circle_PresentPost` returns as soon as the command list has been copied out - not when the frame has been drawn. So the change quiesces the present path first. Without that wait a frame authored under the old canvas is mapped under the new placement, putting the picture on the glass at a size neither geometry asked for, and the canvas buffers it reads from are freed under it.
2. **It allocates before it releases.** Both new canvas buffers are taken first, so a failure leaves the window at the size it had rather than half resized, and says so on the log.
3. **It refits the window surface in place.** A program is entitled to keep what `SDL_GetWindowSurface` handed it, and several do, in a global they set once. So the surface object survives: its pixels are replaced and its dimensions rewritten, and a pointer taken before the change still describes the surface after it. Freeing that object instead would leave a program reading through freed memory, which turns a resize into a fault somewhere else entirely.
4. **It works out the placement again**, from the new canvas against the same scanout. The panel is untouched; only the source size and the rectangle it is placed in change.
5. **It clears the whole display.** The letterbox is outside the canvas, so no rectangle an application draws can ever reach it, and the borders of the previous placement would otherwise stay on the screen. Every back buffer is painted black.
6. **It pushes `SDL_WINDOWEVENT_SIZE_CHANGED`** carrying the new width and height.

The change costs one frame's wait, and a mode change is rare.

### Matching the virtual display to the physical one

**Where the numbers come from is for the application to decide, and only the application.** A build constant, a settings file, an option of its host kernel's own, a value read from a network port. The library offers no way to ask what the physical display is; the one exception is the fallback in [Declaring the display](#declaring-the-display), which reads it for itself, but only as a last resort when nothing else ever named a canvas - never as a substitute for an application's own choice.

So an application that wants its virtual display to **match** the panel determines the physical size for itself and passes it in - which takes only the few lines below, using Circle's public property tags, and needs nothing from this library at all:

```c
#include <circle/bcmpropertytags.h>

CBcmPropertyTags Tags;
TPropertyTagDisplayDimensions Dim;
memset(&Dim, 0, sizeof Dim);
if (Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
    && Dim.nWidth != 0 && Dim.nHeight != 0)
{
    SDL2Circle_DeclareVirtualDevice(32, (int) Dim.nWidth, (int) Dim.nHeight);
}
```

**`examples/gradient`, `examples/keyecho`, `examples/mouseview`, `examples/padview`, `examples/paletted`, `examples/rendertarget`, `examples/tone` and `examples/videocycle` each do exactly this** - every one carries the query in its own kernel source rather than sharing a helper, so each stands alone as a complete worked example. `examples/videocycle` shows the variation an application off core 0 needs: the firmware mailbox belongs to core 0, so its host kernel asks and declares before the application core is released.

**`examples/virtdev` is the opposite demonstration** - it declares a size matching nothing on the board, because the virtual display is whatever the application says it is and need not resemble the hardware.

`examples/virtdev` is a bootable example of all of this - see [Examples](EXAMPLES.md).

## Declaring the base path

SDL gives an application two directories: the one it was installed in (`SDL_GetBasePath`) and one it may write settings and saved games into (`SDL_GetPrefPath`). On a desktop SDL works both out for itself, from where the running program came from and from the user's home directory.

Neither question has an answer here. The payload was chain-loaded over a wire or started from a card; there is no program image to locate, no user and no home directory. Where an application's files were put is a decision somebody made when they built the card, and the only party that knows it is the one embedding this library. So it states it, once, before `SDL_Init`:

```c
SDL2Circle_DeclareBasePath("/games/example");
```

**The library learns nothing else from this.** It stores the string, hands it back from `SDL_GetBasePath`, and composes `SDL_GetPrefPath` below it. It does not read the path, does not check that it exists, and carries no default for any particular application - it cannot know one is there.

The declaration is fixed, on the same terms as the virtual display: accepted once, before anything has asked for a path, and refused afterwards. Both functions return a string the caller releases with `SDL_free`, and both end in a separator, because that is SDL's contract and applications append to the result without checking.

**Not declaring one is not an error.** The answer is then the directory the program is running in, which a host kernel has already entered before starting the application - that is where the card keeps that program's files, so it is the right answer and the one thing this library can establish for itself. It is stated once on the log.

Declaring one is still worth doing, because it is the only way to name somewhere other than where the program was started from.

This used to answer `/`, and that is worth knowing if you meet an older image: `SDL_GetPrefPath` composes `<base><app>/`, so an undeclared base made `/<app>/` and every setting and saved game a program wrote landed in a directory of its own at the root of the card - one per program, none of them beside that program's files.

## Boot switches

A boot argument block sits at a fixed offset inside the kernel image, and a loader writes a plain argument string into it before pushing the image - so a setting can ride a boot without anything being rebuilt. The same write can happen earlier, into a built image on disk rather than over the wire: `tools/stamp-bootargs` checks the block's magic before writing, refuses an image that was not linked against `sdl-app.ld`'s reserved space, and writes Capacity, Length and Text exactly as this library reads them back. A consumer that wants a switch to ride every boot with nothing passed at boot time - a virtual display size, say - stamps it into the image once, at build time, instead of relying on a loader to write it each time.

**This library reads that block itself**, and acts on the switches that describe what it does. An application does not forward them, is never asked for them, and cannot fail to pass them on.

| switch | effect |
|---|---|
| `--rapi-debug-uart` | types bytes arriving on the serial console into the machine as SDL key events, and prints the library's own debug lines. Takes **no value** |
| `--rapi-perf=N` | a performance report every N seconds |
| `--rapi-vfb=WxH` | the virtual framebuffer size - see [Declaring the display](#declaring-the-display). Wins over `SDL2Circle_DeclareVirtualDevice`, the first window's own size and the physical panel fallback |

An application still reads the same block for its own arguments, and still strips every `--rapi-` switch before its program sees them. Those are its arguments; reading the block twice is harmless, because nothing here writes to it.

Serial key injection needs a serial device to read, and the library arms itself with one: the same device it already holds for the console and standard output (`src/console.cpp`), found the moment that device exists and before anything can pump it. **A kernel does nothing** - the switch alone is enough, on any kernel built against this library, whether or not it has heard of injection at all.

A kernel that wants injection to read a *different* device - one other than the console's own - still can, as an override:

```c
SDL2Circle_SetInjectSerial (&m_Serial);   // reads this device instead
```

It must lend a device it already owns rather than construct one, because a second device on the same slot halts the board inside its constructor. Whether anything is injected through either device is the library's decision, taken from the switch it found for itself: armed and a device found, the log names the device; armed and no device anywhere, the log says so as an error.

Anything that decides how the kernel starts stays with the kernel, and whether the core split exists at all is the example: `ARM_ALLOW_MULTI_CORE`, a compile-time choice in the world's own configuration, is what `SDL2Circle_SplitInit` tests with `#ifdef` (`src/split.cpp`), and arming it is the host kernel's own call to that function. No boot argument is consulted, nothing here is read from `cmdline.txt`, and there is no switch for it.
