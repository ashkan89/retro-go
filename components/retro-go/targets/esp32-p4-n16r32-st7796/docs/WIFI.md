# ESP32-WROOM-32D Wi-Fi companion for ESP32-P4

Both `esp32-p4-n16r32-st7796` and `esp32-p4-n16r32-ili9341` use this wiring.
`st7799` is not a repository target; the 480 x 320 target is ST7796.
Build with ESP-IDF **5.5.5**. The P4 still has no internal radio: the original
ESP32 supplies 2.4 GHz Wi-Fi through ESP-Hosted **2.13.1** and
ESP Wi-Fi Remote **1.3.1**. Bluetooth is disabled.

## Connect with power off

All numbers are GPIO numbers. These pins replace the previous radio mapping;
the onboard SD card now owns GPIO39-44 on native SDMMC slot 0.

| P4 GPIO | TINY header | ESP32-WROOM-32D / development board | Function |
|---|---|---|---|
| 49 | Right 17 | GPIO14 | SDIO CLK |
| 50 | Right 16 | GPIO15 | SDIO CMD |
| 52 | Right 15 | GPIO2 | SDIO DAT0 |
| 53 | Right 14 | GPIO4 | SDIO DAT1 / interrupt |
| 54 | Right 13 | EN / CHIP_PU | P4 pulls EN low to reset companion |
| GND | GND | GND | Common signal ground |

The radio uses **SDMMC slot 1, 1-bit SDIO at 1 MHz** through the GPIO matrix.
The card uses **slot 0, 4-bit SDMMC up to 40 MHz** on the onboard PCB traces.
Move the old P4 radio wires from GPIO43/44/39/40/47 to the five pins above.
Remove the old external SPI SD module/wires from GPIO45/46/9/10, and insert
the card in the onboard J5 socket. See [BOARD.md](BOARD.md) for its contacts.

The radio's new header group avoids the SD-card I/O bank and the other
peripherals. Storage powers the onboard card and GPIO39-44 using fixed LDO4
at 3.3 V. The hosted driver's separate LDO/UHS controller stays disabled;
neither bus requests a 1.8 V switch. Slot-scoped teardown preserves the other
device when one client is stopped. UART0 and USB flashing remain available.

Use **3.3 V logic** and short, matched connections under **5 cm**. Prefer a
small PCB or soldered connection with adjacent ground returns. Jumper-wire
signal integrity must be tested; begin at this conservative 1 MHz clock.
Add an external **10 kOhm pull-up to ESP32 3.3 V** on each of CMD (GPIO15),
DAT0 (GPIO2), DAT1 (GPIO4), and DAT3 (GPIO13): four resistors. DAT3 needs its
local pull-up although it is not wired to the P4. Do not pull up CLK or GPIO12.
Leave ESP32 GPIO12/DAT2 disconnected from the P4; retain its default pull-down
(or fit 10 kOhm to GND) so the 3.3 V flash boots normally. P4 GPIO41/42 belong exclusively to the onboard card, not the radio. Firmware enables internal pull-ups
on the active bus pins and ESP32 DAT3 to assist initial prototyping. These
weak internal pulls do not replace the external resistors for reliable operation.

For initial testing, power each development board through its own USB cable
and connect grounds. Do not join their power rails while both are USB powered.
A **bare WROOM module** needs regulated 3.3 V rated for at least 500 mA, local
decoupling, an EN pull-up/reset circuit and GPIO0 high for normal boot. Never
apply 5 V to a bare module's supply, EN or GPIO pins. A development board's
5V/VIN input differs from the bare module supply.

### ESP32 flash voltage and boot straps

The COM9 ESP32 was read as ESP32-D0WD-V3 revision 3.1, 4 MB flash. Its eFuse
summary reports `XPD_SDIO_FORCE=0`: GPIO12 selects flash voltage at reset.
Pulling GPIO12 high would select **1.8 V**, which is wrong for its **3.3 V flash**.

The companion has one small, reproducible patch to upstream ESP-Hosted:
`SDIO_SLAVE_FLAG_DAT2_DISABLED` and `SDIO_SLAVE_FLAG_INTERNAL_PULLUP` are added
to the ESP32 slave driver configuration. This fixed low-speed profile also
forces `SDIO_SLAVE_FLAG_DEFAULT_SPEED` and `SDIO_SLAVE_TIMING_NSEND_PSAMPLE`
(send on the falling edge, sample on the rising edge), matching the 1 MHz
host clock even if an existing SDK config retained upstream high-speed defaults.
The internal pull-ups apply to CMD,
DAT0, DAT1 and DAT3; DAT2/GPIO12 stays excluded.
This is Espressif's explicit driver exception for **1-bit-only** operation without
using DAT2. The matching P4 configuration also selects 1-bit mode. Unlike a
standard unmodified SDIO slave, this firmware does **not** need a GPIO12/DAT2
pull-up. No eFuse changes are made or required. Do not substitute an unmodified
companion binary or switch the host to 4-bit with this firmware.

At 1 MHz the raw data-bit ceiling is 125 kB/s before bus and protocol overhead;
actual Wi-Fi throughput must be measured after wiring. Faster clocks can be
considered only after link validation. A later 4-bit design would require
restoring DAT2, adding its pull-up and resolving the flash-voltage strap.
The inspected eFuse values are saved in `.cache/esp32-efuses.txt`.

GPIO2/DAT0 is another boot strap: its pull-up can prevent serial download mode.
For later COM9 reflashing, disconnect the SDIO bus/pull-ups and P4 reset wire,
or hold GPIO2 low along with GPIO0 when entering download mode. For normal
operation GPIO0 must be high. P4 GPIO54 connects only to ESP32 EN, not GPIO0.

## Build and flash

From the repository root in an ESP-IDF 5.5.5 PowerShell environment:

```powershell
# Builds the pinned upstream companion firmware in .cache/esp-hosted-mcu/slave.
./tools/wifi-companion/build.ps1 -Flash -Port COM9

# Build and install the complete P4 image (ST7796 display on COM3).
python rg_tool.py --target esp32-p4-n16r32-st7796 --release-build build-img
python rg_tool.py --target esp32-p4-n16r32-st7796 --port COM3 install

# Use this target instead for the ILI9341 display hardware.
python rg_tool.py --target esp32-p4-n16r32-ili9341 --release-build build-img
```

The two P4 images select different displays: install only the image matching
the connected screen. The same companion firmware works with either target.
`--no-networking` still removes the application's networking features.

## Enable and verify

Boot both boards after wiring. In the launcher's **Wi-Fi options**, configure
a station profile with a **2.4 GHz** SSID/password and enable Wi-Fi. Existing
profiles on the SD card are retained. No credentials are built into firmware.
The host transport initializes in `rg_network_init()` after the application
runtime and shared 3.3 V I/O supply are ready. CMake removes ESP-Hosted's
automatic global constructor, which asserted during startup on this XiP P4
configuration. Actual radio initialization waits until Wi-Fi is enabled.
With Wi-Fi disabled, the unwired board can still boot. An enabled saved profile requires a working
companion connection; disable it before disconnecting the radio.

Successful verification requires a hosted transport handshake, a station
connection and a DHCP IP address on the P4. Check these in serial logs, then
test the web service or update check. A successful firmware flash alone does
not prove the inter-board link works. ESP32 console logs use COM9 at 115200
baud, independently of the SDIO data link. P4 logging follows the target's
existing console routing; COM3 is its USB Serial/JTAG flashing interface.

If initialization fails, check common ground, EN held high,
matching firmware versions, pull-ups and bus wiring, then reset both boards. Do not
replace ESP-Hosted with ESP-AT firmware: the wire protocols are different.

References: [Espressif ESP-Hosted](https://github.com/espressif/esp-hosted-mcu/tree/v2.13.1),
[SDIO setup](https://github.com/espressif/esp-hosted-mcu/blob/v2.13.1/docs/sdio.md),
[Wi-Fi Remote](https://components.espressif.com/components/espressif/esp_wifi_remote/versions/1.3.1),
and [the ESP-IDF DAT2-disabled flag](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_driver_sdio/include/driver/sdio_slave.h).

## Previous wiring verification: 2026-11-09

These results used P4 GPIO43/44/39/40/47 and an external SPI SD card.
They do not validate the new header group or concurrent onboard SDMMC access.


The existing station profile connected and obtained a DHCP address after
adding firmware internal pull-up assistance and changing the ESP32 to
default-speed, negative-edge-send/positive-edge-sample timing. At 1 MHz the
connected ST7796 P4 passed 20/20 pings with 1400-byte payloads and five
identical HTTP 200 web-page transfers. The 55-second connection capture had
no SDIO CRC errors or RPC response timeouts. Tests used the prototype wiring
without external resistors; fit the four external pull-ups for final hardware.
Both display target launchers were rebuilt. No credentials or eFuses changed.
