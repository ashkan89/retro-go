#include <rg_system.h>

#include <math.h>
#include <string.h>

#include "media_eq.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_EQ"

const int media_eq_frequencies[MEDIA_EQ_BANDS] = {60, 150, 400, 1000, 2400, 6000, 12000};

static const char *band_labels[MEDIA_EQ_BANDS] = {"60", "150", "400", "1k", "2.4k", "6k", "12k"};

// Gains in whole dB, in the same order as media_eq_frequencies.
static const int8_t presets[MEDIA_EQ_PRESET_COUNT][MEDIA_EQ_BANDS] = {
    [MEDIA_EQ_PRESET_FLAT]            = {  0,  0,  0,  0,  0,  0,  0},
    [MEDIA_EQ_PRESET_BASS_BOOST]      = {  8,  6,  3,  0,  0,  0,  0},
    [MEDIA_EQ_PRESET_BASS_REDUCER]    = { -8, -6, -3,  0,  0,  0,  0},
    [MEDIA_EQ_PRESET_TREBLE_BOOST]    = {  0,  0,  0,  0,  3,  6,  8},
    [MEDIA_EQ_PRESET_TREBLE_REDUCER]  = {  0,  0,  0,  0, -3, -6, -8},
    [MEDIA_EQ_PRESET_ROCK]            = {  5,  3, -2, -1,  2,  5,  6},
    [MEDIA_EQ_PRESET_POP]             = { -1,  2,  4,  4,  2, -1, -2},
    [MEDIA_EQ_PRESET_ELECTRONIC]      = {  6,  4,  0, -2,  1,  4,  6},
    [MEDIA_EQ_PRESET_CLASSICAL]       = {  4,  3, -1, -1,  0,  3,  4},
    [MEDIA_EQ_PRESET_VOCAL]           = { -3, -1,  3,  5,  4,  1, -2},
    [MEDIA_EQ_PRESET_CUSTOM]          = {  0,  0,  0,  0,  0,  0,  0},
};

typedef struct
{
    float b0, b1, b2, a1, a2;
} biquad_coeffs_t;

typedef struct
{
    float x1, x2, y1, y2;
} biquad_state_t;

static struct
{
    bool enabled;
    media_eq_preset_t preset;
    int gains[MEDIA_EQ_BANDS];
    uint32_t sample_rate;

    biquad_coeffs_t coeffs[MEDIA_EQ_BANDS];
    biquad_state_t state[2][MEDIA_EQ_BANDS];

    float preamp;           // Headroom compensation for positive gains
    float limiter_gain;     // Smoothed, 1.0 = no reduction
    bool dirty;
} eq = {
    .preset = MEDIA_EQ_PRESET_FLAT,
    .sample_rate = 44100,
    .preamp = 1.0f,
    .limiter_gain = 1.0f,
};

const char *media_eq_preset_name(media_eq_preset_t preset)
{
    switch (preset)
    {
    case MEDIA_EQ_PRESET_FLAT:           return "Flat";
    case MEDIA_EQ_PRESET_BASS_BOOST:     return "Bass Boost";
    case MEDIA_EQ_PRESET_BASS_REDUCER:   return "Bass Reducer";
    case MEDIA_EQ_PRESET_TREBLE_BOOST:   return "Treble Boost";
    case MEDIA_EQ_PRESET_TREBLE_REDUCER: return "Treble Reducer";
    case MEDIA_EQ_PRESET_ROCK:           return "Rock";
    case MEDIA_EQ_PRESET_POP:            return "Pop";
    case MEDIA_EQ_PRESET_ELECTRONIC:     return "Electronic";
    case MEDIA_EQ_PRESET_CLASSICAL:      return "Classical";
    case MEDIA_EQ_PRESET_VOCAL:          return "Vocal";
    default:                             return "Custom";
    }
}

const char *media_eq_band_label(int band)
{
    return (band >= 0 && band < MEDIA_EQ_BANDS) ? band_labels[band] : "";
}

/** RBJ peaking EQ. Q of 1.0 gives roughly one octave of overlap between adjacent bands. */
static void compute_coeffs(int band)
{
    const float q = 1.0f;
    float gain_db = (float)eq.gains[band];
    float freq = (float)media_eq_frequencies[band];

    // Above Nyquist the filter is meaningless; make it a pass-through instead of unstable.
    if (freq >= (float)eq.sample_rate * 0.45f || gain_db == 0.0f)
    {
        eq.coeffs[band] = (biquad_coeffs_t){1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }

    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / (float)eq.sample_rate;
    float cos_w0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cos_w0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha / A;

    if (a0 == 0.0f || !isfinite(a0))
    {
        eq.coeffs[band] = (biquad_coeffs_t){1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }

    eq.coeffs[band] = (biquad_coeffs_t){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

static void recompute(void)
{
    int max_positive = 0;
    for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
    {
        eq.gains[i] = media_clampi(eq.gains[i], MEDIA_EQ_GAIN_MIN, MEDIA_EQ_GAIN_MAX);
        compute_coeffs(i);
        if (eq.gains[i] > max_positive)
            max_positive = eq.gains[i];
    }

    // Pre-attenuate by the largest boost so a bass-heavy preset cannot clip on its own.
    // Overlapping bands can still add a little, which is what the limiter is there for.
    eq.preamp = max_positive > 0 ? powf(10.0f, -(float)max_positive / 20.0f) : 1.0f;
    eq.dirty = false;
}

void media_eq_init(void)
{
    media_eq_flush();
    recompute();
}

void media_eq_set_enabled(bool enabled)
{
    if (eq.enabled != enabled)
    {
        eq.enabled = enabled;
        media_eq_flush();
    }
}

bool media_eq_get_enabled(void)
{
    return eq.enabled;
}

void media_eq_set_gain(int band, int gain_db)
{
    if (band < 0 || band >= MEDIA_EQ_BANDS)
        return;
    gain_db = media_clampi(gain_db, MEDIA_EQ_GAIN_MIN, MEDIA_EQ_GAIN_MAX);
    if (eq.gains[band] == gain_db)
        return;

    eq.gains[band] = gain_db;
    eq.preset = MEDIA_EQ_PRESET_CUSTOM;
    recompute();
}

int media_eq_get_gain(int band)
{
    return (band >= 0 && band < MEDIA_EQ_BANDS) ? eq.gains[band] : 0;
}

void media_eq_set_preset(media_eq_preset_t preset)
{
    if (preset < 0 || preset >= MEDIA_EQ_PRESET_COUNT)
        return;

    eq.preset = preset;
    if (preset != MEDIA_EQ_PRESET_CUSTOM)
    {
        for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
            eq.gains[i] = presets[preset][i];
    }
    recompute();
}

media_eq_preset_t media_eq_get_preset(void)
{
    return eq.preset;
}

void media_eq_reset(void)
{
    media_eq_set_preset(MEDIA_EQ_PRESET_FLAT);
}

void media_eq_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate < 8000 || sample_rate > 192000 || sample_rate == eq.sample_rate)
        return;
    eq.sample_rate = sample_rate;
    recompute();
    media_eq_flush();
}

void media_eq_flush(void)
{
    memset(eq.state, 0, sizeof(eq.state));
    eq.limiter_gain = 1.0f;
}

static inline float biquad_step(const biquad_coeffs_t *c, biquad_state_t *s, float x)
{
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

void media_eq_process(int16_t *pcm, size_t frames, float extra_gain)
{
    if (!pcm || !frames)
        return;

    if (eq.dirty)
        recompute();

    if (!(extra_gain > 0.0f) || !isfinite(extra_gain))
        extra_gain = 0.0f;

    const bool active = eq.enabled;
    const float gain = active ? eq.preamp * extra_gain : extra_gain;

    // Nothing to do at all: skip the whole pass rather than burn cycles on a copy.
    if (!active && gain == 1.0f)
        return;

    // A soft limiter with a fast attack and a slow release. It only reduces gain once a
    // sample would clip, so ordinary material passes through bit-exact.
    const float attack = 0.35f;
    const float release = 0.0006f;
    const float ceiling = 32000.0f;

    for (size_t i = 0; i < frames; ++i)
    {
        float l = (float)pcm[i * 2 + 0];
        float r = (float)pcm[i * 2 + 1];

        if (active)
        {
            for (int band = 0; band < MEDIA_EQ_BANDS; ++band)
            {
                if (eq.gains[band] == 0)
                    continue;
                l = biquad_step(&eq.coeffs[band], &eq.state[0][band], l);
                r = biquad_step(&eq.coeffs[band], &eq.state[1][band], r);
            }
        }

        l *= gain;
        r *= gain;

        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        float target = peak > ceiling ? ceiling / peak : 1.0f;

        if (target < eq.limiter_gain)
            eq.limiter_gain += (target - eq.limiter_gain) * attack;
        else
            eq.limiter_gain += (target - eq.limiter_gain) * release;

        l *= eq.limiter_gain;
        r *= eq.limiter_gain;

        // Hard clamp as the final backstop: an int16 must never wrap around.
        pcm[i * 2 + 0] = (int16_t)media_clampf(l, -32768.0f, 32767.0f);
        pcm[i * 2 + 1] = (int16_t)media_clampf(r, -32768.0f, 32767.0f);
    }
}

float media_eq_limiter_reduction(void)
{
    return 1.0f - eq.limiter_gain;
}
