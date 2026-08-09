/**
 * FLAC decoder, built on dr_flac (public domain).
 *
 * dr_flac pulls through the buffered source, so SD latency never reaches the audio task.
 * Its working buffers are forced into PSRAM: a 4096-sample block at 2 channels alone needs
 * 32 KB, which would otherwise eat most of the internal heap.
 */
#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#include "../media_config.h"
#include "../media_decoder.h"
#include "../media_util.h"

#if MEDIA_CODEC_FLAC

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DEC"

#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

typedef struct
{
    drflac *flac;
    media_decoder_t *dec;
    int32_t *scratch; // Only used when the stream is not stereo
    size_t scratch_frames;
} flac_state_t;

static void *flac_malloc(size_t sz, void *user)
{
    (void)user;
    // MEM_NOPANIC: a failed allocation must surface as "unsupported file", not a reboot.
    return rg_alloc(sz, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
}

static void *flac_realloc(void *p, size_t sz, void *user)
{
    (void)user;
    // rg_alloc has no realloc counterpart; dr_flac only grows buffers during open, and the
    // sizes involved are small enough that copy-on-grow is not a hot path.
    void *n = rg_alloc(sz, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    if (n && p)
        memcpy(n, p, sz); // Over-reads are impossible: dr_flac only ever grows
    free(p);
    return n;
}

static void flac_free(void *p, void *user)
{
    (void)user;
    free(p);
}

static size_t flac_on_read(void *user, void *out, size_t bytes)
{
    flac_state_t *st = user;
    return media_source_read(st->dec->source, out, bytes, 5000);
}

static drflac_bool32 flac_on_seek(void *user, int offset, drflac_seek_origin origin)
{
    flac_state_t *st = user;
    media_source_t *src = st->dec->source;
    int64_t base;

    switch (origin)
    {
    case DRFLAC_SEEK_CUR: base = (int64_t)media_source_tell(src); break;
    case DRFLAC_SEEK_END: base = (int64_t)media_source_size(src); break;
    default:              base = 0; break;
    }

    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)media_source_size(src))
        return DRFLAC_FALSE; // dr_flac probes past the end on purpose and expects a failure

    return media_source_seek(src, (uint64_t)target) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 flac_on_tell(void *user, drflac_int64 *cursor)
{
    flac_state_t *st = user;
    *cursor = (drflac_int64)media_source_tell(st->dec->source);
    return DRFLAC_TRUE;
}

static bool flac_probe(const uint8_t *header, size_t len)
{
    if (len >= 4 && memcmp(header, "fLaC", 4) == 0)
        return true;
    // A FLAC file may carry an ID3v2 tag before the stream marker
    if (len >= 10 && memcmp(header, "ID3", 3) == 0)
        return true;
    return false;
}

static int flac_open(media_decoder_t *dec)
{
    flac_state_t *st = calloc(1, sizeof(flac_state_t));
    if (!st)
        return MEDIA_ERR_NOMEM;

    st->dec = dec;
    dec->state = st;

    const drflac_allocation_callbacks cb = {
        .pUserData = NULL,
        .onMalloc = flac_malloc,
        .onRealloc = flac_realloc,
        .onFree = flac_free,
    };

    st->flac = drflac_open(flac_on_read, flac_on_seek, flac_on_tell, st, &cb);
    if (!st->flac)
    {
        RG_LOGW("drflac_open failed");
        free(st);
        dec->state = NULL;
        return MEDIA_ERR_CORRUPT;
    }

    if (st->flac->channels < 1 || st->flac->channels > 8 ||
        st->flac->sampleRate < MEDIA_PCM_SAMPLE_RATE_MIN / 2 || st->flac->sampleRate > 192000)
    {
        RG_LOGW("Unsupported FLAC geometry: %u ch @ %u Hz", st->flac->channels,
                (unsigned)st->flac->sampleRate);
        drflac_close(st->flac);
        free(st);
        dec->state = NULL;
        return MEDIA_ERR_UNSUPPORTED;
    }

    dec->sample_rate = st->flac->sampleRate;
    dec->channels = (uint8_t)st->flac->channels;
    dec->bits_per_sample = (uint8_t)st->flac->bitsPerSample;
    dec->total_frames = st->flac->totalPCMFrameCount;
    dec->duration_ms = dec->total_frames
                           ? (uint32_t)((dec->total_frames * 1000ULL) / dec->sample_rate)
                           : 0;
    dec->seekable = true;
    dec->gapless = true;

    uint64_t size = media_source_size(dec->source);
    if (dec->duration_ms)
        dec->bitrate = (uint32_t)((size * 8000ULL) / dec->duration_ms);

    if (dec->channels != 2)
    {
        st->scratch_frames = MEDIA_DECODE_BLOCK_FRAMES;
        // dr_flac writes `channels` int16 per frame when reading s16
        st->scratch = rg_alloc(st->scratch_frames * dec->channels * sizeof(int16_t),
                               MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
        if (!st->scratch)
        {
            drflac_close(st->flac);
            free(st);
            dec->state = NULL;
            return MEDIA_ERR_NOMEM;
        }
    }

    return MEDIA_OK;
}

static int flac_decode(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity)
{
    flac_state_t *st = dec->state;
    if (!st || !st->flac || !frame_capacity)
        return -1;

    if (dec->channels == 2)
        return (int)drflac_read_pcm_frames_s16(st->flac, frame_capacity, pcm);

    size_t want = frame_capacity < st->scratch_frames ? frame_capacity : st->scratch_frames;
    int16_t *tmp = (int16_t *)st->scratch;
    drflac_uint64 got = drflac_read_pcm_frames_s16(st->flac, want, tmp);

    const int channels = dec->channels;
    for (drflac_uint64 i = 0; i < got; ++i)
    {
        if (channels == 1)
        {
            pcm[i * 2 + 0] = tmp[i];
            pcm[i * 2 + 1] = tmp[i];
        }
        else
        {
            int32_t left = tmp[i * channels + 0];
            int32_t right = tmp[i * channels + 1];
            for (int c = 2; c < channels; ++c)
            {
                int32_t extra = tmp[i * channels + c] / 2;
                left += extra;
                right += extra;
            }
            pcm[i * 2 + 0] = (int16_t)media_clampi(left, -32768, 32767);
            pcm[i * 2 + 1] = (int16_t)media_clampi(right, -32768, 32767);
        }
    }

    return (int)got;
}

static bool flac_seek(media_decoder_t *dec, uint32_t position_ms)
{
    flac_state_t *st = dec->state;
    if (!st || !st->flac || !dec->sample_rate)
        return false;

    drflac_uint64 frame = ((drflac_uint64)position_ms * dec->sample_rate) / 1000;
    if (dec->total_frames && frame > dec->total_frames)
        frame = dec->total_frames;

    return drflac_seek_to_pcm_frame(st->flac, frame) == DRFLAC_TRUE;
}

static void flac_close(media_decoder_t *dec)
{
    flac_state_t *st = dec->state;
    if (!st)
        return;
    if (st->flac)
        drflac_close(st->flac);
    free(st->scratch);
    free(st);
    dec->state = NULL;
}

const media_decoder_ops_t media_codec_flac_ops = {
    .name = "flac",
    .codec = MEDIA_CODEC_TYPE_FLAC,
    .probe = flac_probe,
    .open = flac_open,
    .decode = flac_decode,
    .seek = flac_seek,
    .close = flac_close,
};

#endif /* MEDIA_CODEC_FLAC */
