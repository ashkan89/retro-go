/**
 * Retro-Go media player - LRC lyrics.
 *
 * The parser accepts the common dialects: multiple timestamps per line, out-of-order lines,
 * ID tags ([ar:] [al:] [ti:] [by:] [offset:]), and plain unsynced text. Unknown tags are
 * ignored rather than treated as lyrics.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

typedef struct
{
    uint32_t time_ms;
    uint16_t offset;    // Byte offset of the text inside the pool
} media_lyric_line_t;

typedef struct
{
    media_lyric_line_t *lines;
    char *pool;             // Concatenated NUL-terminated line texts
    size_t pool_size;
    int count;
    int capacity;
    bool synced;            // False when the file had no timestamps at all
    int32_t offset_ms;      // From an [offset:] tag, added to every timestamp
    char title[MEDIA_TAG_TITLE_LEN];
    char artist[MEDIA_TAG_ARTIST_LEN];
} media_lyrics_t;

/** Parse an in-memory LRC/plain-text blob. Returns NULL if nothing usable was found. */
media_lyrics_t *media_lyrics_parse(const char *text, size_t len);

/** Load "<track>.lrc" (or ".txt") beside `track_path`. Returns NULL when absent. */
media_lyrics_t *media_lyrics_load_sidecar(const char *track_path);

void media_lyrics_free(media_lyrics_t *lyrics);

/**
 * Index of the line active at `position_ms`, or -1 before the first line.
 * Binary search: called once per rendered frame, so it must not scan.
 */
int media_lyrics_find(const media_lyrics_t *lyrics, uint32_t position_ms, int32_t user_offset_ms);

const char *media_lyrics_text(const media_lyrics_t *lyrics, int index);
uint32_t media_lyrics_time(const media_lyrics_t *lyrics, int index);
