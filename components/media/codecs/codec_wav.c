/**
 * WAV/RIFF decoder.
 *
 * Supports PCM 8/16/24/32-bit integer and 32-bit IEEE float, 1..8 channels, and the
 * WAVE_FORMAT_EXTENSIBLE wrapper. Anything else is reported as unsupported rather than
 * being played as noise.
 */
#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#include "../media_config.h"
#include "../media_decoder.h"
#include "../media_util.h"

#if MEDIA_CODEC_WAV

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DEC"

#define WAV_FORMAT_PCM        0x0001
#define WAV_FORMAT_FLOAT      0x0003
#define WAV_FORMAT_EXTENSIBLE 0xFFFE

typedef struct
{
    uint16_t format;
    uint16_t block_align;
    uint8_t *scratch;
    size_t scratch_size;
} wav_state_t;

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool wav_probe(const uint8_t *header, size_t len)
{
    return len >= 12 && memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0;
}

static int wav_open(media_decoder_t *dec)
{
    uint8_t header[12];
    if (media_source_read(dec->source, header, sizeof(header), 3000) != sizeof(header))
        return MEDIA_ERR_IO;
    if (!wav_probe(header, sizeof(header)))
        return MEDIA_ERR_UNSUPPORTED;

    wav_state_t *st = calloc(1, sizeof(wav_state_t));
    if (!st)
        return MEDIA_ERR_NOMEM;

    bool have_fmt = false;
    uint64_t pos = 12;
    uint64_t file_size = media_source_size(dec->source);

    // Walk the chunk list until we reach "data". Chunk sizes are attacker controlled, so every
    // one is validated against the real file size before it is trusted.
    while (pos + 8 <= file_size)
    {
        uint8_t chunk[8];
        if (media_source_read(dec->source, chunk, 8, 3000) != 8)
            break;
        pos += 8;

        uint32_t size = rd32(chunk + 4);
        uint64_t remaining = file_size - pos;
        if (size > remaining)
            size = (uint32_t)remaining;

        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            uint8_t fmt[40] = {0};
            uint32_t want = size < sizeof(fmt) ? size : (uint32_t)sizeof(fmt);
            if (want < 16 || media_source_read(dec->source, fmt, want, 3000) != want)
                break;
            pos += want;

            st->format = rd16(fmt + 0);
            dec->channels = (uint8_t)media_clampi(rd16(fmt + 2), 0, 255);
            dec->sample_rate = rd32(fmt + 4);
            st->block_align = rd16(fmt + 12);
            dec->bits_per_sample = (uint8_t)media_clampi(rd16(fmt + 14), 0, 64);

            if (st->format == WAV_FORMAT_EXTENSIBLE && want >= 40)
                st->format = rd16(fmt + 24); // First two bytes of the SubFormat GUID

            have_fmt = true;

            if (size > want)
            {
                media_source_skip(dec->source, size - want);
                pos += size - want;
            }
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            dec->data_offset = pos;
            dec->data_size = size;
            break;
        }
        else
        {
            media_source_skip(dec->source, size);
            pos += size;
        }

        if (size & 1) // RIFF chunks are word aligned
        {
            media_source_skip(dec->source, 1);
            pos += 1;
        }
    }

    bool bits_ok = (st->format == WAV_FORMAT_PCM &&
                    (dec->bits_per_sample == 8 || dec->bits_per_sample == 16 ||
                     dec->bits_per_sample == 24 || dec->bits_per_sample == 32)) ||
                   (st->format == WAV_FORMAT_FLOAT && dec->bits_per_sample == 32);

    if (!have_fmt || !dec->data_size || !bits_ok || dec->channels < 1 || dec->channels > 8 ||
        dec->sample_rate < MEDIA_PCM_SAMPLE_RATE_MIN / 2 || dec->sample_rate > 192000)
    {
        RG_LOGW("Unsupported WAV: fmt=%u bits=%u ch=%u rate=%u", st->format, dec->bits_per_sample,
                dec->channels, (unsigned)dec->sample_rate);
        free(st);
        return MEDIA_ERR_UNSUPPORTED;
    }

    uint32_t frame_bytes = (uint32_t)dec->channels * (dec->bits_per_sample / 8);
    if (st->block_align == 0 || st->block_align != frame_bytes)
        st->block_align = (uint16_t)frame_bytes; // Trust the derived value over a bogus header

    // Read a whole number of source frames per decode call.
    st->scratch_size = (size_t)MEDIA_DECODE_BLOCK_FRAMES * st->block_align;
    st->scratch = rg_alloc(st->scratch_size, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    if (!st->scratch)
    {
        free(st);
        return MEDIA_ERR_NOMEM;
    }

    dec->state = st;
    dec->total_frames = dec->data_size / st->block_align;
    dec->duration_ms = (uint32_t)((dec->total_frames * 1000ULL) / dec->sample_rate);
    dec->bitrate = dec->sample_rate * frame_bytes * 8;
    dec->seekable = true;
    dec->gapless = true;

    return MEDIA_OK;
}

static int wav_decode(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity)
{
    wav_state_t *st = dec->state;
    if (!st)
        return -1;

    uint64_t consumed = dec->frames_decoded;
    if (dec->total_frames && consumed >= dec->total_frames)
        return 0;

    size_t want = frame_capacity;
    if (want > MEDIA_DECODE_BLOCK_FRAMES)
        want = MEDIA_DECODE_BLOCK_FRAMES;
    if (dec->total_frames && want > dec->total_frames - consumed)
        want = (size_t)(dec->total_frames - consumed);
    if (!want)
        return 0;

    size_t bytes = want * st->block_align;
    size_t got = media_source_read(dec->source, st->scratch, bytes, 4000);
    size_t frames = got / st->block_align;
    if (!frames)
        return 0;

    const int channels = dec->channels;
    const uint8_t *src = st->scratch;

    for (size_t i = 0; i < frames; ++i)
    {
        int32_t left = 0, right = 0;

        for (int c = 0; c < channels; ++c)
        {
            const uint8_t *s = src + (size_t)c * (dec->bits_per_sample / 8);
            int32_t value;

            switch (dec->bits_per_sample)
            {
            case 8:
                value = ((int32_t)s[0] - 128) << 8; // WAV 8-bit is unsigned
                break;
            case 16:
                value = (int16_t)rd16(s);
                break;
            case 24:
                value = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 16;
                break;
            default: // 32-bit int or float
                if (st->format == WAV_FORMAT_FLOAT)
                {
                    float f;
                    uint32_t raw = rd32(s);
                    memcpy(&f, &raw, sizeof(f));
                    if (!(f > -100.0f && f < 100.0f)) // Rejects NaN/Inf as well
                        f = 0.0f;
                    value = (int32_t)(media_clampf(f, -1.0f, 1.0f) * 32767.0f);
                }
                else
                {
                    value = (int32_t)(rd32(s) >> 16);
                    value = (int16_t)value;
                }
                break;
            }

            if (c == 0)
                left = value;
            else if (c == 1)
                right = value;
            else if (channels > 2)
            {
                // Fold any extra channels evenly into both sides rather than dropping them
                left += value / 2;
                right += value / 2;
            }
        }

        if (channels == 1)
            right = left;

        pcm[i * 2 + 0] = (int16_t)media_clampi(left, -32768, 32767);
        pcm[i * 2 + 1] = (int16_t)media_clampi(right, -32768, 32767);
        src += st->block_align;
    }

    return (int)frames;
}

static bool wav_seek(media_decoder_t *dec, uint32_t position_ms)
{
    wav_state_t *st = dec->state;
    if (!st || !dec->sample_rate)
        return false;

    uint64_t frame = ((uint64_t)position_ms * dec->sample_rate) / 1000;
    if (dec->total_frames && frame > dec->total_frames)
        frame = dec->total_frames;

    return media_source_seek(dec->source, dec->data_offset + frame * st->block_align);
}

static void wav_close(media_decoder_t *dec)
{
    wav_state_t *st = dec->state;
    if (!st)
        return;
    free(st->scratch);
    free(st);
    dec->state = NULL;
}

const media_decoder_ops_t media_codec_wav_ops = {
    .name = "wav",
    .codec = MEDIA_CODEC_TYPE_WAV,
    .probe = wav_probe,
    .open = wav_open,
    .decode = wav_decode,
    .seek = wav_seek,
    .close = wav_close,
};

#endif /* MEDIA_CODEC_WAV */
