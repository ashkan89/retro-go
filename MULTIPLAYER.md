# Local P1 / P2 controllers

Open **Options > Multiplayer** and assign each physical input source
to **P1**, **P2**, **Auto**, or **Off**. Enable and map the controller in
**Options > USB controllers** or **Options > Xbox controller** as appropriate.
Emulators read logical player states, not USB device numbers: USB gamepad 1 can
be assigned to P2, and USB gamepad 2 can be assigned to P1.

The built-in buttons default to P1. With no connected explicit P2 assignment,
the first connected external source on Auto supplies P2. A connected explicit
P2 assignment reserves P2, so other Auto sources stay off. Explicitly assigning
multiple devices to the same player intentionally combines their buttons.
If P1's controller is unplugged, the built-in buttons become P1 again so menus
remain accessible. Menus and console-wide controls are operated by P1.

For two external controllers, assign one to P1 and one to P2; optionally set
built-in input to Off. For built-in controls plus one external controller,
leave built-in input on P1 and assign the external controller to P2. Keyboard
and mouse sources also participate in Auto assignment, so use explicit player
assignments when connecting several different peripheral types through a hub.

## P4 physical connections

Both P4 N16R32 display targets have one bank of built-in active-low buttons,
with pull-ups. Connect each switch between its configured GPIO and ground.

| Button | GPIO |
| --- | --- |
| A / B | 2 / 3 |
| Select / Start | 4 / 5 |
| Menu / Option | 6 / 11 |
| Up / Down / Left / Right | 12 / 13 / 14 / 15 |

These pins form one assignable input source, not two GPIO controller ports.
External controllers connect to **HUSB** (using a USB hub for several devices).
**FUSB** is the separate Serial/JTAG connection. USB host input and USB
mass-storage device mode are exclusive. See the
[P4 board wiring guide](components/retro-go/targets/esp32-p4-n16r32-st7796/docs/README.md#reserved-pins-and-usb)
for host/hub power and board connector details.

## Emulator routing audit

| System / frontend | P1 | P2 | Notes |
| --- | --- | --- | --- |
| NES/FDS — Nestopia | NES pad 0 | NES pad 1 | Native emulated `$4016` / `$4017` ports. NSF playback uses P1 transport controls. |
| NES/FDS — Nofrendo | NES input 0 | NES input 1 | Separate eight-button NES mappings. |
| SNES | Joypad 0 | Joypad 1 | Additional ports return no input; special peripherals are outside this mapping. |
| Mega Drive — Gwenesis | Pad 0 | Pad 1 | Independent button press/release events. |
| PC Engine | Joypad register 0 | Joypad register 1 | Directions, I/II, Run and Select. |
| Master System / SG-1000 | Pad 0 | Pad 1 | Pause and other console-wide actions remain P1. |
| ColecoVision | Pad/keypad 0 | Pad/keypad 1 | Choose **Keypad player: P1/P2** in emulator options. P1 Start opens the shared virtual keypad and sends the chosen key to that port. |
| MSX — fMSX | Low joystick byte | High joystick byte | P1's selected keyboard mode still applies; P2 feeds joystick port 2. |
| Atari 2600 — Stella | Left joystick | Right joystick | Directions and fire; P1 Select/Start operate console Select/Reset. Paddle/keypad peripherals need their own mappings. |
| Atari 7800 — ProSystem | Input entries 0–5 | Input entries 6–11 | Independent directions and two fire buttons. Console switches remain P1. |
| ZX Spectrum | Selected Kempston / Sinclair 2 / QAOP controls | Sinclair 1 keys 1–5 | Configure the game's second player for Sinclair 1. P2 left/right/down/up/fire = 1/2/3/4/5. |
| GB / GBC / GBA | Local handheld controls | Not exposed | A second controller does not implement a second handheld or link cable. |
| Game Gear / Lynx | Local handheld controls | No second machine | Multiplayer link emulation is not implemented by these frontends. |
| Game & Watch | Game-specific buttons | No separate controller port | Only the P1 logical controller is read. |
| Doom — PrBoom | Local player | No second local player | A second controller does not create another Doom player. |

The audit follows input assignment through frontend mappings and the core's
port consumers. Host regression tests exercise independent states and button
release; Nestopia additionally runs a generated ROM that reads both emulated
NES controller registers. Hardware controller enumeration, electrical wiring
and game-specific multiplayer behavior still need on-device checks.

Run `python tools/test_multiplayer.py --cxx c++` and
`python tools/test_nestopia.py --cxx c++` for the host checks.

## FDS BIOS

Place the FDS BIOS at `/retro-go/bios/disksys.rom` on the storage device.
Both NES backends prefer that file and accept `/retro-go/bios/fds_bios.bin`
as a legacy fallback. This is the BIOS, separate from the `.fds` game images.
If both names exist, `disksys.rom` takes precedence.
