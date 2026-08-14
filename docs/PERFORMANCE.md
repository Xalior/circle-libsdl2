# Performance reports

Call `SDL2Circle_SetPerfInterval(10)` and the library prints, every 10 seconds, one line per core that has run instrumented code. It is silent until a host asks for it, and how a host decides to ask - a boot switch, a build option, never - is for the host to decide, not the library:

```
sdl2perf: 60.0 fps c2: awake 41.2% (9885M of 24005M): app 0.0% render 50.5% dma 6.1% vsync 4.1% wait 0.0% serve 0.0% audio 0.1% input 0.0% yield 0.0%
```

The frame rate counts presented frames.

## Reading the report

**`awake` is how busy the core was; the percentages after it divide only that awake time.** The two are separate on purpose, because a processor's cycle counter stops while the core is asleep. A core parked for most of every frame and a core running continuously can print identical percentages - what distinguishes them is `awake`, which is the counted cycles measured against the wall clock, and the wall clock never stops. Read the line as "this core was awake 41% of the time, and here is what it did while it was".

## Categories

- `render` - present-path compute
- `serve` - the hardware core doing another core's work, the call mailbox and the log drain
- `audio` - audio callback and device feed
- `input` - USB pump and event synthesis
- `yield` - time given to other scheduler tasks
- `app` - whatever is left, which is the application itself on its own core

Blocking is reported in parts, because each has different fixes:
- `dma` - waiting for the transfer into the framebuffer to finish
- `vsync` - waiting for the raster to reach the vertical blanking interval
- `wait` - one core waiting on another across the frame mailbox - a wait on software rather than on the display

Each is kept separate from `render` on purpose: at a locked frame rate the blocking absorbs every spare cycle, and combined with `render` it would make the present path look saturated when it is mostly idle.

## Notes

The `awake` figure assumes the processor clock reported at the first report holds for the run. A board that is thermally throttling is changing that clock during the measurement, so treat `awake` as approximate there.

The categories do not overlap. Sections nest - a wait happens inside the present that is waiting - and each one is charged only for the cycles its own body used, with its children's time attributed to them. So the percentages partition the core's cycles and `app` is genuinely what is left.

A single-core build reports core 0 alone, which is the whole machine there. Under the core split each active core reports its own line, which is how a question like "how busy is the presentation core, could it afford filtering" gets a measured answer.

Unarmed, the instrument costs one branch per section. Arming it is `SDL2Circle_SetPerfInterval(seconds)` in `SDL2/SDL_circle.h`, and that is the only way to enable it - the library reads nothing from boot configuration for this.
