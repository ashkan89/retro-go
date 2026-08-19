/**
 * Retro-Go media player - library index.
 *
 * The index has two halves with very different lifetimes:
 *
 *  - library.idx  : derived data (one fixed-size media_track_t per track). Rebuildable at
 *                   any time, written atomically, versioned.
 *  - stats.bin    : user data (favourites, play counts, resume positions) keyed by path hash
 *                   so a full rebuild never loses it.
 *
 * Only a compact 24-byte entry plus a display-name pool is held in RAM. Full records are
 * read back from SD by record number when something actually needs them, which keeps an
 * N8R2 usable with a library far larger than its PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

#define MEDIA_LIBRARY_DB_VERSION 1

enum
{
    MEDIA_ENTRY_FAVORITE = (1 << 0),
    MEDIA_ENTRY_HAS_ART = (1 << 1),
    MEDIA_ENTRY_HAS_LYRICS = (1 << 2),
};

typedef struct
{
    uint32_t id;            // Record number in library.idx, 1-based
    uint32_t album_hash;
    uint32_t dir_hash;
    uint32_t name_offset;   // "title\0artist\0" inside the pool
    uint16_t track_number;
    uint16_t duration_s;
    uint16_t year;
    uint8_t disc_number;
    uint8_t flags;
} media_entry_t;

typedef enum
{
    MEDIA_VIEW_ALBUMS = 0,
    MEDIA_VIEW_ARTISTS,
    MEDIA_VIEW_GENRES,
    MEDIA_VIEW_ALL_TRACKS,
    MEDIA_VIEW_FAVORITES,
    MEDIA_VIEW_RECENT,
    MEDIA_VIEW_MOST_PLAYED,
    MEDIA_VIEW_COUNT,
} media_view_t;

typedef struct
{
    bool scanning;
    bool complete;
    uint32_t tracks_found;
    uint32_t files_seen;
    uint32_t folders_seen;
    uint32_t albums;
    char current[96];       // Relative path being scanned, for the progress screen
} media_scan_status_t;

/** Root directory (default "/sd/media"). Must be called before anything else. */
void media_library_init(const char *root);
void media_library_deinit(void);
const char *media_library_root(void);
void media_library_set_root(const char *root);

/** Load the on-disk index into RAM. Returns false when absent or incompatible. */
bool media_library_load(void);

/** Delete the derived index (library.idx) so it is rebuilt. Keeps stats.bin, which is user data. */
void media_library_clear_cache(void);

/** Start a background scan. `full` discards the existing index instead of updating it. */
bool media_library_scan_start(bool full);
void media_library_scan_stop(void);
media_scan_status_t media_library_scan_status(void);

/** True while the index is usable (loaded or a scan has produced entries). */
bool media_library_ready(void);
uint32_t media_library_track_count(void);

/** Slim entry by position in the master list, or NULL. */
const media_entry_t *media_library_entry(uint32_t index);
const char *media_library_entry_title(const media_entry_t *entry);
const char *media_library_entry_artist(const media_entry_t *entry);

/** Read a full record. Returns false when the id is unknown or the file is unreadable. */
bool media_library_get_track(uint32_t id, media_track_t *out);

/** Locate a track by path, indexing it on the fly when the library has not seen it. */
uint32_t media_library_find_by_path(const char *path);

/* ---- Views ------------------------------------------------------------------------- */

/**
 * Build a list of entry indices for `view`. `filter_hash` narrows albums/artists/genres to
 * one group (0 = list the groups themselves). Returns the count and fills `*out` with an
 * array owned by the library, valid until the next media_library_query() call.
 */
uint32_t media_library_query(media_view_t view, uint32_t filter_hash, const uint32_t **out);

/** Group lists for the album/artist/genre browsers. */
uint32_t media_library_group_count(media_view_t view);
const media_group_t *media_library_group(media_view_t view, uint32_t index);

/* ---- User data ---------------------------------------------------------------------- */

bool media_library_is_favorite(uint32_t id);
void media_library_set_favorite(uint32_t id, bool favorite);

/** Record that a track was played. Writes are debounced; call media_library_commit() to flush. */
void media_library_note_played(uint32_t id);
void media_library_note_skipped(uint32_t id);
void media_library_note_position(uint32_t id, uint32_t position_ms, uint32_t duration_ms);
uint32_t media_library_get_position(uint32_t id);

/** Flush pending user data to SD if it is dirty. Cheap when it is not. */
void media_library_commit(void);

/**
 * Install the callback the scanner consults before every file. It should return 0 when the
 * system is idle, 1 while playing comfortably and 2 when the audio buffer is running low.
 * Installed by the playback controller; without it the scanner runs at full speed.
 */
void media_library_set_pressure_source(int (*cb)(void));
