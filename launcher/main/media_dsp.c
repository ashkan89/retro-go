#include "media_dsp.h"

#include <math.h>
#include <string.h>

#ifndef MEDIA_DSP_PI
#define MEDIA_DSP_PI 3.14159265358979f
#endif

const uint16_t media_eq_frequencies[MEDIA_EQ_BANDS] = {80, 250, 1000, 4000, 12000};

/* Shelves at the ends, peaking bells in between. */
typedef enum { FILTER_LOW_SHELF, FILTER_PEAK, FILTER_HIGH_SHELF } filter_kind_t;
static const filter_kind_t band_kind[MEDIA_EQ_BANDS] = {
    FILTER_LOW_SHELF, FILTER_PEAK, FILTER_PEAK, FILTER_PEAK, FILTER_HIGH_SHELF,
};

static const int8_t presets[MEDIA_EQ_PRESET_COUNT][MEDIA_EQ_BANDS] = {
    [MEDIA_EQ_PRESET_FLAT]     = {  0,  0,  0,  0,  0},
    [MEDIA_EQ_PRESET_BASS]     = {  8,  4,  0, -1,  0},
    [MEDIA_EQ_PRESET_VOCAL]    = { -3,  0,  5,  3, -1},
    [MEDIA_EQ_PRESET_TREBLE]   = { -2,  0,  0,  4,  7},
    [MEDIA_EQ_PRESET_ROCK]     = {  6,  2, -2,  3,  5},
    [MEDIA_EQ_PRESET_JAZZ]     = {  4,  1,  0,  2,  3},
    /* Equal-loudness style tilt: lift both ends for quiet listening. */
    [MEDIA_EQ_PRESET_LOUDNESS] = {  9,  2, -2,  2,  7},
    [MEDIA_EQ_PRESET_CUSTOM]   = {  0,  0,  0,  0,  0},
};

typedef struct {
    float b0, b1, b2, a1, a2;
} biquad_t;

typedef struct {
    float x1, x2, y1, y2;
} biquad_state_t;

static struct {
    int gain[MEDIA_EQ_BANDS];
    media_eq_preset_t preset;
    bool enabled;
    bool dirty;
    uint32_t rate;
    biquad_t coefficients[MEDIA_EQ_BANDS];
    biquad_state_t history[MEDIA_EQ_BANDS][2];
} eq = {
    .enabled = false,
    .dirty = true,
};

const char *media_eq_preset_name(media_eq_preset_t preset)
{
    static const char *names[MEDIA_EQ_PRESET_COUNT] = {
        "Flat", "Bass", "Vocal", "Treble", "Rock", "Jazz", "Loudness", "Custom",
    };
    return names[preset % MEDIA_EQ_PRESET_COUNT];
}

static int clamp_gain(int gain)
{
    if (gain < MEDIA_EQ_GAIN_MIN) return MEDIA_EQ_GAIN_MIN;
    if (gain > MEDIA_EQ_GAIN_MAX) return MEDIA_EQ_GAIN_MAX;
    return gain;
}

void media_eq_set_preset(media_eq_preset_t preset)
{
    preset %= MEDIA_EQ_PRESET_COUNT;
    eq.preset = preset;
    if (preset != MEDIA_EQ_PRESET_CUSTOM) {
        for (int i = 0; i < MEDIA_EQ_BANDS; i++)
            eq.gain[i] = presets[preset][i];
    }
    eq.dirty = true;
}

media_eq_preset_t media_eq_get_preset(void)
{
    return eq.preset;
}

void media_eq_set_gain(int band, int gain_db)
{
    if (band < 0 || band >= MEDIA_EQ_BANDS)
        return;
    eq.gain[band] = clamp_gain(gain_db);
    /* Touching a slider means the curve is no longer the named preset. */
    eq.preset = MEDIA_EQ_PRESET_CUSTOM;
    eq.dirty = true;
}

int media_eq_get_gain(int band)
{
    return (band >= 0 && band < MEDIA_EQ_BANDS) ? eq.gain[band] : 0;
}

void media_eq_set_enabled(bool enabled)
{
    if (eq.enabled != enabled) {
        eq.enabled = enabled;
        media_eq_reset();
    }
}

bool media_eq_get_enabled(void)
{
    return eq.enabled;
}

bool media_eq_is_active(void)
{
    if (!eq.enabled)
        return false;
    for (int i = 0; i < MEDIA_EQ_BANDS; i++)
        if (eq.gain[i])
            return true;
    return false;
}

void media_eq_reset(void)
{
    memset(eq.history, 0, sizeof(eq.history));
}

/* Standard Audio EQ Cookbook forms, normalised by a0. */
static void design(uint32_t rate)
{
    for (int i = 0; i < MEDIA_EQ_BANDS; i++) {
        float frequency = media_eq_frequencies[i];
        /* Keep every band below Nyquist; 12 kHz is above it for 22 kHz audio. */
        if (frequency > rate * 0.45f)
            frequency = rate * 0.45f;

        float amplitude = powf(10.0f, eq.gain[i] / 40.0f);
        float omega = 2.0f * MEDIA_DSP_PI * frequency / rate;
        float sn = sinf(omega), cs = cosf(omega);
        /* Q of 0.9 for the bells gives overlapping but not muddy bands. */
        float alpha = sn / (2.0f * 0.9f);
        float b0, b1, b2, a0, a1, a2;

        if (band_kind[i] == FILTER_PEAK) {
            b0 = 1.0f + alpha * amplitude;
            b1 = -2.0f * cs;
            b2 = 1.0f - alpha * amplitude;
            a0 = 1.0f + alpha / amplitude;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha / amplitude;
        } else {
            float shelf = 2.0f * sqrtf(amplitude) * alpha;
            if (band_kind[i] == FILTER_LOW_SHELF) {
                b0 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cs + shelf);
                b1 = 2.0f * amplitude * ((amplitude - 1.0f) - (amplitude + 1.0f) * cs);
                b2 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cs - shelf);
                a0 = (amplitude + 1.0f) + (amplitude - 1.0f) * cs + shelf;
                a1 = -2.0f * ((amplitude - 1.0f) + (amplitude + 1.0f) * cs);
                a2 = (amplitude + 1.0f) + (amplitude - 1.0f) * cs - shelf;
            } else {
                b0 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cs + shelf);
                b1 = -2.0f * amplitude * ((amplitude - 1.0f) + (amplitude + 1.0f) * cs);
                b2 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cs - shelf);
                a0 = (amplitude + 1.0f) - (amplitude - 1.0f) * cs + shelf;
                a1 = 2.0f * ((amplitude - 1.0f) - (amplitude + 1.0f) * cs);
                a2 = (amplitude + 1.0f) - (amplitude - 1.0f) * cs - shelf;
            }
        }

        eq.coefficients[i] = (biquad_t){
            .b0 = b0 / a0, .b1 = b1 / a0, .b2 = b2 / a0, .a1 = a1 / a0, .a2 = a2 / a0,
        };
    }
    eq.rate = rate;
    eq.dirty = false;
}

void media_eq_process(int16_t *frames, size_t frame_count, uint32_t sample_rate)
{
    if (!frames || !frame_count || !sample_rate || !media_eq_is_active())
        return;

    if (eq.dirty || eq.rate != sample_rate)
        design(sample_rate);

    /* Boosting can push a loud track past full scale, so the output is clamped
       rather than allowed to wrap round into a loud click. */
    for (int band = 0; band < MEDIA_EQ_BANDS; band++) {
        if (!eq.gain[band])
            continue;
        const biquad_t *c = &eq.coefficients[band];
        for (int channel = 0; channel < 2; channel++) {
            biquad_state_t *s = &eq.history[band][channel];
            float x1 = s->x1, x2 = s->x2, y1 = s->y1, y2 = s->y2;
            int16_t *sample = frames + channel;
            for (size_t i = 0; i < frame_count; i++, sample += 2) {
                float x0 = *sample;
                float y0 = c->b0 * x0 + c->b1 * x1 + c->b2 * x2 - c->a1 * y1 - c->a2 * y2;
                x2 = x1; x1 = x0;
                y2 = y1; y1 = y0;
                *sample = y0 > 32767.0f ? 32767 : (y0 < -32768.0f ? -32768 : (int16_t)y0);
            }
            s->x1 = x1; s->x2 = x2; s->y1 = y1; s->y2 = y2;
        }
    }
}
