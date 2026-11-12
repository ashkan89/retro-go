# WT9932P4-TINY N16R32 + ST7796 (480 x 320)

Target: `esp32-p4-n16r32-st7796`, ESP-IDF **5.5.5**. This is the Wireless-Tag
TINY development board with an external SPI ST7796 display and the onboard
rear microSD socket. It uses 16 MB QIO flash at 80 MHz and 32 MB HEX PSRAM at
200 MHz. P4 silicon revisions **1.0 through 1.99** are selected explicitly;
P4X / revision 3.x needs another target. Revision checks stay enabled.

## CPU and wiring

The CPU runs at **360 MHz**. ESP-IDF 5.5.5's pre-v3 runtime clock driver
rejects 400 MHz even when its experimental Kconfig override is enabled.
That setting caused the factory app to assert in `esp_clk_init` and reboot
on the connected revision 1.3 board before application startup.

Use the onboard J5 card socket in **4-bit SDMMC slot 0**, GPIO39-44, LDO4
at 3.3 V, up to 40 MHz. Remove the old external SPI SD module/wires on
GPIO45/46/9/10. Wi-Fi moves to slot 1 on GPIO49/50/52/53/54, including reset.
USB mass-storage mode uses the same onboard card and bus settings as FAT.

See [BOARD.md](BOARD.md) for the complete GPIO/header table, card contacts,
migration instructions, power requirements and official sources. The LCD
uses exposed SPI2 CS/MOSI/CLK pins at its existing 80 MHz setting; I2S, buttons,
battery and vibration use the exposed pins in that table. The onboard GPIO51
WS2812 uses GRB byte order, matching the manufacturer BSP. Use common ground,
3.3 V logic, and the existing external amplifier/backlight/motor drivers.

The emulator hot loops retain cached flash/PSRAM execution and the P4-specific
memory/JIT settings. JPEG artwork retains the hardware/software fallback paths.
See [WIFI.md](WIFI.md) for the external ESP32-WROOM-32D companion wiring.

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

## Reserved pins and USB

The [shared pin audit](BOARD.md) reserves FUSB, FPC, BOOT, UART/strap pins, and
GPIO51 for their board functions. Slot 0 belongs to the onboard card. Slot 1
routes the radio through GPIO49/50/52/53 with EN/reset on GPIO54. The two buses
have separate signals; neither reuses the other's pins.

HUSB supplies USB HID/XInput host or exclusive SD-card MSC device mode.
Host mode negotiates **full speed (12 Mbit/s)** on this same connector so
keyboards, mice and controllers can be used behind a USB hub. ESP-IDF 5.5.5
has no transaction translator for full/low-speed devices behind a high-speed
hub ([Espressif limitations](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/usb_host.html#features-limitations)).
`RG_USB_HOST_FULL_SPEED` applies before root-port power, including when a hub
is attached at boot. P4 HAL initialization and reset wrappers also restore the
pre-v3 UTMI host pulldowns and A-host session signals required for attachment
detection, including after root-port recovery. This backports the newer IDF
host setup without modifying the installed SDK. MSC device mode retains
high-speed capability.
Host operation requires a suitable powered/current-limited 5 V VBUS connection;
use a powered hub when the peripherals need more current. Enable **USB HID
input** in the input settings for keyboards, mice and HID gamepads, or **Xbox
controller input** for XInput controllers. HID gamepads may need button mapping.
Test with the hub attached at boot, hot-plugged, and unplugged/reconnected.
FUSB provides the separate Serial/JTAG flashing/debug interface.

Startup/reset regression: `python tools/test_usb_host.py --cc cc`.

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

## Boot-loop repair: COM3, 2026-11-11

The 400 MHz image repeatedly asserted at `0x48000d02` before factory startup.
Disassembly placed the assertion in `esp_clk_init`: the pre-v3 runtime clock
driver rejects 400 MHz. Both revision-1.x target defaults now select 360 MHz
and disable the experimental override; the native board checks enforce this.
The corrected factory app reached `Retro-Go ready` across three reset captures.
The corrected launcher also mounted `/sd` and reached `Retro-Go ready`.
All eight release apps were rebuilt with 360 MHz configurations and packed
into a 13,959,424-byte image. Image CRC, identity, partition layout and exact
app bytes passed validation; the bootloader and all apps were flashed with
esptool hash verification while preserving NVS and the selected boot app.
The final 45-second capture booted `retro-core`, mounted `/sd` (after a
probing-speed retry), and loaded a NES game without an assertion or reboot.
The external SDIO Wi-Fi companion did not initialize during that launcher
capture; this was logged without an assertion or reboot.

## Previous configuration build verification

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

## Previous 360 MHz / external-SPI hardware validation

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
  define pre-v3 revision selection, CPU clock choices, and HEX PSRAM mode.
- [Espressif ST7796 reference driver](https://github.com/espressif/esp-bsp/blob/master/components/lcd/esp_lcd_st7796/esp_lcd_st7796_general.c)
