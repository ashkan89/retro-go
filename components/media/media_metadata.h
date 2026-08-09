/**
 * Retro-Go media player - tag parsing.
 *
 * Every parser here treats file contents as hostile: sizes are validated against the real
 * file length before use, frame counts are bounded, and no allocation is made from a length
 * field without first clamping it against the limits in media_config.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

typedef enum
{
    MEDIA_ART_NONE = 0,
    MEDIA_ART_EMBEDDED,
    MEDIA_ART_FOLDER,   // cover.jpg / folder.jpg beside the track
    MEDIA_ART_SIDECAR,  // <track>.jpg
} media_art_source_t;

typedef struct
{
    char title[MEDIA_TAG_TITLE_LEN];
    char artist[MEDIA_TAG_ARTIST_LEN];
    char album[MEDIA_TAG_ALBUM_LEN];
    char album_artist[MEDIA_TAG_ARTIST_LEN];
    char genre[MEDIA_TAG_SHORT_LEN];
    char composer[MEDIA_TAG_SHORT_LEN];
    char comment[MEDIA_TAG_SHORT_LEN];

    uint16_t year;
    uint16_t track_number;
    uint16_t disc_number;

    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t bitrate;
    uint8_t channels;
    uint8_t bits_per_sample;
    media_codec_t codec;

    int16_t replaygain_track;   // dB * 100 or MEDIA_REPLAYGAIN_NONE
    int16_t replaygain_album;

    bool has_embedded_art;
    uint64_t art_offset;        // Byte offset of the compressed image inside the file
    uint32_t art_length;

    bool has_embedded_lyrics;
    uint64_t lyrics_offset;
    uint32_t lyrics_length;
    uint8_t lyrics_kind;        // 0 = plain, 1 = LRC-formatted text, 2 = ID3 SYLT
    uint8_t lyrics_encoding;    // ID3 text encoding byte, 0xFF when not applicable
} media_metadata_t;

/** Parse tags and stream geometry. Returns false only when the file cannot be opened. */
bool media_metadata_read(const char *path, media_metadata_t *out);

/** Copy the parsed values into a library record, applying the documented fallbacks. */
void media_metadata_apply(media_track_t *track, const media_metadata_t *meta);

/**
 * Read the embedded picture. Returns a PSRAM buffer the caller must free(), or NULL.
 * The size is bounded by MEDIA_MAX_ARTWORK_BYTES.
 */
uint8_t *media_metadata_read_artwork(const char *path, const media_metadata_t *meta, size_t *len_out);

/**
 * Read embedded lyrics as a NUL-terminated UTF-8 string. Returns a malloc'd buffer or NULL.
 * `is_synced_out` reports whether the text carries LRC timestamps.
 */
char *media_metadata_read_lyrics(const char *path, const media_metadata_t *meta, bool *is_synced_out);

/**
 * Locate cover art for a track, preferring embedded, then <track>.jpg, then cover/folder
 * images in the containing directory. `out_path` is only written for file-based sources.
 */
media_art_source_t media_metadata_find_artwork(const char *track_path, const media_metadata_t *meta,
                                               char *out_path, size_t out_size);
