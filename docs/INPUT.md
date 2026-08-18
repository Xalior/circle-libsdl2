# Input: keyboard, mouse, joysticks

## Keyboard layout

A keyboard reports positions, and what is printed on the key at a position depends on which country the keyboard was sold in. SDL keeps those two things apart and so does this library.

**A scancode is a position, and it never moves with the layout.** The scancodes in `SDL_KEYDOWN` and `SDL_KEYUP`, and the array `SDL_GetKeyboardState` returns, are USB HID usage codes exactly as the keyboard sent them. A game that binds the key to the left of Z gets the same key on a British keyboard, a German one and an American one. This is what makes a set of controls portable between boards.

**Typed text is the layout's business, and comes from Circle's.** The `SDL_TEXTINPUT` event carries the character the key actually prints, read through the layout the board's `cmdline.txt` selects:

```
keymap=UK
```

Circle carries `US`, `UK`, `DE`, `ES`, `FR`, `IT` and `DV`. **The names are uppercase and matched exactly, and a card that names none of them - or names one in the wrong case - gets German.** That is Circle's built-in default, and it is chosen silently: no warning, no log line. It presents as individual keys printing the wrong character rather than as a setting that was ignored, which makes it look like faulty hardware.

So on a board set to `UK`, shift-2 types `"`, shift-3 types `£` and the key beside Enter types `#`; on `DE`, the Y and Z positions swap the letters they print. The text is UTF-8, as SDL requires - a character outside ASCII, such as `£`, arrives as the several bytes UTF-8 spells it with.

**AltGr types; the other modifiers do not.** A key held with control, with the left alt, or with a GUI key is a command rather than text, and produces no `SDL_TEXTINPUT` - SDL's own behaviour. The right alt is AltGr, which on the European layouts is a third level holding characters of its own rather than a command modifier, so it produces text where the layout defines some. The US layout defines none, so on a US board AltGr types nothing.

**The keycode (`keysym.sym`) does not follow the layout**, although desktop SDL's does. It stays what a US keyboard would report, for the same reason scancodes stay physical: applications bind their actions to keycodes, out of configuration files and out of compiled-in defaults, and a keycode that moved with `keymap=` would silently rebind a game's controls on any board not set to `us`. `SDL_GetKeyFromScancode` and `SDL_GetScancodeFromKey` answer accordingly, and remain each other's inverse. An event's `keysym.sym` is the same answer everywhere except the keypad, whose keys have two meanings - see [The keypad has two faces](#the-keypad-has-two-faces).

**The keypad types the same characters whatever the layout says.** Every layout prints the same things on it, so the keypad is not routed through the layout. What it does depend on is num lock - see [The keypad has two faces](#the-keypad-has-two-faces).

## Lock keys

**Caps lock, num lock and scroll lock are states, not held keys.** Each is on or off between presses, and the key that sets it is up almost all of the time the lock is on. SDL carries all three in the same modifier word as shift and control - `KMOD_CAPS`, `KMOD_NUM` and `KMOD_SCROLL` - so `SDL_GetModState` and the `keysym.mod` of every key event report whether the lock is on, never whether the key is down.

**The state changes on the press**, as it does on a machine with a lamp on the keyboard: the `SDL_KEYDOWN` for the caps lock key already carries the new `KMOD_CAPS`, and so does the `SDL_KEYUP` that follows it, because nothing changed in between. An application can light an indicator straight from the press.

**Holding a modifier with a lock key changes nothing.** Shift-caps-lock is caps lock, and so is control-caps-lock; the lock keys are the one part of the keyboard that means the same whatever is held with them.

**Caps lock is the layout's own state**, the same one that decides the case of the letters `SDL_TEXTINPUT` carries, so what an application reads from `KMOD_CAPS` and what it receives as text can never disagree. All three locks start off at boot.

**The lamps on the keyboard light.** A keyboard does not light its own caps, num and scroll lamps - the host sends it a report saying which of the three are on - and this library sends that report whenever a lock changes, so the keyboard agrees with what the machine thinks. The report is submitted and not waited for, because the core that would wait is the one core that cannot be stopped; the lamp changes a moment later, which is all a lamp needs.

**On the Pi 3 the lamps stay dark.** Circle drives USB there through the DWHCI controller, which has no asynchronous control transfer to submit - it refuses one outright - and the only way left is to stop the core until the keyboard answers, up to three seconds if it never does. That is not a trade worth making for a lamp. Nothing an application reads changes: the lock states are the layout's, and they are reported identically on every board.

**An application can set the locks, and it reaches all the way down.** `SDL_SetModState` with `KMOD_CAPS` in it turns caps lock on for real - the layout's own lock, the case of the letters that follow, and the lamp. Upstream SDL cannot do this, because there the layout belongs to the host operating system and SDL can only move its own idea of the lock; here both sides are ours, so they are kept in step. The rest of `SDL_SetModState` behaves as SDL defines it.

## The keypad has two faces

The keypad is what num lock is for. Every digit key on it carries a second meaning, printed on the same key cap, and the lock chooses between them - a digit with the lock on, a navigation key with it off:

| Key | Lock on | Lock off |
|---|---|---|
| `7` `8` `9` | `7` `8` `9` | Home, Up, Page Up |
| `4` `5` `6` | `4` `5` `6` | Left, nothing, Right |
| `1` `2` `3` | `1` `2` `3` | End, Down, Page Down |
| `0` `.` | `0` `.` | Insert, Delete |

**Shift inverts the lock**, as it does on a real keyboard, so one digit can be typed without disturbing a keypad that is set to navigate, and one arrow pressed without disturbing a keypad that is set to type.

**The four operators are not on the lock.** `/`, `*`, `-` and `+` type their character whichever way num lock is set, having no second meaning for it to choose between.

**The scancode never moves.** SDL keeps the keypad's positions distinct from the arrow cluster's, and so does this library: keypad 8 is `SDL_SCANCODE_KP_8` whether it is typing an `8` or moving the cursor up, so an application that wants to know which physical key was struck always can.

**The meaning is in the keycode.** The `keysym.sym` of a key event is `SDLK_KP_8` while the lock is on and `SDLK_UP` while it is off, so an application that handles the arrow keys handles the keypad's arrows without knowing the keypad exists. No `SDL_TEXTINPUT` follows a keypad key that is navigating, because a navigation key types nothing.

This is the one place where an event's `keysym.sym` and `SDL_GetKeyFromScancode` differ, and deliberately. The query is a standing question about the keyboard - *where is the keypad's 8* - and its answer stays put, which is what keeps it and `SDL_GetScancodeFromKey` exact inverses and what lets an application find that key while the keypad is navigating. The event is a keystroke that happened, and what it meant is what the lock said at the time.

**Desktop SDL does not do this**; there the keypad's keycode is `SDLK_KP_8` whichever way the lock is set, and an application that wants the navigation meaning works it out itself. The difference is deliberate. Circle's layout tables deliberately return nothing for these keys while the lock is off, precisely because that is when they are not characters, and supplying the meaning they hand back is this library's half of the arrangement - there is nowhere else for it to go.

## Key repeat

A USB keyboard reports which keys are down and nothing more, so a held key produces one report and then stops changing. The repeats every machine appears to make are made by the machine, and this library makes them.

**A repeat is an `SDL_KEYDOWN` with `key.repeat` set**, carrying the same scancode and keycode as the press it repeats, and followed by the `SDL_TEXTINPUT` that key produces. The modifiers on it are the ones held at the moment the repeat is made rather than a copy of the ones held at the original press, which is what lets a letter held down start repeating in upper case the moment shift goes down. It is a press in every other respect. An application that wants real presses only - a game counting how many times a key was struck - ignores an event with the flag set; one that is collecting text uses the repeats, which is the whole point of holding a key down.

**One key repeats at a time: the one pressed most recently.** Press a second key while the first is held and the repeat moves to the second; release the second and the first does not resume. This is what a keyboard controller of the period did and what a person expects.

**The first repeat comes after half a second, and the rest at ten a second.** The delay is long enough that ordinary typing never provokes one. Neither value can be changed by an application, and there is nothing to turn on: an application that reads key events already receives them.

**Modifiers and lock keys never repeat**, and never take the repeat away from a key that has it. Holding shift is one event rather than a stream, and holding a letter and then pressing shift goes on repeating the letter in upper case.

The repeats are generated on the pump that reads the keyboard, so they arrive when the application next reads its events. A main loop that polls more slowly than the repeat interval receives repeats at its own rate rather than a backlog of them.

## Joysticks and game controllers

SDL has different ways of reading the same piece of hardware, and this library offers both.

A **joystick** is the device as it really is: however many axes, hats and buttons it happens to have, numbered in the order the device reports them. Every pad Circle can bind appears this way - the generic HID driver accepts any device that presents as a gamepad, and in addition there are drivers for the PlayStation, Xbox and Switch pads. Nothing has to be configured; plug it in and it is there.

A **game controller** is the same device seen through a **mapping**: a line of text that says which raw axis, button or hat direction corresponds to each control on a standard modern pad, so an application can ask for "the A button" and get an answer. Mappings come from a database file, the community-maintained `gamecontrollerdb.txt`, loaded with `SDL_GameControllerAddMappingsFromFile`.

**A device with no line in that database is not a game controller**, and `SDL_IsGameController` says so. It is still a fully working joystick, and an application that reads raw axes and buttons works with it perfectly. This is the normal answer for anything that is not shaped like a console pad - a steering wheel, a flight stick, an arcade panel - and the library gives it rather than inventing a layout that would put the accelerator somewhere surprising.

Database lines are tagged with the platform they were recorded on, and only lines for the running platform load. No published database contains lines tagged for Circle, so lines tagged `Linux` are accepted as well - and they are the right ones, because the joystick GUIDs this library builds have exactly the shape Linux builds for a USB device, including the CRC of the device name. An unmodified `gamecontrollerdb.txt` therefore works as it stands.

Both layers deliver the SDL events an application expects - `SDL_JOYAXISMOTION`, `SDL_JOYBUTTONDOWN`/`UP`, `SDL_JOYHATMOTION`, `SDL_CONTROLLERAXISMOTION`, `SDL_CONTROLLERBUTTONDOWN`/`UP` - and both handle devices that arrive and leave while the application is running: `SDL_JOYDEVICEADDED` carries a device index, `SDL_JOYDEVICEREMOVED` an instance ID, exactly as SDL defines them.

**Rumble is coarse, and the library does not pretend otherwise.** Circle offers settings - off, weak, strong - so `SDL_JoystickRumble` and `SDL_GameControllerRumble` take the stronger of SDL's two magnitudes and choose the closest of those, honouring the duration. There is no per-motor control and no envelope underneath to expose.

`SDL_Haptic` - SDL's force-feedback API, with its effect shapes and directions - **drives nothing**, because nothing under it could carry an effect faithfully, and an effect that silently becomes a buzz is worse than one that reports it cannot be played. The calls are all there and all link: every query answers that there is no haptic device, and every action fails with a message saying so, which is what sends an application's "rumble if it can" path down its other branch.

Also unimplemented, and reporting failure rather than pretending: controller LEDs, trigger rumble, motion sensors, touchpads, and virtual joysticks.

## When there is no USB host controller

**This library builds and initialises the USB host controller itself**, inside `SDL2Circle_ArmCoreRuntime`. A host kernel constructs nothing for it. So a board that reaches `SDL_Init` with no working controller says so once, at warning, and carries on with no keyboard, mouse or pad:

```
input: no usb host controller: input off
```

Two things reach that state, and the log does not distinguish them because the board cannot:

- **`SDL2Circle_ArmCoreRuntime` has not run**, or did not run on core 0, or ran after `SDL_Init`. This is a start-up ordering mistake in the host kernel rather than an absent controller. [CORE-SPLIT.md](CORE-SPLIT.md) has the order a kernel starts in.
- **The board's own USB block did not come up.** The controller object exists, so Circle reports it as active, but no device it manages will ever be enumerated.

### With `--rapi-debug-uart`, the board stops instead

**Serial key injection does not go through USB.** It reads bytes off the serial device and puts events straight into the queue, so it works perfectly on a board where nothing was ever enumerated. An automated run therefore passes in full - keys arrive, menus move, screenshots come out right - and the first person to plug a real keyboard in finds nothing happens, with the warning above long since scrolled off the console.

So the pair is refused rather than run. The application does not start, and the board says on the console that it stopped and that there is no input hardware.

It is a halt, not a hang: core 0 keeps serving the console, the scheduler and the watchdog throughout, which is what lets the message keep arriving.

## Examples

`examples/keyecho` displays the last key event's scancode and keycode, modifier lights, lock lights and a grid of held keys. `examples/mouseview` shows the pointer on screen with buttons and motion. `examples/padview` counts every attached joystick, gamepad and wheel and logs them arriving and leaving; the first two get a panel each, with name, GUID, USB IDs, live axis bars and button lights, and anything beyond the second is reported as a count rather than drawn.
