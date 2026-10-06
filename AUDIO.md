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
| NES | `retro-core/components/nofrendo/nes/nes.c`, `main_nes.c` | APU synthesis per scanline, delivered in quarter-frame chunks before the display callback. Fractional PAL/NTSC sample counts are preserved; a busy display skips video work. |
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

7. **NES rendering could block the audio producer.** The adapter checked display
   readiness but still submitted a new surface to the blocking single-slot
   display queue. It now skips PPU drawing when the display is busy, and drops a
   completed video frame if the display is still busy at submission. CPU and APU
   emulation continue; menu redraw requests still submit normally.
8. **NES sound registers were sampled only at quarter-frame boundaries.** Every
   sample in a chunk used the last register state, losing intervening sound
   changes. The APU now synthesizes after each scanline's CPU execution, while
   delivery remains batched. Integer phase accumulation replaces the previous
   per-scanline 64-bit division. At 32 kHz NTSC, frame counts now alternate
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
at scanline 241, including exact one-second sample totals and scanline sound
changes. It checks all APU filters for identical output across different
batch sizes, busy-display behavior, redraws, and 262144 sprite pattern/flip/
priority combinations. Native Zig is also supported through `--cc /path/to/zig`.

On the handheld, compare the same busy scene with normal rendering and a
higher frame-skip setting, then with audio muted. Improvement with frame skip
supports producer starvation; noise remaining with the MAX98357A shut down
suggests a hardware source. Check speaker and headphone routes separately.
Only device testing can establish whether the reported repeated thumps and
rendering-related jitter are eliminated.
