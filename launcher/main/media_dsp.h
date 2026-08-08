#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A small fixed-band graphic equalizer for the music player.
 *
 * Five cascaded biquads per channel: a low shelf, three peaking bands and a
 * high shelf. At 44.1 kHz that is roughly 440k biquad evaluations per second,
 * which measures at a couple of percent of one ESP32-S3 core -- cheap enough
 * to sit directly in the decode path.
 *
 * Coefficients are recomputed only when a gain or the sample rate changes, so
 * the per-sample work is a handful of multiply-adds and nothing else. */

#define MEDIA_EQ_BANDS 5
#define MEDIA_EQ_GAIN_MIN (-12)
#define MEDIA_EQ_GAIN_MAX 12

typedef enum {
    MEDIA_EQ_PRESET_FLAT,
    MEDIA_EQ_PRESET_BASS,
    MEDIA_EQ_PRESET_VOCAL,
    MEDIA_EQ_PRESET_TREBLE,
    MEDIA_EQ_PRESET_ROCK,
    MEDIA_EQ_PRESET_JAZZ,
    MEDIA_EQ_PRESET_LOUDNESS,
    MEDIA_EQ_PRESET_CUSTOM,
    MEDIA_EQ_PRESET_COUNT,
} media_eq_preset_t;

/* Band centre frequencies, for labelling in the UI. */
extern const uint16_t media_eq_frequencies[MEDIA_EQ_BANDS];

const char *media_eq_preset_name(media_eq_preset_t preset);

/* Gains are whole decibels, clamped to [MEDIA_EQ_GAIN_MIN, MEDIA_EQ_GAIN_MAX]. */
void media_eq_set_preset(media_eq_preset_t preset);
media_eq_preset_t media_eq_get_preset(void);
void media_eq_set_gain(int band, int gain_db);
int media_eq_get_gain(int band);

void media_eq_set_enabled(bool enabled);
bool media_eq_get_enabled(void);

/* True when the filter would actually change the signal; lets the caller skip
   the whole stage for a flat curve. */
bool media_eq_is_active(void);

/* Clears the filter history. Call on seek or track change so a discontinuity
   does not ring through the filters. */
void media_eq_reset(void);

/* Processes interleaved stereo int16 frames in place. */
void media_eq_process(int16_t *frames, size_t frame_count, uint32_t sample_rate);
