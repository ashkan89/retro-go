# ESP32-S3 performance audit

Scope: N16R8 and N8R2 target definitions, shared display/audio drivers, emulator
frontends and compiler options. These are source-level findings; no physical board
or game benchmark was available. Do not interpret build/test success as measured
FPS, audio underrun, power, or wiring validation.

## Existing hardware configuration

Both variants already select dual-core operation, 240 MHz CPU, QIO flash at
80 MHz, and PSRAM at 80 MHz. N16R8 selects 16 MB flash and octal PSRAM (8 MB);
N8R2 selects 8 MB flash and quad PSRAM (2 MB). They have the same CPU capability;
extra memory does not itself raise emulator execution speed. Their different
PSRAM interfaces can affect bandwidth and cache-miss costs, so benchmark both
variants independently. Most emulation
cores already explicitly use -O2 or -O3, independently of the SDK's -Os default.

Emulation runs on core 0. Display and audio helpers run on core 1. Audio has a
higher task priority than display, a bounded internal-RAM software queue, and
8 DMA buffers of 192 stereo frames. At 32 kHz the software queue holds 48 ms
and DMA holds another 48 ms. These are capacity figures, not measured latency.
Reducing the queues needs an underrun/latency measurement on demanding games.

## Changes

- Display source-buffer release wakes blocked callers through a FreeRTOS event
  group. Previously `rg_display_sync(true)` polled with a minimum one-tick sleep;
  the configured 100 Hz tick could delay detecting completion by almost 10 ms.
  Nonblocking checks and source-buffer ownership rules are preserved. Completion
  means the renderer has finished reading the source, not that the LCD has scanned
  out the frame. SPI still uses its independent DMA staging buffers.
- Native-width RGB565 BE rows copy directly into DMA staging buffers, avoiding
  per-pixel map loads. Native-width indexed and RGB565 LE rows also bypass the
  x map. Scaled rows retain their existing mapping. Partial updates and
  repeated-row handling remain active.
- At full volume external I2S output sends the consumer's samples directly to
  the legacy driver's DMA write. It avoids a second stack-buffer copy and redundant
  sample gain operations. Other volume levels keep the original integer rounding;
  short writes and errors still propagate.
- SPI transaction descriptors explicitly allocate internal RAM, avoiding PSRAM
  access from the completion ISR. The SPI bus transfer limit explicitly matches
  the allocated staging buffer length. On S3 with IDF 5.5+, the SPI completion
  interrupt also runs on the configured display core instead of inheriting
  the emulator core that initializes the bus.
- Mega Drive and Doom frontend components now explicitly use -O2, matching the
  other emulator frontends. Their emulation cores already specify optimization.

## Display bandwidth limit

For RGB565 over single-lane SPI, payload-only maximum FPS is
`SPI_clock / (width * height * 16)`. Command transfers, interrupt/queue overhead,
scaling, checksums, and LCD timing lower actual full-screen throughput.

| Panel target | Configured SPI | Pixels sent for a full screen | Payload ceiling |
|---|---:|---:|---:|
| ILI9341 | 40 MHz | 320 x 240 | 32.55 FPS |
| ST7789 | 80 MHz | 320 x 240 | 65.10 FPS |
| ST7789V2 | 80 MHz | 300 x 240 | 69.44 FPS |
| ST7796 | 80 MHz | 480 x 320 | 32.55 FPS |

Partial updates can exceed these full-screen rates when much of the image is
unchanged. A 60 Hz emulation rate does not imply 60 complete LCD updates per
second. Forcing 100% CPU use cannot remove this bus limit, and would waste power
when the game is already keeping its intended timing. Keep audio and simulation
at their native rate while allowing automatic display frame skipping under load.

## Wiring review

The eight N16R8/N8R2 display variants use the following bus assignments:

| Function | GPIOs |
|---|---|
| LCD SPI2 | MOSI 13, CLK 14, CS 10, DC 11, reset 12, backlight 21 |
| SD SPI3 | MISO 39, MOSI 41, CLK 40, CS 42 |
| Shared I2S | BCLK 5, WS 4, DATA 6 |
| Audio output enables | speaker 9, headphones 46 |
| Battery ADC | 1 |
| USB | native USB pins 19/20 |

LCD and SD use separate SPI hosts. No assigned external peripheral uses the
N16R8's octal-memory pins 33-37. GPIO3 (Down), GPIO45 (A), and GPIO46 (headphone
enable) are strapping pins: verify external pulls, attached DAC modules, and
button states during reset against the module datasheet. Firmware cannot repair
an incorrect electrical strap. Keep LCD clock/data wiring short, use a solid
ground return and local decoupling, and verify signal integrity before increasing
any clock. Physical wiring, supply stability, amplifier loading, and panel
revision cannot be established from the target files alone.

## Verification and next measurements

Local ESP-IDF 5.5.5 app builds passed for all five emulator applications
(`retro-core`, `prboom-go`, `gwenesis`, `fmsx`, `retro-legacy`) on
N16R8/ILI9341, and for `retro-core` on N8R2/ST7796. These are app builds against
the build tool's temporary 3 MB partition, not a complete release image or a
validation of every display variant's full-image layout. No device was flashed.
The two new host tests also run in CI and release validation.

Host checks:

```sh
python tools/test_display_render.py --cc cc
python tools/test_i2s_gain.py --cc cc
python tools/test_audio_dma.py --cc cc
python tools/test_frameskip.py --cc cc
python tools/test_nes_timing.py --cc cc
```

The rendering check exercises the actual production function for native/scaled
BE/LE/indexed surfaces, padded strides, offsets, repeated rows, and unchanged
frames. The gain check exhausts all int16 values at every volume from 0 to 100,
all three external routes and mute states, and tests short/error DMA writes.
The DMA check exercises FIFO order, ring wrap, and complete-buffer batching.
These tests do not verify physical DMA deadlines or FreeRTOS wakeup latency.

On each board, compare identical game/save-state/settings scenes before and after:
emulation FPS, displayed full/partial frames, display busy/block time, audio write
errors and underruns, and free/largest internal heap block. Include NES scrolling,
SNES audio-heavy games, Mega Drive FM-heavy games, and large ROM loads on N8R2.
Check menu entry, save/load, speed changes, mute and headphone routing. Run for
at least 10 minutes per demanding scene with the normal controller attached.
Cache-size changes, further IRAM placement, higher clocks, SIMD kernels, and
smaller audio queues need these measurements and RAM/partition checks before
being made defaults.

References: [ESP-IDF speed optimization](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/performance/speed.html),
[SPI driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/peripherals/spi_master.html),
[hardware schematic checklist](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html).
