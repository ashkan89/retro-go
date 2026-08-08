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

### Launcher music player

The launcher Music tab browses `/sd/media` and plays MP3, AAC, M4A/MP4, FLAC and WAV files,
plus M3U/M3U8 playlists. Relative M3U paths are resolved from the playlist directory;
absolute SD paths may start with either `/sd/` or `/`. Playback position, play mode and
sleep timer are saved automatically, and audio keeps playing while you browse other tabs.

Tags come from ID3v1/ID3v2 (MP3), Vorbis comments (FLAC), RIFF INFO (WAV) and iTunes-style
`ilst` atoms (M4A). Cover art is taken from the embedded picture when there is one, or from
a `cover`/`folder`/`album`/`front` `.jpg`/`.png` next to the track.

The player uses the album art as a full-bleed background. Rather than dimming
the whole image, a gradient scrim darkens only the top and bottom edges, so the
artwork stays sharp through the middle while the text over it stays readable.
Without art it falls back to a deep blue gradient.

Pages, cycled with `Select` and shown as dots in the header:

| Page | What it shows |
| --- | --- |
| Now Playing | Large title/artist/album over the artwork, format and bitrate |
| Spectrum | 24-band analyser; `X` cycles Bars, Blocks, Mirror and Wave styles |
| Level Meter | Per-channel RMS bars with peak hold and a dB scale |
| Waveform | Live stereo oscilloscope |
| Lyrics | Scrolling synchronized `.lrc` |
| Equalizer | Five-band graphic EQ with presets |
| Queue | The current play queue, scrollable and playable |
| Details | Tags plus buffer, underrun and decode diagnostics |

Controls:

- `A`: pause/resume (play the highlighted entry on Queue, toggle EQ on Equalizer).
- `B`: return to the launcher; audio keeps playing.
- `Start`: stop. `Option` + `Start`: sleep timer.
- `Select`: next page. `Option` + `Select`: repeat-all, repeat-one or shuffle.
- `L`/`R`: previous/next track. `Left`/`Right`: seek, accelerating while held.
- `Up`/`Down`: volume. `Option` + `Up`/`Down`: brightness.
- On Queue: `Up`/`Down` moves the selection. On Equalizer: `Left`/`Right` picks a
  band, `Up`/`Down` sets its gain, `X` cycles presets.
- `Menu`: options. Everything the shortcuts do is also here — equalizer (with a
  per-band submenu), play mode, sleep timer, spectrum style, volume, brightness,
  track info, the launcher's own settings, and Stop playback.

The equalizer is five cascaded biquads per channel (low shelf, three bells, high
shelf) at ±12 dB, applied to the decoded audio before it reaches the DAC.
Presets: Flat, Bass, Vocal, Treble, Rock, Jazz, Loudness, plus Custom, which is
what you get as soon as you move a band by hand. Settings persist.

Seeking is sample-accurate for MP3 — the Xing/Info seek table is used when present so VBR
files land where you asked. Container formats (FLAC, WAV, M4A) need their header, so they
can only be restarted from the beginning; `Left`/`Right` act as previous/next track there.

Put a UTF-8 `.lrc` file next to a track for synchronized lyrics. Unsynchronized `USLT`
lyrics embedded in an MP3 are shown when no `.lrc` exists.

Only MP3 support is mandatory. Building with `-DMEDIA_ENABLE_EXTRA_CODECS=0` drops the AAC,
FLAC, WAV and M4A decoders and saves roughly 180 KB of flash.

Streaming uses a 512 KB PSRAM ring (falling back through 320/128/64 KB if memory is short),
filled in 32 KB reads staged through a small internal DMA buffer. That is around thirteen
seconds of audio at 320 kbit/s, which is what lets a directory scan or a cover-art decode
happen on the UI thread without the music skipping.

A short read from the card is never taken as end of stream. `ferror()` and `feof()` are
checked separately: a genuine EOF ends the track, an I/O error re-seeks to the last known
good offset and retries, and only eight consecutive failures give up. Conflating the two is
what made a flaky card look like a track that kept finishing early and restarting.

The player text uses `MediaSans15`, generated by `tools/generate_media_font.py`: Open Sans
Semibold for Latin (drawn for on-screen UI, and semibold so stems survive against bright
artwork) merged with DejaVu Sans for the Arabic block and the presentation forms the bidi
shaper emits. Glyph extents are measured rather than clamped to the nominal size, so
descenders and Arabic marks are not squashed. Row heights throughout the player derive from
`rg_gui_get_font_height()`, so swapping the font does not make rows collide.

The player does not use `rg_display_submit()`. That hands the whole framebuffer to the
display task, which runs on `RG_TASK_AFFINITY_IO` — the same core as the audio writer, the
decoder and the SD reader — and byte-swaps and hashes every pixel each frame. Instead the
player tracks its own dirty rectangles and pushes them with `rg_display_write_rect()`, which
transfers on the calling task. All display work therefore stays on the UI core and only
changed pixels cross the bus.

Boards with a headphone-jack switch can define `RG_GPIO_HEADPHONE_DETECT` in their target
`config.h`. `RG_HEADPHONE_DETECT_LEVEL` selects the inserted level (default `0`) and
`RG_HEADPHONE_DETECT_PULLUP` selects the internal pull-up (default `1`). Playback pauses on
removal and resumes after a debounced insertion.

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
 └─────────────────────────────┘                  │                └─────────────────────────────┘
   MAX98357A (or similar I2S DAC)                  │
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
