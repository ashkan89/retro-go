# WT9932P4-TINY: shared P4 board wiring

Applies to `esp32-p4-n16r32-st7796` and `esp32-p4-n16r32-ili9341`.
These are the original WT9932P4-TINY / WT0132P4-A1 N16R32 boards with
revision 1.x silicon, 16 MB flash and 32 MB HEX PSRAM. P4X / revision 3.x
firmware is different; the revision compatibility checks remain enabled.

## CPU frequency

Both targets request **400 MHz** with ESP-IDF 5.5.5's
`CONFIG_ESP_FORCE_400MHZ_ON_REV_LESS_V3` and `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400`.
The board feature page advertises a 400 MHz maximum. Espressif's chip-specific
Kconfig nevertheless requires additional qualification for revision <3.0
and labels the override experimental. The earlier connected board reported
revision 1.3; its qualification for 400 MHz has not been established here.
Firmware revision compatibility checks remain set to v1.x silicon.

Flash remains 80 MHz QIO and PSRAM remains 200 MHz HEX. Existing 360 MHz
measurements in the performance notes describe the previous configuration.

## Onboard microSD socket J5

Use the card socket on the back of the TINY board. Some board kits supply
an unpopulated footprint; the socket, pull-ups and decoupling must be fitted.
The manufacturer schematic and BSP agree on this **4-bit SDMMC slot 0** map:

| J5 contact | Signal | GPIO / supply |
|---|---|---|
| 1 | DAT2 | 41 |
| 2 | DAT3 | 42 |
| 3 | CMD | 44 |
| 4 | Card power | LDO4, 3.3 V |
| 5 | CLK | 43 |
| 6 | Ground | GND |
| 7 | DAT0 | 39 |
| 8 | DAT1 | 40 |

These are PCB connections, not new jumper wires. Remove the old external SPI
SD module/wires from GPIO45 (CS), GPIO46 (MOSI), GPIO9 (CLK), and GPIO10 (MISO).
Insert the card in J5. GPIO39-44 are now exclusively for the onboard card;
disconnect any old Wi-Fi wires from them before using this configuration.

Firmware enables LDO4 at **3.3 V** before accessing the card, including in USB
mass-storage mode. The schematic includes 10 kOhm CMD/DAT pull-ups and card
decoupling on that rail. Use ordinary 3.3 V SD signaling, with no 1.8 V/UHS
voltage switching. The clock is capped at **40 MHz** and negotiated down for
cards that need 20 MHz. There is no configured card-detect/write-protect GPIO.

Normal FAT access and exclusive USB MSC mode share the same host/slot settings.
USB mode restarts into an exclusive raw-card session; the PC and local FAT
filesystem never own the card together. Storage teardown releases slot 0,
preserving the companion's separate slot 1 when it is active.

## External peripherals

All numbers below are GPIO numbers. Header positions follow the manufacturer's
left/right pin table, using the board silkscreen for orientation.

| Connection | GPIO | Header position |
|---|---:|---|
| LCD CS | 28 | Left 20 |
| LCD MOSI / SDA | 29 | Left 21 |
| LCD SCK | 30 | Left 22 |
| LCD DC | 31 | Left 23 |
| LCD RESET | 32 | Left 17 |
| LCD backlight driver | 33 | Left 16 |
| I2S BCLK | 20 | Left 5 |
| I2S LRCLK / WS | 21 | Left 6 |
| I2S DATA to speaker/headphone DACs | 22 | Left 7 |
| MAX98357A SD_MODE circuit | 17 | Left 2 |
| PCM5102A XSMT | 19 | Left 4 |
| A button | 2 | Right 12 |
| B button | 3 | Right 11 |
| Select button | 4 | Right 10 |
| Start button | 5 | Right 9 |
| Menu button | 6 | Right 8 |
| Option button | 11 | Right 5 |
| Up button | 12 | Right 4 |
| Down button | 13 | Right 3 |
| Left button | 14 | Right 2 |
| Right button | 15 | Right 1 |
| Battery divider midpoint, ADC1_CH0 | 16 | Left 1 |
| Vibration transistor input | 18 | Left 3 |
| Onboard WS2812, GRB byte order | 51 | Already on PCB |
| Wi-Fi SDIO CLK to ESP32 GPIO14 | 49 | Right 17 |
| Wi-Fi SDIO CMD to ESP32 GPIO15 | 50 | Right 16 |
| Wi-Fi SDIO DAT0 to ESP32 GPIO2 | 52 | Right 15 |
| Wi-Fi SDIO DAT1/IRQ to ESP32 GPIO4 | 53 | Right 14 |
| Wi-Fi reset to ESP32 EN | 54 | Right 13 |

The LCD uses exposed GPIO28/29/30 on SPI2. These belong to the alternate octal
pin bank; the existing single-data-line driver routes them through the GPIO
matrix. The default single-line CS/MOSI pins GPIO7/8 are not on the headers.
ILI9341 stays at 40 MHz;
ST7796 stays at its existing 80 MHz setting. LCD MISO stays disconnected.
I2S uses exposed general-purpose pins, with independent output-enable pins.
Buttons use internal pull-ups and connect to ground; Start + Select opens Menu.
Battery, amplifier, backlight and vibration circuits retain their external
drivers/protection described in the root AUDIO.md and BATTERY.md files.

The radio uses **slot 1**, 1-bit SDIO at the existing conservative 1 MHz rate.
Its five signals are grouped on adjacent exposed header positions, avoiding
the card, LCD, audio, buttons, LED, and boot/UART pins. Move all five old radio
wires to the new pins above. See [WIFI.md](WIFI.md) for the required pull-ups
and ESP32 flash-voltage strap handling; the companion firmware is unchanged.

GPIO0/1/7/8 serve the FPC interfaces. GPIO24/25 serve FUSB. GPIO35 is BOOT,
and GPIO34/36/37/38 are straps or UART0 pins; they are not peripheral assignments
in this map. GPIO26/27 remain available for a future MIPI display. HUSB provides
the native high-speed USB connection for HID/XInput host or MSC device mode.
Those USB modes remain exclusive. Power through USB or the 5 V input, use
common ground and 3.3 V signal levels; the 3.3 V header is a power output.

## Validation and references

`python tools/test_p4_board.py --cc cc` checks both target pin maps, signal
collisions, the native slot settings, USB raw-card lifecycle/error cleanup and
the older 1-bit SDMMC configuration. Compilation/host mocks do not establish
400 MHz qualification, physical SD signal integrity, or the rewired radio link.
The new configuration has not been flashed as part of this change.

- [Wireless-Tag board feature and header-pin table](https://wiki.wireless-tag.com/docs/en/WT9932P4-TINY/board_features.html)
- [Wireless-Tag user guide V2.0, J5 and header tables](https://res.8ms.xyz/wiki_assets/WT9932P4-TINY/WT9932P4-TINY_UserGuide_V2.0.pdf)
- [Wireless-Tag schematic V1.3, J5 and LDO4](https://res.8ms.xyz/wiki_assets/WT9932P4-TINY/WT9932P4-TINY_SCH_V1.3.pdf)
- [Manufacturer BSP: slot 0, bus width, GPIOs, LDO4 and GRB LED](https://github.com/Wireless-TAG-Maker/WT_BSP/blob/main/components/wt_bsp/boards/WT9932P4-TINY/board.c)
- [Espressif 5.5.5 CPU-frequency qualification requirement](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_system/port/soc/esp32p4/Kconfig.cpu)
