/**
 * Retro-Go media player - equaliser and output conditioning.
 *
 * Seven cascaded peaking biquads per channel, processed in blocks. Coefficients are only
 * recomputed when a gain or the sample rate changes, never per sample.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEDIA_EQ_BANDS 7
#define MEDIA_EQ_GAIN_MIN -12
#define MEDIA_EQ_GAIN_MAX 12

typedef enum
{
    MEDIA_EQ_PRESET_FLAT = 0,
    MEDIA_EQ_PRESET_BASS_BOOST,
    MEDIA_EQ_PRESET_BASS_REDUCER,
    MEDIA_EQ_PRESET_TREBLE_BOOST,
    MEDIA_EQ_PRESET_TREBLE_REDUCER,
    MEDIA_EQ_PRESET_ROCK,
    MEDIA_EQ_PRESET_POP,
    MEDIA_EQ_PRESET_ELECTRONIC,
    MEDIA_EQ_PRESET_CLASSICAL,
    MEDIA_EQ_PRESET_VOCAL,
    MEDIA_EQ_PRESET_CUSTOM,
    MEDIA_EQ_PRESET_COUNT,
} media_eq_preset_t;

/** Centre frequencies, in Hz, of the seven bands. */
extern const int media_eq_frequencies[MEDIA_EQ_BANDS];

const char *media_eq_preset_name(media_eq_preset_t preset);
const char *media_eq_band_label(int band);

void media_eq_init(void);
void media_eq_set_enabled(bool enabled);
bool media_eq_get_enabled(void);

/** Gains are whole dB in [-12, +12]. Setting one switches the active preset to Custom. */
void media_eq_set_gain(int band, int gain_db);
int media_eq_get_gain(int band);

void media_eq_set_preset(media_eq_preset_t preset);
media_eq_preset_t media_eq_get_preset(void);
void media_eq_reset(void);

/** Must be called whenever the output rate changes; recomputes every coefficient. */
void media_eq_set_sample_rate(uint32_t sample_rate);

/** Clear filter history. Call on seek and track change to avoid a transient. */
void media_eq_flush(void);

/**
 * Process `frames` interleaved stereo samples in place. Applies the EQ (when enabled), the
 * automatic headroom preamp, `extra_gain` (linear, for ReplayGain and fades) and a soft
 * limiter. Safe to call with extra_gain == 1.0f and the EQ disabled: it becomes a no-op
 * apart from the limiter, which only engages above the threshold.
 */
void media_eq_process(int16_t *pcm, size_t frames, float extra_gain);

/** Peak reduction currently applied by the limiter, 0.0 - 1.0. For the debug overlay. */
float media_eq_limiter_reduction(void);
