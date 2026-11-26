# Experimental Nestopia backend

This component builds the native C++ API from the pinned
[ashkan89/nestopia-liberto fork](https://github.com/ashkan89/nestopia-liberto),
not its libretro frontend. The reviewed revision is
`f1e02d12664834536edbf7352995a3d0cd1caef4`.
Retro-Go's adapter is `../../main/main_nestopia.cpp`.

Initialize sources with `git submodule update --init --recursive`.
Select **Retro-Go > NES emulator** in menuconfig. Only the selected core and
its adapter compile; Nofrendo builds do not require the Nestopia checkout.
Nestopia uses C++ exceptions and disables its optional zlib dependency.
Its component also disables the narrowing diagnostic for upstream's ASCII
case-folding initializer, which promotes to `int` before assigning to 16-bit
`wchar_t` on Xtensa. The native regression test compiles and runs this path
with the production options and a 16-bit wide-character type even on Linux.
The S3 release targets use Nofrendo by default. P4 targets select Nestopia.

## Embedded execution path

`prepare_core.py` prepares a build-local copy of the pinned sources. It checks
every changed source anchor and fails on an unexpected upstream layout; the
submodule itself remains unchanged. `RG_NESTOPIA_PORT=OFF` builds the original
core for comparisons. The normal build enables the port on both P4 and S3.

- Separate ownership of the CPU callback map and PPU screen reduces the hot
  machine object to approximately 13 KiB. The machine and working screen request
  internal SRAM, with normal-heap fallback when it is unavailable. The large
  callback table and cartridge data can remain in PSRAM.
- Direct opcode dispatch uses the existing instruction implementations. The
  common no-DMA check is inline; DMA transfers, interrupt timing and bus effects
  keep their original implementation. P4 places the dispatcher, common opcode
  handlers, addressing helpers and standard memory callbacks in SRAM.
- Sprite X counters use a shared visible-dot clock and per-sprite deadlines.
  This avoids decrementing every pending sprite on every pixel. Rendering-off
  advances X timing while preserving frozen sprite shifters, including partial
  fetches. The audio mixer, resampler and filters retain their original math.
- Complete tile spans batch private sprite shifters while preserving the
  original background fetch and active OAM evaluation sequence.
  Cartridge address/scanline hooks and custom PPU accessors use a second tile
  specialization which calls the original bus operations at their original
  dots. Verified standard fetches read the same mapped memory directly. The
  known MMC3 IRQ-only address callback can use those direct reads as well;
  replacing an address hook clears that permission. Only A12 transitions need
  notification for that timer. Other address callbacks retain every fetch.
  Batches stop at CPU access boundaries, the dot-64 OAM reset and the final
  special sprite-zero tile. Secondary-OAM clearing and completed evaluation
  combine private writes inside these spans, preserving every observable latch
  and address value. Active evaluation retains its original dot sequence.
  This removes the old callback exclusion for MMC3 and its derivatives,
  MMC2/MMC4 latches, MMC5 graphics, JY Company, Oeka Kids and Nanjing boards.
  Scanline hooks remain outside the batched visible spans, in the scheduler.
- With standard accessors and either no callback or the known IRQ-only callback,
  pixel composition groups eight pixels after the tile's bus operations. Those
  callbacks cannot observe intermediate pixel stores or sprite-zero status.
  CPU accesses still stop the scheduler at their original dot, and arbitrary
  mapper callbacks retain the scalar pixel/status sequence. Dot-255 handling
  remains in the original scheduler. The inner tile function is explicitly in
  SRAM on P4 and forced inline into its SRAM parent on S3; marking only the outer
  function had left compiler-generated inner functions in cached memory.
- MMC3 synchronization uses conservative IRQ bounds to avoid entering the PPU
  after every CPU instruction. Disabled IRQs, remaining counter edges and the
  current background fetch pattern determine when synchronization is needed.
  Low-table fetches cannot raise A12 before sprite fetching. High-table fetches
  can remain rejected by the existing hold filter throughout a background run.
  CPU PPU/mapper accesses still synchronize before changing registers. The
  scheduler stops deferral at scanline boundaries, preserving pre-render frame
  shortening and frame-end NMI/audio ordering. Original bus callbacks and IRQ
  timestamps remain in use; other timer unit types retain their original hook.
- Cartridge detection and factory functions are marked cold so their large
  switches compile compactly. Runtime mapper read/write handlers retain their
  normal optimization. This makes room for the second tile specialization in
  existing devices' smaller emulator partitions.
  The iNES parser also compiles for size, outside runtime emulation.
- Aligned unfiltered RGB conversion groups four palette lookups and output
  writes, reducing narrow PSRAM stores without changing pixel values.
- The prepared core also offers `Video::PrepareRgbFrame()` for unfiltered
  16-bit output. Call it between frames, before `Execute`, including after a
  decoder, controller or output-format change. Eligible tested cartridge
  families with ordinary pads rasterize directly into RGB using a cached
  32-entry palette; palette RAM and encoded colors retain their original
  semantics. `Blit` then copies the completed frame into the separate display
  surface. Light sensors, other controllers and unsupported formats/boards
  return `RESULT_NOP` and retain indexed rendering. Disabling RGB clears the
  previous screen until the next frame renders. Indexed mode is the default
  for clients that do not call this optional API.
- P4 standard PRG-ROM reads can follow their live bank pointers directly. At
  board reset, every address in each eligible page must match the original
  pure reader and component. Changing a reader, component or linked cheat hook
  revokes that page's permission; writer-only changes preserve it. Bank switches
  update the existing bank pointers. Custom reads retain their callbacks.
  S3 retains its smaller original CPU map layout.
- Verified standard-memory spans combine secondary-OAM read/write pairs before
  the unchanged background bus operations. Complete sprite-fetch spans retain
  each pattern address and both nametable opens at their original dots.
  Arbitrary cartridge callbacks retain the interleaved path, and CPU access
  boundaries and the last sprite-fetch slot keep their original handling.
  Consecutive sprite Y misses commit their final private counters/write once;
  complete in-range copies use the original three phases directly. A corruption
  event still latches its first primary-OAM byte before copying the row.
- Asynchronous display targets use one source framebuffer and convert only
  when the display releases the previous frame. The display borrows that buffer
  until consumption completes, avoiding an additional RGB copy. Conversion uses
  `Video::Blit` after emulation and audio submission. Off/Auto waits for the
  display to release that surface and presents every completed frame.
  Manual frameskip still works. Other emulators retain their adaptive policy.

P4 continues to use the shared PPA scaler and PIE pixel conversion. Emulation
stays on the main core and display/I2S tasks use the target's existing task
affinities. The CPU, PPU and APU retain their emulated-cycle synchronization.
S3 and P4 place the PPU execution loop and tile path in IRAM to reduce
instruction-cache contention, together with the sprite evaluation phases.
On P4, the cold FDS BIOS uses external BSS to
leave executable SRAM for these paths.

Enable `-D RG_NESTOPIA_PROFILE=ON` with `idf.py` for `NES-MEM`, `NES-ROM`,
`NES-MAPPER`, `NES-CONFIG`, `NES-PERF`, `NES-AUDIO` and `NES-CORE` serial records. The mapper record reports
the loaded cartridge's mapper/submapper IDs. The latter reports CPU cycles spent
inside PPU execution, APU transition integration and audio sample processing.
These counters have overhead and are disabled in normal builds. A 300-frame
window reports emulated, converted and submitted frame rates separately;
physical SPI panel updates can have a lower rate than emulation.
Add `-D RG_NESTOPIA_PROFILE_CORE=OFF` to measure frontend FPS without per-core
cycle-counter overhead; the `NES-CORE` totals are then zero. Release builds
disable profiling entirely.
Profiling keeps normal launcher and ROM selection; it does not select a test
ROM or change saved startup settings. `NES-PRG` reports the number of pages
using verified direct PRG reads in prepared builds.
`NES-AUDIO` reports samples generated per wall-clock second and the live sprite
limit setting. On P4, profiling also enables I2S delivery timing diagnostics;
late writes indicate a delivery deficit, rather than counting DMA underrun
interrupts. Sustained emulation below the physical NES frame rate produces
fewer samples than the hardware consumes, which buffering alone cannot fix.

## Review of the P4 integration

The review covers `87e6f384` and `45f4f33a` (the two experimental P4 commits)
and the subsequent build fixes through `23c05ca1`.

Fixed integration defects:

- ZIP data previously used a nonseekable stream buffer. Nestopia queries stream
  lengths and seeks while loading images; the adapter now implements bounded
  absolute, relative and end-relative seeks.
- Image loading does not apply the desired machine region automatically. The
  adapter now selects `GetDesiredMode()` before powering on, then chooses its
  frame pacing and overscan from that region.
- Audio requests now use the allocated buffer capacity and submit only the
  sample count returned by the fork's patched sound API. This drains fractional
  sample carry without a hardcoded per-frame sample count or stale audio tail.
- Battery RAM, EEPROM and FDS patch callbacks were absent. The adapter now
  restores these files on image loading and powers off the machine during
  Retro-Go's shutdown event to trigger the save callbacks before SD unmount.
- FDS images now insert disk 1 side A after power-on. A **Disk side** menu
  option allows changing media sides. The BIOS path is
  `/retro-go/bios/disksys.rom` on the storage device, with `fds_bios.bin` as a
  legacy fallback.
- Backend selection now excludes the unused adapter and core from compilation.
  Missing Nestopia sources produce an actionable configuration error.
- Firmware CI initializes the submodule and runs a native core regression test.

## Saves and validation

Nestopia states and Nofrendo states are incompatible. They use Retro-Go's shared
state slot paths: use different slots when switching backends. Persistent native
data is stored separately as `<SRAM path>.nestopia.battery`,
`.nestopia.eeprom`, and `.nestopia.ips`. Saves are flushed on normal exit;
abrupt power loss does not run the shutdown callback.

Run `python tools/test_nestopia.py --cxx c++` with GCC or Clang, or pass the
absolute path to `zig.exe`. This compiles the pinned core and extracts the
adapter's actual stream, image-loading and persistence functions. Generated
ROMs exercise plain/ZIP loading, PAL/NTSC selection, 2,400 frames of
audio with buffer guards, state round trips and battery write/reload. The ZIP
container extraction is mocked; its returned ROM bytes pass through the real
adapter and core. No commercial ROMs are needed.

These checks do not establish hardware frame rate, I2S behavior, compatibility
across all mappers, or FDS/EEPROM gameplay persistence. Those require device and
game-specific testing. An ESP-IDF build verifies firmware linking and partition
fit separately from the host test.

For a differential comparison of the prepared and original cores:

```sh
python tools/test_nestopia.py --cxx c++ --upstream --artifacts .cache/nes-reference
python tools/test_nestopia.py --cxx c++ --artifacts .cache/nes-port --reference .cache/nes-reference
python tools/test_nestopia.py --cxx c++ --direct-rgb --artifacts .cache/nes-port --reference .cache/nes-reference
python tools/test_nestopia_oam.py --cxx c++
```

The additional 57,600 generated-ROM frames exercise PAL/NTSC backgrounds, sprites,
emphasis, NMI, OAM/DMC DMA, both controllers, save-state reloads, all base audio
channels, rapid rendering toggles during visible pixels and sprite fetches,
OAM latch-read checksums, all five decoder presets, and crowded sprites with
both the eight-sprite limit and unlimited sprites.
The real mapper IDs are 0, 2, 3, 4, 5, 9, 10, 64, 90, 96, 118, 119, 163, 165
and 209, covering standard fetches, A12 IRQs, CHR latches, MMC5 custom graphics,
nametable-driven banking and scanline hooks. NROM/UxROM/CNROM/MMC3 cases also
exercise PRG/CHR bank changes and MMC3 IRQ state. Another 15 scenarios exercise
every combination of address-line, CHR, nametable and scanline callbacks,
recording callback order, addresses and dot timestamps while changing banks
inside callbacks. Frame/audio fingerprints, final state bytes and callback trace
digests must match the original core. This covers callback mechanisms, not
every supported mapper's register logic or every commercial game.
Both cores render two warm-up frames first: upstream does not initialize the
background shift pixels at power-on, so the first NTSC visible frame can depend
on prior heap contents. A second allocation pattern checks the same post-boot
fingerprints.
The runner explicitly enables assertions even when Zig defines `NDEBUG`.
`tools/test_nestopia_tiles.py` additionally performs 262,144 comparisons of the
actual scalar, batched-sprite and eight-pixel implementations over 8-184-dot spans,
including clipping changes, private shift state and clock wrap.
`tools/test_nestopia_irq_bounds.py` compares the actual original and bounded
A12 timers across 600,000 instruction boundaries. It checks IRQ timestamps and
delivery boundaries with NTSC/PAL clocks, hold filters, delayed IRQs, register
writes, irregular address bursts and both background pattern layouts.
`tools/test_nestopia_video.py` compares 256 RGB conversions against upstream,
covering both output depths, buffer alignment, padded rows and buffer guards.

## ESP32-P4 NES and FDS benchmark

### Build and run

Use ESP-IDF 5.5.5 and the board's SPI panel target. The earlier WT9932P4-TINY
capture identifies rev 1.3 with the ILI9341 target. The later FDS capture used
the ST7796 target. Both pre-v3 targets now select the SDK's supported 360 MHz
setting. Keep that clock for every comparison. Use an ESP-IDF installation
without the experimental 400 MHz patch.

Run this from the repository root in an ESP-IDF PowerShell environment.

```powershell
$board = 'esp32-p4-n16r32-st7796'  # Use esp32-p4-n16r32-ili9341 for that panel
python rg_tool.py --target $board --release-build build retro-core
```

This produces `retro-core/build/retro-core.bin` and its ELF and linker map.
The regular `build-img` command assembles the complete installation image and
expands the `retro-core` partition to fit its roughly 2.11 MB app. Use
`flash retro-core` only if the installed partition table already has room for
the app binary. A benchmark installation with just the factory updater,
launcher and emulator bundle can be built with
`python rg_tool.py --target $board --release-build build-img factory launcher retro-core`.

The standard S3 build uses Nofrendo for NES. For example, build the N16R8
ST7789 app with
`python rg_tool.py --target esp32-s3-n16r8-st7789 --release-build build retro-core`.
For an FDS-capable S3 app, use `idf.py -C retro-core menuconfig` after that
build and select Nestopia under **Retro-Go > NES emulator**. Then run
`idf.py -C retro-core -DRG_NESTOPIA_BATCH_FDS_TIMER=ON app` with the pinned
source below. This app needs a matching S3 installation with enough room
in its `retro-core` partition. The `rg_tool.py` target preset selects
Nofrendo when it configures the app. Nestopia on S3 places the tile kernels
in instruction RAM and the PPU scheduler in cached flash to keep Xtensa
exception handlers within jump range.

For a controlled A/B, build the same frontend and display/audio drivers with
the scalar or batched FDS timer. The normal build above configures the project
and selects the board's SDK defaults.

```powershell
New-Item -ItemType Directory -Force .cache/nes-fds-ab | Out-Null
foreach ($variant in @('OFF', 'ON')) {
    idf.py -C retro-core "-DRG_NESTOPIA_BATCH_FDS_TIMER=$variant" app
    if ($LASTEXITCODE) { throw "Build failed for FDS timer variant $variant" }
    Copy-Item retro-core/build/retro-core.bin ".cache/nes-fds-ab/retro-core-$variant.bin"
}
```

Use the pinned submodule revision in both builds. If your submodule worktree
is at a different commit, extract the pin into an independent directory and
pass its absolute path with `-DRG_NESTOPIA_SOURCE_DIR=...` to `idf.py`, set
the `RG_NESTOPIA_SOURCE_DIR` environment variable before a fresh build, or use
`--source ...` to the host runner. The generated core stays in the build
directory. This example uses `.cache/nes-pinned` for the independent copy.

```powershell
$pin = 'f1e02d12664834536edbf7352995a3d0cd1caef4'
New-Item -ItemType Directory -Force .cache/nes-pinned | Out-Null
$archive = Join-Path (Get-Location) '.cache/nes-pinned.tar'
git -C retro-core/components/nestopia/upstream archive --format=tar "--output=$archive" $pin source
tar -xf $archive -C .cache/nes-pinned
$source = (Resolve-Path .cache/nes-pinned).Path
idf.py -C retro-core "-DRG_NESTOPIA_SOURCE_DIR=$source" app
```

### Replay manifest

Put a UTF-8 or ASCII file named `<full ROM filename>.bench` beside each
configured NES or FDS image under `/roms/nes` on the SD card. Launch that game
from the NES tab. Each launch automatically runs the selected scene and returns
to the launcher. The menu key aborts a run. Remove the sidecar to resume normal
play. Use one sidecar per trial and archive the CSV between firmware variants.
The FDS BIOS search uses `/retro-go/bios/disksys.rom` and then
`/retro-go/bios/fds_bios.bin`.

```text
# Frames are zero-based from power-on, including warm-up.
warmup 10
seconds 60
mode normal
frame 100 08 -1
frame 105 00 -1
frame 200 82 -1
frame 500 81 -1
frame 530 82 -1
frame 900 00 -1
```

The second event field is a hex pad mask held until the next row. The bits are
`01` A, `02` B, `04` Select, `08` Start, `10` Up, `20` Down, `40` Left and
`80` Right. The third field is `-1` or a zero-based disk-side index. For a
two-sided FDS title, `frame 1000 00 1` selects disk 1 side B at frame 1000. Use a
title-specific sequence that reaches the disk prompt. Frames must increase
strictly, with at most 1024 events. Each duration is converted to the same
number of emulated frames using native region timing. Both durations are
bounded to 600 seconds. A lagging target takes longer in wall time to finish.

Use **Frameskip Auto** and record scaling, filter, crop and sprite-limit
settings. Start with original size and filter off. Repeat every scene using
the same settings and starting save data. Use an idle or boot-through-load
FDS scene, a gameplay scene with expansion audio and a two-sided title with a
scripted change. For the slowest case use `seconds 600`. To include initial BIOS
frames in the timed window, use `warmup 0` in a separate boot run. The benchmark
uses distinct `/retro-go/saves/nes/<ROM>.sram.bench-save.nestopia.*` persistent saves and starts from
power-on. Archive those saves separately or start from a clean set for each
trial. A normal return flushes changes to those disposable saves.

The app appends results beside the input as `<ROM filename>.bench.v2.csv`. Mode
codes are 0 normal, 1 no LCD, 2 no audio, 3 both sinks bypassed, and 4 normal
with per-frame profiling disabled. Modes 2 and 3 run unpaced for throughput
isolation. Mode 4 provides an instrumentation-cost comparison. Keep both
normal sinks active when measuring user-visible pacing.

The existing `-DRG_NESTOPIA_PROFILE=ON -DRG_NESTOPIA_PROFILE_CORE=ON`
diagnostic build adds `NES-CORE` serial counters for PPU work, APU clock
walking and sample generation. Run that build separately, since periodic
logging and cycle collection affect frame timing.

- `changed_transfers` counts display updates with pixel data after the SPI
  driver drains outstanding transfers. `unchanged_updates` counts submissions
  whose pixels the partial-update path reused. The SPI path does not expose
  physical scanout or panel refreshes that held the preceding image. An
  in-flight transfer can cross either end of the measured window.
- Interval percentiles are emulation frame-end intervals in 100 µs bins,
  with a final overflow bin. A deadline miss is a frame interval over the
  native period plus 1 ms. Skipped submissions include fixed frameskip and a
  busy display. RGB conversion, display source acquisition, PPA scaling,
  the final SPI drain and I2S writes have separate time totals. The I2S
  queue depth is sampled at frame boundaries. The CSV also counts frames
  borrowed directly from the RGB raster and frames whose pixel writes were
  suppressed while emulation continued.
- I2S full waits measure producer backpressure. I2S write errors and gaps
  longer than the entire DMA buffer capacity are recorded separately. The
  legacy I2S driver does not expose a PCM underrun counter.
- BIOS file access, image loading, warm-up, the side-change API call and
  observed FDS drive read/idle transitions have separate fields. Drive
  timings are proxies for loading and report `-1` when the event is absent.
  FDS reads and writes use the in-memory disk image within gameplay.
  The separately timed power-off phase is where the core passes changes to
  the adapter for IPS output.

### Host replay and functional check

```powershell
python tools/test_nestopia.py --cxx C:/tools/zig/zig.exe --artifacts .cache/nes-host `
    --benchmark 'X:/roms/nes/game.fds' --bios 'X:/retro-go/bios/disksys.rom' --repeat 2
```

The runner uses the same core, sample rate, native RGB565 format, controller
mask and side-change sequence. By default it reads the ROM's sidecar.
`--manifest`, `--sprite-limit` and `--palette` allow explicit selection.
Extract the selected ZIP member first. The host runner hashes uncropped
RGB565 and mono PCM after warm-up, times Execute and Blit separately, and
reports unpaced host throughput. Run again with `--scalar-fds-timer` and a
separate artifact directory to compare. Match frame counts and checksums
before comparing relative speed. The generated regression ROM and FDS
fixture also run with `tools/test_nestopia.py` without local game images.
Use `--benchmark-copy` to measure the intermediate RGB frame copy against
direct access to the core raster. The host has no LCD backpressure.

```powershell
$cxx = 'C:/tools/zig/zig.exe'
python tools/test_nestopia.py --cxx $cxx --fds-only --opt-level 3 --scalar-fds-timer --artifacts .cache/fds-scalar
python tools/test_nestopia.py --cxx $cxx --fds-only --opt-level 3 --artifacts .cache/fds-batched --reference .cache/fds-scalar
```

| Host replay | Native FPS | Timed frames | RGB565 fingerprint | PCM fingerprint |
| --- | ---: | ---: | --- | --- |
| Contra (USA) | 60.098814 | 3606 | `138b43b7412c3ba1` | `9cd2e47ae0f756a3` |
| Mario Bros. (Europe) | 50.006979 | 3000 | `a6251dd6a820c7e8` | `6ca48b063922d258` |

Each NES pair produced identical hashes across both launches and across the
scalar and batched builds with the local BIOS loaded. The two
generated FDS scenarios delivered identical video, PCM and save-state bytes
with either timer and matched the unmodified pinned core. Across three
interleaved host runs per timer, the 1,440-frame FDS fixture took a median
3.215 s in Execute with the scalar timer and 3.087 s with batching. These
host measurements describe fixture cost while the SDK builds were running.

Two host replay passes per presentation path used 3,606 timed frames per
game. Each game produced identical RGB565 and PCM hashes with both paths.
These times cover frontend RGB delivery only. The device benchmark measures
LCD delivery separately.

| NTSC gameplay | Intermediate copy time | Direct raster time |
| --- | ---: | ---: |
| Contra (USA) | 75.0–93.9 ms | 4.8–5.5 ms |
| Super Mario Bros. 3 (USA), Rev 1 | 72.0–82.0 ms | 6.7–6.8 ms |

### Port comparison

The launcher passes a file path through the app boot arguments. The native
adapter loads the image, calls `Emulator::Execute`, submits batches of 32 kHz
PCM and hands borrowed 256×240 little-endian RGB565 surfaces with 512-byte
pitch to the display task. The P4 emulation task owns the machine on CPU 0.
For compatible Nestopia games on async targets, the display worker borrows
the core raster itself. The previous intermediate frame was measured in
PSRAM in a P4 capture. During a busy LCD transfer, Auto continues CPU, PPU
and APU execution and suppresses writes into the borrowed raster. It still
checks sprite zero and executes mapper callbacks. Light-sensor controllers
keep the indexed render path. The default S3 Nofrendo backend alternates
its two indexed surfaces and borrows the completed one without a snapshot
copy. Auto may drop the submission while the most recent frame is still
borrowed.

The I2S worker supplies the pacing clock through bounded backpressure. At
32 kHz the 1536-frame software ring and the eight 192-frame DMA buffers
each hold 48 ms. On dual-core P4 and S3 the legacy driver's I2S/GDMA
interrupt is allocated on the audio worker's core. Nestopia caches the
mixer window divisor and uses a 32-bit divide with an exact correction,
keeping the original quotient and preserving PCM. `rg_system_tick` updates
statistics. Manual frameskip can also wait for display ownership. The
existing renderer scales sharp pixels on the CPU or uses PPA bilinear
scaling in the smooth mode.

| Source | What transfers to this port |
| --- | --- |
| [Retro-Go Nofrendo adapter](https://github.com/ducalex/retro-go/blob/4ced120669750ca7228fd0414211430c1d923166/retro-core/main/main_nes.c) | Keep normal launcher and state handlers, submit rotating or borrowed surfaces and batch PCM. |
| [Nestopia libretro adapter](https://github.com/libretro/nestopia/blob/39e4b456cea6f5e962ce3e65bcdcd7313711a905/libretro/libretro.cpp) | Use core clock ratios for the region and follow the core's audio output count. The native frontend uses 32 kHz directly with I2S and has no frontend resampler. It avoids the upstream frontend's optional software NTSC filter. |
| [tangstoney P4 Nofrendo adapter](https://github.com/tangstoney/esp32p4-nesemu/blob/7baf1171ff83aca5b0a526cd3143560660601731/main/nofrendo-esp32p4/osd_p4.c) | Separate emulation from panel delivery. The MIPI-DSI port with an embedded ROM and a different core and audio rate has a different scanout and memory budget. |
| [RetroESP32-P4 NES app](https://github.com/giltal/RetroESP32-P4/blob/af143812f704bff5f76afc5407b6134e7ea0561d/apps/nes/main/main.c) | Keep ROM selection in the launcher, the game loop in the emulator app, and reboot to the launcher on exit. |
| [FCEUmm disk and IRQ](https://github.com/libretro/libretro-fceumm/blob/fb919fb6fa881aaa6279b765d480220579e5c606/src/fds.c), [FDS APU](https://github.com/libretro/libretro-fceumm/blob/fb919fb6fa881aaa6279b765d480220579e5c606/src/fds_apu.c) | Batch idle timer clocks and keep disk data resident. Nestopia's event clocks, mount delays and sound implementation define the compatibility reference. |

ESP-IDF 5.5.5 defaults pre-v3 silicon to 360 MHz. Both targets retain HEX
PSRAM XiP with 64-byte cache lines. The existing PPA path aligns DMA output
buffers to that cache-line size.
[Clock Kconfig](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_system/port/soc/esp32p4/Kconfig.cpu),
[PPA buffer requirements](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/ppa.html),
[SMP scheduling](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/system/freertos_idf.html),
[I2S DMA](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/i2s.html).

Review the linker map when comparing the core's `-O3` and IRAM placement
against the SDK's size-optimized defaults. The v5.5.5 pre-v3 compiler
configuration uses `rv32imafc_zicsr_zifencei_xesppie` and `ilp32f`.
[ESP-IDF speed guide](https://github.com/espressif/esp-idf/blob/v5.5.5/docs/en/api-guides/performance/speed.rst).

Payload-only RGB565 ceilings at the configured SPI clocks are 32.55 complete
updates/s for 480×320 at 80 MHz and 32.55 for 320×240 at 40 MHz. A 240×224
viewport raises those ceilings to 93.01 and 46.50 respectively, before LCD
commands, DMA overhead and scaling. Partial updates can deliver changed
regions faster. The P4 renderer already uses PIE where it fits RGB565
packing. ESP-DSP offers P4 kernels for operations such as dot products and
filters. An ESP-DL model would consume frame time and memory on the same
high-performance cores as the emulator and its output workers. The existing
SPI and I2S paths provide the baseline for measuring that budget.
[ESP-DSP](https://github.com/espressif/esp-dsp/blob/3c8ac0fdfec83740b783e200862c8d0c056de0ad/README.md),
[ESP-DL](https://github.com/espressif/esp-dl/blob/24ac077101e77f14f66561b0306158ce42486e1b/README.md),
[ESP32-P4 revision 1.3 TRM](https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_technical_reference_manual_en.pdf).

### Target results

The A/B below requires the physical board. Compare the earlier batched
timer build with the current build on the same SD card and at 360 MHz.
Keep the ROM, replay, scaling, and filter settings fixed. With Auto
frameskip, compare `mode no-lcd` with normal gameplay to estimate how much LCD
backpressure costs. The current CSV also reports how often the LCD borrowed
the PPU raster and how many emulated frames suppressed raster writes.
Record at least two passes per scene for each build, then perform a
ten-minute soak of the slowest FDS scene. The FDS entries are scene
templates for two local titles. An earlier ST7796 FDS trace in this
workspace recorded roughly 16–17.5 ms in Execute and 1.7 ms of frontend
video time per frame.

| Game / scene | Native FPS | Emulated FPS A/B | LCD transfer FPS A/B | Frame p95/p99 A/B | I2S gap events A/B | Principal cost |
| --- | ---: | --- | --- | --- | --- | --- |
| Contra (USA), gameplay | 60.099 | Pending | Pending | Pending | Pending | Pending |
| Mario Bros. (Europe), PAL timing | 50.007 | Pending | Pending | Pending | Pending | Pending |
| FDS title 1, BIOS boot and first disk load | 60.099 | Pending | Pending | Pending | Pending | Pending |
| FDS title 1, gameplay with expansion audio | 60.099 | Pending | Pending | Pending | Pending | Pending |
| FDS title 2, gameplay and side change | 60.099 | Pending | Pending | Pending | Pending | Pending |

Use both builds for a separate compatibility pass with disposable saves.
Save and reload NES SRAM and emulator states, reset during play, verify
both controllers, change FDS sides from the menu, and verify a disk write
survives normal exit and relaunch. Use the title's own save or loading screen
to decide whether it completed an operation.
