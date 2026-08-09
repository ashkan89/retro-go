/**
 * Retro-Go media player - M3U/M3U8 playlists.
 *
 * Paths are resolved against the playlist's own directory and validated against the media
 * root, so a playlist cannot be used to walk the rest of the card.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

typedef struct
{
    char *pool;         // NUL-separated absolute paths
    size_t pool_used;
    size_t pool_capacity;
    uint32_t *offsets;
    int count;
    int capacity;
    char name[MEDIA_TAG_ALBUM_LEN];
} media_playlist_t;

/** Read a .m3u/.m3u8 file. Returns NULL when it is missing, empty or entirely invalid. */
media_playlist_t *media_playlist_load(const char *path, const char *root);
void media_playlist_free(media_playlist_t *playlist);

const char *media_playlist_entry(const media_playlist_t *playlist, int index);

/** Write `paths` out as UTF-8 M3U8 with an #EXTM3U header. Atomic. */
bool media_playlist_save(const char *path, const char *const *paths, int count);

/** Append one path to an existing playlist, creating it when necessary. */
bool media_playlist_append(const char *path, const char *track_path);

/** Enumerate playlists in `<root>/Playlists` and in the root itself. */
typedef struct
{
    char path[MEDIA_MAX_PATH + 1];
    char name[MEDIA_TAG_ALBUM_LEN];
} media_playlist_info_t;

int media_playlist_list(const char *root, media_playlist_info_t *out, int max);
