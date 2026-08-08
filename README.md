# Table of contents
- [Description](#description)
- [Installation](#installation)
- [ESP32-S3 N16R8 / N8R2 (DIY builds)](#esp32-s3-n16r8--n8r2-diy-builds)
- [Usage](#usage)
- [Issues](#issues)
- [Development](#development)
- [Acknowledgements](#acknowledgements)
- [License](#license)

# Description
Retro-Go is a firmware to play retro games on ESP32-based devices (officially supported are
ODROID-GO and MRGC-G32, check [this list for other devices](components/retro-go/README.md)).
The project consists of a launcher and half a dozen applications that have been heavily
optimized to reduce their cpu, memory, and flash needs without reducing compatibility!

### Supported systems:
- Nintendo: **NES, SNES (slow), Gameboy, Gameboy Color, Game & Watch**
- Sega: **SG-1000, Master System, Mega Drive / Genesis, Game Gear**
- Coleco: **Colecovision**
- NEC: **PC Engine**
- Atari: **Lynx**
- Others: **DOOM** (including mods!)

### Retro-Go features:
- In-game menu
- Favorites and recently played
- GB color palettes, RTC adjust and save
- NES color palettes, PAL roms, NSF support
- More emulators and applications
- Scaling and filtering options
- Better performance and compatibility
- Turbo Speed/Fast forward
- Customizable launcher
- Cover art and save state previews
- Multiple save slots per game
- Wifi file manager
- And more!

### Screenshots
![Preview](assets/retro-go-preview.jpg)


# Installation

### ODROID-GO
  1. Download `retro-go_1.x_odroid-go.fw` from the [release page](https://github.com/ashkan89/retro-go/releases/) and copy it to `/odroid/firmware` on your sdcard.
  2. Power up the device while holding down B.
  3. Select retro-go in the files list and flash it.

### MyRetroGameCase G32 (GBC)
  1. Download `retro-go_1.x_mrgc-g32.fw` from the [release page](https://github.com/ashkan89/retro-go/releases/) and copy it to `/espgbc/firmware` on your sdcard.
  2. Power up the device while holding down MENU (the volume knob).
  3. Select retro-go in the files list and flash it.

### Other devices
  1. Download the .img for your device from the [release page](https://github.com/ashkan89/retro-go/releases/).
  2. Connect your device to a computer with a USB cable.
  3. Flash the image with esptool:
     - [Command line](https://github.com/espressif/esptool/releases/): Run `esptool.py write_flash --flash_size detect 0x0 retro-go_*.img`
     - [Web version](https://espressif.github.io/esptool-js/): Connect your device, click Erase Flash, then select your .img file and set address to 0x0, finally click Program)

Your particular device may require extra steps (like holding a button during power up) or different esptool flags or a special cable. If the above steps fail, you might need to ask the manufacturer for instructions on how to flash new firmware!

If your device is not already supported or if a prebuilt version isn't available for it you can check the [development section](#Development) for more information on how to build for your device.


# ESP32-S3 N16R8 / N8R2 (DIY builds)

These targets are for hand-wired / DIY handhelds built around a bare **ESP32-S3-N16R8** (16MB flash, 8MB Octal PSRAM) or **ESP32-S3-N8R2** (8MB flash, 2MB Quad PSRAM) module or dev board (e.g. an ESP32-S3-DevKitC-1), rather than a pre-made retro handheld. There is no `.fw`/`.img` release for these since the wiring is up to you, so they must be [built from source](#development) with `rg_tool.py`.

Each memory variant is available with 3 display controllers, for 6 build targets total:

| Build target (`--target`)     | Module          | Display controller | Resolution |
|--------------------------------|-----------------|---------------------|------------|
| `esp32-s3-n16r8-ili9341`       | ESP32-S3-N16R8  | ILI9341             | 320x240    |
| `esp32-s3-n16r8-st7789v2`      | ESP32-S3-N16R8  | ST7789V2 (1.69")    | 300x240 (visible 280x240) |
| `esp32-s3-n16r8-st7796`        | ESP32-S3-N16R8  | ST7796              | 480x320    |
| `esp32-s3-n8r2-ili9341`        | ESP32-S3-N8R2   | ILI9341             | 320x240    |
| `esp32-s3-n8r2-st7789v2`       | ESP32-S3-N8R2   | ST7789V2 (1.69")    | 300x240 (visible 280x240) |
| `esp32-s3-n8r2-st7796`         | ESP32-S3-N8R2   | ST7796              | 480x320    |

Build/flash example: `python rg_tool.py --target esp32-s3-n16r8-ili9341 --port COM3 build-img install`

### Features
- ESP32-S3 dual-core Xtensa LX7 @ 240MHz, with emulator/game logic pinned to core 0 and IO/audio helper tasks pinned to core 1 for smoother frame pacing.
- SPI TFT display (ILI9341, ST7789V2 or ST7796 depending on target) at up to 80MHz SPI clock, with backlight control.
- microSD card storage over its own dedicated SPI bus (separate from the display bus, so there's no contention between the two).
- USB HID host support: USB gamepads, XInput controllers, and USB mass-storage (MSC) devices (drag-and-drop ROM/save management from a PC without pulling the SD card).
- 10-button digital gamepad matrix (D-Pad, A, B, Start, Select, Menu, Option) plus a virtual Menu shortcut (Start+Select) if a dedicated Menu button isn't wired.
- WS2812 addressable RGB status LED.
- Haptic feedback (vibration motor) driver output.
- External I2S DAC audio output (e.g. MAX98357A class-D amplifier module) — no onboard battery-voltage ADC on this target, so battery percentage isn't available.
- Optional 3.5mm headphone jack with automatic switching: a stereo PCM5102A DAC shares the I2S bus with the speaker amp, and a detect pin mutes whichever one isn't in use. Volume is remembered separately per output. See [Headphone jack](#headphone-jack-optional).
- Firmware updater support (downloads and flashes new images from `/sd/retro-go/firmware` via the factory app).

### Wiring diagram

```
                                  ┌───────────────────────────────┐
                                  │           ESP32-S3             │
                                  │  N16R8: 16MB flash / 8MB OPI   │
                                  │  N8R2 :  8MB flash / 2MB QPI   │
                                  └───────────────┬─────────────────┘
                                                   │
        Display — SPI2 @ 40-80MHz                 │                 microSD — SPI3 (dedicated bus)
 ┌─────────────────────────────┐                  │                ┌─────────────────────────────┐
 │ GPIO13 ───────── MOSI (SDA) │                  │                │ GPIO41 ───────── MOSI (CMD)  │
 │ GPIO14 ───────── SCK        │                  │                │ GPIO40 ───────── SCK (CLK)   │
 │ GPIO10 ───────── CS         │                  │                │ GPIO42 ───────── CS (DAT3)   │
 │ GPIO11 ───────── DC         │                  │                │ GPIO39 ───────── MISO (DAT0) │
 │ GPIO12 ───────── RESET      │                  │                └─────────────────────────────┘
 │ GPIO21 ───────── LED (BL)   │                  │                       microSD card slot
 │  N/C   ───────── MISO       │                  │
 │  3V3   ───────── VCC        │                  │
 │  GND   ───────── GND        │                  │
 └─────────────────────────────┘                  │
      ILI9341 / ST7789V2 / ST7796 panel            │
                                                   │
        I2S audio — external DAC                  │                Status LED / Haptics
 ┌─────────────────────────────┐                  │                ┌─────────────────────────────┐
 │ GPIO5  ───────── BCLK       │                  │                │ GPIO48 ───────── DIN (WS2812)│
 │ GPIO4  ───────── LRC / WS   │                  │                │ GPIO38 ───────── Vibrator    │
 │ GPIO6  ───────── DIN        │                  │                │                  motor driver │
 │ GPIO9  ───────── SD_MODE    │                  │                └─────────────────────────────┘
 └─────────────────────────────┘                  │
   MAX98357A (or similar I2S DAC)                  │                Headphone jack (optional)
                                                   │                ┌─────────────────────────────┐
                                                   │                │ GPIO5/4/6 ───── shared I2S  │
                                                   │                │ GPIO46 ───────── XSMT       │
                                                   │                │ GPIO1  ───────── jack detect │
                                                   │                └─────────────────────────────┘
                                                   │                  PCM5102A + 3.5mm TRRS jack
                                                   │
                        Gamepad matrix — active-low buttons to GND, internal pull-ups enabled
      ┌──────────────────────────────────────────────────────────────────────────────────────┐
      │  UP = GPIO17     DOWN = GPIO3     LEFT = GPIO8      RIGHT = GPIO18                    │
      │  A  = GPIO45     B    = GPIO47    START = GPIO15    SELECT = GPIO16                   │
      │  MENU = GPIO2 (recovery/boot-menu button, or hold START+SELECT)   OPTION = GPIO7      │
      └──────────────────────────────────────────────────────────────────────────────────────┘
```

<details>
  <summary>Full pinout table</summary>

  | Function              | Signal        | GPIO         |
  |------------------------|--------------|--------------|
  | Display (SPI2)         | MOSI          | GPIO13       |
  | Display (SPI2)         | CLK           | GPIO14       |
  | Display (SPI2)         | CS            | GPIO10       |
  | Display (SPI2)         | DC            | GPIO11       |
  | Display (SPI2)         | RESET         | GPIO12       |
  | Display (SPI2)         | Backlight     | GPIO21       |
  | Display (SPI2)         | MISO          | not connected (write-only panel) |
  | microSD (SPI3)         | MISO          | GPIO39       |
  | microSD (SPI3)         | MOSI          | GPIO41       |
  | microSD (SPI3)         | CLK           | GPIO40       |
  | microSD (SPI3)         | CS            | GPIO42       |
  | I2S audio (ext. DAC)   | BCK           | GPIO5        |
  | I2S audio (ext. DAC)   | WS / LRC      | GPIO4        |
  | I2S audio (ext. DAC)   | DATA / DIN    | GPIO6        |
  | I2S audio (speaker)    | MAX98357A SD_MODE | GPIO9    |
  | I2S audio (headphones) | PCM5102A XSMT | GPIO46       |
  | I2S audio (headphones) | Jack detect   | GPIO1        |
  | Status LED             | WS2812 DIN    | GPIO48       |
  | Haptics                | Vibrator      | GPIO38       |
  | Gamepad                | UP            | GPIO17       |
  | Gamepad                | DOWN          | GPIO3        |
  | Gamepad                | LEFT          | GPIO8        |
  | Gamepad                | RIGHT         | GPIO18       |
  | Gamepad                | A             | GPIO45       |
  | Gamepad                | B             | GPIO47       |
  | Gamepad                | START         | GPIO15       |
  | Gamepad                | SELECT        | GPIO16       |
  | Gamepad                | MENU (recovery)| GPIO2       |
  | Gamepad                | OPTION        | GPIO7        |

  All gamepad buttons are wired to GND and rely on the ESP32-S3's internal pull-ups (`pullup = 1`, active low) — no external resistors needed.
</details>

> **Note:** GPIO45 is a strapping pin (selects VDD_SPI voltage at reset) and GPIO3 is used by JTAG signal source selection. Both default to the correct state for this board already, but avoid holding the A or Down buttons while resetting/flashing if you experience boot issues. The display and SD card intentionally use two separate SPI buses (SPI2 and SPI3) so both peripherals can be active without bus contention.

Source of truth for all pin assignments: [`config.h`](components/retro-go/targets/esp32-s3-n16r8-ili9341/config.h) (identical GPIO map across all 6 targets — only the display driver differs).

### Headphone jack (optional)

The MAX98357A is a filterless class-D amplifier. Its outputs are a bridge-tied PWM pair with **no common ground**,
so you must never wire a headphone jack to them — there is no safe sleeve connection, and the ~300 kHz carrier
would go straight into your ears. Headphones get their own DAC instead.

The ESP32-S3's I2S transmitter is a broadcast bus: BCK, WS and DATA can drive several slaves in parallel, and each
slave simply ignores the data when it is shut down. So the headphone DAC hangs off the *same three pins* as the
speaker amplifier, and two enable GPIOs decide which one is allowed to make noise. No second I2S peripheral, no
I2C, no change to the existing speaker wiring.

#### Parts

| Qty | Part | Notes |
|-----|------|-------|
| 1 | PCM5102A breakout ("GY-PCM5102" purple board) | Stereo I2S DAC. Its internal charge pump makes the output ground-centred, so **no output coupling capacitors are needed**. UDA1334A works too. |
| 1 | 3.5mm 4-pole (TRRS) PCB jack, e.g. PJ-320A / PJ-327 | The 4th contact is what detects insertion. A 3-pole jack with a switched contact also works, see the note below. |
| 2 | 220 Ω resistor, 1/8 W | Series output resistors. |
| 1 | 470 kΩ resistor | Sets the MAX98357A channel mode when enabled. |
| 1 | 100 nF ceramic capacitor | Hardware debounce on the detect line (optional but recommended). |
| — | 22 AWG wire | Run the jack's ground back to the PCM5102A ground pin, not to the class-D amp's ground. |

Optional, for full volume into over-ear headphones: a **PAM8908** or **NJM4556A** headphone amplifier between the
PCM5102A and the jack. Wire its shutdown pin to GPIO46 instead of XSMT — the firmware treats the two identically.

#### Wiring

```
                    ESP32-S3
                       │
   ┌───────────────────┼───────────────────┐          shared I2S bus, both slaves
   │                   │                   │          receive the same samples
 GPIO5 (BCK) ──────────┼───────────┐       │
 GPIO4 (WS)  ──────────┼─────────┐ │       │
 GPIO6 (DATA)──────────┼───────┐ │ │       │
                       │       │ │ │       │
        ┌──────────────▼──┐    │ │ │  ┌────▼─────────────┐
        │   MAX98357A     │◄───┘ │ │  │    PCM5102A      │
        │   (speaker)     │◄─────┘ │  │   (headphones)   │
        │                 │◄───────┘  │                  │
        │                 │           │  BCK / LCK / DIN │
        │ SD_MODE ◄─470k──┼── GPIO9   │  SCK ── GND      │  ← tie SCK low: internal PLL
        │                 │           │  XSMT ◄───────── │── GPIO46
        │ OUT+ ── speaker │           │  LOUT ─┬─ 220R ──┼──► TIP
        └─────────────────┘           │  ROUT ─┼─ 220R ──┼──► RING1
                                      │  AGND ─┴─────────┼──► SLEEVE
                                      └──────────────────┘
                                                              RING2 ──► GPIO1
```

**Jack detect (GPIO1).** A stereo plug's barrel is long enough to bridge both the sleeve and ring2 contacts of a
4-pole jack. So ring2 is grounded whenever a plug is in, and floating otherwise. GPIO1 runs with its internal
pull-up enabled, which gives:

| State | GPIO1 | Result |
|-------|-------|--------|
| Nothing plugged in | HIGH (pull-up) | Speaker |
| Plug inserted | LOW (bridged to sleeve) | Headphones |
| Nothing wired to GPIO1 at all | HIGH (pull-up) | Speaker |

That last row is the point of this arrangement: **a board built without the headphone mod behaves exactly as
before**, so the feature is safe to leave enabled in the shared target configs. Add the 100 nF capacitor from GPIO1
to GND to soak up contact chatter; the firmware also requires the line to hold its new state for 200 ms
(`RG_AUDIO_HP_DEBOUNCE_MS`) before it acts.

If you use a 3-pole jack with a normally-closed switch contact instead, the polarity inverts (closed to ground when
*empty*). Wire the switch contact to GPIO1 and set `RG_GPIO_SND_HP_DETECT_LEVEL 1` in your target's `config.h`.

**Speaker enable (GPIO9).** The MAX98357A's SD_MODE pin is not a plain enable — the *voltage* on it selects the
channel: below 0.16 V shuts the amp down, 0.16–0.77 V selects the (L+R)/2 mono mix. The common breakouts already
have a 100 kΩ resistor from SD_MODE to GND, so feeding GPIO9 through **470 kΩ** gives 3.3 × 100/570 ≈ 0.58 V when
high (mono mix, which is what you want for a single speaker) and 0 V when low (shutdown). If your board has no
pulldown, add your own 100 kΩ from SD_MODE to GND.

**Headphone enable (GPIO46).** Drives the PCM5102A's XSMT (soft mute) pin, high = unmuted. GPIO46 is a strapping
pin, but it selects whether the ROM prints its boot log — harmless either way, and our idle state is low, which is
the default. Remove the XSMT jumper/pull-up on the breakout if it has one. This pin is optional: without it the
headphone DAC just runs continuously, which is inaudible when nothing is plugged in but does add a small amount of
idle noise and current draw. Comment out `RG_GPIO_SND_HP_ENABLE` if you skip it.

**Grounding.** Class-D amplifiers dump switching current into their ground return. Run the jack's sleeve back to
the PCM5102A's own ground pin with a dedicated wire and join the grounds at a single point near the module, or you
will hear the speaker amp's carrier in the headphones even when the speaker is off.

**Output level.** The PCM5102A puts out 2.1 Vrms and is specified to drive 1 kΩ, so it cannot supply the current
32 Ω headphones want at full swing. The 220 Ω series resistors keep it inside its comfort zone and are plenty loud
for IEMs; add a headphone amplifier if you want to drive high-impedance over-ears properly.

#### Firmware behaviour

Enabled by `RG_AUDIO_USE_HEADPHONE_JACK 1`, already set in all 6 ESP32-S3 target configs. In the menu,
`Audio out` gains three choices:

| Setting | Behaviour |
|---------|-----------|
| `Auto (Speaker)` / `Auto (Headphones)` | Follows the jack. The parenthesised part is the live detected state. |
| `Speaker` | Forces the speaker even with a plug inserted. |
| `Headphones` | Forces the headphone DAC even with nothing plugged in. |

`Auto` is the default and is what an existing saved `Ext DAC` setting upgrades to. Switching between these three
retargets the output over GPIO without reinstalling the I2S driver, so it doesn't glitch the audio.

**Volume is stored per route** (`Volume` and `VolumeHP` in the settings), because a speaker level applied to
headphones is unpleasant at best. Headphones start at 25% on first use. Changing the volume while headphones are
connected only affects the headphone level, and vice versa.

Related knobs, all overridable in a target `config.h`: `RG_AUDIO_HP_DEBOUNCE_MS` (200),
`RG_AUDIO_HP_POLL_INTERVAL_MS` (50), `RG_AUDIO_HP_DEFAULT_VOLUME` (25),
`RG_GPIO_SND_HP_DETECT_LEVEL` (0), `RG_GPIO_SND_HP_ENABLE_INVERT`, `RG_GPIO_SND_AMP_ENABLE_INVERT`.


# Usage

## Game covers / artwork
Game covers should be placed in the `romart` folder at the base of your sd card. You can obtain a pre-made pack [here](https://github.com/ashkan89/retro-go-covers). Retro-Go is also compatible with the older Go-Play romart pack.

You can add missing cover art by creating a PNG image (160x168, 8bit). Two naming schemes are supported:
- Filename-based: `/romart/nes/Super Mario.png` (notice the rom extension is *not* included)
- CRC32-based: `/romart/nes/A/ABCDE123.png` where `nes` is the same as the rom folder, and `ABCDE123` is the CRC32 of the game (press A -> Properties in the launcher to find it), and `A` is the first character of the CRC32

_Note: CRC32-based, which is what is used in the pre-made pack, is much slower than name-based! This type is useful because filenames vary greatly despite having identical CRCs, but if you generate your own art I suggest you use filename-based format and delete all CRC-based art from your SD Card to improve responsiveness._


## BIOS files
Some emulators support loading a BIOS. The files should be placed as follows:
- GB: `/retro-go/bios/gb_bios.bin`
- GBC: `/retro-go/bios/gbc_bios.bin`
- FDS: `/retro-go/bios/fds_bios.bin`
- MSX: In folder `/retro-go/bios/msx/` put: `MSX.ROM` `MSX2.ROM` `MSX2EXT.ROM` `MSX2P.ROM` `MSX2PEXT.ROM` `FMPAC.ROM` `DISK.ROM` `MSXDOS2.ROM` `PAINTER.ROM` `KANJI.ROM`


## Game & Watch
The roms must be packed with [LCD-Game-Shrinker](https://github.com/bzhxx/LCD-Game-Shrinker) and a tutorial can be [found here](https://gist.github.com/DNA64/16fed499d6bd4664b78b4c0a9638e4ef).


## Wifi
To use wifi you will need to create a `/retro-go/config/wifi.json` config file. You can define up to 4 different networks, then selectable in the menu. Its content should look like this:

````json
{
  "ssid0": "my-network",
  "password0": "my-password",
  "ssid1": "my-other-network",
  "password1": "my-password",
  "ssid2": "my-third-network",
  "password2": "my-password",
  "ssid3": "my-last-network",
  "password3": "my-password"
}
````

### Time synchronization
Time synchronization happens in the launcher immediately after a successful connection to the network.
This is done via NTP by contacting `pool.ntp.org` and cannot be disabled at this time.
Timezone can be configured in the launcher's options menu.

### File manager
You can find the IP of your device in the *about* menu of retro-go. Then on your PC navigate to
http://192.168.x.x/ to access the file manager.


## External DAC (headphones)

Retro-Go supports [the external DAC mod for the ODROID-GO](https://github.com/backofficeshow/odroid-go-audio-hat)
which allows high quality audio through headphones. You can switch to it in the menu `Audio Out: Ext DAC`.

<details>
  <summary>Pinout</summary>

  | GO PIN | PCM5102A PIN |
  |--------|---------|
  | 1 | GND |
  | 2 | - |
  | 3 | LCK |
  | 4 | DIN |
  | 5 | BCK |
  | 6 | VIN |
  | 7 | - |
  | 8 | - |
  | 9 | - |
  | 10 | - |
</details>


# Issues

### Black screen / Boot loops
Retro-Go typically detects and resolves application crashes and freezes automatically. However, if you do
get stuck in a boot loop, you can hold `DOWN` while powering up the device to return to the launcher.

### Sound quality
The volume isn't correctly attenuated on the GO, resulting in upper volume levels that are too loud and
lower levels that are distorted due to DAC resolution. A quick way to improve the audio is to cut one
of the speaker wire and add a `33 Ohm (or thereabout)` resistor in series. Soldering is better but not
required, twisting the wires tightly will work just fine.
[A more involved solution can be seen here.](https://wiki.odroid.com/odroid_go/silent_volume)
Alternatively you can use the headphones DAC mod mentioned earlier in this document.

### Game Boy SRAM *(aka Save/Battery/Backup RAM)*
In Retro-Go, save states will provide you with the best and most reliable save experience. That being said, please
read on if you need or want SRAM saves. The SRAM format is compatible with VisualBoyAdvance so it may be used to
import or export saves.

You can configure automatic SRAM saving in the options menu. A longer delay will reduce stuttering at the cost
of losing data when powering down too quickly. Also note that when *resuming* a game, Retro-Go will give priority
to a save state if present.

### ZIP files
Most Retro-Go applications now support ZIP files. ZIP archives should contain only one ROM file and nothing else. ZIP support also depends on available memory and larger ROMs may fail to load on some devices unfortunately.


# Development
If you wish to build or modify Retro-Go, you can find help in the following documents:

- Build instructions in [BUILDING.md](BUILDING.md)
- Theming instructions [THEMING.md](THEMING.md)
- Porting instructions in [PORTING.md](PORTING.md)
- Translating instructions in [LOCALIZATION.md](LOCALIZATION.md)


# Acknowledgements
- The NES/GBC/SMS emulators and base library were originally from the "Triforce" fork of the [official Go-Play firmware](https://github.com/othercrashoverride/go-play) by crashoverride, Nemo1984, and many others.
- The design of the launcher was originally inspired/copied from [pelle7's go-emu](https://github.com/pelle7/odroid-go-emu-launcher).
- PCE-GO is a fork of [HuExpress](https://github.com/kallisti5/huexpress) and [pelle7's port](https://github.com/pelle7/odroid-go-pcengine-huexpress/) was used as reference.
- The Lynx emulator is a port of [libretro-handy](https://github.com/libretro/libretro-handy).
- The SNES emulator is a port of [Snes9x 2005](https://github.com/libretro/snes9x2005).
- The DOOM engine is a port of [PrBoom 2.5.0](http://prboom.sourceforge.net/).
- The Genesis emulator is a port of [Gwenesis](https://github.com/bzhxx/gwenesis/) by bzhxx.
- The Game & Watch emulator is a port of [lcd-game-emulator](https://github.com/bzhxx/lcd-game-emulator) by bzhxx.
- The MSX emulator is a port of [fMSX](https://fms.komkon.org/fMSX/) by Marat Fayzullin.
- PNG support is provided by [lodepng](https://github.com/lvandeve/lodepng/).
- PCE cover art is from [Christian_Haitian](https://github.com/christianhaitian).
- Some icons from [Rokey](https://iconarchive.com/show/seed-icons-by-rokey.html).
- Background images from [es-theme-gbz35](https://github.com/rxbrad/es-theme-gbz35).
- Special thanks to [RGHandhelds](https://www.rghandhelds.com/) and [MyRetroGamecase](https://www.myretrogamecase.com/) for sending me a [G32](https://www.myretrogamecase.com/products/game-mini-g32-esp32-retro-gaming-console-1) device.
- The [ODROID-GO](https://forum.odroid.com/viewtopic.php?f=159&t=37599) community for encouraging the development of retro-go!

# License
Everything in this project is licensed under the [GPLv2 license](COPYING) with the exception of the following components:
- fmsx/components/fmsx (MSX Emulator, custom non-commercial license)
- handy-go/components/handy (Lynx emulator, zlib)
