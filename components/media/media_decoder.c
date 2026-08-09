#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#include "media_decoder.h"
#include "media_util.h"

// Enough to step over a partial frame and still find two consecutive MPEG sync words when a
// stream drops us into the middle of one.
#define MEDIA_PROBE_BYTES 1024

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DEC"

#if MEDIA_CODEC_WAV
extern const media_decoder_ops_t media_codec_wav_ops;
#endif
#if MEDIA_CODEC_MP3
extern const media_decoder_ops_t media_codec_mp3_ops;
#endif
#if MEDIA_CODEC_FLAC
extern const media_decoder_ops_t media_codec_flac_ops;
#endif

static const media_decoder_ops_t *const registry[] = {
#if MEDIA_CODEC_WAV
    &media_codec_wav_ops,
#endif
#if MEDIA_CODEC_MP3
    &media_codec_mp3_ops,
#endif
#if MEDIA_CODEC_FLAC
    &media_codec_flac_ops,
#endif
};

static const media_decoder_ops_t *find_ops(media_codec_t codec)
{
    for (size_t i = 0; i < RG_COUNT(registry); ++i)
    {
        if (registry[i]->codec == codec)
            return registry[i];
    }
    return NULL;
}

bool media_decoder_format_known(const char *path)
{
    return media_codec_from_path(path) != MEDIA_CODEC_NONE;
}

bool media_decoder_format_supported(const char *path)
{
    return find_ops(media_codec_from_path(path)) != NULL;
}

media_decoder_t *media_decoder_open(const char *path, size_t buffer_bytes, media_err_t *err)
{
    media_err_t result = MEDIA_ERR_UNSUPPORTED;

    if (err)
        *err = result;

    if (!path || !*path)
        return NULL;

    media_codec_t codec = media_codec_from_path(path);
    const media_decoder_ops_t *ops = find_ops(codec);

    media_decoder_t *dec = calloc(1, sizeof(media_decoder_t));
    if (!dec)
    {
        if (err)
            *err = MEDIA_ERR_NOMEM;
        return NULL;
    }

    dec->source = media_source_open(path, buffer_bytes);
    if (!dec->source)
    {
        free(dec);
        if (err)
            *err = MEDIA_ERR_NOTFOUND;
        return NULL;
    }

    // The server's Content-Type outranks everything: a stream URL usually has no usable
    // extension, and an Icecast connection drops you into the middle of a frame so the first
    // bytes are not a recognisable header either.
    const char *mime = media_source_content_type(dec->source);
    media_codec_t mime_codec = media_codec_from_mime(mime);

    if (mime_codec != MEDIA_CODEC_NONE)
    {
        const media_decoder_ops_t *by_mime = find_ops(mime_codec);
        if (by_mime)
        {
            ops = by_mime;
            codec = mime_codec;
        }
        else
        {
            // Recognised, but this build has no decoder for it. Saying which format it is
            // beats a bare "unsupported".
            RG_LOGW("'%s' is %s, which is not compiled in", rg_basename(path),
                    media_codec_name(mime_codec));
            media_source_close(dec->source);
            free(dec);
            if (err)
                *err = MEDIA_ERR_UNSUPPORTED;
            return NULL;
        }
    }

    // With a Content-Type in hand there is nothing to sniff: the server is more authoritative
    // than the first few bytes of a broadcast, and skipping the probe means playback starts
    // without first waiting for a probe window to arrive.
    if (!ops || mime_codec == MEDIA_CODEC_NONE)
    {
        // Peeked rather than read, because a live broadcast cannot be rewound afterwards. The
        // window is generous so a stream that drops us mid-frame can still be recognised.
        uint8_t header[MEDIA_PROBE_BYTES] = {0};
        size_t header_len = media_source_peek(dec->source, header, sizeof(header), 5000);

        for (size_t i = 0; header_len >= 4 && i < RG_COUNT(registry); ++i)
        {
            if (!registry[i]->probe(header, header_len))
                continue;

            // The extension's guess only loses to the bytes when it is outright wrong.
            if (ops && registry[i] != ops)
                RG_LOGW("'%s' is not %s, using %s instead", rg_basename(path), ops->name,
                        registry[i]->name);
            ops = registry[i];
            break;
        }
    }

    if (!ops)
    {
        RG_LOGW("No decoder for '%s'", rg_basename(path));
        media_source_close(dec->source);
        free(dec);
        if (err)
            *err = codec == MEDIA_CODEC_NONE ? MEDIA_ERR_UNSUPPORTED : MEDIA_ERR_UNSUPPORTED;
        return NULL;
    }

    dec->ops = ops;
    result = (media_err_t)ops->open(dec);

    if (result != MEDIA_OK || !dec->sample_rate)
    {
        RG_LOGW("%s decoder rejected '%s': %s", ops->name, rg_basename(path),
                media_error_name(result));
        if (dec->state)
            ops->close(dec);
        media_source_close(dec->source);
        free(dec);
        if (err)
            *err = result == MEDIA_OK ? MEDIA_ERR_CORRUPT : result;
        return NULL;
    }

    // The audio path only ever reconfigures I2S within this range; anything else would run
    // at the wrong speed rather than simply sounding bad.
    if (dec->sample_rate < MEDIA_PCM_SAMPLE_RATE_MIN || dec->sample_rate > MEDIA_PCM_SAMPLE_RATE_MAX)
    {
        RG_LOGW("Sample rate %u out of range", (unsigned)dec->sample_rate);
        ops->close(dec);
        media_source_close(dec->source);
        free(dec);
        if (err)
            *err = MEDIA_ERR_UNSUPPORTED;
        return NULL;
    }

    // A codec may advertise seeking, but it can only actually seek if the source can. A live
    // broadcast, or a server that refuses Range, is forward-only.
    if (!media_source_seekable(dec->source))
    {
        dec->seekable = false;
        dec->duration_ms = 0;
        dec->total_frames = 0;
    }

    RG_LOGI("Decoding '%s' with %s: %u Hz, %u ch, %u bit, %u ms%s", rg_basename(path), ops->name,
            (unsigned)dec->sample_rate, dec->channels, dec->bits_per_sample,
            (unsigned)dec->duration_ms, dec->seekable ? "" : ", forward-only");

    if (err)
        *err = MEDIA_OK;
    return dec;
}

void media_decoder_close(media_decoder_t *dec)
{
    if (!dec)
        return;
    if (dec->ops && dec->state)
        dec->ops->close(dec);
    media_source_close(dec->source);
    free(dec);
}

int media_decoder_decode(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity)
{
    if (!dec || !dec->ops || !pcm || !frame_capacity)
        return -1;
    if (dec->eos)
        return 0;

    int frames = dec->ops->decode(dec, pcm, frame_capacity);
    if (frames > 0)
        dec->frames_decoded += (uint64_t)frames;
    else
        dec->eos = true;

    return frames;
}

bool media_decoder_seek(media_decoder_t *dec, uint32_t position_ms)
{
    if (!dec || !dec->ops || !dec->ops->seek || !dec->seekable)
        return false;

    if (dec->duration_ms && position_ms > dec->duration_ms)
        position_ms = dec->duration_ms;

    if (!dec->ops->seek(dec, position_ms))
        return false;

    dec->base_ms = position_ms;
    dec->frames_decoded = 0;
    dec->eos = false;
    return true;
}

uint32_t media_decoder_position_ms(const media_decoder_t *dec)
{
    if (!dec || !dec->sample_rate)
        return 0;
    return dec->base_ms + (uint32_t)((dec->frames_decoded * 1000ULL) / dec->sample_rate);
}

uint32_t media_decoder_duration_ms(const media_decoder_t *dec)
{
    return dec ? dec->duration_ms : 0;
}
