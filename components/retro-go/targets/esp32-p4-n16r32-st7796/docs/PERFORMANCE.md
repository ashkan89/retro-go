# P4 performance and GBA validation

Measured on COM3, WT9932P4-TINY N16R32, P4 v1.3, IDF 5.5.5,
360 MHz CPU, 32 MiB HEX PSRAM at 200 MHz, ST7796 SPI display.

## PPA and PIE screen scaling (2026-11-09)

The launcher diagnostic verifies 24 combinations of RGB565 LE/BE and indexed
RGB565 LE/BE at NES, GBA, SMS, Game Boy and Mega Drive dimensions, including
320x241. Readback checks solid color bands and interpolated boundary pixels.
All passed on COM3. PIE conversion also passed all 64 pairs of halfword address
alignments, short/tail lengths and destination guards. Hardware misaligned
accesses must be enabled explicitly, and the signed right shift must be masked
before merging the swapped bytes.

| Synthetic diagnostic | Time |
|---|---:|
| RGB565 scalar byte conversion, 320 repetitions of one 480-pixel row | 3.433 ms |
| PIE conversion, same workload | 0.506 ms (6.8x faster) |
| RGB565 PPA scaling, representative emulator sizes | 1.4–3.5 ms |
| Palette expansion + PPA scaling | 2.1–7.3 ms |
| NES-sized indexed frame, previous software filter, 336x315 | 21.757 ms/frame |
| Same frame, PPA bilinear + PIE | 23.073 ms/frame |
| GBA-sized RGB565 frame, previous software filter, 480x320 | 31.373 ms/frame |
| Same frame, PPA bilinear + PIE | 31.632 ms/frame |

Renderer measurements average eight full redraws of generated color bands,
including completed SPI transfers. The diagnostic drains all SPI descriptors
before and after each batch: the normal SPI `lcd_sync()` intentionally does
nothing, so calling it alone would undercount outstanding transfers. Both paths
use identical fitted viewports, and checksums are invalidated on each redraw.

These results demonstrate faster byte conversion and real hardware bilinear
interpolation. They do **not** show a game-FPS increase: the smoother PPA path
cost about 1.32 ms/frame for the indexed NES fixture and 0.26 ms for the GBA
fixture compared with the simpler previous filter. Emulation continues on its
own core; the SPI transfer limit remains. Neural inference is not enabled.
Palette buffers and PPA output remain reusable PSRAM allocations, rather than
consuming memory needed for DMA/audio and emulator working data.

## Contra / NES

The original 40 MHz build reached 60 emulated FPS on static scenes but fell
to about 24 FPS during scrolling. Display submission blocked the emulator
while the SPI renderer consumed the source frame.

The new P4 path copies accepted frames into a PSRAM snapshot owned by the
display task. If the display still owns its previous snapshot, submission drops
that presentation immediately. CPU emulation and audio continue. Source offset,
row padding, palette ownership and resizing are covered by the native submission
test. Doom's explicit source-release wait also uses this ownership distinction.

LCD SPI runs at **80 MHz at the user's request**. DMA buffers contain 16 rows
instead of four, and the SPI completion interrupt runs on the display core.
The latest Contra captures sustain **60 emulated FPS and about 30 presented
FPS** during scrolling. Static or small partial changes can present more often.
P4 auto mode starts without forced initial frame skipping; manual frame-skip
settings remain effective, and CPU overload can still trigger adaptive skipping.

These are measurements for Contra, not a full compatibility/performance claim
for every game or emulator. The shared submission improvement applies to all
emulator apps. Existing engines remain in place; replacing the CPU engine cannot
remove the display's physical bandwidth limit.

At 480x320 RGB565, one full frame carries 2,457,600 bits. Ideal full-screen
limits are 16.3 FPS at 40 MHz and 32.6 FPS at 80 MHz, before commands and other
overhead. Fit scaling, unchanged regions and native-size modes change the number
of pixels sent. Sixty full-screen frames per second need a faster display link.

## Audio

A 30-second Contra capture at 80 MHz recorded no late I2S writes, no submission
errors, and an occupied software queue throughout. `BUSY:100%` includes waiting
for audio pacing; it is not a measurement of CPU arithmetic occupancy.
The user reported light buzzing/distortion that increases with screen activity,
improves at 30% speaker volume, and is quiet in the paused menu. This remains
an audible hardware validation item; a full queue alone cannot diagnose the
analog signal or prove clean audio.
The user also confirmed that rendering-related buzz remains with digital volume
at zero. This strongly suggests coupling into the analog output or its supply /
ground return; it is not evidence of a PCM queue underrun. For MAX98357A, the
[manufacturer datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf)
specifies local 0.1 uF and 10 uF supply bypass capacitors and careful grounding.
Check these at the amplifier, and avoid sharing its supply return with the
display/backlight current path. SPI remains at 80 MHz as requested.

The P4 target now uses a 5 ms digital gain ramp for volume/mute/resume. Menu mute
keeps only the selected analog output powered and fades its PCM to zero rather
than abruptly clearing DMA and toggling amplifier shutdown. Other targets keep
their existing mute behavior. The native gain test checks both paths, including
cross-buffer fade continuity. Optional `RG_AUDIO_TIMING_DIAGNOSTICS=1` restores
the five-second diagnostic log; release/default builds disable it.
The user confirmed that menu/volume clicks were reduced or gone after flashing
this change. The zero-volume rendering buzz remains a separate hardware issue.

## PPA and GBA

The new `gbsp` app uses Midworld's gpSP ARM-to-RV32 JIT, with 8 MiB ROM caching,
PSRAM code caches and explicit instruction-cache publication for IDF 5.5.
The launcher GBA entry now points to a real application included in this target.
See [the GBA integration notes](../../../../../gbsp/README.md) for ROM, BIOS,
save and shoulder-button details.

PPA SRM provides hardware bilinear scaling for compatible RGB565 surfaces.
It requires exact scale factors in sixteenth steps; unsupported dimensions,
indexed surfaces and nearest-neighbour filtering use the software renderer.
GBA's 240x160 to 480x320 scale is exact. This hardware accelerates image
operations, not console CPU execution, and there is no AI model in the renderer.

The copyright-free hardware smoke test on this board passed **121 native RV32
blocks**, ARM7 framebuffer execution, the display-task PPA path, every output
pixel of the 2x PPA scale, live frame-skip changes, and a serialized-state
round trip restoring GBA RAM. It writes temporary cache files, cleans them up,
and returns to Contra when available. This does not establish commercial-game
compatibility or GBA gameplay FPS. Diagnostic builds must be explicitly enabled
with `-DRG_GBA_SELFTEST=ON`; final builds use `OFF`.

The user then launched a real GBA game with the production app. The subsequent
30-second log recorded about **59-60 emulated FPS** during gameplay, with
roughly 15-30 presentations per second as frame-skip and scaling settings were
changed. Menu intervals are excluded from that observation. The title was not
reported, so this is one observed run, not a per-title compatibility benchmark.

Reference sources and pinned revisions are recorded in `gbsp/README.md`.
