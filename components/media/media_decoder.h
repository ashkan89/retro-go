/**
 * Retro-Go media player - codec abstraction.
 *
 * Every decoder normalises its output to interleaved signed 16-bit stereo at the track's
 * native sample rate. Nothing outside this directory contains codec-specific logic.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_source.h"
#include "media_types.h"

typedef struct media_decoder_s media_decoder_t;

typedef struct
{
    const char *name;
    media_codec_t codec;

    /** Cheap header sniff. `header` holds the first `len` bytes of the file. */
    bool (*probe)(const uint8_t *header, size_t len);

    /** Parse headers and fill in the format fields of `dec`. Returns a media_err_t. */
    int (*open)(media_decoder_t *dec);

    /**
     * Decode up to `frame_capacity` stereo frames into `pcm`.
     * Returns frames produced, 0 on clean end of stream, or -1 on a fatal decode error.
     * Recoverable frame errors are handled internally by skipping.
     */
    int (*decode)(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity);

    /** Seek to `position_ms`. Returns true on success; the caller resyncs its counters. */
    bool (*seek)(media_decoder_t *dec, uint32_t position_ms);

    void (*close)(media_decoder_t *dec);
} media_decoder_ops_t;

struct media_decoder_s
{
    const media_decoder_ops_t *ops;
    media_source_t *source;
    void *state;

    /* Format, filled by ops->open() */
    uint32_t sample_rate;
    uint32_t bitrate;
    uint32_t duration_ms;
    uint64_t total_frames;      // 0 when unknown (e.g. a VBR MP3 with no Xing header)
    uint64_t data_offset;       // First byte of audio payload
    uint64_t data_size;
    uint8_t channels;           // Source channel count (output is always stereo)
    uint8_t bits_per_sample;
    bool seekable;
    bool gapless;               // True when the codec reports exact frame boundaries

    /* Maintained by the framework */
    uint64_t frames_decoded;    // Since the last seek
    uint32_t base_ms;           // Position of the last seek target
    bool eos;
};

/**
 * Open `path` through a buffered source. `buffer_bytes` sizes the compressed reserve.
 * On failure returns NULL and stores the reason in `*err` when `err` is non-NULL.
 */
media_decoder_t *media_decoder_open(const char *path, size_t buffer_bytes, media_err_t *err);
void media_decoder_close(media_decoder_t *dec);

int media_decoder_decode(media_decoder_t *dec, int16_t *pcm, size_t frame_capacity);
bool media_decoder_seek(media_decoder_t *dec, uint32_t position_ms);
uint32_t media_decoder_position_ms(const media_decoder_t *dec);
uint32_t media_decoder_duration_ms(const media_decoder_t *dec);

/** True when the extension/header is recognised, even if the codec is compiled out. */
bool media_decoder_format_known(const char *path);
/** True when a decoder for this file is actually compiled in. */
bool media_decoder_format_supported(const char *path);
