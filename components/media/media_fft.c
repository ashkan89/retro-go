#include <rg_system.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "media_config.h"
#include "media_fft.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_FFT"

static struct
{
    int size;
    int bands;
    bool ready;

    int16_t *tap;           // Ring of recent mono samples, MEDIA_VIZ_TAP_SAMPLES
    volatile uint32_t write; // Producer cursor (audio task)
    uint32_t read;          // Last analysed position (UI task)

    float *re, *im;
    float *window;
    float *twiddle_re, *twiddle_im;
    uint16_t *reverse;
    uint8_t *band_start, *band_end;

    float agc;              // Slowly tracked reference magnitude
    media_spectrum_t out;
} fft;

static void free_all(void)
{
    free(fft.tap), fft.tap = NULL;
    free(fft.re), fft.re = NULL;
    free(fft.im), fft.im = NULL;
    free(fft.window), fft.window = NULL;
    free(fft.twiddle_re), fft.twiddle_re = NULL;
    free(fft.twiddle_im), fft.twiddle_im = NULL;
    free(fft.reverse), fft.reverse = NULL;
    free(fft.band_start), fft.band_start = NULL;
    free(fft.band_end), fft.band_end = NULL;
    fft.ready = false;
}

bool media_fft_init(int size, int bands)
{
    if (size != 128 && size != 256 && size != 512)
        size = 256;
    bands = media_clampi(bands, 8, MEDIA_FFT_MAX_BANDS);

    if (fft.ready && fft.size == size && fft.bands == bands)
        return true;

    free_all();

    fft.size = size;
    fft.bands = bands;

    fft.tap = rg_alloc(MEDIA_VIZ_TAP_SAMPLES * sizeof(int16_t), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.re = rg_alloc((size_t)size * sizeof(float), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.im = rg_alloc((size_t)size * sizeof(float), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.window = rg_alloc((size_t)size * sizeof(float), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.twiddle_re = rg_alloc((size_t)(size / 2) * sizeof(float), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.twiddle_im = rg_alloc((size_t)(size / 2) * sizeof(float), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.reverse = rg_alloc((size_t)size * sizeof(uint16_t), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.band_start = rg_alloc((size_t)bands, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    fft.band_end = rg_alloc((size_t)bands, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);

    if (!fft.tap || !fft.re || !fft.im || !fft.window || !fft.twiddle_re || !fft.twiddle_im ||
        !fft.reverse || !fft.band_start || !fft.band_end)
    {
        RG_LOGW("Not enough memory for a %d-point FFT", size);
        free_all();
        return false;
    }

    memset(fft.tap, 0, MEDIA_VIZ_TAP_SAMPLES * sizeof(int16_t));

    for (int i = 0; i < size; ++i)
        fft.window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (size - 1)));

    for (int i = 0; i < size / 2; ++i)
    {
        float angle = -2.0f * (float)M_PI * i / size;
        fft.twiddle_re[i] = cosf(angle);
        fft.twiddle_im[i] = sinf(angle);
    }

    int bits = 0;
    while ((1 << bits) < size)
        bits++;
    for (int i = 0; i < size; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        fft.reverse[i] = (uint16_t)r;
    }

    // Logarithmic band edges over bins 1..size/2. Bin 0 (DC) is deliberately excluded.
    int max_bin = size / 2;
    for (int i = 0; i < bands; ++i)
    {
        float lo = powf((float)max_bin, (float)i / bands);
        float hi = powf((float)max_bin, (float)(i + 1) / bands);
        int start = media_clampi((int)lo, 1, max_bin - 1);
        int end = media_clampi((int)hi, start + 1, max_bin);
        fft.band_start[i] = (uint8_t)start;
        fft.band_end[i] = (uint8_t)end;
    }

    fft.out.bands = bands;
    fft.agc = 1.0f;
    fft.ready = true;

    RG_LOGI("FFT ready: %d points, %d bands", size, bands);
    return true;
}

void media_fft_deinit(void)
{
    free_all();
    memset(&fft.out, 0, sizeof(fft.out));
}

bool media_fft_ready(void)
{
    return fft.ready;
}

int media_fft_size(void)
{
    return fft.size;
}

void media_fft_reset(void)
{
    if (fft.tap)
        memset(fft.tap, 0, MEDIA_VIZ_TAP_SAMPLES * sizeof(int16_t));
    memset(fft.out.value, 0, sizeof(fft.out.value));
    memset(fft.out.peak, 0, sizeof(fft.out.peak));
    fft.out.rms_left = fft.out.rms_right = 0;
    fft.out.peak_left = fft.out.peak_right = 0;
    fft.agc = 1.0f;
    fft.read = fft.write;
}

void media_fft_feed(const int16_t *pcm, size_t frames)
{
    if (!fft.ready || !pcm || !frames)
        return;

    // Deliberately lossy: the visualiser is allowed to miss samples, the audio task is not.
    uint32_t w = fft.write;
    float sum_l = 0, sum_r = 0, peak_l = 0, peak_r = 0;

    for (size_t i = 0; i < frames; ++i)
    {
        int l = pcm[i * 2 + 0];
        int r = pcm[i * 2 + 1];

        fft.tap[w & (MEDIA_VIZ_TAP_SAMPLES - 1)] = (int16_t)((l + r) / 2);
        w++;

        sum_l += (float)l * l;
        sum_r += (float)r * r;
        float al = fabsf((float)l), ar = fabsf((float)r);
        if (al > peak_l)
            peak_l = al;
        if (ar > peak_r)
            peak_r = ar;
    }

    __sync_synchronize();
    fft.write = w;

    // Level meters are cheap enough to update straight from the audio task.
    float rms_l = sqrtf(sum_l / frames) / 32768.0f;
    float rms_r = sqrtf(sum_r / frames) / 32768.0f;

    fft.out.rms_left += (rms_l - fft.out.rms_left) * 0.3f;
    fft.out.rms_right += (rms_r - fft.out.rms_right) * 0.3f;

    peak_l /= 32768.0f;
    peak_r /= 32768.0f;
    fft.out.peak_left = peak_l > fft.out.peak_left ? peak_l : fft.out.peak_left * 0.92f;
    fft.out.peak_right = peak_r > fft.out.peak_right ? peak_r : fft.out.peak_right * 0.92f;
}

/** In-place iterative radix-2 decimation-in-time FFT. */
static void fft_transform(void)
{
    const int n = fft.size;

    for (int stage = 2; stage <= n; stage <<= 1)
    {
        int half = stage / 2;
        int step = n / stage;

        for (int i = 0; i < n; i += stage)
        {
            for (int j = 0; j < half; ++j)
            {
                int k = j * step;
                float wr = fft.twiddle_re[k];
                float wi = fft.twiddle_im[k];

                int a = i + j;
                int b = a + half;

                float tr = wr * fft.re[b] - wi * fft.im[b];
                float ti = wr * fft.im[b] + wi * fft.re[b];

                fft.re[b] = fft.re[a] - tr;
                fft.im[b] = fft.im[a] - ti;
                fft.re[a] += tr;
                fft.im[a] += ti;
            }
        }
    }
}

bool media_fft_analyze(void)
{
    if (!fft.ready)
        return false;

    uint32_t w = fft.write;
    if (w == fft.read)
        return false; // No new audio since the last pass
    fft.read = w;

    const int n = fft.size;

    // Take the most recent n samples, bit-reversed into place with the window applied.
    for (int i = 0; i < n; ++i)
    {
        uint32_t index = w - (uint32_t)n + (uint32_t)i;
        float sample = (float)fft.tap[index & (MEDIA_VIZ_TAP_SAMPLES - 1)] / 32768.0f;
        int dst = fft.reverse[i];
        fft.re[dst] = sample * fft.window[i];
        fft.im[dst] = 0.0f;
    }

    fft_transform();

    // Track the loudest band this frame so the AGC has something to converge on.
    float frame_max = 0.0f;
    float magnitudes[MEDIA_FFT_MAX_BANDS];

    for (int b = 0; b < fft.bands; ++b)
    {
        int start = fft.band_start[b];
        int end = fft.band_end[b];
        float sum = 0.0f;

        for (int k = start; k < end; ++k)
            sum += sqrtf(fft.re[k] * fft.re[k] + fft.im[k] * fft.im[k]);

        float magnitude = sum / (float)(end - start);
        // A gentle high-frequency tilt: music has far less energy up top, and without it the
        // right-hand bars would never move.
        magnitude *= 1.0f + 1.6f * ((float)b / fft.bands);
        magnitudes[b] = magnitude;
        if (magnitude > frame_max)
            frame_max = magnitude;
    }

    // Slow AGC with a floor, so silence does not get amplified into noise.
    if (frame_max > fft.agc)
        fft.agc += (frame_max - fft.agc) * 0.25f;
    else
        fft.agc += (frame_max - fft.agc) * 0.02f;
    if (fft.agc < 0.004f)
        fft.agc = 0.004f;

    for (int b = 0; b < fft.bands; ++b)
    {
        float normalised = magnitudes[b] / fft.agc;
        // Compress the top end so a loud transient does not peg every bar
        normalised = sqrtf(media_clampf(normalised, 0.0f, 1.6f) / 1.6f);

        float current = fft.out.value[b];
        // Rise fast, fall slowly: this is what makes bars read as "musical" rather than noisy.
        if (normalised > current)
            current += (normalised - current) * 0.55f;
        else
            current += (normalised - current) * 0.18f;

        fft.out.value[b] = media_clampf(current, 0.0f, 1.0f);

        if (fft.out.value[b] > fft.out.peak[b])
            fft.out.peak[b] = fft.out.value[b];
        else
            fft.out.peak[b] = media_clampf(fft.out.peak[b] - 0.012f, 0.0f, 1.0f);
    }

    fft.out.generation++;
    return true;
}

const media_spectrum_t *media_fft_spectrum(void)
{
    return &fft.out;
}

size_t media_fft_copy_waveform(int16_t *out, size_t count)
{
    if (!fft.ready || !out || !count)
        return 0;

    if (count > MEDIA_VIZ_TAP_SAMPLES)
        count = MEDIA_VIZ_TAP_SAMPLES;

    uint32_t w = fft.write;
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t index = w - (uint32_t)count + (uint32_t)i;
        out[i] = fft.tap[index & (MEDIA_VIZ_TAP_SAMPLES - 1)];
    }

    return count;
}
