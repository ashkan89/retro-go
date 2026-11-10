# ESP32-S3 performance audit

## Loading and scheduling follow-up (November 10, 2026)

The four N16R8 defaults now restore flash execution: `SPIRAM_XIP_FROM_PSRAM`,
`SPIRAM_FETCH_INSTRUCTIONS` and `SPIRAM_RODATA` are disabled. The recent XiP
default copies each application's code and constants at boot, including every
launcher-to-emulator switch. This is separate from loading a ROM from SD.
Restoring the previous policy removes that boot copy and leaves more PSRAM for
ROMs and framebuffers. It may trade away instruction-fetch performance; no S3
before/after hardware timings are available. N8R2 already used flash execution.
P4 keeps XiP: its faster memory interface and larger RAM budget are different,
and its existing PPA/PIE/JIT paths are retained.

Shared `rg_storage_read_at()` now seeks only at entry and after a failed chunk
is reopened. Successful 16 KiB chunks stay sequential, preserving libc buffering
and avoiding redundant FatFs seeks. Absolute-offset semantics, bounded transfers,
short-read detection and sticky-error recovery are preserved. Fault injection
covers a failure after a successful first chunk and a subsequent random read.

Other storage limits found during the review:

- S3/P4 SDSPI starts at at most 10 MHz. A failed data command enables
  single-sector reads until remount and may reduce the clock repeatedly, down to
  1 MHz. A mount recovered at probing speed also stays slow. These protective
  paths can dominate ROM load time; inspect the SD recovery/clock messages
  before attributing a slow load to PSRAM.
- The explicit SD formatter chooses 512-byte clusters. FatFs cannot combine
  file data across cluster boundaries into one contiguous disk read, so large
  files on such cards have more command/FAT overhead. This change does not
  reformat existing media or alter that storage contract.
- IDF 5.5.5's `SDSPI_HOST_DEFAULT()` already selects 16-sector bounce-buffer
  chunks for non-DMA destinations. Setting that field again is unnecessary.
- GBA eagerly fills its ROM cache (up to 24 MiB on P4, 4 MiB on S3, subject to
  free memory). Reducing this preload moves SD page misses into gameplay.
  Keep the larger cache for smoothness; startup and sustained FPS are separate
  measurements. Large compressed GBA games also incur unpacking/cache-file I/O.

### Emulator and task paths reviewed

| Application / emulators | Timing and execution path | Decision |
|---|---|---|
| retro-core: NES | scanline audio, native refresh timing, bounded audio submission | retain existing timing and sprite optimizations |
| retro-core: GB/GBC, SMS/GG/Coleco, SNES | per-frame emulation, audio backpressure, adaptive presentation skipping | retain native simulation/audio rate and stable frame ownership |
| retro-core: PC Engine | timed vsync plus dedicated PSG task on audio core | retain separate synthesis; needs hardware latency profiling before moving it |
| retro-core: Game & Watch / Lynx | frontend/core timing and shared display/audio | covered by shared loader and renderer changes; game benchmarks remain needed |
| gwenesis: Mega Drive | incremental FM/PSG audio and nonblocking presentation | retain CPU overlap and frame skipping |
| fmsx: MSX | core refresh/UPeriod and batched stereo audio | retain core-specific scheduling |
| prboom-go: Doom | game tic clock, asynchronous mixer/output and display ownership | retain independent simulation/render clocks |
| retro-legacy: Atari 7800 / Spectrum | core step, audio-paced producer and display availability | retain presentation skipping without skipping audio |
| gbsp: GBA | interpreter on S3; RV32 JIT and ROM paging on P4 | retain executable-cache publication and ROM reserve |

Emulator main tasks use CPU0; display/SPI completion and audio workers use
CPU1. Audio priority 9 exceeds SPI reclamation 7 and rendering 6; input is 6,
system monitor 5, main 1. The PC Engine synthesis worker is priority 8. These
workers block on queues/DMA, allowing the main task to execute. Raising main
above device service tasks can increase underruns and queue stalls. Launcher
dual-core media builds have their own topology and must be assessed separately.
There is no hardware benchmark evidence here that reversing these affinities
improves emulator throughput.

### Hardware opportunities and limits

S3 is a dual-core **Xtensa LX7**, whereas P4's application cores are **RISC-V**.
S3 SIMD requires S3-specific kernels; the P4 PIE assembly cannot be reused on
it. P4 already dispatches RGB565 conversion through PIE and scale/rotation
through PPA with software fallback and explicit buffer/cache handling.
PPA accelerates scale/rotate/mirror, blending and fill; hardware JPEG assists
compressed images, not raw emulator pixels. Camera ISP, CSI, H.264 and neural
image inference do not accelerate console instruction emulation or an SPI LCD.
AI upscaling would add work and memory traffic and needs its own measured budget.
The LP core is not another application core for these FreeRTOS emulator tasks.

PPA, CPU frame snapshots, JIT and instruction/data fetches can compete for PSRAM
bandwidth. Keep frequently accessed state and DMA bookkeeping internal where
memory permits; measure cache changes instead of maximizing every cache or
allocating all RAM. SPI panel payload ceilings below remain binding even if
emulation reaches full speed. Aim for native game speed, bounded audio latency
and consistent presentation deadlines, rather than forcing busy loops to show
100% utilization.

Primary references: [S3 speed and task placement](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/performance/speed.html),
[S3 external RAM and XiP](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/external-ram.html),
[P4 PPA and memory contention](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/ppa.html),
[P4 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf).

Host fault, timing and renderer tests validate functional behavior, not device
load time, FPS, cache contention, electrical stability or actual RTOS deadlines.
An A/B run on each board with the same SD card, ROM, save state and settings is
still required before claiming a measured speedup or optimal affinity.

Follow-up host validation passed: storage fault injection (including the new
sequential/reopen cases), 120 renderer cases, frame snapshot ownership, PPA
geometry/conversion dispatch, 150 audio FIFO/DMA scenarios, exhaustive int16
gain/mute/ramp checks, 32 NES timing scenarios and 262144 sprite cases,
Auto/Manual frameskip checks, and all eight release tests. Storage tests now
support Windows directory APIs and explicitly keep C assertions enabled.
ESP-IDF 5.5.5 app builds passed for `retro-core` on N16R8/ILI9341
(1,181,200 bytes) and `fmsx` on P4 N16R32/ST7796. The generated S3 config
confirms all three XiP/code/rodata relocation options are disabled. These are
representative app builds, not complete release images or every board variant.
No device was flashed and no new on-device performance measurements were made.

## P4 changes reviewed for S3 (November 2026)

The N16R8 ILI9341, ST7796, ST7789 and ST7789V2 targets enable PSRAM
frame snapshots (`RG_DISPLAY_ASYNC_FRAMES`). The earlier S3 XiP default is
reverted in the follow-up above.
Snapshots let emulation continue while the display task reads a stable copy;
they cost an extra source framebuffer in PSRAM. XiP copies flash instructions
and rodata to octal PSRAM at boot. These changes require hardware benchmarking
before claiming a speed improvement.

N8R2 retains its smaller memory configuration: copying app code/data and adding
another framebuffer would compete with ROM loading in its 2 MiB quad PSRAM.
Both memory variants already use the recent shared bulk frame-copy path,
software JPEG fallback fixes, display wakeups, audio optimizations and quiet
release logging. P4 PPA scaling, PIE assembly, hardware JPEG, RV32 JIT and
ESP-Hosted companion settings cannot be transferred to S3 as board options.
S3 continues to use its native Wi-Fi and software scaling/JPEG paths.

Both S3 memory variants also drop obsolete defaults for the bootloader and
FreeRTOS mutex checks. IDF 5.5 derives those values. The image header flash-mode
string is corrected to `dio`, as required by IDF for booting QIO flash; the
`CONFIG_ESPTOOLPY_FLASHMODE_QIO` selection still enables QIO at runtime. This
prevents the build tool repeatedly deleting valid builds because generated
SDK settings disagree with those legacy defaults.

N16R8 includes the experimental GBA interpreter with a separate 2 MiB app
partition; GBA opts out of XiP to preserve its PSRAM ROM cache. N8R2 has neither
flash space for that additional partition nor sufficient PSRAM for this port's
runtime reserve. See [GBA setup and limitations](gbsp/README.md).

Validation: ESP-IDF 5.5.5 built GBA for all four N16R8 display variants
(approximately 948 KB each, within the 2 MiB partition) and the ILI9341
launcher with XiP enabled (1,546,560 bytes, within its 0x1C0000 partition).
Eight release tests, 120 host renderer cases and the frame-ownership harness
passed. No S3 device boot, game compatibility or FPS measurement was performed.

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
