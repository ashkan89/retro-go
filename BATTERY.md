# ESP32-S3 battery monitor and power wiring

This wiring is for **one protected 3.7 V nominal Li-ion/LiPo cell, 4.2 V fully
charged (1S)**. It is not a circuit for 2S packs, 4.35 V cells, LiFePO4, or
non-rechargeable cells. The ESP32 board does not charge a battery by itself.

## Firmware pins

| Firmware target | Battery ADC pin | ADC channel | Headphones |
|---|---|---|---|
| All eight `esp32-s3-n16r8-*` / `esp32-s3-n8r2-*` display variants | **GPIO1** | ADC1 channel 0 | Manually select Headphones under Options → Audio out. Auto uses Speaker. |
| Original `esp32-s3-devkit` | **GPIO4** | ADC1 channel 3 | Existing target audio configuration |

On the N16R8/N8R2 variants, **disconnect the old headphone-jack detect wire from
GPIO1**. GPIO1 is now exclusively the battery ADC. Do not short that ADC node to
ground through a jack's switching contact. The I2S headphone DAC/output selection
remains available. LCD, SD card, USB, and gamepad pins keep their existing mapping.
The original DevKit keeps its previous GPIO4 battery wiring.

Other ESP32-S3 boards (`crokpocket`, `rachel-esp32`, `brutzelboy`, `t-deck-plus`,
`fri3d-2024`) already enable battery monitoring and retain their board-specific
pins, divider ratios, and profiles. Follow their original hardware documentation;
the DIY circuit here is for the nine development-board targets in the table.

The nine targets in the table use calibrated ADC1 millivolts, so battery sensing can run while Wi-Fi
is active. The existing input task averages four samples every two seconds. The
shared profile doubles the divider voltage and reports an approximate voltage-only
percentage: 0% at 3.30 V, 100% at 4.20 V, clamped to that range. Actual state of
charge depends on the cell, temperature, load, and charging state; this is not a
coulomb-counting fuel gauge. No charger-status input is configured, so the firmware
does not independently detect charging. With the divider fitted, a disconnected
cell/sense switch reads low and the battery indicator is hidden.

## Voltage-sense circuit

Use two **100 kΩ, 1% resistors** and one **100 nF ceramic capacitor** (10 V or
higher rating). Place the capacitor and divider near the ESP32 pin.

```text
Protected cell P+ -- SW-B -- R1 100kΩ --+---- GPIO1  (N16R8/N8R2)
                                     |      GPIO4  (original S3 DevKit)
                              +------+------+
                              |             |
                           R2 100kΩ       C1 100nF
                              |             |
Protected cell P- ------------+-------------+---- ESP32 GND
```

`SW-B` is the sense pole of the master switch described below. Both resistors are
required: do not leave the ADC floating and do not connect the cell directly to a
GPIO. Measure at the protected battery terminals, **before** the 5 V boost and not
at the charger's SYS/OUT pin.

`Vadc = Vcell × R2 / (R1 + R2) = Vcell / 2`.

| Cell voltage | Expected ADC voltage | Firmware percentage (estimate) |
|---|---|---|
| 4.20 V | 2.10 V | 100% |
| 3.70 V | 1.85 V | about 44% |
| 3.30 V | 1.65 V | 0% |

The divider draws about 21 µA at 4.2 V while its switch is on. The ADC capacitor
reduces noise and supplies the sampling transient for this high-resistance divider.
Before connecting the ADC, check the midpoint with a multimeter: it should be half
the measured cell voltage. Never use USB 5 V as the battery measurement input.

## Power and charging circuit

Use a protected cell/pack (or a correctly rated 1S protection/BMS board), a
**1S 4.2 V Li-ion charger with power-path/load-sharing**, and a regulated **5 V boost
converter**. A BQ24074-based power-path module is one possible charger topology;
follow the actual module's pin labels, charge-current settings, and current limits.
A simple charger without load sharing is not a substitute when charging while
playing. Charge current must meet the cell manufacturer's specification.

```text
Dedicated charging USB 5V --------> Charger IN
Protected cell P+ ----------------> Charger BAT+
Protected cell P- ----------------> Charger BAT-/GND

Charger SYS/OUT --> 5V boost VIN
5V boost OUT+ ---- SW-A ----------> ESP32 board 5V/VIN
All system grounds --------------> ESP32 GND / protected P-

Protected cell P+ -- SW-B --------> R1 at top of sense circuit above
```

Use a **DPDT master switch**: SW-A and SW-B open/close together. SW-A isolates the
boost output from the board; SW-B disconnects battery sensing when the board is
off, preventing an unpowered ADC from being fed through its protection diodes.
Power the board and its external peripherals from the switched system rails.
The boost can still draw standby current while SW-A is open; select a suitable
low-standby/enable-capable module if long storage life is needed.

Supply the **board's 5 V pin**, not its 3V3 pin, from this 5 V regulator. Do not
connect a raw 4.2 V cell to 3V3 or directly to 5V/VIN. Size the regulator, protection,
cell, switch, and wiring for the complete system's peak load (ESP32, display,
speaker amplifier, and USB devices); a genuine 5 V / 2 A output capability gives
more headroom than a small ESP32-only supply. Converter input/cell current is
higher than its 5 V output current and must also be within ratings.

**When programming through the board's CH343/native USB port, switch SW-A/SW-B
OFF first.** Do not connect external boost 5 V and USB VBUS to the board together
unless your exact board has verified source isolation, or an appropriate external
power multiplexer isolates those supplies. Board clones differ. The charger may
use its own dedicated charging USB input according to its module documentation.

## Firmware builds and checks

Rebuild the applications after changing target wiring. All nine `esp32-s3-*` target configs
include the shared `components/retro-go/targets/esp32-s3-battery.h` profile.
`python tools/test_battery.py --cc cc` checks all target pin assignments, divider
math, presence handling, ADC averaging/errors, and optional manual-headphone routing.
An earlier firmware image, including an image flashed before this change, does
not gain battery monitoring until it is rebuilt and reflashed.

For the connected N16R8 ILI9341 setup, build all apps with a release version that
matches your GitHub data assets (example, in an ESP-IDF shell):

```sh
PROJECT_VER=5.0.1 python rg_tool.py --target esp32-s3-n16r8-ili9341 build-img \
  factory launcher retro-core prboom-go gwenesis fmsx retro-legacy
```

Flash the rebuilt image before connecting the new GPIO1 divider; the older image
still configures that pin for the headphone-jack pull-up.

## Hardware references

- [Espressif S3 DevKit pin map and power supply options](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
  identifies GPIO1 as ADC1_CH0 and GPIO4 as ADC1_CH3, and lists USB, 5 V, and 3V3
  board power options as mutually exclusive.
- [Espressif ADC calibration and noise guidance](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_calibration.html)
  describes reference-voltage calibration, multisampling, and the 100 nF input capacitor.
- [TI BQ24074 power-path charger](https://www.ti.com/product/BQ24074)
  explains simultaneous system power and battery charging with load sharing.
