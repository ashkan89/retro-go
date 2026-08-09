/**
 * Retro-Go media player - spectrum analysis.
 *
 * A real radix-2 FFT over a Hann-windowed mono mixdown of the audio actually being played.
 * Twiddle factors and every working buffer are allocated once, so an analysis frame makes no
 * allocations at all. Band values are smoothed with separate rise/fall rates and normalised
 * by a slow AGC so quiet tracks still fill the display.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEDIA_FFT_MAX_BANDS 32

typedef struct
{
    int bands;
    float value[MEDIA_FFT_MAX_BANDS];   // 0.0 - 1.0, smoothed
    float peak[MEDIA_FFT_MAX_BANDS];    // 0.0 - 1.0, with decay
    float rms_left, rms_right;
    float peak_left, peak_right;
    uint32_t generation;                // Increments on every completed analysis
} media_spectrum_t;

/** Allocate the FFT working set for `size` points (128/256/512) and `bands` output bins. */
bool media_fft_init(int size, int bands);
void media_fft_deinit(void);
bool media_fft_ready(void);
int media_fft_size(void);

/** Reset all bands and levels to silence (on stop/track change). */
void media_fft_reset(void);

/**
 * Feed interleaved stereo samples. Non-blocking and lossy by design: if the visualiser is
 * behind, older samples are simply overwritten. Safe to call from the audio task.
 */
void media_fft_feed(const int16_t *pcm, size_t frames);

/**
 * Run one analysis pass over the most recent window. Call from the UI task at the visualiser
 * rate (20-30 Hz), independent of the render rate. Returns false when there is nothing new.
 */
bool media_fft_analyze(void);

/** Latest results. Always safe to read; values are only written by media_fft_analyze(). */
const media_spectrum_t *media_fft_spectrum(void);

/**
 * Copy the most recent `count` mono samples for waveform/oscilloscope drawing, scaled to
 * -32768..32767. Returns the number actually written.
 */
size_t media_fft_copy_waveform(int16_t *out, size_t count);
