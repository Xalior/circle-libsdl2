# Examples

Each is a complete bootable kernel exercising one subsystem. They are the library's worked examples and its test harness at the same time — useful as templates, and the way a change is proven before it ships.

- **`examples/gradient`** — animated full-screen gradient (video path)

- **`examples/rendertarget`** — a picture composed into an off-screen texture and then magnified over the whole window, which is how a game gets a fixed low-resolution look on any panel. It checks the render-target contract on the way in — what the renderer reports, what the output size and the viewport become while a target is set, what is read back out of one, and the two things that are refused — and puts every answer on the serial console (render targets)

- **`examples/keyecho`** — scancode display, modifier lights, held-key grid (input)

- **`examples/mouseview`** — the pointer on screen with the button lights, the wheel total and the frame's own relative reading beside it, and a bar per event type so a click that produced no event is visible. It also hands its serial port to the library's input-injection channel, so the pointer can be driven from a terminal — `mouse to 100 100`, `mouse tap left` — when there is nobody at the desk. It carries a patchable-defaults block at image offset 0x800, so a network loader can stamp a switch into the image before it boots, and the build refuses to leave behind an image that lost the block (mouse)

- **`examples/tone`** — 1 kHz sine over HDMI via the callback API (audio)

- **`examples/padview`** — every attached joystick, gamepad and wheel on screen at once: name, GUID, USB IDs, whether the mapping database recognised it, live axis bars, hat and button lights, the mapped controller view where there is one, and a running log of devices arriving and leaving (joystick and game controller)

- **`examples/virtdev`** — an application declaring the display it is to be given, then checking every SDL answer about the display against what it declared, and making each declaration the library refuses so the reason for it is on the log (virtual device)

- **`examples/videocycle`** — the whole video and audio subsystem destroyed and recreated in a loop, at alternating source geometry, while a presentation core keeps running: what a settings menu does on every change, with nobody at the keyboard (core split, present-path lifetime)

- **`examples/dispinfo`** — no SDL at all: a serial-only probe that logs what the firmware reports about the attached display, and what Circle's framebuffer returns for a series of allocation requests. It is where the raw numbers behind the presentation geometry come from

- **`examples/cxxthreads`** — the C++ standard library's threading, run on core 1 where an application runs and where none of it used to work: recursive mutexes, a condition variable woken by a thread the application core created, per-thread `thread_local` storage and its destructors, a timed wait that runs out, and `call_once`. It reports each result by name on the serial console, so a board with no display still says what happened (C++ runtime)
