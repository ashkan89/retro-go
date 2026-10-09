#include <rg_system.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "media_transition.h"

void media_pcm_reader_init(media_pcm_reader_t *r, media_decoder_t *decoder, uint32_t rate)
{
    memset(r, 0, sizeof(*r));
    r->decoder = decoder;
    r->rate = rate;
    r->inverse_rate = rate ? 1.0f / rate : 0;
    if (decoder && rate && decoder->sample_rate > rate)
    {
        // Fourth-order Butterworth anti-alias filter for high-rate files. Upsampling
        // needs only interpolation; equal-rate PCM bypasses both paths bit-for-bit.
        const float q[2] = {0.5411961f, 1.3065630f};
        float angle = 6.283185307f * rate * 0.44f / decoder->sample_rate;
        float cosine = cosf(angle), sine = sinf(angle);
        r->filtered = true;
        for (int i = 0; i < 2; ++i)
        {
            float alpha = sine / (2 * q[i]), scale = 1 / (1 + alpha);
            r->filter[i].b0 = r->filter[i].b2 = (1 - cosine) * 0.5f * scale;
            r->filter[i].b1 = (1 - cosine) * scale;
            r->filter[i].a1 = -2 * cosine * scale;
            r->filter[i].a2 = (1 - alpha) * scale;
        }
    }
}

static bool read_frame(media_pcm_reader_t *r, int16_t frame[2])
{
    if (r->cursor == r->count)
    {
        r->count = media_decoder_decode(r->decoder, r->block, 512);
        r->cursor = 0;
        if (r->count <= 0)
        {
            r->failed = r->count < 0;
            r->count = 0;
            return false;
        }
    }
    frame[0] = r->block[r->cursor * 2];
    frame[1] = r->block[r->cursor * 2 + 1];
    if (r->filtered)
        for (int c = 0; c < 2; ++c)
        {
            float sample = frame[c];
            for (int i = 0; i < 2; ++i)
            {
                float value = r->filter[i].b0 * sample + r->filter[i].z1[c];
                r->filter[i].z1[c] = r->filter[i].b1 * sample - r->filter[i].a1 * value + r->filter[i].z2[c];
                r->filter[i].z2[c] = r->filter[i].b2 * sample - r->filter[i].a2 * value;
                sample = value;
            }
            frame[c] = (int16_t)(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
        }
    r->cursor++;
    return true;
}

int media_pcm_reader_read(media_pcm_reader_t *r, int16_t *pcm, size_t frames)
{
    if (!r->decoder || !r->rate || !r->decoder->sample_rate)
        return -1;
    if (!r->started)
    {
        r->started = true;
        if (!read_frame(r, r->a))
            r->ended = true;
        else if (!read_frame(r, r->b))
        {
            memcpy(r->b, r->a, sizeof(r->b));
            r->last = true;
        }
    }
    size_t produced = 0;
    while (produced < frames && !r->ended)
    {
        float fraction = r->phase * r->inverse_rate;
        for (int c = 0; c < 2; ++c)
            pcm[produced * 2 + c] = r->a[c] + (int32_t)((r->b[c] - r->a[c]) * fraction);
        produced++;
        r->phase += r->decoder->sample_rate;
        while (r->phase >= r->rate && !r->ended)
        {
            r->phase -= r->rate;
            if (r->last)
                r->ended = true;
            else
            {
                memcpy(r->a, r->b, sizeof(r->a));
                if (!read_frame(r, r->b))
                {
                    memcpy(r->b, r->a, sizeof(r->b));
                    r->last = true;
                }
            }
        }
    }
    return produced ? (int)produced : (r->failed ? -1 : 0);
}

bool media_tail_init(media_tail_t *tail, size_t frames)
{
    media_tail_free(tail);
    if (!frames)
        return true;
    tail->pcm = rg_alloc(frames * 2 * sizeof(int16_t), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    if (!tail->pcm)
        return false;
    tail->capacity = frames;
    return true;
}

void media_tail_free(media_tail_t *tail)
{
    free(tail->pcm);
    memset(tail, 0, sizeof(*tail));
}

size_t media_tail_push(media_tail_t *tail, int16_t *pcm, size_t frames)
{
    if (!tail->capacity)
        return frames;
    size_t emitted = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        int16_t left = pcm[i * 2], right = pcm[i * 2 + 1];
        if (tail->count == tail->capacity)
        {
            pcm[emitted * 2] = tail->pcm[tail->head * 2];
            pcm[emitted * 2 + 1] = tail->pcm[tail->head * 2 + 1];
            emitted++;
            if (++tail->head == tail->capacity) tail->head = 0;
            tail->count--;
        }
        size_t end = tail->head + tail->count;
        if (end >= tail->capacity) end -= tail->capacity;
        tail->pcm[end * 2] = left;
        tail->pcm[end * 2 + 1] = right;
        tail->count++;
    }
    return emitted;
}

size_t media_tail_pop(media_tail_t *tail, int16_t *pcm, size_t frames)
{
    size_t count = frames < tail->count ? frames : tail->count;
    for (size_t i = 0; i < count; ++i)
    {
        pcm[i * 2] = tail->pcm[tail->head * 2];
        pcm[i * 2 + 1] = tail->pcm[tail->head * 2 + 1];
        if (++tail->head == tail->capacity) tail->head = 0;
    }
    tail->count -= count;
    return count;
}

void media_crossfade_mix(int16_t *out, const int16_t *incoming, size_t frames,
                        size_t offset, size_t total)
{
    uint32_t denominator = total > 1 ? (uint32_t)(total - 1) : 1;
    uint64_t initial = (uint64_t)offset * 32768;
    uint32_t weight = total > 1 ? (uint32_t)(initial / denominator) : 32768;
    uint32_t remainder = (uint32_t)(initial % denominator);
    uint32_t step = 32768 / denominator, carry = 32768 % denominator;
    for (size_t i = 0; i < frames; ++i)
    {
        if (weight > 32768) weight = 32768;
        for (int c = 0; c < 2; ++c)
            out[i * 2 + c] = (int16_t)(((int32_t)out[i * 2 + c] * (int32_t)(32768 - weight) +
                                       (int32_t)incoming[i * 2 + c] * (int32_t)weight) / 32768);
        weight += step;
        remainder += carry;
        if (remainder >= denominator) { remainder -= denominator; weight++; }
    }
}
