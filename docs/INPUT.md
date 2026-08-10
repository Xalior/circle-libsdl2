# Input: keyboard, mouse, joysticks

## Keyboard layout

A keyboard reports POSITIONS, and what is printed on the key at a position depends on which country the keyboard was sold in. SDL keeps those two things apart and so does this library.

**A scancode is a position, and it never moves with the layout.** The scancodes in `SDL_KEYDOWN` and `SDL_KEYUP`, and the array `SDL_GetKeyboardState` returns, are USB HID usage codes exactly as the keyboard sent them. A game that binds the key to the left of Z gets the same key on a British keyboard, a German one and an American one. This is what makes a set of controls portable between boards.

**Typed text is the layout's business, and comes from Circle's.** The `SDL_TEXTINPUT` event carries the character the key actually prints, read through the layout the board's `cmdline.txt` selects:

```
keymap=UK
```

Circle carries `US`, `UK`, `DE`, `ES`, `FR`, `IT` and `DV`. **The names are uppercase and matched exactly, and a card that names none of them — or names one in the wrong case — gets German.** That is Circle's built-in default, and it is chosen silently: no warning, no log line. It presents as individual keys printing the wrong character rather than as a setting that was ignored, which makes it look like faulty hardware.

So on a board set to `UK`, shift-2 types `"`, shift-3 types `£` and the key beside Enter types `#`; on `DE`, the Y and Z positions swap the letters they print. The text is UTF-8, as SDL requires — a character outside ASCII, such as `£`, arrives as the several bytes UTF-8 spells it with.

**AltGr types; the other modifiers do not.** A key held with control, with the left alt, or with a GUI key is a command rather than text, and produces no `SDL_TEXTINPUT` — SDL's own behaviour. The right alt is AltGr, which on the European layouts is a third level holding characters of its own rather than a command modifier, so it produces text where the layout defines some. The US layout defines none, so on a US board AltGr types nothing.

**The keycode (`keysym.sym`) does not follow the layout**, although desktop SDL's does. It stays what a US keyboard would report, for the same reason scancodes stay physical: applications bind their actions to keycodes, out of configuration files and out of compiled-in defaults, and a keycode that moved with `keymap=` would silently rebind a game's controls on any board not set to `us`. `SDL_GetKeyFromScancode` and `SDL_GetScancodeFromKey` answer accordingly, and remain each other's inverse.

**The keypad types its digits and operators whatever the layout says.** Every layout prints the same characters on it, and Circle's tables gate the digits behind num lock, which nothing here turns on.

## Joysticks and game controllers

SDL has different ways of reading the same piece of hardware, and this library offers both.

A **joystick** is the device as it really is: however many axes, hats and buttons it happens to have, numbered in the order the device reports them. Every pad Circle can bind appears this way — the generic HID driver accepts any device that presents as a gamepad, and in addition there are drivers for the PlayStation, Xbox and Switch pads. Nothing has to be configured; plug it in and it is there.

A **game controller** is the same device seen through a **mapping**: a line of text that says which raw axis, button or hat direction corresponds to each control on a standard modern pad, so an application can ask for "the A button" and get an answer. Mappings come from a database file, the community-maintained `gamecontrollerdb.txt`, loaded with `SDL_GameControllerAddMappingsFromFile`.

**A device with no line in that database is not a game controller**, and `SDL_IsGameController` says so. It is still a fully working joystick, and an application that reads raw axes and buttons works with it perfectly. This is the normal answer for anything that is not shaped like a console pad — a steering wheel, a flight stick, an arcade panel — and the library gives it rather than inventing a layout that would put the accelerator somewhere surprising.

Database lines are tagged with the platform they were recorded on, and only lines for the running platform load. No published database contains lines tagged for Circle, so lines tagged `Linux` are accepted as well — and they are the right ones, because the joystick GUIDs this library builds have exactly the shape Linux builds for a USB device, including the CRC of the device name. An unmodified `gamecontrollerdb.txt` therefore works as it stands.

Both layers deliver the SDL events an application expects — `SDL_JOYAXISMOTION`, `SDL_JOYBUTTONDOWN`/`UP`, `SDL_JOYHATMOTION`, `SDL_CONTROLLERAXISMOTION`, `SDL_CONTROLLERBUTTONDOWN`/`UP` — and both handle devices that arrive and leave while the application is running: `SDL_JOYDEVICEADDED` carries a device index, `SDL_JOYDEVICEREMOVED` an instance ID, exactly as SDL defines them.

**Rumble is coarse, and the library does not pretend otherwise.** Circle offers settings — off, weak, strong — so `SDL_JoystickRumble` and `SDL_GameControllerRumble` take the stronger of SDL's two magnitudes and choose the closest of those, honouring the duration. There is no per-motor control and no envelope underneath to expose.

`SDL_Haptic` — SDL's force-feedback API, with its effect shapes and directions — is **not implemented at all**, because nothing under it could carry an effect faithfully, and an effect that silently becomes a buzz is worse than one that reports it cannot be played.

Also unimplemented, and reporting failure rather than pretending: controller LEDs, trigger rumble, motion sensors, touchpads, and virtual joysticks.

## Examples

`examples/keyecho` displays scancode, modifiers and a grid of held keys. `examples/mouseview` shows the pointer on screen with buttons and motion. `examples/padview` shows all attached joysticks, gamepads and wheels: name, GUID, USB IDs, live axis bars, button lights, and hot-plug events.
