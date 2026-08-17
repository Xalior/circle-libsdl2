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

**The keycode (`keysym.sym`) does not follow the layout**, although desktop SDL's does. It stays what a US keyboard would report, for the same reason scancodes stay physical: applications bind their actions to keycodes, out of configuration files and out of compiled-in defaults, and a keycode that moved with `keymap=` would silently rebind a game's controls on any board not set to `us`. `SDL_GetKeyFromScancode` and `SDL_GetScancodeFromKey` answer accordingly, and remain each other's inverse.

**The keypad types its digits and operators whatever the layout says, and whatever num lock says.** Every layout prints the same characters on it, so the keypad is not routed through the layout at all. This is a real difference from a desk machine, and worth knowing: turning num lock off does not turn the keypad into a set of arrow keys, because Circle's tables gate the keypad digits behind num lock and num lock starts off - a board fresh from the boot would have a keypad that typed nothing until somebody found the key.

## Lock keys

**Caps lock, num lock and scroll lock are states, not held keys.** Each is on or off between presses, and the key that sets it is up almost all of the time the lock is on. SDL carries all three in the same modifier word as shift and control - `KMOD_CAPS`, `KMOD_NUM` and `KMOD_SCROLL` - so `SDL_GetModState` and the `keysym.mod` of every key event report whether the lock is on, never whether the key is down.

**The state changes on the press**, as it does on a machine with a lamp on the keyboard: the `SDL_KEYDOWN` for the caps lock key already carries the new `KMOD_CAPS`, and so does the `SDL_KEYUP` that follows it, because nothing changed in between. An application can light an indicator straight from the press.

**Holding a modifier with a lock key changes nothing.** Shift-caps-lock is caps lock, and so is control-caps-lock; the lock keys are the one part of the keyboard that means the same whatever is held with them.

**Caps lock is the layout's own state**, the same one that decides the case of the letters `SDL_TEXTINPUT` carries, so what an application reads from `KMOD_CAPS` and what it receives as text can never disagree. All three locks start off at boot.

**The keyboard's own lamps are not lit.** This library never writes the LEDs on the USB keyboard, so the state is the application's to show on screen. `examples/keyecho` shows all three.

## Key repeat

A USB keyboard reports which keys are down and nothing more, so a held key produces one report and then stops changing. The repeats every machine appears to make are made by the machine, and this library makes them.

**A repeat is an `SDL_KEYDOWN` with `key.repeat` set**, carrying the same scancode, keycode and modifiers as the press it repeats, and followed by the same `SDL_TEXTINPUT` that press produced. It is a press in every other respect. An application that wants real presses only - a game counting how many times a key was struck - ignores an event with the flag set; one that is collecting text uses the repeats, which is the whole point of holding a key down.

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

`SDL_Haptic` - SDL's force-feedback API, with its effect shapes and directions - is **not implemented at all**, because nothing under it could carry an effect faithfully, and an effect that silently becomes a buzz is worse than one that reports it cannot be played.

Also unimplemented, and reporting failure rather than pretending: controller LEDs, trigger rumble, motion sensors, touchpads, and virtual joysticks.

## Examples

`examples/keyecho` displays scancode, modifier lights, lock lights and a grid of held keys. `examples/mouseview` shows the pointer on screen with buttons and motion. `examples/padview` shows all attached joysticks, gamepads and wheels: name, GUID, USB IDs, live axis bars, button lights, and hot-plug events.
