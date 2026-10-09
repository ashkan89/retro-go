#pragma once
#include "media_decoder.h"

/* A streaming rate converter: two sample lookahead, bounded memory, no I2S restart. */
typedef struct
{
    media_decoder_t *decoder;
    uint32_t rate, phase;
    float inverse_rate;
    bool filtered;
    struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } filter[2];
    int16_t block[512 * 2], a[2], b[2];
    int count, cursor;
    bool started, last, ended, failed;
} media_pcm_reader_t;

void media_pcm_reader_init(media_pcm_reader_t *reader, media_decoder_t *decoder, uint32_t rate);
int media_pcm_reader_read(media_pcm_reader_t *reader, int16_t *pcm, size_t frames);

/* Retain the final window even when duration is unknown or a VBR estimate is inaccurate. */
typedef struct
{
    int16_t *pcm;
    size_t capacity, head, count;
} media_tail_t;
bool media_tail_init(media_tail_t *tail, size_t frames);
void media_tail_free(media_tail_t *tail);
size_t media_tail_push(media_tail_t *tail, int16_t *pcm, size_t frames);
size_t media_tail_pop(media_tail_t *tail, int16_t *pcm, size_t frames);

/* Complementary ramps preserve headroom, including correlated full-scale signals. */
void media_crossfade_mix(int16_t *out, const int16_t *incoming, size_t frames,
                        size_t offset, size_t total);
