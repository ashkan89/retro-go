# ESP32-S3 performance audit

## FDS rendering and program-memory follow-up (November 14, 2026)

FDS with ordinary pads can now use the same direct-RGB raster as supported
cartridges. Screen-sensitive controllers and unsupported output formats retain
indexed rendering. The display still borrows a separate completed RGB surface;
the validated CPU memcpy path and Auto presentation policy are unchanged.

On P4, the pure FDS RAM readers at $8000-$DFFF and BIOS reader at $E000-$FFFF
register with the existing verified-page cache. The three RAM page references
follow the fixed arrays, so disk loading and self-modifying program writes are
immediately visible. Reader replacement revokes the page, and resets revalidate
it. The $6000-$7FFF callbacks remain in use and move to IRAM alongside the FDS
RAM/BIOS wrappers. Cartridge read dispatch and its cache layout are unchanged.
FDS disk registers, timer clocks, IRQ delivery, side changes and expansion audio
keep their existing implementations. The memory/register separation follows
[NESdev's FDS reference](https://www.nesdev.org/wiki/Family_Computer_Disk_System).

A generated BIOS and two-sided disk fixture compares 1,440 frames with upstream,
including video, PCM and byte-for-byte final states. It exercises FDS expansion
sound/modulation, IRQ acknowledgement, RAM code including self-modification,
disk ejection/side changes, a mid-run state reload, the Zapper fallback, and page
revocation/reset. RGB and indexed runs match. The existing 57,600-frame cartridge
suite, callback traces, final states and 2,400 audio guard frames also match;
these establish correctness, not a hardware FPS guarantee. S3 compiles with its
original CPU map and omits the P4 FDS read-cache additions.

The live baseline for `Super Mario Bros. 2 (Japan) (En).fds` on COM3 uses
240x224, Filter OFF, Auto and Sprite limit OFF. After startup exclusions, 86
300-frame windows have 52.755 median FPS (51.92-55.17), 17,180 us execution,
1,719.5 us video and 28,088.4 PCM samples/second. No presentations were omitted
in the retained baseline windows. An optimized hardware capture is still pending
the SD-card recovery/power cycle; no new FDS FPS result is asserted yet.

The live flash-table check rejected the older cached diagnostic before writing:
this firmware layout now gives retro-core `0x20a000` bytes. Cold XML/ROM-set
parsers use size optimization to fit diagnostics; emulator hot paths remain -O3.

## Nestopia scaling backpressure correction (November 14, 2026)

The previous every-presentation wait also paced emulation from the LCD. This was
a regression for large viewports: a complete 480x320 RGB565 frame takes at least
30.72 ms over the configured single-lane 80 MHz SPI link, before command and
queue overhead ([ESP-IDF SPI driver documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/spi_master.html)).
Partial updates can avoid unchanged rows, but scrolling games
cannot rely on that saving. Filter OFF uses the software nearest-neighbor path;
PPA does not remove the panel's transfer budget.

Nestopia Auto now checks source-buffer release without waiting. If the display
still owns the previous RGB surface, CPU/PPU/APU execution and audio continue,
and that presentation is omitted. The next available presentation uses the
newly completed frame; no backlog of old frames accumulates. It does not restore
the old escalating Auto frameskip ladder. Manual 0 still presents every frame
and waits when necessary, and explicit manual skip values retain their cadence.

The production-path regression test covers 600 native-rate presentations, a
35-ms display consumer, unchanged pixels/metadata during a borrow, Manual 0/2,
menu crop redraws and Auto/Manual switches. The slow-display simulation advances
600 emulated frames in about 10 seconds in Auto rather than over 21 seconds
under the previous blocking policy. Existing renderer, scaler and frameskip
checks pass, and the S3 release builds successfully.

Live Mario 3 checks on COM3 cover Fit (342x320), Full (480x320) and OFF (240x224),
with Filter OFF, Sprite limit ON and Auto. Full-scaling windows mostly remain
around 58-60 emulated FPS while the number of submitted presentations varies
with dirty rows and display availability. Scaling OFF had no busy-display
omissions. These are live inputs, not identical-input replay; some unscaled
windows still exceed the CPU frame budget, so this is not a locked-60 claim.

A vector-copy experiment averaged 301 us for memcpy and 231 us for PIE across
48 alternating, poisoned copies into the actual RGB destination. Those initial
frame checks passed, but an expanded synthetic-pattern test over 256 hardware
alignment/length/guard cases failed. The experiment and diagnostic were removed;
normal RGB output retains memcpy. The failed benchmark is not a supported copy
path or a measured whole-frame FPS improvement. The direct-RGB 57,600-frame
comparison, 2,400 audio guard frames, traces and final states match upstream
exactly; the existing indexed/RGB paths and their ownership rules are retained.

During diagnostic installation, an earlier saved partition table proved stale:
at that point the device had a `0x210000`-byte retro-core partition at `0x2d0000`,
not `0x214000`. A 2,163,216-byte diagnostic exceeded it by 528 bytes and the launcher
correctly rejected app switching. The overlapping write also affected Doom's
first sector. Doom was rebuilt from current sources and its app-only restoration
was hash-verified. The helper now reads the live table before every write and
checks the sector-rounded image extent. Flashing commands targeted app partitions
only. Size-optimizing the cold UNIF parser and cartridge database makes room for the
diagnostics without reducing optimization of emulation hot paths.
The user subsequently installed release 5.8.9 with a `0x20c000`-byte retro-core
partition; subsequent app-only diagnostic writes read and honor that new limit.
The final normal P4 image is 2,136,752 bytes with profiling and the failed RGB
copy diagnostic absent, leaving 9,552 bytes in that installed partition.
It was app-only flashed and hash-verified on COM3; the user confirmed the game
running afterward. The final startup log shows the native 240x224 viewport;
the scaled measurements above came from the earlier profiling build.

## Nestopia RGB and sprite follow-up (November 14, 2026)

The no-frameskip follow-up removes the second palette lookup from normal
16-bit output: eligible cartridges render RGB directly into the internal PPU
working screen, then copy that completed frame into the separate surface
borrowed by the display. Palette writes, emphasis and decoder changes update
the small cached output palette. Light sensors and other screen observers keep
indexed output. Off/Auto now waits for display ownership instead of dropping a
completed presentation when the display is busy.

Two further PPU paths group secondary-OAM evaluation and complete sprite fetches
between CPU access boundaries. They require standard memory accessors and
either no address observer or the known IRQ-only MMC3 observer. Other callbacks
retain the interleaved path. Both pattern addresses, 8x16-sprite address changes,
nametable opens and their dot timestamps remain intact. This follows the
observable OAM phases described by [NESdev's sprite-evaluation documentation](https://www.nesdev.org/wiki/PPU_sprite_evaluation)
and avoids replacing [MMC3's address-edge behavior](https://www.nesdev.org/wiki/MMC3)
with a fixed one-interrupt-per-line approximation.

The P4 PRG read path verifies all 256 readers and their components in a page
before following a standard bank pointer directly. Reader replacements and
cheat links revoke that page's permission. Writer-only mappings do not revoke
it, and bank changes remain visible through the live pointers. A broader cache
of uniform read callbacks was slower in the live Mario 3 trial and was removed.
S3 omits the PRG transformation to retain its original CPU map memory usage.

Both indexed and direct-RGB paths pass the 57,600-frame upstream comparison,
including per-frame video, PCM audio, final save states and callback traces.
Additional checks exercise RGB opt-in, controller/format fallbacks, mode changes,
and unaligned/padded output with byte guards. Live performance remains a separate
measurement; passing these comparisons does not establish locked 60 FPS.
The sprite fixture now alternates the 8x16 pattern-table bit across adjacent
slots. A separate 200,000-span randomized test compares grouped OAM evaluation
against the pinned per-dot phases, including misaligned addresses, both sprite
limits, overflow and corruption. It reproduced and guards the first-byte latch
ordering when corruption occurs at the start of a grouped span.

Diagnostic builds now report generated PCM samples per wall-clock second and
I2S delivery timing. Slow gameplay windows supply fewer than 32,000 samples per
second to a 32 kHz output. The delivery-gap counters are not hardware underrun
interrupt counts. The existing complete-DMA-block queue is retained: increasing
its reserve cannot eliminate a sustained sample deficit. Driver review uses the
installed SDK's [ESP-IDF 5.5.5 I2S documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/i2s.html).
The user reported improved jitter on the first RGB build; heavy-scene slowdown
still requires further optimization.

Live captures use Sprite limit ON, frameskip Off/Auto and the 240x224 viewport.
The first two 300-frame windows after each ROM load are excluded, as are windows
whose emulated, rendered and submitted rates differ. Both mapper-4 games report
all 128 direct PRG pages active. These are different live scenes, without an
identical-input replay, so small differences cannot isolate individual changes.

| Game / capture | Steady 300-frame windows | Median emulated/submitted FPS | Range |
|---|---:|---:|---:|
| Mario 3, first two level runs | 27 | 58.71 | 58.18-60.24 |
| Tournament Fighters | 29 | 60.22 | 56.54-60.58 |
| Mario 3, later capture | 250 | 60.22 | 60.09-60.34 |

The later Mario capture does not establish full speed in every level. The user
confirmed an active Tournament Fighters fight and good graphics/audio in both
games. Full-speed windows often have zero late audio-delivery gaps; slower
windows still produce fewer samples than the output consumes. **Heavy scenes
can still fall below 60 FPS.** An incidental FDS capture remained around 55 FPS;
disk images retain indexed rendering and do not use the direct PRG path.

A final copy experiment used a cache-line-aligned RGB destination and the
installed SDK's [AXI GDMA memory-copy API](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/system/async_memcpy.html).
It poisoned and flushed the destination before each DMA trial, checked every
byte and included submission/completion waits. At 400 MHz, CPU copy plus cache
writeback averaged 270,094 cycles (~675 us), versus 290,948 cycles (~727 us) for
DMA. DMA verification passed, but the trial did not improve the live Mario
capture (58.6 median FPS). Both the DMA and framebuffer-allocation experiments
were removed; the tested PRG/OAM/RGB sources were restored byte-for-byte.

The normal P4 release is app-only flashed and hash-verified on COM3, with
profiling disabled (2,160,688 bytes). It fits the installed `0x214000`-byte
partition. The final S3 release links at 1,858,416 bytes with its original CPU
port layout. The user confirmed game startup, graphics and sound on the installed
normal release. A local rebuild after the source commit is 2,160,704 bytes, also
within the installed partition, with profiling disabled. The upstream submodule
and managed dependencies are unchanged.

## Nestopia mapper callback paths (November 14, 2026)

Tournament Fighters on the COM3 P4 exposed an exclusion in the earlier tile
optimization: any cartridge PPU callback forced scalar execution. Eight live
300-frame baseline windows recorded 39.93 median emulated/submitted FPS,
39.24-40.56 FPS range, and 23,507 microseconds median execution time. Detailed
core profiling was disabled, so the zero `NES-CORE` counters in these captures
do not measure execution-path coverage. This is a live-game comparison, not
a deterministic input replay.
Both reported slow games identify as mapper 4 (MMC3). Contra identifies as
mapper 2 and provides a check of the earlier path without an A12 timer.

Complete callback tiles alone did not produce a useful Tournament Fighters
speedup. A subsequent partial-tile sprite attempt measured around 35 FPS and
was discarded. Detailed profiling put approximately 17-18 milliseconds of each
23-millisecond frame inside the PPU; audio integration/sampling was under one
millisecond.

Earlier whole-scheduler direct reads, a general A12 notification filter and a
scanline sprite cache were measured without a useful game speedup and discarded.

The retained implementation defers MMC3's per-instruction PPU synchronization
while an IRQ cannot occur. Bounds use IRQ enable/count state, the filter's hold
interval and the current background fetch pattern. Low-table fetches cannot
raise A12 before sprite fetching; repeated high-table edges remain rejected
throughout a background run when the hold interval exceeds eight dots and the
next edge is already blocked. IRQ clocks and delivery boundaries are retained.
CPU PPU/mapper accesses synchronize before register changes, and
deferral stops at scanline boundaries. Other timer unit types use the original
instruction hook.

An initial version delayed frame-end NMI scheduling until after the CPU's final
audio update. It matched video but changed audio filter state, so it was never
flashed. The scanline boundary and pre-render guards fix that ordering, and the
full video/audio/state comparison now passes.

Complete callback tiles retain their original fetches. Verified standard
accessors can additionally use direct reads with the known MMC3 IRQ-only
address callback; only A12 transitions need notification to that timer. Replacing
the address hook clears this permission. Other callbacks keep every notification.
With standard accessors and no arbitrary callback, eight-pixel composition
commits pixels and sprite-zero status after that tile's bus operations. These
callbacks cannot observe the intermediate writes; CPU access boundaries and the
special final tile keep their original handling. Empty sprite tiles use a
background-only loop.

ELF inspection found that marking the outer tile function for SRAM had left
compiler-generated inner functions in cached memory. Their P4 sections are now
explicit; S3 forces them inline into the SRAM parent to avoid a template section
conflict. P4 also places the CPU dispatcher, common handlers, addressing helpers
and small memory callbacks in executable SRAM. The machine and PPU working
screen still reside in internal SRAM on the tested P4; the larger RGB surface
can fall back to PSRAM.

Final profiling uses normal input and the 240x224 viewport, with frontend
counters enabled and detailed core scopes disabled. Startup/menu windows whose
converted/submitted counts differ are excluded. Captures are live gameplay,
not an identical-input replay; small differences between intermediate builds
are not established speedups.

| Game | 300-frame windows | Median emulated/submitted FPS | Range | Median execution |
|---|---:|---:|---:|---:|
| Tournament Fighters, previous speedup | 8 | 39.93 | 39.24-40.56 | 23,507 us |
| Tournament Fighters, mapper follow-up | 24 | 52.71 | 48.07-57.22 | 17,409 us |
| Super Mario Bros. 3, mapper follow-up | 12 | 50.76 | 49.33-52.51 | 17,991 us |
| Contra, mapper follow-up | 3 | 60.16 | 58.85-60.19 | 14,501 us |

Tournament Fighters' median FPS increased approximately 32%. Mario 3's earlier
profiling iteration measured about 36-37 FPS; that is not an untouched-release
baseline. The later eight-pixel iteration measured 49.46 FPS before the final
CPU/memory placement and background-loop changes. A short Sprite limit ON trial
did not establish locked 60 FPS. **Tournament Fighters and Mario 3 still fall
below 60 in heavy scenes.** No universal full-speed claim is made.

All callback installation sites in the pinned core were reviewed:

| Mechanism | Affected board implementations | Execution |
|---|---|---|
| A12 address IRQ timer | MMC3/derivatives, RAMBO-1, JY Company, Future Media, TF1201, Batlab SRRX, Dragon Ninja | MMC3 uses transition-only notifications with matching IRQ clocks; other units keep every fetch |
| Nametable address banking | Bandai Oeka Kids | Original address callback before the fetch |
| Custom CHR access | MMC2/MMC4, MMC5, JY Company, Waixing SH2, CNROM open bus | Original CHR accessor, including bank/latch side effects |
| Custom nametable access | MMC5 | Original nametable accessor |
| Visible/blank scanline hooks | MMC5, JY Company, Nanjing | Original scheduler hook outside the batched visible span |

The native differential suite now checks 57,600 PAL/NTSC frames across 15 real
mapper IDs and all 15 combinations of address, CHR, nametable and scanline
callbacks. Every video/audio fingerprint, final state, per-frame video digest,
and callback trace digest matches the original pinned core. Callbacks change
banks while recording their order, addresses and pixel-dot timestamps. A second
execution of the previous callback-tile implementation also matched all 600
state, trace and per-frame artifact files. The 262,144 scalar/batched/eight-pixel
comparisons cover 8-184-dot spans, clipping changes, private shift state
and clock wrap. The A12 scheduling unit checks 600,000 CPU instruction boundaries
with identical IRQ clocks and delivery boundaries for NTSC/PAL, hold 0/8/16,
delayed IRQs, register writes, irregular address bursts and both pattern layouts.
The video comparison renders two warm-up frames before measuring: upstream
leaves background shift bytes uninitialized at power-on. This avoids comparing
allocator-dependent pixels in the first NTSC visible frame. A second heap
allocation pattern produces identical post-boot fingerprints.
This checks the shared mechanisms; it does not establish compatibility or
full speed for every game and mapper register implementation.

Cartridge detection/factory functions are marked cold, preserving normal
optimization of runtime mapper handlers. Images are checked against the actual
installed `retro-core` partition at `0x2d0000`, capacity `0x214000`, rather than
the build's larger generic partition. The device partition table was read and
the existing emulator partition was backed up. Only the emulator application
was written; esptool verified its hash.
The final P4 release is 2,156,512 bytes; profiling strings are absent. The S3
N16R8 ILI9341 release also links (1,853,856 bytes). Only P4 hardware was measured.
After the final app-only flash, release serial captures reached NES startup and
the gameplay viewport without a panic. The upstream submodule is unchanged.

## Nestopia execution and presentation (November 13, 2026)

The COM3 device is an ESP32-P4 revision 1.3, built for
`esp32-p4-n16r32-st7796` at 400 MHz with 200 MHz HEX PSRAM. A Contra serial
capture with the original core recorded 27 steady 300-frame windows: median
46.34 emulated FPS and 7.72 submitted FPS, with 21,266 microseconds median
execution time. Observed emulation ranged from 39.33 to 52.52 FPS across game
scenes. These are live-game observations, not a deterministic input replay.

The retained implementation was measured again with per-core cycle scopes
disabled: 31 steady 300-frame Contra windows had a median of **58.92 emulated
FPS and 58.92 submitted FPS**, versus 46.34/7.72 originally. Median execution
time fell from 21,266 to 15,308 microseconds. Every steady window converted and
submitted all 300 frames. The measured range was 55.60-60.42 FPS: this is a
substantial improvement, but **a locked 60 FPS in every scene is not established**.
The current viewport is 240x224 on the 80 MHz SPI ST7796; larger scaled viewports
have different display bandwidth costs. Normal firmware disables all profiling.
The 2,177,680-byte P4 release was written only to the installed `retro-core`
partition at `0x2d0000` (capacity `0x214000`). Esptool verified the flash contents;
a 70-second release serial capture reached NES rendering without a panic.
Profiling/replay strings are absent from the release image. The device's
bootloader and partition table were preserved.
After benchmarking, the saved test ROM boot selection was cleared and startup
was set to Launcher. The diagnostic replay hook was removed from the source;
two subsequent resets loaded the launcher with the optimized emulator restored.

An independent RGB worker, a CPU callback cache, deferred pixel composition and
a paired RGB lookup were tested and discarded after slower or unchanged hardware
results. The ESP32-S3 N16R8 ILI9341 release also links successfully (1,871,808-byte
application), with its PPU loop/tile path in IRAM. Only the P4 is connected, so
there is no S3 hardware FPS claim.

The embedded port separates the large CPU callback map from hot state, requests
internal SRAM for the machine and PPU working screen, uses direct opcode
dispatch, and inlines the no-DMA path. Sprite X counters use dot deadlines to
avoid per-sprite counter writes at every pixel. Uninterrupted complete tile spans
batch sprite shifters, OAM clearing and completed evaluation. Standard PPU fetches
avoid indirect dispatch; cartridge callbacks used scalar execution in that
iteration. The mapper follow-up above removes this restriction. The PPU
loop, tile path and sprite evaluation phases use IRAM on S3/P4; the cold P4 FDS
BIOS uses external BSS to leave executable SRAM for these paths. Aligned RGB
conversion groups output stores. The original mixer, resampler,
filters, CPU instructions, PPU sprite shifters and bus timing remain in use.
The first memory/dispatch iteration alone did not materially improve Contra;
the complete port also includes the sprite and presentation changes.

Nestopia's automatic presentation policy now follows the asynchronous display
queue. Unlike Nofrendo's scanline renderer, skipping RGB output in Nestopia
does not avoid its PPU simulation. Adaptive skip increasing to five therefore
removed five of every six presentations while leaving most of the work.
Manual frameskip and other emulators' adaptive policies remain available.
The frontend uses one source surface on asynchronous display targets and
converts the completed screen after audio submission when the display is free.
The display borrows this surface until consumption completes, eliminating an
extra RGB frame copy. P4 PPA/PIE rendering and
the existing display/audio task affinities are retained.

Native differential validation compares the original pinned core with the
prepared port: 7,680 PAL/NTSC stress frames have identical complete video/audio
fingerprints and identical final state bytes. The generated ROMs exercise
backgrounds, sprites, emphasis, NMI, OAM/DMC DMA, controllers, state reloads,
all base audio channels, rendering toggles during pixels and sprite fetches,
and crowded limited/unlimited sprites. Another 131,072 randomized cases compare
the actual scalar and batched sprite code over 8-184 dots, including clock wrap,
changing clipping and priority. OAM latch-read checksums and all five decoder
presets also match upstream.
Generated NROM, UxROM, CNROM and MMC3 programs cover PRG/CHR bank changes and
MMC3 IRQ state, including the scalar path used for cartridge PPU callbacks.
The unfiltered RGB conversion matches upstream in 256 additional checks with
16/32-bit output, different alignments, row padding and buffer guards.
Another 2,400 frames check audio counts/buffer guards, ZIP/plain loading and
persistence. The runner enables assertions explicitly when using Zig.

Additional checks passed for manual/automatic frameskip, saved emulator
options, display snapshot ownership, 240 renderer cases, 63 audio producer
schedules, and Nofrendo's timing/sprite regression suite. Build-local core
preparation leaves the pinned submodule unchanged and fails if source anchors
change. See `retro-core/components/nestopia/README.md` for profiling and
differential-test commands.

## Additional hot-path fixes (November 10, 2026)

- The I2S producer now notifies the writer only when the queue crosses from
  less than one DMA block to at least one block. The writer already drains
  complete blocks until none remain. This avoids waking a priority-9 task for
  sub-block scanline audio, without changing the queue capacity, sample clock,
  backpressure or DMA block size. Notification eligibility is computed under
  the existing queue lock. The notification remains pending if the writer has
  not yet entered its wait. Shutdown still sends an unconditional wakeup.
  In the reverse direction, the writer signals queue space only after draining
  a previously full queue, which is the only condition that blocks a producer.
  This also avoids stale space signals and redundant semaphore calls.
- The software display path now converts little-endian RGB565 palettes to SPI
  byte order once per frame (256 entries), preserving a plain lookup in the
  pixel loop. Previously those palettes had incorrect colors whenever PPA was
  unavailable or bypassed. Conversion uses a 512-byte temporary palette on the
  display task's stack; the task's stack allocation remains 4 KiB.
- fMSX keeps its frame timestamp in 64 bits, matching the system timer. This
  avoids truncating absolute microsecond timestamps at long uptimes and signed
  overflow when accounting for audio-queue waits near the 32-bit boundary.
- GBA's initial backup-signature search uses `memchr` to find candidate first
  bytes before calling `memcmp`. The old path compared at every ROM byte for
  each signature. Byte-aligned and unaligned matches are both preserved;
  backup-type precedence, ROM preload size and paging are unchanged.

New host checks exercise 126 producer schedules (including concurrent drain
before notification, ring wrap, full-queue backpressure and stop), 240 renderer
cases across software/PPA/fallback and both palette byte orders, 10000 MSX
frames across 32-bit boundaries and a 24-hour uptime, and 209254 signature
cases against the original bytewise search. With 8192 single-sample submissions
and 192-frame DMA blocks, the producer test observes 42 notifications instead
of the previous 8192. This is a scheduler-call count, not a measured device
speedup. The new tests run in CI and release validation.

Validation for these additional changes: nine packaging/release unit tests and
the existing frame ownership, frameskip, audio conversion and DMA checks passed.
ESP-IDF 5.5.5 app builds passed for `retro-core` and `fmsx` on S3 N16R8/ILI9341,
and `fmsx` and `gbsp` on P4 N16R32/ST7796. The generated renderer function stack
frames are 688 bytes on S3 and 704 bytes on P4; these are individual function
frames, not measurements of total task stack use. No device was flashed and no
new on-device load-time, FPS or scheduling-latency measurements were made.

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
