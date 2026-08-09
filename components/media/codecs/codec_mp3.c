/**
 * MPEG Layer III decoder, built on minimp3 (public domain).
 *
 * Handles CBR, VBR (Xing/Info and VBRI) and free-format streams. Duration comes from the
 * VBR header when present and falls back to a byte-rate estimate, which keeps the progress
 * bar honest for the common "no header" case.
 */
#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#include "../media_config.h"
#include "../media_decoder.h"
#include "../media_util.h"

#if MEDIA_CODEC_MP3

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DEC"

#define MINIMP3_ONLY_MP3            // Layer 1/2 are not worth the flash on these boards
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// Enough for two maximum-size frames plus a header, so a frame is never split across refills.
#define MP3_BUF_SIZE 8192
#define MP3_REFILL_THRESHOLD 4096

typedef struct
{
    mp3dec_t mp3d;
    uint8_t *buf;
    size_t len;         // Valid bytes in buf
    size_t pos;         // Consumed bytes in buf
    uint64_t stream_pos; // File offset of buf[0]

    int16_t pending[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pending_frames;
    int pending_read;

    uint32_t xing_frames;
    uint32_t xing_bytes;
    uint8_t toc[100];
    bool has_toc;
} mp3_state_t;

static bool mp3_probe(const uint8_t *header, size_t len)
{
    if (len < 4)
        return false;
    if (memcmp(header, "ID3", 3) == 0)
        return true;
    // Frame sync: 11 set bits, a valid layer and a valid bitrate index
    return header[0] == 0xFF && (header[1] & 0xE0) == 0xE0 && (header[1] & 0x18) != 0x08 &&
           (header[1] & 0x06) != 0x00 && (header[2] & 0xF0) != 0xF0 && (header[2] & 0x0C) != 0x0C;
}

/** Top up `buf`, moving any unconsumed bytes to the front. Returns bytes available. */
static size_t mp3_refill(media_decoder_t *dec, mp3_state_t *st)
{
    if (st->pos > 0)
    {
        size_t keep = st->len - st->pos;
        if (keep)
            memmove(st->buf, st->buf + st->pos, keep);
        st->stream_pos += st->pos;
        st->len = keep;
        st->pos = 0;
    }

    if (st->len < MP3_BUF_SIZE)
    {
        size_t got = media_source_read(dec->source, st->buf + st->len, MP3_BUF_SIZE - st->len, 4000);
        st->len += got;
    }

    return st->len - st->pos;
}

/** Parse a Xing/Info or VBRI header sitting inside the first audio frame. */
static void mp3_parse_vbr_header(media_decoder_t *dec, mp3_state_t *st, const uint8_t *frame,
                                 size_t frame_len, const mp3dec_frame_info_t *info)
{
    if (frame_len < 40 || info->channels < 1)
        return;

    // Xing/Info sits after the side info block, whose size depends on version and channels.
    int mpeg25 = (frame[1] & 0x10) == 0;
    int version = (frame[1] >> 3) & 3;
    bool is_mpeg1 = !mpeg25 && version == 3;
    size_t offset = 4 + (is_mpeg1 ? (info->channels == 1 ? 17 : 32) : (info->channels == 1 ? 9 : 17));

    if (offset + 8 > frame_len)
        return;

    if (memcmp(frame + offset, "Xing", 4) == 0 || memcmp(frame + offset, "Info", 4) == 0)
    {
        uint32_t flags = ((uint32_t)frame[offset + 4] << 24) | ((uint32_t)frame[offset + 5] << 16) |
                         ((uint32_t)frame[offset + 6] << 8) | frame[offset + 7];
        size_t p = offset + 8;

        if (flags & 0x01)
        {
            if (p + 4 > frame_len)
                return;
            st->xing_frames = ((uint32_t)frame[p] << 24) | ((uint32_t)frame[p + 1] << 16) |
                              ((uint32_t)frame[p + 2] << 8) | frame[p + 3];
            p += 4;
        }
        if (flags & 0x02)
        {
            if (p + 4 > frame_len)
                return;
            st->xing_bytes = ((uint32_t)frame[p] << 24) | ((uint32_t)frame[p + 1] << 16) |
                             ((uint32_t)frame[p + 2] << 8) | frame[p + 3];
            p += 4;
        }
        if (flags & 0x04)
        {
            if (p + 100 > frame_len)
                return;
            memcpy(st->toc, frame + p, 100);
            st->has_toc = true;
            p += 100;
        }
    }
    else if (offset >= 36 && frame_len >= 36 + 26 && memcmp(frame + 36, "VBRI", 4) == 0)
    {
        st->xing_bytes = ((uint32_t)frame[36 + 10] << 24) | ((uint32_t)frame[36 + 11] << 16) |
                         ((uint32_t)frame[36 + 12] << 8) | frame[36 + 13];
        st->xing_frames = ((uint32_t)frame[36 + 14] << 24) | ((uint32_t)frame[36 + 15] << 16) |
                          ((uint32_t)frame[36 + 16] << 8) | frame[36 + 17];
    }
}

static int mp3_open(media_decoder_t *dec)
{
    mp3_state_t *st = calloc(1, sizeof(mp3_state_t));
    if (!st)
        return MEDIA_ERR_NOMEM;

    st->buf = rg_alloc(MP3_BUF_SIZE, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    if (!st->buf)
    {
        free(st);
        return MEDIA_ERR_NOMEM;
    }

    mp3dec_init(&st->mp3d);
    dec->state = st;

    // Decode the first real frame to learn the format. minimp3 skips ID3 and junk itself.
    mp3dec_frame_info_t info = {0};
    int samples = 0;
    int attempts = 0;

    while (attempts++ < 64)
    {
        size_t avail = mp3_refill(dec, st);
        if (avail < 4)
            break;

        const uint8_t *frame = st->buf + st->pos;
        samples = mp3dec_decode_frame(&st->mp3d, frame, (int)avail, st->pending, &info);

        if (info.frame_bytes <= 0)
            break; // Needs more data than we can hold: not a usable stream

        if (samples > 0)
        {
            dec->data_offset = st->stream_pos + st->pos + info.frame_offset;
            mp3_parse_vbr_header(dec, st, frame + info.frame_offset,
                                 (size_t)(avail - info.frame_offset), &info);
            st->pos += info.frame_bytes;
            break;
        }

        st->pos += info.frame_bytes;
    }

    if (samples <= 0 || info.hz < MEDIA_PCM_SAMPLE_RATE_MIN / 2 || info.channels < 1)
    {
        RG_LOGW("No decodable MP3 frame found");
        free(st->buf);
        free(st);
        dec->state = NULL;
        return MEDIA_ERR_CORRUPT;
    }

    dec->sample_rate = (uint32_t)info.hz;
    dec->channels = (uint8_t)info.channels;
    dec->bits_per_sample = 16;
    dec->bitrate = (uint32_t)info.bitrate_kbps * 1000;
    dec->seekable = true;
    dec->gapless = false; // Encoder/decoder delay is not compensated

    uint64_t file_size = media_source_size(dec->source);
    dec->data_size = file_size > dec->data_offset ? file_size - dec->data_offset : 0;

    if (st->xing_frames)
    {
        // samples-per-frame is 1152 for MPEG1 and 576 for MPEG2/2.5
        uint32_t spf = (uint32_t)samples;
        dec->total_frames = (uint64_t)st->xing_frames * spf;
        dec->duration_ms = (uint32_t)((dec->total_frames * 1000ULL) / dec->sample_rate);
        if (st->xing_bytes && dec->duration_ms)
            dec->bitrate = (uint32_t)(((uint64_t)st->xing_bytes * 8000ULL) / dec->duration_ms);
    }
    else if (dec->bitrate)
    {
        dec->duration_ms = (uint32_t)((dec->data_size * 8000ULL) / dec->bitrate);
        dec->total_frames = ((uint64_t)dec->duration_ms * dec->sample_rate) / 1000;
    }

    // The frame we consumed above is real audio; hand it to the caller on the first decode().
    st->pending_frames = samples;
    st->pending_read = 0;

    return MEDIA_OK;
}

/** Copy `frames` from the pending buffer (which holds `channels` interleaved) as stereo. */
static int mp3_emit(media_decoder_t *dec, mp3_state_t *st, int16_t *pcm, size_t capacity)
{
    int available = st->pending_frames - st->pending_read;
    if (available <= 0)
        return 0;

    int count = available;
    if ((size_t)count > capacity)
        count = (int)capacity;

    const int16_t *src = st->pending + (size_t)st->pending_read * dec->channels;

    if (dec->channels >= 2)
    {
        for (int i = 0; i < count; ++i)
        {
            pcm[i * 2 + 0] = src[i * 2 + 0];
            pcm[i * 2 + 1] = src[i * 2 + 1];
        }
    }
    else
    {
        for (int i = 0; i < count; ++i)
        {
            pcm[i * 2 + 0] = src[i];
            pcm[i * 2 + 1] = src[i];
        }
    }

    st->pending_read += count;
    return count;
}

static int mp3_decode(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity)
{
    mp3_state_t *st = dec->state;
    if (!st || !frame_capacity)
        return -1;

    int emitted = mp3_emit(dec, st, pcm, frame_capacity);
    if (emitted)
        return emitted;

    st->pending_frames = 0;
    st->pending_read = 0;

    // Bad frames are common in the wild (tag remnants, truncated files). Skip a bounded number
    // before giving up so a single corrupt region does not end the track.
    for (int attempts = 0; attempts < 128; ++attempts)
    {
        size_t avail = st->len - st->pos;
        if (avail < MP3_REFILL_THRESHOLD)
            avail = mp3_refill(dec, st);

        if (avail < 4)
            return 0; // Clean end of stream

        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(&st->mp3d, st->buf + st->pos, (int)avail, st->pending, &info);

        if (info.frame_bytes <= 0)
        {
            // minimp3 wants more data. If the source is exhausted we are done.
            if (media_source_eof(dec->source) && st->len - st->pos < MP3_REFILL_THRESHOLD)
                return 0;
            if (mp3_refill(dec, st) == avail)
                return 0; // No progress possible
            continue;
        }

        st->pos += info.frame_bytes;

        if (samples > 0)
        {
            if (info.hz > 0 && (uint32_t)info.hz != dec->sample_rate)
            {
                // A mid-stream rate change would desync the output clock; stop cleanly instead.
                RG_LOGW("Sample rate changed mid-file (%u -> %d), stopping",
                        (unsigned)dec->sample_rate, info.hz);
                return 0;
            }
            if (info.bitrate_kbps > 0)
                dec->bitrate = (uint32_t)info.bitrate_kbps * 1000;
            dec->channels = (uint8_t)(info.channels >= 2 ? 2 : 1);
            st->pending_frames = samples;
            st->pending_read = 0;
            return mp3_emit(dec, st, pcm, frame_capacity);
        }
    }

    RG_LOGW("Too many undecodable MP3 frames, stopping");
    return 0;
}

static bool mp3_seek(media_decoder_t *dec, uint32_t position_ms)
{
    mp3_state_t *st = dec->state;
    if (!st || !dec->duration_ms || !dec->data_size)
        return false;

    if (position_ms > dec->duration_ms)
        position_ms = dec->duration_ms;

    uint64_t offset;
    if (st->has_toc)
    {
        // Xing TOC: 100 entries mapping percent-of-time to percent-of-bytes (0..255)
        double percent = (double)position_ms * 100.0 / (double)dec->duration_ms;
        if (percent > 99.0)
            percent = 99.0;
        int index = (int)percent;
        double frac = percent - index;
        double a = st->toc[index];
        double b = index < 99 ? st->toc[index + 1] : 256.0;
        double point = a + (b - a) * frac;
        offset = dec->data_offset + (uint64_t)((point / 256.0) * (double)dec->data_size);
    }
    else
    {
        offset = dec->data_offset + ((uint64_t)position_ms * dec->data_size) / dec->duration_ms;
    }

    if (!media_source_seek(dec->source, offset))
        return false;

    // Drop the decoder's bit reservoir; minimp3 resyncs on the next frame header.
    mp3dec_init(&st->mp3d);
    st->len = st->pos = 0;
    st->stream_pos = offset;
    st->pending_frames = st->pending_read = 0;
    return true;
}

static void mp3_close(media_decoder_t *dec)
{
    mp3_state_t *st = dec->state;
    if (!st)
        return;
    free(st->buf);
    free(st);
    dec->state = NULL;
}

const media_decoder_ops_t media_codec_mp3_ops = {
    .name = "mp3",
    .codec = MEDIA_CODEC_TYPE_MP3,
    .probe = mp3_probe,
    .open = mp3_open,
    .decode = mp3_decode,
    .seek = mp3_seek,
    .close = mp3_close,
};

#endif /* MEDIA_CODEC_MP3 */
