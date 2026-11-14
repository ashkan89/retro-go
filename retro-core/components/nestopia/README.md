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
The 8 MB S3 release targets use Nofrendo to keep all applications within flash;
16 MB S3 and P4 targets retain the experimental Nestopia backend.

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
- Asynchronous display targets use one source framebuffer and convert only
  when the display releases the previous frame. The display borrows that buffer
  until consumption completes, avoiding an additional RGB copy. Conversion uses
  `Video::Blit` after emulation and audio submission. Auto frameskip follows display
  availability, since skipping RGB output does not skip Nestopia's PPU work.
  Manual frameskip still works. Other emulators retain their adaptive policy.

P4 continues to use the shared PPA scaler and PIE pixel conversion. Emulation
stays on the main core and display/I2S tasks use the target's existing task
affinities. The CPU, PPU and APU retain their emulated-cycle synchronization.
S3 and P4 place the PPU execution loop and tile path in IRAM to reduce
instruction-cache contention, together with the sprite evaluation phases.
On P4, the cold FDS BIOS uses external BSS to
leave executable SRAM for these paths.

Enable `-D RG_NESTOPIA_PROFILE=ON` with `idf.py` for `NES-MEM`, `NES-ROM`,
`NES-MAPPER`, `NES-PERF` and `NES-CORE` serial records. The mapper record reports
the loaded cartridge's mapper/submapper IDs. The latter reports CPU cycles spent
inside PPU execution, APU transition integration and audio sample processing.
These counters have overhead and are disabled in normal builds. A 300-frame
window reports emulated, converted and submitted frame rates separately;
physical SPI panel updates can have a lower rate than emulation.
Add `-D RG_NESTOPIA_PROFILE_CORE=OFF` to measure frontend FPS without per-core
cycle-counter overhead; the `NES-CORE` totals are then zero. Release builds
disable profiling entirely.
Profiling keeps normal launcher and ROM selection; it does not select a test
ROM or change saved startup settings.

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
