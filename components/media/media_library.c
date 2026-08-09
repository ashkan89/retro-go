#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "media_library.h"
#include "media_metadata.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DB"

#define LIBRARY_MAGIC 0x4744454Du /* "MEDG" */
#define STATS_MAGIC   0x53544D4Du /* "MMTS" */
#define CACHE_DIR_NAME ".retrogo-media"

/* Room for the root plus "/.retrogo-media/library.idx.tmp" and a NUL. */
#define LIB_DIR_MAX  (MEDIA_MAX_PATH + 24)   // root + "/.retrogo-media"
#define LIB_PATH_MAX (MEDIA_MAX_PATH + 48)   // cache dir + "/library.idx"

#define INDEX_FILE  "library.idx"
#define STATS_FILE  "stats.bin"

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t track_count;
    uint32_t scan_time;
    uint32_t root_hash;
    uint32_t reserved[4];
} library_header_t;

typedef struct __attribute__((packed))
{
    uint32_t path_hash;
    uint32_t last_played;
    uint32_t last_position_ms;
    uint16_t play_count;
    uint16_t skip_count;
    uint8_t favorite;
    uint8_t reserved[3];
} stats_record_t;

typedef struct
{
    uint32_t hash;      // path hash, for incremental scanning
    uint32_t mtime;
    uint32_t size;
} scan_key_t;

static struct
{
    char root[MEDIA_MAX_PATH + 1];
    // Derived paths append a fixed suffix to the root, so they need headroom of their own.
    char cache_dir[LIB_DIR_MAX];
    char index_path[LIB_PATH_MAX];
    char stats_path[LIB_PATH_MAX];

    media_entry_t *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;

    char *pool;
    uint32_t pool_used;
    uint32_t pool_capacity;

    scan_key_t *keys;   // Parallel to entries, only populated during/after a scan

    media_group_t *albums;
    uint32_t album_count;
    media_group_t *artists;
    uint32_t artist_count;
    media_group_t *genres;
    uint32_t genre_count;

    stats_record_t *stats;
    uint32_t stats_count;
    uint32_t stats_capacity;
    bool stats_dirty;
    int64_t stats_dirty_since;

    uint32_t *query_result;
    uint32_t query_capacity;

    rg_mutex_t *lock;
    rg_task_t *scan_task;
    volatile bool scan_running;
    volatile bool scan_stop;
    volatile bool scan_full;
    media_scan_status_t status;

    bool loaded;
    uint32_t max_tracks;

    // Kept open across lookups; closed before the index file is replaced or freed.
    FILE *index_fp;
} lib;

static void index_close(void)
{
    if (lib.index_fp)
    {
        fclose(lib.index_fp);
        lib.index_fp = NULL;
    }
}

/* -------------------------------------------------------------------------------------- */
/* Storage helpers                                                                          */
/* -------------------------------------------------------------------------------------- */

/* Group arrays retired by the previous build_groups(); see the comment there. */
static media_group_t *retired_albums;
static media_group_t *retired_artists;
static media_group_t *retired_genres;

static void rebuild_paths(void)
{
    snprintf(lib.cache_dir, sizeof(lib.cache_dir), "%s/%s", lib.root, CACHE_DIR_NAME);
    snprintf(lib.index_path, sizeof(lib.index_path), "%s/%s", lib.cache_dir, INDEX_FILE);
    snprintf(lib.stats_path, sizeof(lib.stats_path), "%s/%s", lib.cache_dir, STATS_FILE);
}

static bool ensure_cache_dir(void)
{
    if (rg_storage_exists(lib.cache_dir))
        return true;
    return rg_storage_mkdir(lib.cache_dir);
}

/* -------------------------------------------------------------------------------------- */
/* Slim entry storage                                                                       */
/* -------------------------------------------------------------------------------------- */

static bool entries_reserve(uint32_t needed)
{
    if (needed <= lib.entry_capacity)
        return true;
    if (needed > lib.max_tracks)
        return false;

    uint32_t capacity = lib.entry_capacity ? lib.entry_capacity * 2 : 256;
    while (capacity < needed)
        capacity *= 2;
    if (capacity > lib.max_tracks)
        capacity = lib.max_tracks;

    media_entry_t *entries = realloc(lib.entries, (size_t)capacity * sizeof(media_entry_t));
    if (!entries)
        return false;
    lib.entries = entries;

    scan_key_t *keys = realloc(lib.keys, (size_t)capacity * sizeof(scan_key_t));
    if (!keys)
        return false;
    lib.keys = keys;

    lib.entry_capacity = capacity;
    return true;
}

/** Append "title\0artist\0" to the pool and return the offset, or UINT32_MAX on failure. */
static uint32_t pool_add(const char *title, const char *artist)
{
    size_t tlen = strlen(title);
    size_t alen = strlen(artist);
    size_t need = tlen + alen + 2;

    if (lib.pool_used + need > lib.pool_capacity)
    {
        uint32_t capacity = lib.pool_capacity ? lib.pool_capacity * 2 : 16384;
        while (capacity < lib.pool_used + need)
            capacity *= 2;
        // The pool is the single biggest RAM cost of the library; cap it by profile.
        if (capacity > lib.max_tracks * 96u)
            capacity = lib.max_tracks * 96u;
        if (lib.pool_used + need > capacity)
            return UINT32_MAX;

        char *pool = realloc(lib.pool, capacity);
        if (!pool)
            return UINT32_MAX;
        lib.pool = pool;
        lib.pool_capacity = capacity;
    }

    uint32_t offset = lib.pool_used;
    memcpy(lib.pool + offset, title, tlen + 1);
    memcpy(lib.pool + offset + tlen + 1, artist, alen + 1);
    lib.pool_used += (uint32_t)need;
    return offset;
}

const char *media_library_entry_title(const media_entry_t *entry)
{
    if (!entry || !lib.pool || entry->name_offset >= lib.pool_used)
        return "";
    return lib.pool + entry->name_offset;
}

const char *media_library_entry_artist(const media_entry_t *entry)
{
    const char *title = media_library_entry_title(entry);
    if (!*title)
        return "";
    const char *artist = title + strlen(title) + 1;
    if (artist >= lib.pool + lib.pool_used)
        return "";
    return artist;
}

/* -------------------------------------------------------------------------------------- */
/* Statistics (favourites, play counts, resume positions)                                   */
/* -------------------------------------------------------------------------------------- */

static stats_record_t *stats_find(uint32_t path_hash, bool create)
{
    for (uint32_t i = 0; i < lib.stats_count; ++i)
    {
        if (lib.stats[i].path_hash == path_hash)
            return &lib.stats[i];
    }

    if (!create)
        return NULL;

    if (lib.stats_count >= lib.stats_capacity)
    {
        uint32_t capacity = lib.stats_capacity ? lib.stats_capacity * 2 : 128;
        // Bounded: this file is loaded whole, so it must never grow without limit.
        if (capacity > 8192)
            capacity = 8192;
        if (lib.stats_count >= capacity)
            return NULL;
        stats_record_t *records = realloc(lib.stats, (size_t)capacity * sizeof(stats_record_t));
        if (!records)
            return NULL;
        lib.stats = records;
        lib.stats_capacity = capacity;
    }

    stats_record_t *record = &lib.stats[lib.stats_count++];
    memset(record, 0, sizeof(*record));
    record->path_hash = path_hash;
    return record;
}

static void stats_load(void)
{
    void *data = NULL;
    size_t len = 0;

    if (!rg_storage_read_file(lib.stats_path, &data, &len, 0))
        return;

    if (len >= 8 && *(uint32_t *)data == STATS_MAGIC)
    {
        uint32_t count = ((uint32_t *)data)[1];
        size_t expected = 8 + (size_t)count * sizeof(stats_record_t);
        if (count <= 8192 && expected <= len)
        {
            lib.stats = calloc(count ? count : 1, sizeof(stats_record_t));
            if (lib.stats)
            {
                memcpy(lib.stats, (uint8_t *)data + 8, (size_t)count * sizeof(stats_record_t));
                lib.stats_count = count;
                lib.stats_capacity = count ? count : 1;
                RG_LOGI("Loaded %u play statistics", (unsigned)count);
            }
        }
        else
        {
            RG_LOGW("stats.bin is inconsistent, ignoring it");
        }
    }

    free(data);
}

static void stats_save(void)
{
    if (!lib.stats || !lib.stats_count)
        return;
    if (!ensure_cache_dir())
        return;

    size_t len = 8 + (size_t)lib.stats_count * sizeof(stats_record_t);
    uint8_t *buffer = malloc(len);
    if (!buffer)
        return;

    ((uint32_t *)buffer)[0] = STATS_MAGIC;
    ((uint32_t *)buffer)[1] = lib.stats_count;
    memcpy(buffer + 8, lib.stats, len - 8);

    // Atomic write: a power loss mid-save must not destroy the user's favourites.
    if (rg_storage_write_file(lib.stats_path, buffer, len, RG_FILE_ATOMIC_WRITE))
        lib.stats_dirty = false;

    free(buffer);
}

static void stats_apply(media_entry_t *entry, uint32_t path_hash)
{
    stats_record_t *record = stats_find(path_hash, false);
    if (record && record->favorite)
        entry->flags |= MEDIA_ENTRY_FAVORITE;
}

/* -------------------------------------------------------------------------------------- */
/* Index file I/O                                                                           */
/* -------------------------------------------------------------------------------------- */

static void entry_from_track(media_entry_t *entry, const media_track_t *track, uint32_t id)
{
    entry->id = id;
    entry->album_hash = track->album_hash;
    entry->dir_hash = track->dir_hash;
    entry->track_number = track->track_number;
    entry->duration_s = (uint16_t)media_clampi((int)(track->duration_ms / 1000), 0, 65535);
    entry->year = track->year;
    entry->disc_number = (uint8_t)media_clampi(track->disc_number, 0, 255);
    entry->flags = 0;
    if (track->has_embedded_art)
        entry->flags |= MEDIA_ENTRY_HAS_ART;
    if (track->has_lyrics)
        entry->flags |= MEDIA_ENTRY_HAS_LYRICS;
    if (track->favorite)
        entry->flags |= MEDIA_ENTRY_FAVORITE;
}

bool media_library_get_track(uint32_t id, media_track_t *out)
{
    if (!id || !out)
        return false;

    // Called from the UI, the decode task and the scanner, so the shared handle needs a lock.
    if (lib.lock && !rg_mutex_take(lib.lock, 2000))
        return false;

    if (!lib.index_fp)
        lib.index_fp = fopen(lib.index_path, "rb");

    FILE *fp = lib.index_fp;
    if (!fp)
    {
        if (lib.lock)
            rg_mutex_give(lib.lock);
        return false;
    }

    bool ok = false;
    long offset = (long)sizeof(library_header_t) + (long)(id - 1) * (long)sizeof(media_track_t);

    if (fseek(fp, offset, SEEK_SET) == 0 && fread(out, 1, sizeof(*out), fp) == sizeof(*out))
    {
        // Guard against a truncated or mismatched file producing garbage strings
        out->path[MEDIA_MAX_PATH] = 0;
        out->title[sizeof(out->title) - 1] = 0;
        out->artist[sizeof(out->artist) - 1] = 0;
        out->album[sizeof(out->album) - 1] = 0;
        out->album_artist[sizeof(out->album_artist) - 1] = 0;
        out->genre[sizeof(out->genre) - 1] = 0;
        ok = out->id == id && out->path[0] != 0;
    }

    if (lib.lock)
        rg_mutex_give(lib.lock);

    if (ok)
    {
        stats_record_t *record = stats_find(out->path_hash, false);
        if (record)
        {
            out->favorite = record->favorite != 0;
            out->play_count = record->play_count;
            out->skip_count = record->skip_count;
            out->last_played = record->last_played;
            out->last_position_ms = record->last_position_ms;
        }
    }

    return ok;
}

/* -------------------------------------------------------------------------------------- */
/* Group building                                                                           */
/* -------------------------------------------------------------------------------------- */

static int compare_groups(const void *a, const void *b)
{
    return media_strnatcasecmp(((const media_group_t *)a)->name, ((const media_group_t *)b)->name);
}

static media_group_t *group_find(media_group_t *groups, uint32_t count, uint32_t hash)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (groups[i].hash == hash)
            return &groups[i];
    }
    return NULL;
}

/**
 * Albums, artists and genres are derived by re-reading the full records once after a scan or
 * a load. It costs one linear pass over the index file, which is far cheaper than keeping
 * every string resident.
 */
static void build_groups(void)
{
    // Stop any concurrent reader before the pointers change, then retire the old arrays so
    // one already inside a group cannot fault. They are released on the next rebuild.
    lib.album_count = lib.artist_count = lib.genre_count = 0;
    __sync_synchronize();

    free(retired_albums), retired_albums = lib.albums, lib.albums = NULL;
    free(retired_artists), retired_artists = lib.artists, lib.artists = NULL;
    free(retired_genres), retired_genres = lib.genres, lib.genres = NULL;

    if (!lib.entry_count)
        return;

    uint32_t capacity = lib.entry_count < 512 ? lib.entry_count + 1 : 512;
    uint32_t genre_capacity = capacity / 4 + 1;

    media_group_t *albums = calloc(capacity, sizeof(media_group_t));
    media_group_t *artists = calloc(capacity, sizeof(media_group_t));
    media_group_t *genres = calloc(genre_capacity, sizeof(media_group_t));
    uint32_t album_count = 0, artist_count = 0, genre_count = 0;

    if (!albums || !artists || !genres)
    {
        RG_LOGW("Not enough memory for group lists");
        free(albums), free(artists), free(genres);
        return;
    }

    FILE *fp = fopen(lib.index_path, "rb");
    media_track_t *track = fp ? malloc(sizeof(media_track_t)) : NULL;

    if (!fp || !track || fseek(fp, (long)sizeof(library_header_t), SEEK_SET) != 0)
    {
        free(track);
        if (fp)
            fclose(fp);
        free(albums), free(artists), free(genres);
        return;
    }

    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        if (fread(track, 1, sizeof(*track), fp) != sizeof(*track))
            break;

        track->album[sizeof(track->album) - 1] = 0;
        track->album_artist[sizeof(track->album_artist) - 1] = 0;
        track->artist[sizeof(track->artist) - 1] = 0;
        track->genre[sizeof(track->genre) - 1] = 0;

        media_group_t *album = group_find(albums, album_count, track->album_hash);
        if (!album && album_count < capacity)
        {
            album = &albums[album_count++];
            album->hash = track->album_hash;
            media_utf8_copy(album->name, sizeof(album->name),
                            track->album[0] ? track->album : "Unknown Album");
            media_utf8_copy(album->artist, sizeof(album->artist),
                            track->album_artist[0] ? track->album_artist : "Unknown Artist");
            album->year = track->year;
            album->first_track_id = track->id;
        }
        if (album)
            album->track_count++;

        const char *artist_name = track->album_artist[0] ? track->album_artist
                                  : (track->artist[0] ? track->artist : "Unknown Artist");
        uint32_t artist_hash = rg_hash(artist_name, strlen(artist_name));
        media_group_t *artist = group_find(artists, artist_count, artist_hash);
        if (!artist && artist_count < capacity)
        {
            artist = &artists[artist_count++];
            artist->hash = artist_hash;
            media_utf8_copy(artist->name, sizeof(artist->name), artist_name);
            artist->first_track_id = track->id;
        }
        if (artist)
            artist->track_count++;

        if (track->genre[0])
        {
            uint32_t genre_hash = rg_hash(track->genre, strlen(track->genre));
            media_group_t *genre = group_find(genres, genre_count, genre_hash);
            if (!genre && genre_count < genre_capacity)
            {
                genre = &genres[genre_count++];
                genre->hash = genre_hash;
                media_utf8_copy(genre->name, sizeof(genre->name), track->genre);
                genre->first_track_id = track->id;
            }
            if (genre)
                genre->track_count++;
        }
    }

    free(track);
    fclose(fp);

    if (album_count)
        qsort(albums, album_count, sizeof(media_group_t), compare_groups);
    if (artist_count)
        qsort(artists, artist_count, sizeof(media_group_t), compare_groups);
    if (genre_count)
        qsort(genres, genre_count, sizeof(media_group_t), compare_groups);

    lib.albums = albums;
    lib.artists = artists;
    lib.genres = genres;
    __sync_synchronize();
    lib.album_count = album_count;
    lib.artist_count = artist_count;
    lib.genre_count = genre_count;

    lib.status.albums = lib.album_count;

    RG_LOGI("Groups: %u albums, %u artists, %u genres", (unsigned)lib.album_count,
            (unsigned)lib.artist_count, (unsigned)lib.genre_count);
}

/* -------------------------------------------------------------------------------------- */
/* Loading                                                                                  */
/* -------------------------------------------------------------------------------------- */

static void clear_index(void)
{
    free(lib.entries), lib.entries = NULL;
    free(lib.keys), lib.keys = NULL;
    free(lib.pool), lib.pool = NULL;
    lib.entry_count = lib.entry_capacity = 0;
    lib.pool_used = lib.pool_capacity = 0;
    free(lib.albums), lib.albums = NULL, lib.album_count = 0;
    free(lib.artists), lib.artists = NULL, lib.artist_count = 0;
    free(lib.genres), lib.genres = NULL, lib.genre_count = 0;
    lib.loaded = false;
}

bool media_library_load(void)
{
    clear_index();
    index_close();

    FILE *fp = fopen(lib.index_path, "rb");
    if (!fp)
        return false;

    library_header_t header;
    if (fread(&header, 1, sizeof(header), fp) != sizeof(header))
    {
        fclose(fp);
        return false;
    }

    if (header.magic != LIBRARY_MAGIC || header.version != MEDIA_LIBRARY_DB_VERSION ||
        header.record_size != sizeof(media_track_t))
    {
        RG_LOGW("Index is version %u/%u, expected %u/%u - it will be rebuilt", header.version,
                header.record_size, MEDIA_LIBRARY_DB_VERSION, (unsigned)sizeof(media_track_t));
        fclose(fp);
        return false;
    }

    if (header.track_count == 0 || header.track_count > lib.max_tracks)
    {
        RG_LOGW("Index holds %u tracks, which exceeds this device's limit of %u",
                (unsigned)header.track_count, (unsigned)lib.max_tracks);
        if (header.track_count == 0)
        {
            fclose(fp);
            return false;
        }
        header.track_count = lib.max_tracks;
    }

    if (!entries_reserve(header.track_count))
    {
        fclose(fp);
        return false;
    }

    media_track_t *track = malloc(sizeof(media_track_t));
    if (!track)
    {
        fclose(fp);
        return false;
    }

    for (uint32_t i = 0; i < header.track_count; ++i)
    {
        if (fread(track, 1, sizeof(*track), fp) != sizeof(*track))
            break;

        track->title[sizeof(track->title) - 1] = 0;
        track->artist[sizeof(track->artist) - 1] = 0;
        track->path[MEDIA_MAX_PATH] = 0;

        if (!track->id || !track->path[0])
            continue;

        uint32_t offset = pool_add(track->title, track->artist);
        if (offset == UINT32_MAX)
        {
            RG_LOGW("Name pool full at %u tracks", (unsigned)lib.entry_count);
            break;
        }

        media_entry_t *entry = &lib.entries[lib.entry_count];
        entry_from_track(entry, track, track->id);
        entry->name_offset = offset;
        stats_apply(entry, track->path_hash);

        lib.keys[lib.entry_count] = (scan_key_t){track->path_hash, track->mtime, track->file_size};
        lib.entry_count++;
    }

    free(track);
    fclose(fp);

    if (!lib.entry_count)
        return false;

    lib.loaded = true;
    lib.status.tracks_found = lib.entry_count;
    lib.status.complete = true;

    build_groups();

    RG_LOGI("Library loaded: %u tracks (%u KB pool)", (unsigned)lib.entry_count,
            (unsigned)(lib.pool_used / 1024));
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* Scanning                                                                                 */
/* -------------------------------------------------------------------------------------- */

/**
 * The scanner walks the tree breadth-first from an explicit work list rather than by
 * recursing, so its memory use is bounded by the number of folders rather than by depth,
 * and it can be interrupted cleanly at any point.
 */
typedef struct
{
    char *pool;         // NUL-separated absolute paths
    uint32_t used;
    uint32_t capacity;
    uint32_t *offsets;
    uint32_t count;
    uint32_t head;      // Next entry to pop
    uint32_t offset_capacity;
} dir_queue_t;

typedef struct
{
    FILE *out;
    uint32_t written;
    media_track_t *track;
    media_metadata_t *meta;
    dir_queue_t queue;
    int depth;          // Depth of the directory currently being scanned

    /* The new index is built entirely inside the context and only published at the end.
     * That leaves lib.entries/lib.pool valid for the UI for the whole scan, which is what
     * makes browsing (and playing) during an indexing pass safe. */
    media_entry_t *entries;
    scan_key_t *keys;
    char *pool;
    uint32_t capacity;
    uint32_t pool_used;
    uint32_t pool_capacity;
} scan_ctx_t;

/* Arrays retired by the previous publish. Freed at the start of the next scan, by which
 * point no UI pass can still be walking them. */
static media_entry_t *retired_entries;
static scan_key_t *retired_keys;
static char *retired_pool;

static void free_retired(void)
{
    free(retired_entries), retired_entries = NULL;
    free(retired_keys), retired_keys = NULL;
    free(retired_pool), retired_pool = NULL;
    free(retired_albums), retired_albums = NULL;
    free(retired_artists), retired_artists = NULL;
    free(retired_genres), retired_genres = NULL;
}

static bool ctx_reserve(scan_ctx_t *ctx, uint32_t needed)
{
    if (needed <= ctx->capacity)
        return true;
    if (needed > lib.max_tracks)
        return false;

    uint32_t capacity = ctx->capacity ? ctx->capacity * 2 : 256;
    while (capacity < needed)
        capacity *= 2;
    if (capacity > lib.max_tracks)
        capacity = lib.max_tracks;

    media_entry_t *entries = realloc(ctx->entries, (size_t)capacity * sizeof(media_entry_t));
    if (!entries)
        return false;
    ctx->entries = entries;

    scan_key_t *keys = realloc(ctx->keys, (size_t)capacity * sizeof(scan_key_t));
    if (!keys)
        return false;
    ctx->keys = keys;

    ctx->capacity = capacity;
    return true;
}

static uint32_t ctx_pool_add(scan_ctx_t *ctx, const char *title, const char *artist)
{
    size_t tlen = strlen(title);
    size_t alen = strlen(artist);
    size_t need = tlen + alen + 2;

    if (ctx->pool_used + need > ctx->pool_capacity)
    {
        uint32_t capacity = ctx->pool_capacity ? ctx->pool_capacity * 2 : 16384;
        while (capacity < ctx->pool_used + need)
            capacity *= 2;
        if (capacity > lib.max_tracks * 96u)
            capacity = lib.max_tracks * 96u;
        if (ctx->pool_used + need > capacity)
            return UINT32_MAX;

        char *pool = realloc(ctx->pool, capacity);
        if (!pool)
            return UINT32_MAX;
        ctx->pool = pool;
        ctx->pool_capacity = capacity;
    }

    uint32_t offset = ctx->pool_used;
    memcpy(ctx->pool + offset, title, tlen + 1);
    memcpy(ctx->pool + offset + tlen + 1, artist, alen + 1);
    ctx->pool_used += (uint32_t)need;
    return offset;
}

static void queue_free(dir_queue_t *q)
{
    free(q->pool);
    free(q->offsets);
    memset(q, 0, sizeof(*q));
}

static bool queue_push(dir_queue_t *q, const char *path, uint8_t depth)
{
    size_t len = strlen(path);
    if (len > MEDIA_MAX_PATH)
        return false;

    if (q->count >= q->offset_capacity)
    {
        uint32_t capacity = q->offset_capacity ? q->offset_capacity * 2 : 64;
        // A media tree with more than this many folders is pathological; stop growing.
        if (capacity > 4096)
            return false;
        uint32_t *offsets = realloc(q->offsets, (size_t)capacity * sizeof(uint32_t));
        if (!offsets)
            return false;
        q->offsets = offsets;
        q->offset_capacity = capacity;
    }

    if (q->used + len + 2 > q->capacity)
    {
        uint32_t capacity = q->capacity ? q->capacity * 2 : 4096;
        while (capacity < q->used + len + 2)
            capacity *= 2;
        if (capacity > 512 * 1024)
            return false;
        char *pool = realloc(q->pool, capacity);
        if (!pool)
            return false;
        q->pool = pool;
        q->capacity = capacity;
    }

    q->offsets[q->count++] = q->used;
    q->pool[q->used] = (char)depth;      // Depth is stored inline ahead of the path
    memcpy(q->pool + q->used + 1, path, len + 1);
    q->used += (uint32_t)len + 2;
    return true;
}

static const char *queue_pop(dir_queue_t *q, int *depth_out)
{
    if (q->head >= q->count)
        return NULL;
    const char *slot = q->pool + q->offsets[q->head++];
    if (depth_out)
        *depth_out = (uint8_t)slot[0];
    return slot + 1;
}

/** Index of a previously scanned record for this exact, unchanged file, or -1. */
static int previous_index_of(uint32_t path_hash, uint32_t mtime, uint32_t size)
{
    // The live index is still the previous one during a scan, so it is the reference.
    if (!lib.keys)
        return -1;
    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        if (lib.keys[i].hash != path_hash)
            continue;
        // A changed size or timestamp means the tags may have changed too.
        if (lib.keys[i].mtime != mtime || lib.keys[i].size != size)
            return -1;
        return (int)i;
    }
    return -1;
}

/** Throttle the scanner when the audio pipeline needs the SD card more than we do. */
static void scan_throttle(void);

static bool scan_write_track(scan_ctx_t *ctx, media_track_t *track)
{
    if (ctx->written >= lib.max_tracks)
        return false;

    track->id = ctx->written + 1;

    if (fwrite(track, 1, sizeof(*track), ctx->out) != sizeof(*track))
    {
        RG_LOGE("Failed to write the index");
        return false;
    }

    if (!ctx_reserve(ctx, ctx->written + 1))
        return false;

    uint32_t offset = ctx_pool_add(ctx, track->title, track->artist);
    if (offset == UINT32_MAX)
        return false;

    media_entry_t *entry = &ctx->entries[ctx->written];
    entry_from_track(entry, track, track->id);
    entry->name_offset = offset;
    stats_apply(entry, track->path_hash);

    ctx->keys[ctx->written] = (scan_key_t){track->path_hash, track->mtime, track->file_size};
    ctx->written++;
    return true;
}

static int scan_entry_cb(const rg_scandir_t *file, void *arg)
{
    scan_ctx_t *ctx = arg;

    if (lib.scan_stop)
        return RG_SCANDIR_STOP;

    if (media_path_is_hidden(file->basename))
        return RG_SCANDIR_CONTINUE;

    if (file->is_dir)
    {
        if (ctx->depth + 1 < MEDIA_MAX_SCAN_DEPTH)
            queue_push(&ctx->queue, file->path, (uint8_t)(ctx->depth + 1));
        return RG_SCANDIR_CONTINUE;
    }

    if (!file->is_file)
        return RG_SCANDIR_CONTINUE;

    lib.status.files_seen++;

    if (!media_path_is_audio(file->basename))
        return RG_SCANDIR_CONTINUE;

    if (strlen(file->path) > MEDIA_MAX_PATH)
    {
        RG_LOGW("Skipping over-long path");
        return RG_SCANDIR_CONTINUE;
    }

    uint32_t path_hash = rg_hash(file->path, strlen(file->path));
    media_track_t *track = ctx->track;

    if (!lib.scan_full)
    {
        int previous = previous_index_of(path_hash, (uint32_t)file->mtime, (uint32_t)file->size);
        if (previous >= 0)
        {
            // Unchanged file: carry the old record over instead of re-reading its tags. This
            // is what makes a quick rescan take seconds rather than minutes.
            memset(track, 0, sizeof(*track));
            if (media_library_get_track(lib.entries[previous].id, track))
            {
                track->path_hash = path_hash;
                if (!scan_write_track(ctx, track))
                    return RG_SCANDIR_STOP;
                lib.status.tracks_found = ctx->written;
                return RG_SCANDIR_CONTINUE;
            }
        }
    }

    memset(track, 0, sizeof(*track));
    strcpy(track->path, file->path);
    track->path_hash = path_hash;
    track->dir_hash = rg_hash(file->dirname, strlen(file->dirname));
    track->file_size = (uint32_t)file->size;
    track->mtime = (uint32_t)file->mtime;

    if (media_metadata_read(file->path, ctx->meta))
    {
        media_metadata_apply(track, ctx->meta);
    }
    else
    {
        // Unreadable file: still index it so the user can see (and delete) it.
        media_path_stem(track->title, sizeof(track->title), track->path);
        media_utf8_copy(track->album, sizeof(track->album), rg_basename(rg_dirname(track->path)));
        track->codec = (uint8_t)media_codec_from_path(track->path);
    }

    if (!scan_write_track(ctx, track))
        return RG_SCANDIR_STOP;

    lib.status.tracks_found = ctx->written;

    // Reading tags means opening a file; do it politely with respect to playback.
    scan_throttle();

    return RG_SCANDIR_CONTINUE;
}

static void scan_task(void *arg)
{
    (void)arg;

    scan_ctx_t *ctx = calloc(1, sizeof(scan_ctx_t));
    media_track_t *track = calloc(1, sizeof(media_track_t));
    media_metadata_t *meta = calloc(1, sizeof(media_metadata_t));
    char temp_path[LIB_PATH_MAX + 16];
    bool ok = false;

    // Anything retired by the previous scan is definitely unreferenced by now.
    free_retired();

    lib.status.scanning = true;
    lib.status.complete = false;
    lib.status.tracks_found = 0;
    lib.status.files_seen = 0;
    lib.status.folders_seen = 0;
    lib.status.current[0] = 0;

    if (!ctx || !track || !meta || !ensure_cache_dir())
    {
        RG_LOGE("Cannot start the scan");
        goto finish;
    }

    ctx->track = track;
    ctx->meta = meta;

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", lib.index_path);
    ctx->out = fopen(temp_path, "wb");
    if (!ctx->out)
    {
        RG_LOGE("Cannot create '%s'", temp_path);
        goto finish;
    }

    library_header_t header = {
        .magic = LIBRARY_MAGIC,
        .version = MEDIA_LIBRARY_DB_VERSION,
        .record_size = sizeof(media_track_t),
        .track_count = 0,
        .scan_time = (uint32_t)time(NULL),
        .root_hash = rg_hash(lib.root, strlen(lib.root)),
    };

    if (fwrite(&header, 1, sizeof(header), ctx->out) != sizeof(header))
    {
        RG_LOGE("Cannot write the index header");
        goto finish;
    }

    queue_push(&ctx->queue, lib.root, 0);

    for (const char *dir; (dir = queue_pop(&ctx->queue, &ctx->depth)) != NULL;)
    {
        if (lib.scan_stop)
            break;

        lib.status.folders_seen++;

        // Show the path relative to the media root so the progress line stays readable
        size_t root_len = strlen(lib.root);
        const char *relative = dir;
        if (strncmp(dir, lib.root, root_len) == 0 && dir[root_len] == '/')
            relative = dir + root_len + 1;
        else if (strcmp(dir, lib.root) == 0)
            relative = "/";
        media_utf8_copy(lib.status.current, sizeof(lib.status.current), relative);

        rg_storage_scandir(dir, scan_entry_cb, ctx,
                           RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_STAT | RG_SCANDIR_SORT);

        scan_throttle();
    }

    header.track_count = ctx->written;
    ok = ctx->written > 0 && !lib.scan_stop;

    if (ok && (fseek(ctx->out, 0, SEEK_SET) != 0 ||
               fwrite(&header, 1, sizeof(header), ctx->out) != sizeof(header)))
        ok = false;

    fclose(ctx->out);
    ctx->out = NULL;

    if (ok)
    {
        // The handle must be released before the file it points at is replaced.
        if (lib.lock && rg_mutex_take(lib.lock, 5000))
        {
            index_close();
            rg_mutex_give(lib.lock);
        }
        else
        {
            index_close();
        }

        // Atomic publish: the old index stays valid until the rename succeeds.
        remove(lib.index_path);
        if (rename(temp_path, lib.index_path) != 0)
        {
            RG_LOGE("Failed to publish the new index");
            ok = false;
        }
    }

    if (!ok)
        remove(temp_path);

finish:
    if (ok && ctx)
    {
        /* Publish. Dropping the count first means a reader that is mid-iteration stops
         * before any pointer changes underneath it, and the old arrays are retired rather
         * than freed so a reader already inside one cannot fault. */
        uint32_t new_count = ctx->written;

        lib.entry_count = 0;
        __sync_synchronize();

        retired_entries = lib.entries;
        retired_keys = lib.keys;
        retired_pool = lib.pool;

        lib.entries = ctx->entries;
        lib.keys = ctx->keys;
        lib.pool = ctx->pool;
        lib.entry_capacity = ctx->capacity;
        lib.pool_used = ctx->pool_used;
        lib.pool_capacity = ctx->pool_capacity;
        __sync_synchronize();
        lib.entry_count = new_count;

        ctx->entries = NULL;
        ctx->keys = NULL;
        ctx->pool = NULL;

        lib.loaded = lib.entry_count > 0;
        build_groups();
        RG_LOGI("Scan complete: %u tracks in %u folders", (unsigned)lib.entry_count,
                (unsigned)lib.status.folders_seen);
    }
    else
    {
        // A failed or cancelled scan simply leaves the previous index in place.
        RG_LOGW("Scan did not complete, keeping the previous index");
    }

    if (ctx)
    {
        if (ctx->out)
            fclose(ctx->out);
        queue_free(&ctx->queue);
        free(ctx->entries);
        free(ctx->keys);
        free(ctx->pool);
    }
    free(ctx);
    free(track);
    free(meta);

#ifdef ESP_PLATFORM
    RG_LOGI("Scan task exiting, stack headroom was %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif

    lib.status.scanning = false;
    lib.status.complete = true;
    lib.scan_task = NULL;
    lib.scan_running = false;
}

/* -------------------------------------------------------------------------------------- */

// Set by the player so the scanner can back off. A function pointer avoids a circular
// dependency between the library and the playback controller.
static int (*resource_pressure_cb)(void);

void media_library_set_pressure_source(int (*cb)(void))
{
    resource_pressure_cb = cb;
}

static void scan_throttle(void)
{
    // 0 = idle, 1 = playing comfortably, 2 = the audio buffer is low, back right off.
    int pressure = resource_pressure_cb ? resource_pressure_cb() : 0;

    if (pressure >= 2)
        rg_task_delay(250);
    else if (pressure == 1)
        rg_task_delay(25);
    else
        rg_task_delay(1); // Still yield: the UI shares this core
}

/* -------------------------------------------------------------------------------------- */
/* Public API                                                                               */
/* -------------------------------------------------------------------------------------- */

void media_library_init(const char *root)
{
    if (!lib.lock)
        lib.lock = rg_mutex_create();

    const media_profile_t *profile = media_profile();
    lib.max_tracks = (uint32_t)profile->library_cache_tracks;
    if (lib.max_tracks > MEDIA_MAX_LIBRARY_TRACKS)
        lib.max_tracks = MEDIA_MAX_LIBRARY_TRACKS;

    media_library_set_root(root && *root ? root : RG_STORAGE_ROOT "/media");
    stats_load();
}

void media_library_deinit(void)
{
    media_library_scan_stop();
    media_library_commit();
    index_close();
    clear_index();
    free_retired();
    free(lib.stats), lib.stats = NULL;
    lib.stats_count = lib.stats_capacity = 0;
    free(lib.query_result), lib.query_result = NULL;
    lib.query_capacity = 0;
}

const char *media_library_root(void)
{
    return lib.root;
}

void media_library_set_root(const char *root)
{
    if (!root || !*root)
        return;
    index_close();
    media_utf8_copy(lib.root, sizeof(lib.root), root);
    // Strip a trailing slash so path comparisons stay simple
    size_t len = strlen(lib.root);
    while (len > 1 && lib.root[len - 1] == '/')
        lib.root[--len] = 0;
    rebuild_paths();
}

bool media_library_scan_start(bool full)
{
    if (lib.scan_running)
        return false;
    if (!rg_storage_exists(lib.root))
    {
        RG_LOGW("Media root '%s' does not exist", lib.root);
        return false;
    }

    lib.scan_stop = false;
    lib.scan_full = full;
    lib.scan_running = true;

    if (full)
        remove(lib.index_path);

    // Priority 1 is the same as the launcher's main task, so scanning shares time with the
    // UI instead of starving it.
    // The scanner walks the same tag parsers as the artwork worker, so it needs the same
    // headroom; the directory work list itself lives on the heap.
    lib.scan_task = rg_task_create("media_scan", &scan_task, NULL, 8 * 1024, RG_TASK_PRIORITY_1,
                                   RG_TASK_AFFINITY_MAIN);
    if (!lib.scan_task)
    {
        lib.scan_running = false;
        return false;
    }

    RG_LOGI("Started a %s scan of '%s'", full ? "full" : "quick", lib.root);
    return true;
}

void media_library_scan_stop(void)
{
    if (!lib.scan_running)
        return;
    lib.scan_stop = true;
    for (int i = 0; i < 600 && lib.scan_running; ++i)
        rg_task_delay(10);
    if (lib.scan_running)
        RG_LOGE("Scan task did not stop");
}

media_scan_status_t media_library_scan_status(void)
{
    return lib.status;
}

bool media_library_ready(void)
{
    return lib.loaded && lib.entry_count > 0;
}

uint32_t media_library_track_count(void)
{
    return lib.entry_count;
}

const media_entry_t *media_library_entry(uint32_t index)
{
    if (index >= lib.entry_count || !lib.entries)
        return NULL;
    return &lib.entries[index];
}

uint32_t media_library_find_by_path(const char *path)
{
    if (!path || !lib.entries)
        return 0;

    uint32_t hash = rg_hash(path, strlen(path));
    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        if (lib.keys && lib.keys[i].hash == hash)
            return lib.entries[i].id;
    }
    return 0;
}

/* ---- Views ------------------------------------------------------------------------- */

static bool query_reserve(uint32_t needed)
{
    if (needed <= lib.query_capacity)
        return true;
    uint32_t capacity = lib.query_capacity ? lib.query_capacity * 2 : 128;
    while (capacity < needed)
        capacity *= 2;
    uint32_t *result = realloc(lib.query_result, (size_t)capacity * sizeof(uint32_t));
    if (!result)
        return false;
    lib.query_result = result;
    lib.query_capacity = capacity;
    return true;
}

static int compare_album_order(const void *a, const void *b)
{
    const media_entry_t *ea = &lib.entries[*(const uint32_t *)a];
    const media_entry_t *eb = &lib.entries[*(const uint32_t *)b];

    // Track number metadata takes priority when sorting an album
    if (ea->disc_number != eb->disc_number)
        return (int)ea->disc_number - (int)eb->disc_number;
    if (ea->track_number != eb->track_number)
        return (int)ea->track_number - (int)eb->track_number;
    return media_strnatcasecmp(media_library_entry_title(ea), media_library_entry_title(eb));
}

static int compare_title(const void *a, const void *b)
{
    const media_entry_t *ea = &lib.entries[*(const uint32_t *)a];
    const media_entry_t *eb = &lib.entries[*(const uint32_t *)b];
    return media_strnatcasecmp(media_library_entry_title(ea), media_library_entry_title(eb));
}

static int compare_recent(const void *a, const void *b)
{
    const media_entry_t *ea = &lib.entries[*(const uint32_t *)a];
    const media_entry_t *eb = &lib.entries[*(const uint32_t *)b];
    stats_record_t *sa = stats_find(lib.keys[ea - lib.entries].hash, false);
    stats_record_t *sb = stats_find(lib.keys[eb - lib.entries].hash, false);
    uint32_t ta = sa ? sa->last_played : 0;
    uint32_t tb = sb ? sb->last_played : 0;
    if (ta != tb)
        return ta > tb ? -1 : 1;
    return 0;
}

static int compare_most_played(const void *a, const void *b)
{
    const media_entry_t *ea = &lib.entries[*(const uint32_t *)a];
    const media_entry_t *eb = &lib.entries[*(const uint32_t *)b];
    stats_record_t *sa = stats_find(lib.keys[ea - lib.entries].hash, false);
    stats_record_t *sb = stats_find(lib.keys[eb - lib.entries].hash, false);
    uint32_t ca = sa ? sa->play_count : 0;
    uint32_t cb = sb ? sb->play_count : 0;
    if (ca != cb)
        return ca > cb ? -1 : 1;
    return 0;
}

uint32_t media_library_query(media_view_t view, uint32_t filter_hash, const uint32_t **out)
{
    if (out)
        *out = NULL;
    if (!lib.entries || !lib.entry_count)
        return 0;

    if (!query_reserve(lib.entry_count))
        return 0;

    uint32_t count = 0;

    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        const media_entry_t *entry = &lib.entries[i];
        bool include = false;

        switch (view)
        {
        case MEDIA_VIEW_ALBUMS:
            include = filter_hash && entry->album_hash == filter_hash;
            break;
        case MEDIA_VIEW_ARTISTS:
        case MEDIA_VIEW_GENRES:
            // Artist and genre membership needs the full record; the group lists already
            // carry the counts, so resolve lazily here.
            include = false;
            break;
        case MEDIA_VIEW_ALL_TRACKS:
            include = true;
            break;
        case MEDIA_VIEW_FAVORITES:
            include = (entry->flags & MEDIA_ENTRY_FAVORITE) != 0;
            break;
        case MEDIA_VIEW_RECENT:
        case MEDIA_VIEW_MOST_PLAYED:
        {
            stats_record_t *record = lib.keys ? stats_find(lib.keys[i].hash, false) : NULL;
            include = record && (view == MEDIA_VIEW_RECENT ? record->last_played : record->play_count);
            break;
        }
        default:
            break;
        }

        if (include)
            lib.query_result[count++] = i;
    }

    // Artists and genres need one pass over the full records; do it only when asked.
    if ((view == MEDIA_VIEW_ARTISTS || view == MEDIA_VIEW_GENRES) && filter_hash)
    {
        FILE *fp = fopen(lib.index_path, "rb");
        media_track_t *track = fp ? malloc(sizeof(media_track_t)) : NULL;
        if (fp && track)
        {
            fseek(fp, (long)sizeof(library_header_t), SEEK_SET);
            for (uint32_t i = 0; i < lib.entry_count; ++i)
            {
                if (fread(track, 1, sizeof(*track), fp) != sizeof(*track))
                    break;
                track->album_artist[sizeof(track->album_artist) - 1] = 0;
                track->artist[sizeof(track->artist) - 1] = 0;
                track->genre[sizeof(track->genre) - 1] = 0;

                const char *value;
                if (view == MEDIA_VIEW_ARTISTS)
                    value = track->album_artist[0] ? track->album_artist
                            : (track->artist[0] ? track->artist : "Unknown Artist");
                else
                    value = track->genre;

                if (value[0] && rg_hash(value, strlen(value)) == filter_hash)
                    lib.query_result[count++] = i;
            }
        }
        free(track);
        if (fp)
            fclose(fp);
    }

    if (count > 1)
    {
        if (view == MEDIA_VIEW_ALBUMS)
            qsort(lib.query_result, count, sizeof(uint32_t), compare_album_order);
        else if (view == MEDIA_VIEW_RECENT)
            qsort(lib.query_result, count, sizeof(uint32_t), compare_recent);
        else if (view == MEDIA_VIEW_MOST_PLAYED)
            qsort(lib.query_result, count, sizeof(uint32_t), compare_most_played);
        else
            qsort(lib.query_result, count, sizeof(uint32_t), compare_title);
    }

    if (out)
        *out = lib.query_result;
    return count;
}

uint32_t media_library_group_count(media_view_t view)
{
    switch (view)
    {
    case MEDIA_VIEW_ALBUMS:  return lib.album_count;
    case MEDIA_VIEW_ARTISTS: return lib.artist_count;
    case MEDIA_VIEW_GENRES:  return lib.genre_count;
    default:                 return 0;
    }
}

const media_group_t *media_library_group(media_view_t view, uint32_t index)
{
    const media_group_t *groups = NULL;
    uint32_t count = 0;

    switch (view)
    {
    case MEDIA_VIEW_ALBUMS:  groups = lib.albums, count = lib.album_count; break;
    case MEDIA_VIEW_ARTISTS: groups = lib.artists, count = lib.artist_count; break;
    case MEDIA_VIEW_GENRES:  groups = lib.genres, count = lib.genre_count; break;
    default: break;
    }

    return index < count ? &groups[index] : NULL;
}

/* ---- User data ---------------------------------------------------------------------- */

static uint32_t hash_of_id(uint32_t id)
{
    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        if (lib.entries[i].id == id)
            return lib.keys ? lib.keys[i].hash : 0;
    }
    return 0;
}

static media_entry_t *entry_of_id(uint32_t id)
{
    for (uint32_t i = 0; i < lib.entry_count; ++i)
    {
        if (lib.entries[i].id == id)
            return &lib.entries[i];
    }
    return NULL;
}

static void mark_dirty(void)
{
    lib.stats_dirty = true;
    if (!lib.stats_dirty_since)
        lib.stats_dirty_since = rg_system_timer();
}

bool media_library_is_favorite(uint32_t id)
{
    media_entry_t *entry = entry_of_id(id);
    return entry && (entry->flags & MEDIA_ENTRY_FAVORITE);
}

void media_library_set_favorite(uint32_t id, bool favorite)
{
    uint32_t hash = hash_of_id(id);
    if (!hash)
        return;

    stats_record_t *record = stats_find(hash, true);
    if (!record)
        return;

    record->favorite = favorite ? 1 : 0;

    media_entry_t *entry = entry_of_id(id);
    if (entry)
    {
        if (favorite)
            entry->flags |= MEDIA_ENTRY_FAVORITE;
        else
            entry->flags &= (uint8_t)~MEDIA_ENTRY_FAVORITE;
    }

    // Favourites are an explicit user action, so persist them immediately.
    lib.stats_dirty = true;
    stats_save();
    lib.stats_dirty_since = 0;
}

void media_library_note_played(uint32_t id)
{
    uint32_t hash = hash_of_id(id);
    if (!hash)
        return;
    stats_record_t *record = stats_find(hash, true);
    if (!record)
        return;
    if (record->play_count < 0xFFFF)
        record->play_count++;
    record->last_played = (uint32_t)time(NULL);
    mark_dirty();
}

void media_library_note_skipped(uint32_t id)
{
    uint32_t hash = hash_of_id(id);
    if (!hash)
        return;
    stats_record_t *record = stats_find(hash, true);
    if (record && record->skip_count < 0xFFFF)
    {
        record->skip_count++;
        mark_dirty();
    }
}

void media_library_note_position(uint32_t id, uint32_t position_ms, uint32_t duration_ms)
{
    uint32_t hash = hash_of_id(id);
    if (!hash)
        return;

    stats_record_t *record = stats_find(hash, true);
    if (!record)
        return;

    // Only long files (podcasts, audiobooks, live sets) remember where they were left, and
    // only when the listener is neither at the very start nor essentially finished.
    bool worth_remembering = duration_ms > 15u * 60u * 1000u && position_ms > 30u * 1000u &&
                             position_ms + 30u * 1000u < duration_ms;

    record->last_position_ms = worth_remembering ? position_ms : 0;
    mark_dirty();
}

uint32_t media_library_get_position(uint32_t id)
{
    uint32_t hash = hash_of_id(id);
    if (!hash)
        return 0;
    stats_record_t *record = stats_find(hash, false);
    return record ? record->last_position_ms : 0;
}

void media_library_commit(void)
{
    if (!lib.stats_dirty)
        return;

    // Debounce: flash and FAT writes are expensive, and playback updates these constantly.
    if (lib.stats_dirty_since && (rg_system_timer() - lib.stats_dirty_since) < 30 * 1000000LL)
        return;

    stats_save();
    lib.stats_dirty_since = 0;
}
