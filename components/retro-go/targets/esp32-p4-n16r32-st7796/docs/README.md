# WT9932P4-TINY N16R32 + ST7796 (480 x 320)

Target: `esp32-p4-n16r32-st7796`. ESP-IDF **5.5.5** is required for the
configuration verified here. The board is the
[Wireless-Tag WT9932P4-TINY](https://en.wireless-tag.com/product-item-72.html),
using an external SPI ST7796 display. The module
does not contain a display, SD socket, battery charger, buttons, or audio codec.
These connections are our DIY assignment, not a manufacturer carrier pinout.
JPEG artwork uses `espressif/esp_jpeg` 1.3.2, resolved by the ESP-IDF component
manager during the first build; the firmware itself needs no network connection.

## Silicon and memory

- 16 MB QIO flash at 80 MHz; 32 MB in-package **16-line** PSRAM at 200 MHz.
- Selects P4 revisions **1.0 through 1.99**, including 1.3, explicitly. A module
  name alone cannot prove the fitted silicon revision. Run
  `python -m esptool --chip esp32p4 --port COM3 chip_id` before flashing and
  check the reported revision. Do not use `--force` to bypass incompatibility.
- The newer **N16R32X / P4 v3.2** module needs a separate configuration.
- CPU: **360 MHz**, the supported IDF default for pre-v3 silicon. 400 MHz on
  pre-v3 chips requires additional Espressif qualification; no overclock is used.
- The multi-system emulator's hot loops use the flash cache on P4, preserving
  the small executable low-SRAM region for the OS and I/O. Other targets keep
  their existing IRAM placement.
- Wi-Fi uses an external ESP32-WROOM-32D running ESP-Hosted over SDIO.
  See [WIFI.md](WIFI.md) for wiring, pull-ups and the required flash-voltage
  handling. Bluetooth remains disabled.

## Wiring from the current S3 build

All signal numbers below are **GPIO numbers**. The last column is the core
module's physical pad number from the WT0132P4-A1 datasheet, not a carrier header.
Use 3.3 V logic and common ground. Preserve the existing peripheral power and
driver circuits; do not connect a speaker, motor, or raw LCD backlight to a GPIO.

| Existing connection | S3 GPIO | P4 GPIO | Module pad |
|---|---:|---:|---:|
| ST7796 CS | 10 | 28 | 24 |
| ST7796 MOSI / SDA | 13 | 29 | 25 |
| ST7796 SCK | 14 | 30 | 26 |
| ST7796 DC | 11 | 31 | 27 |
| ST7796 RESET | 12 | 32 | 28 |
| LCD backlight driver | 21 | 33 | 29 |
| SD CS | 42 | 45 | 44 |
| SD MOSI / CMD | 41 | 46 | 45 |
| SD SCK | 40 | 9 | 62 |
| SD MISO / DAT0 | 39 | 10 | 63 |
| I2S BCLK | 5 | 20 | 78 |
| I2S LRCLK / WS | 4 | 21 | 79 |
| I2S DATA to both DACs | 6 | 22 | 80 |
| MAX98357A SD_MODE circuit | 9 | 17 | 75 |
| PCM5102A XSMT | 46 | 19 | 77 |
| A button | 45 | 2 | 55 |
| B button | 47 | 3 | 56 |
| Select button | 16 | 4 | 57 |
| Start button | 15 | 5 | 58 |
| Menu button | 2 | 6 | 59 |
| Option button | 7 | 11 | 65 |
| Up button | 17 | 12 | 66 |
| Down button | 3 | 13 | 67 |
| Left button | 8 | 14 | 68 |
| Right button | 18 | 15 | 69 |
| Battery divider midpoint | 1 | 16 / ADC1_CH0 | 74 |
| Onboard WS2812 data | 48 | 51 | See TINY board header |
| Vibration transistor input | 38 | 18 | 76 |

GPIO0/1/7/8 are not exposed on this board's headers. SD CS and MOSI use the
confirmed exposed GPIO45/46 instead; SD clock and MISO stay on GPIO9/10.
GPIO45/46 use the P4's VDD_IO_5 power bank. On this module it is supplied by
LDO4, which the firmware must enable at 3.3 V **before** initializing SPI.
This applies even though the SD card itself already has external power.
Filesystem and USB MSC startup share the same I/O-bank initialization.
This supersedes the earlier GPIO7/8 wiring assignment and firmware image.

LCD MISO stays disconnected. Buttons switch to ground with internal pull-ups.
Start + Select also opens Menu. No headphone-detect GPIO is assigned; use the
Audio out setting, as on the S3 build. See the root AUDIO.md and BATTERY.md for
the shared amplifier routing and protected 1S battery / 100k + 100k divider.
The P4 target uses the new ADC oneshot driver with curve-fitting calibration.
If the chip lacks usable calibration eFuses, battery monitoring is disabled
with a log message and the rest of the system continues to boot.
Remove the battery/LED/vibration definitions in config.h if those accessories
are omitted. The bare core module's VCC is pad 41 (5 V in the module datasheet);
CHIP_PU is pad 70. A carrier's power input may differ.

LCD uses SPI2's native CS/MOSI/CLK pins, RGB565, landscape 480 x 320, and the
existing S3 ST7796 orientation/gamma settings. This target selects **80 MHz**.
Use 40 MHz if the physical wiring cannot sustain 80 MHz. SD uses a separate SPI3 bus
capped at 10 MHz by the storage recovery driver. Keep wires short and retain SD pull-ups/decoupling. The two buses can
transfer independently. The init driver now waits 120 ms after reset/sleep-out.

## Hardware screen scaling

The default **Filter: Both** uses the P4 PPA's hardware bilinear scaler on the
display task (core 1), alongside emulation on core 0. This applies to RGB565
and indexed RGB565 frames, including NES, SMS, PCE and Mega Drive. Indexed
frames are expanded into a reusable RGB565 PSRAM buffer at source resolution;
the PPA handles interpolation and scaling. A separate aligned PSRAM output
buffer avoids DMA overwriting the emulator's frame. Scratch buffers grow only
when required and each is capped at 4 MiB. Native-size frames bypass scaling.

PPA scale factors have 1/16 precision. **Fit** selects the largest uniform
hardware factor that fits, preserves the whole image, and centers the result.
This can leave a few more border pixels than software scaling. **Full** selects
independent hardware factors for each axis and may also leave a narrow border.

| Source | Smooth Fit on 480 x 320 |
|---|---|
| GBA 240 x 160 | 480 x 320 |
| NES 256 x 240 | 336 x 315 |
| SMS 256 x 192 | 416 x 312 |
| Game Boy 160 x 144 | 350 x 315 |
| SNES 256 x 224 | 352 x 308 |
| Mega Drive 320 x 224 | 440 x 308 |
| Doom 320 x 200 | 480 x 300 |

The P4 PIE vector unit converts RGB565 rows to the LCD byte order eight pixels
at a time, enabling hardware misaligned accesses for odd-width/cropped rows.
Remaining pixels use a scalar path.
Filter Off keeps nearest-neighbor rendering; single-axis filters and unsupported
zoom geometries retain software rendering. Allocation/driver failure also falls
back to software. Bilinear filtering smooths edges and can soften pixel art;
Filter Off remains available for crisp pixels.

This is image-processing acceleration, without a neural model. Neural super
resolution would add inference cost and is not enabled. The 80 MHz SPI link
still needs at least 30.72 ms to transmit a full 480 x 320 RGB565 frame (about
32.6 full-screen frames/s before command overhead). Hardware scaling reduces
rendering work but does not remove that transfer limit.

Reference: [Espressif PPA API and scale precision](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/ppa.html).
Native regression: `python tools/test_p4_scaling.py --cc cc`. Optional launcher
diagnostics: configure `RG_P4_SELFTEST=ON` for pixel readback, endian/palette,
bilinear, odd-height and PIE alignment/tail tests on the device. Restore it to
`OFF` for production builds.

## Reserved pins and future wireless targets

GPIO34-38 (straps, with GPIO37/38 also used for the UART0 console), GPIO24-27
(USB full-speed functions), and GPIO41/42/48 are left unused by this peripheral map. GPIO2-5 use
the GPIO function rather than external pad JTAG; use USB Serial/JTAG or UART
for debugging instead. Flash and PSRAM connections are internal to the module.

The ESP32-WROOM-32D radio uses GPIO39/40/43/44 for SDMMC slot 1's
D0/D1/CLK/CMD and GPIO47 for EN/reset. Its 1-bit firmware leaves DAT2 disabled
to preserve the ESP32 GPIO12 flash-voltage boot strap. The SD card stays on SPI3;
GPIO45/46 remain exclusively SD-card signals. See [WIFI.md](WIFI.md).

USB HID/XInput host and SD-card MSC device mode use the P4 high-speed USB
interface (module pads **16 DM / 17 DP**). Use a suitable USB carrier/circuit.
Host operation needs a powered, current-limited 5 V VBUS supply; the module's
DP/DM pads do not supply VBUS. Host and MSC device modes are exclusive, as on S3.
Keep the flash/debug console on UART0 GPIO37/38, or the separate USB Serial/JTAG
interface supported by your carrier. USB wiring is not a GPIO remap of S3 USB.

## Build and install

From an ESP-IDF 5.5.5 environment at the repository root:

```powershell
python rg_tool.py --target esp32-p4-n16r32-st7796 build-img
python rg_tool.py --target esp32-p4-n16r32-st7796 --port COM3 install
```

Networking is enabled through the external SDIO radio; `--no-networking`
disables the application networking features.
Default apps include factory, launcher, retro-core, prboom-go, gwenesis, fmsx,
and retro-legacy. Install the complete image first. A packed `.img` includes
padding and must be written at flash offset **0**, although its P4 bootloader
is at **0x2000**. The factory updater uses that same P4 bootloader range.
Do not flash an S3 image on P4.

## Local build verification

Verified with ESP-IDF 5.5.5: all eight default applications build, and the full
image fits in 16 MB with the configured partition headroom. The packed image
passed footer/CRC, partition, chip-ID, v1.0-v1.99 revision-range, and exact
application-byte checks. All five release unit tests passed. The 28 wiring
assignments and module pad numbers were checked against both target configs
and the manufacturer pin table.

Native checks now pass with Zig as the host compiler: NES timing and sprite
patterns, display rendering, frame ownership/drop behavior, DMA FIFO ordering,
and both immediate and ramped audio gain paths. The CI workflow runs these too.

## Emulation performance and GBA

The display runs at 80 MHz SPI, as requested for the connected hardware, with
16-row DMA buffers. P4 emulator apps use owned frame snapshots and drop busy
presentations instead of waiting for LCD transfers. Contra scrolling now
sustains 60 emulated FPS in the measured runs. The 480x320 SPI link still limits
full-screen presentation to at most 32.6 FPS before overhead.

The target now includes an eighth app, `gbsp`, for Game Boy Advance, and uses
the P4 PPA for compatible RGB565 scaling. The hardware test verified native
RV32 JIT execution, PPA output pixels and save-state restoration. See
[performance measurements and audio findings](PERFORMANCE.md) and
[GBA setup and limitations](../../../../../gbsp/README.md).

## Hardware validation still required

On the connected COM3 board, revision v1.3 and 32 MB PSRAM were confirmed.
Enabling LDO4 resolved the SD initialization CRC error (0x109); the launcher
mounted `/sd` and read its settings and a 1,650-entry CRC cache from the card.
Mounting succeeded across three reset tests. All eight applications were then
updated on COM3 and flash hashes verified; the final launcher boot mounted the
card successfully too. The card was not formatted.

A later warm restart exposed a second initialization issue: IDF's SDIO CMD52
probe could return a function-number error (0x102) before CMD0 reset. This
memory-card-only SPI socket now reports SDIO CMD52/CMD5 probes as unsupported
in both filesystem and USB MSC initialization, so the normal SD memory-card
handshake proceeds. Three consecutive reset tests passed with this fix. Normal
SD data errors still use the existing recovery/retry checks. This does not
change a future radio's separate SDIO transport.

Compilation cannot verify wiring or a particular display module. Before calling
the port hardware-verified, check the boot log for v1.x, 16 MB flash and 32 MB
PSRAM/memory-test success; then check full-screen colors/orientation, every
button, SD reads/writes, emulation/audio, headphone routing, battery voltage
against a meter, LED/motor, USB HID/MSC, and a complete SD-image update/reboot.

## Sources checked

- [Wireless-Tag module datasheet v1.0 (pin table, module pads, N16R32 memory, power)](https://res.8ms.xyz/wiki_assets/WT99P4C5-S1/WT0132P4-A1_datasheet_v1.0_en.pdf)
- [Wireless-Tag N16R32X / v3.2 listing](https://shop.wireless-tag.com/products/new-p4-v3-2-version-esp32-p4-som-board-wt0132p4-a1-som-module-with-esp32-p4-chip-multi-interface-for-esp32-p4-ev-board-with-lcd-display)
- [Espressif P4 revision compatibility](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32p4/api-reference/system/chip_revision.html)
- [Espressif P4 GPIO functions and boot straps](https://documentation.espressif.com/esp32-p4_datasheet_en.html)
- [Espressif GPIO power-bank and SD I/O supply requirements](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html)
- ESP-IDF 5.5.5 local Kconfig.hw_support, Kconfig.cpu and esp32p4/Kconfig.spiram
  confirm pre-v3 revision selection, qualified-only 400 MHz, and HEX PSRAM mode.
- [Espressif ST7796 reference driver](https://github.com/espressif/esp-bsp/blob/master/components/lcd/esp_lcd_st7796/esp_lcd_st7796_general.c)
