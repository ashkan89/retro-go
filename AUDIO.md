# Emulator audio investigation

All ESP32 emulators feed signed 16-bit stereo frames to `rg_audio_submit()`.
That service selects the output and applies the saved speaker/headphone volume.
The shared I2S driver copies samples into an internal-RAM queue; a priority-9
task on the audio core scales them and writes them to I2S DMA. Display scaling
and SPI completion tasks run at priorities 6 and 7 on the same core. The
ESP32-S3 DIY configurations put emulator execution on core 0 and audio/display
tasks on core 1. The speaker amplifier and optional headphone DAC share the
same I2S clock/data bus; emulator selection does not change the physical wiring.

## Audio delivery by emulator

| Emulator | Producer / adapter | Delivery and rendering relationship |
| --- | --- | --- |
| NES | `retro-core/components/nofrendo/nes/nes.c`, `main_nes.c` | APU synthesis/delivery in quarter-frame batches before the display callback. Fractional PAL/NTSC sample counts are preserved; two framebuffers overlap rendering and display transfers. |
| Game Boy / Color | `retro-core/components/gnuboy/sound.c`, `main_gbc.c` | Small buffer flushes during execution; remaining samples precede the RGB565 display callback. |
| SMS / Game Gear / Coleco | `retro-core/components/smsplus/sound/sound.c`, `main_sms.c` | PSG chunks during scanlines, plus the frame tail. |
| SNES | `retro-core/main/main_snes.c` | Legacy APU mixes quarter-frame chunks at HBlank; optional Blargg APU uses its sample-ready callback. |
| Genesis | `gwenesis/main/main.c` | YM2612/PSG catch-up at quarter-frame checkpoints and a final tail before display synchronization. |
| PC Engine | `retro-core/main/main_pce.c` | Separate priority-8 PSG task, 62 stereo frames per submission; paced by I2S queue back-pressure. PSG/main-task synchronization remains an existing limitation. |
| Lynx | `retro-core/main/main_lynx.cpp` | Complete APU block after `UpdateFrame()`, before display synchronization; a long emulated frame can still delay delivery. |
| Game & Watch | `retro-core/main/main_gw.c` | Complete frame audio before the display check. |
| MSX | `fmsx/main/main.c` | Priority-8 audio task consumes requests every eight scanlines from `PlayAllSound()`; requests still depend on emulation progress. Fractional samples carry across requests. |
| Doom | `prboom-go/main/main.c` | Independent priority-8 music/SFX mixer, paced by I2S queue back-pressure. |
| Spectrum | `retro-legacy/main/spectrum.c` | Beeper audio every second half-frame, before framebuffer conversion; already removes DC. |
| Atari 2600 | `retro-legacy/main/stella.cpp` | Stella audio fragment after TIA frame execution, before framebuffer conversion, with fractional frame sample counts preserved. |
| Atari 7800 | `retro-legacy/main/prosystem.c` | Now submits completed TIA/POKEY audio in approximately quarter-frame chunks during scanline execution, before framebuffer conversion. |

## Confirmed problems and changes

1. **Partial I2S DMA writes across producer stalls.** The legacy ESP-IDF
   `i2s_write()` retains a current DMA pointer and byte offset. With TX
   auto-clear enabled, the S3 DMA completion handler clears transmitted buffers.
   A partial buffer left across a long stall can be consumed/cleared before
   the remaining samples arrive. The writer now waits for one whole DMA block
   in the software queue and copies it across ring wrap before writing. This
   adds at most one block of batching delay (192 frames = 6 ms at 32 kHz on
   these S3 configurations), without allocating another buffer.
2. **Atari 7800 silence mapped to full-scale negative DC.** TIA produces
   unipolar levels with silence at zero. POKEY has an idle bias of 8. The adapter
   incorrectly treated these as unsigned PCM centered at 128, sending -32768
   for TIA silence. That can turn transitions to DMA silence into thumps.
   A continuous DC-removal filter now centers the combined signal, initializes
   from its first level, and clamps the result to signed 16-bit range. Its
   state persists across chunks/frames and is reset on ROM init/reset/load.
3. **Atari 7800 waited for the entire emulated frame.** The core now exposes a
   scanline audio hook so the adapter can submit already-generated samples
   during heavy MARIA execution. NTSC output also carries the fractional sample
   count (533, 533, 534), yielding exactly 32000 samples over 60 frames. PAL
   produces 640 samples per frame.
4. **SNES used the PAL allocation size as every frame's sample count.** The
   legacy APU submitted 641 samples even for 60 Hz NTSC frames. At 32 kHz this
   makes audio back-pressure limit NTSC execution to about 49.9 fps. Mixing now
   uses the ROM's PAL/NTSC refresh rate and carries fractional samples; the
   existing allocation remains large enough for PAL.
5. **Legacy video loop ignored a busy display.** It now skips framebuffer
   conversion/submission when `rg_display_sync(false)` reports pending work.
   Emulation and audio still execute, including when frames are skipped.
6. **MSX and Atari 2600 discarded fractional samples.** MSX's eight-scanline
   requests now accumulate fractional samples and always request whole stereo
   frames. This also uses 64-bit arithmetic for the duration/rate product.
   Atari 2600 now carries fractions between frame fragments instead of losing
   20 samples/sec at 60 Hz. Its initial size check covers the largest fragment.

7. **NES display gating reduced visible FPS.** Skipping PPU drawing whenever the
   previous display transfer was busy prevented the two framebuffers from
   overlapping rendering and transfer. Dropping completed frames compounded
   that loss. Those changes have been reverted. NES starts with zero fixed
   frameskip, and a busy-display probe no longer schedules an extra skipped
   frame when the measured frame time is within budget. The existing elapsed-
   time and system overload controls still skip frames when necessary.
8. **NES timing overhead and sample drift.** Per-scanline synthesis has been
   reverted to at most five APU batches per frame, retaining delivery during
   emulation so audio reaches the I2S queue before the display callback. Integer
   phase accumulation replaces the previous per-scanline 64-bit division.
   At 32 kHz NTSC, frame counts now alternate
   533, 533, 534 instead of dropping 20 samples each second; PAL remains 640.
   Envelope lookup tables keep their nominal size. Sprite-zero collision and
   drawing also share one pattern fetch instead of fetching the same row twice.

## Limits and device validation

These are source-confirmed defects, not proof that every audible artifact has
the same cause. A finite queue cannot compensate for emulation that remains
slower than real time. Whole-frame producers can still starve output during
long CPU stalls; amplifier power/ground noise can also follow display activity.
Increasing gain would amplify such noise, so it should follow timing fixes.

The Atari smoke harness checks TIA/POKEY silence, PAL/NTSC sample totals,
quarter-frame delivery, audio with rendering skipped, and preservation of a
generated TIA tone. It also compiles the actual I2S consumer loop and SNES
audio tick function with mocked producers/mixers: 75 DMA batching/FIFO/ring-wrap
scenarios and PAL/NTSC sample totals with both SNES filter settings. Tests also
check Atari 2600's frame sample totals and MSX's actual duration-based
audio request function, including tiny requests and whole stereo frames. The
generator is `tools/test_audio_dma.py`; its host mode needs a native Clang
compiler, while the ESP-IDF selftest mode uses the installed Xtensa compiler.
The mocks do not exercise physical I2S DMA or speaker circuitry. Firmware
builds check the shared driver's compilation.

`python tools/test_nes_timing.py --cc /path/to/clang` compiles the actual NES
frame loop and APU with mocked CPU, display, and memory. It checks 32 scenarios
covering PAL/NTSC, 22.05/32 kHz, mono/stereo, rendered/skipped frames, and startup
at scanline 241, including exact one-second sample totals, continuous tone
output, and a five-call synthesis limit per frame. It checks all APU filters for
identical output across different batch sizes, overlapping display work,
redraws, frame-skip decisions within and beyond the time budget, and 262144
sprite pattern/flip/priority combinations. Native Zig is also supported through
`--cc /path/to/zig`.

On the handheld, compare the same busy scene with normal rendering and a
higher frame-skip setting, then with audio muted. Improvement with frame skip
supports producer starvation; noise remaining with the MAX98357A shut down
suggests a hardware source. Check speaker and headphone routes separately.
Only device testing can establish whether the reported repeated thumps and
rendering-related jitter are eliminated.

## Media player scheduling (ESP32-S3 launcher)

The launcher enables `RG_MEDIA_DUAL_CORE`. Core 1 runs launcher UI/input,
`rg_display` (priority 6), `rg_spi` (7), `media_audio` (8), and the I2S writer
`rg_audio` (9). Core 0 runs compressed source prefetch (4), audio decoding (7),
next-track preparation (3), and artwork decoding (1). The PCM ring separates
codec timing from playback; binary FreeRTOS semaphores wake ring producers and
consumers, and the I2S software queue provides DMA back-pressure. There is no
media audio encoder task: WAV, MP3 and FLAC are playback decoders.

The launcher entry task is explicitly created on core 1 before system/peripheral
initialization. Changing the application affinity macros alone does not move
ESP-IDF's `app_main`, whose affinity comes from sdkconfig. This also places
interrupt allocation during launcher initialization on the output/UI core.
Emulator applications retain their existing target affinities.

Audio position is published as an atomic 32-bit snapshot after each submitted
chunk and on flush. Position and buffering status reads do not acquire the audio
mutex, so UI polling cannot wait behind a blocking I2S submission. Flush and
sample-rate changes still serialize with chunk processing. Positive sleeps
shorter than a scheduler tick now block for at least one tick: at the configured
100 Hz tick rate, the previous 4/5 ms sleeps became zero-tick yields and could
starve lower-priority work on the same core.

This layout uses ESP-IDF 5.5.5's documented
[FreeRTOS core affinity and priority scheduling](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/freertos_idf.html)
and [interrupt allocation rules](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/intr_alloc.html).
Espressif specifies that external interrupts are allocated on the installing
core; it does not prescribe this particular application task layout.

Validation: `python tools/test_media_audio.py --cc gcc` exercises PCM ordering,
concurrent rings, transitions, buffering, UI status reads during locked audio
submission, and affinities for all eight release targets with both emulator
layouts and the launcher layout. `python tools/test_audio_dma.py --cc gcc`
checks 150 DMA batching/FIFO/wrap scenarios. These host checks cannot establish
physical playback deadlines. On hardware, compare the underrun overlay during
44.1/48 kHz MP3/FLAC playback while scrolling, seeking, changing artwork, and
streaming; also check pause/resume, short files and speaker/headphone switching.

Firmware validation: the `esp32-s3-n16r8-ili9341` launcher builds successfully
with ESP-IDF 5.5.5 (`python rg_tool.py --target esp32-s3-n16r8-ili9341 build launcher`).
The generated app is `launcher/build/launcher.bin`; it has not been flashed or
measured on a physical board.
