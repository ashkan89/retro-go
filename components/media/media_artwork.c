#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "media_artwork.h"
#include "media_metadata.h"
#include "media_net.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_ART"

#define MAX_CACHE_ENTRIES 24
#define REQUEST_SLOTS 4

typedef struct
{
    uint32_t key;           // path hash mixed with the requested dimension
    uint32_t path_hash;
    rg_image_t *image;
    media_palette_t palette;
    size_t bytes;
    uint32_t last_used;
    bool negative;          // Looked and found nothing; do not look again
} cache_entry_t;

typedef struct
{
    char path[MEDIA_MAX_PATH + 1];
    int max_dim;
    bool background;
    int width, height;
} request_t;

static struct
{
    cache_entry_t entries[MAX_CACHE_ENTRIES];
    int entry_count;
    size_t bytes_used;
    uint32_t clock;

    request_t requests[REQUEST_SLOTS];
    int request_count;

    rg_mutex_t *lock;
    rg_task_t *task;
    volatile bool running;
    volatile bool stop;

    rg_image_t *background;
    uint32_t background_key;
    int background_w, background_h;

    int (*pressure_cb)(void);
    void (*ready_cb)(void);

    volatile bool busy;     // A request is being serviced right now (outside the lock)

    /* Tags parsed on the way to finding the artwork, waiting to be collected. */
    char tags_path[MEDIA_MAX_PATH + 1];
    media_metadata_t tags;
    bool tags_ready;
} art;

static uint32_t make_key(const char *path, int dim)
{
    return rg_hash(path, strlen(path)) ^ ((uint32_t)dim * 2654435761u);
}

static size_t image_bytes(const rg_image_t *image)
{
    if (!image)
        return 0;
    return (size_t)image->height * (size_t)image->stride + sizeof(rg_image_t);
}

static cache_entry_t *cache_find(uint32_t key)
{
    for (int i = 0; i < art.entry_count; ++i)
    {
        if (art.entries[i].key == key)
            return &art.entries[i];
    }
    return NULL;
}

static void cache_drop(int index)
{
    if (index < 0 || index >= art.entry_count)
        return;

    cache_entry_t *entry = &art.entries[index];
    art.bytes_used -= entry->bytes;
    rg_surface_free(entry->image);
    memmove(&art.entries[index], &art.entries[index + 1],
            (size_t)(art.entry_count - index - 1) * sizeof(cache_entry_t));
    art.entry_count--;
}

/** Evict least-recently-used entries until the new one fits within both budgets. */
static void cache_make_room(size_t incoming)
{
    const media_profile_t *profile = media_profile();
    int max_entries = media_clampi(profile->artwork_cache_entries, 2, MAX_CACHE_ENTRIES);

    while (art.entry_count > 0 &&
           (art.entry_count >= max_entries || art.bytes_used + incoming > profile->artwork_cache_bytes))
    {
        int oldest = 0;
        for (int i = 1; i < art.entry_count; ++i)
        {
            if (art.entries[i].last_used < art.entries[oldest].last_used)
                oldest = i;
        }
        cache_drop(oldest);
    }
}

static cache_entry_t *cache_insert(uint32_t key, uint32_t path_hash, rg_image_t *image)
{
    size_t bytes = image_bytes(image);
    cache_make_room(bytes);

    if (art.entry_count >= MAX_CACHE_ENTRIES)
    {
        rg_surface_free(image);
        return NULL;
    }

    cache_entry_t *entry = &art.entries[art.entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->path_hash = path_hash;
    entry->image = image;
    entry->bytes = bytes;
    entry->last_used = ++art.clock;
    entry->negative = image == NULL;

    if (image)
        entry->palette = media_image_palette(image);

    art.bytes_used += bytes;
    return entry;
}

/* -------------------------------------------------------------------------------------- */
/* Worker                                                                                   */
/* -------------------------------------------------------------------------------------- */

/** Load a cover that lives beside the track, locally or on a server. */
static rg_image_t *load_cover_file(const char *art_path, int max_dim)
{
    if (!media_net_is_url(art_path))
        return media_image_load_file(art_path, max_dim);

    size_t len = 0;
    uint8_t *data = media_net_fetch_file(art_path, MEDIA_MAX_ARTWORK_BYTES, &len);
    if (!data)
        return NULL;

    rg_image_t *image = media_image_decode(data, len, max_dim);
    free(data);
    return image;
}

/** Decode the artwork for one track, returning a new surface or NULL. */
static rg_image_t *load_artwork(const char *path, int max_dim)
{
    // A radio station's inline metadata can name a cover image directly. There are no tags to
    // look for in a JPEG, so decode it and skip the parser entirely.
    if (media_path_is_image(path))
        return load_cover_file(path, max_dim);

    media_metadata_t *meta = calloc(1, sizeof(media_metadata_t));
    if (!meta)
        return NULL;

    rg_image_t *image = NULL;
    char art_path[MEDIA_MAX_PATH + 1];

    if (media_metadata_read(path, meta))
    {
        // Parsing the tags is the expensive part -- for a remote file it is a range request.
        // Publish them so the player can show a real title and album rather than a filename.
        if (rg_mutex_take(art.lock, 2000))
        {
            media_utf8_copy(art.tags_path, sizeof(art.tags_path), path);
            art.tags = *meta;
            art.tags_ready = true;
            rg_mutex_give(art.lock);
        }

        media_art_source_t source = media_metadata_find_artwork(path, meta, art_path, sizeof(art_path));

        if (source == MEDIA_ART_EMBEDDED)
        {
            size_t len = 0;
            uint8_t *data = media_metadata_read_artwork(path, meta, &len);
            if (data)
            {
                image = media_image_decode(data, len, max_dim);
                free(data);
            }
            // A broken embedded picture should still allow the folder cover to be used.
            if (!image)
                source = media_metadata_find_artwork(path, NULL, art_path, sizeof(art_path));
        }

        if (!image && art_path[0])
            image = load_cover_file(art_path, max_dim);
    }

    free(meta);
    return image;
}

static void worker_task(void *arg)
{
    (void)arg;
    art.running = true;

    while (!art.stop)
    {
        request_t request;
        bool have_request = false;

        if (rg_mutex_take(art.lock, 100))
        {
            if (art.request_count > 0)
            {
                request = art.requests[0];
                memmove(&art.requests[0], &art.requests[1],
                        (size_t)(art.request_count - 1) * sizeof(request_t));
                art.request_count--;
                have_request = true;
            }
            rg_mutex_give(art.lock);
        }

        if (!have_request)
        {
            art.busy = false;
            rg_task_delay(20);
            continue;
        }

        art.busy = true;

        // Decoding a JPEG is CPU and SD heavy. When the audio buffer is low it can wait: a
        // missing cover is invisible next to a dropout. The wait is bounded, though -- a live
        // stream can hold the buffer near its low mark indefinitely, and deferring for ever
        // meant remote tracks never got their tags or cover at all.
        int64_t defer_until = rg_system_timer() + 3 * 1000000LL;
        int pressure = art.pressure_cb ? art.pressure_cb() : 0;
        while (pressure >= 2 && !art.stop && rg_system_timer() < defer_until)
        {
            rg_task_delay(100);
            pressure = art.pressure_cb ? art.pressure_cb() : 0;
        }
        if (art.stop)
            break;

        if (request.background)
        {
            const rg_image_t *source = NULL;
            uint32_t key = make_key(request.path, media_profile()->artwork_max_dim);

            if (rg_mutex_take(art.lock, 2000))
            {
                cache_entry_t *entry = cache_find(key);
                source = entry ? entry->image : NULL;
                rg_image_t *background =
                    source ? media_image_make_background(source, request.width, request.height, 110)
                           : NULL;

                if (background)
                {
                    rg_surface_free(art.background);
                    art.background = background;
                    art.background_key = key;
                    art.background_w = request.width;
                    art.background_h = request.height;
                }
                rg_mutex_give(art.lock);

                if (background && art.ready_cb)
                    art.ready_cb();
            }
            art.busy = false;
            continue;
        }

        rg_image_t *image = load_artwork(request.path, request.max_dim);
        uint32_t key = make_key(request.path, request.max_dim);
        uint32_t path_hash = rg_hash(request.path, strlen(request.path));

        if (rg_mutex_take(art.lock, 5000))
        {
            if (!cache_find(key))
                cache_insert(key, path_hash, image);
            else
                rg_surface_free(image); // Raced with another request; keep the resident copy
            rg_mutex_give(art.lock);
        }
        else
        {
            rg_surface_free(image);
        }

        if (art.ready_cb)
            art.ready_cb();

        art.busy = false;
        rg_task_delay(1);
    }

    art.busy = false;

#ifdef ESP_PLATFORM
    RG_LOGI("Artwork worker exiting, stack headroom was %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif

    art.running = false;
}

/* -------------------------------------------------------------------------------------- */
/* Public API                                                                               */
/* -------------------------------------------------------------------------------------- */

void media_artwork_init(void)
{
    if (art.lock)
        return;

    art.lock = rg_mutex_create();
    if (!art.lock)
    {
        RG_LOGE("Failed to create the artwork lock");
        return;
    }

    art.stop = false;
    // Priority 1 keeps it level with the UI: artwork should never outrank drawing.
    // 5 KB was not enough: the tag parsers plus the path buffers in load_artwork() and
    // media_metadata_find_artwork() overflowed it. The large parser buffers now live on the
    // heap, and this has headroom for the JPEG decoder on top.
    art.task = rg_task_create("media_art", &worker_task, NULL, 8 * 1024, RG_TASK_PRIORITY_1,
                              RG_TASK_AFFINITY_MAIN);
    if (!art.task)
        RG_LOGE("Failed to start the artwork worker");
}

void media_artwork_deinit(void)
{
    if (!art.lock)
        return;

    art.stop = true;
    for (int i = 0; i < 400 && art.running; ++i)
        rg_task_delay(10);

    if (art.running)
    {
        RG_LOGE("Artwork worker did not stop; leaving the cache in place");
        return;
    }

    media_artwork_flush();
    rg_mutex_free(art.lock);
    art.lock = NULL;
    art.task = NULL;
}

void media_artwork_lock(void)
{
    if (art.lock)
        rg_mutex_take(art.lock, 1000);
}

void media_artwork_unlock(void)
{
    if (art.lock)
        rg_mutex_give(art.lock);
}

static bool queue_request(const char *path, int max_dim, bool background, int width, int height)
{
    if (art.request_count < 0)
        art.request_count = 0;

    if (art.request_count >= REQUEST_SLOTS)
    {
        // The queue is a work list, not a history: drop the oldest so the newest (which is
        // almost always what the user is looking at) is serviced first.
        memmove(&art.requests[0], &art.requests[1], (size_t)(REQUEST_SLOTS - 1) * sizeof(request_t));
        art.request_count = REQUEST_SLOTS - 1;
    }

    for (int i = 0; i < art.request_count; ++i)
    {
        if (art.requests[i].background == background && art.requests[i].max_dim == max_dim &&
            strcmp(art.requests[i].path, path) == 0)
            return true; // Already queued
    }

    request_t *request = &art.requests[art.request_count++];
    memset(request, 0, sizeof(*request));
    media_utf8_copy(request->path, sizeof(request->path), path);
    request->max_dim = max_dim;
    request->background = background;
    request->width = width;
    request->height = height;
    return true;
}

const rg_image_t *media_artwork_get(const char *track_path, int max_dim, bool request)
{
    if (!track_path || !*track_path || !art.lock)
        return NULL;

    uint32_t key = make_key(track_path, max_dim);
    cache_entry_t *entry = cache_find(key);

    if (entry)
    {
        entry->last_used = ++art.clock;
        return entry->image;
    }

    if (request && strlen(track_path) <= MEDIA_MAX_PATH)
        queue_request(track_path, max_dim, false, 0, 0);

    return NULL;
}

media_palette_t media_artwork_palette(const char *track_path)
{
    media_palette_t palette = {0};

    if (!track_path || !*track_path)
        return palette;

    uint32_t key = make_key(track_path, media_profile()->artwork_max_dim);
    cache_entry_t *entry = cache_find(key);

    if (entry && entry->image)
    {
        entry->last_used = ++art.clock;
        return entry->palette;
    }

    // No artwork (yet): a hash-derived palette keeps the UI coloured and stable per track
    // rather than flat grey.
    return media_image_palette_from_hash(rg_hash(track_path, strlen(track_path)));
}

const rg_image_t *media_artwork_background(const char *track_path, int width, int height)
{
    if (!track_path || !*track_path || !art.lock || width < 8 || height < 8)
        return NULL;

    uint32_t key = make_key(track_path, media_profile()->artwork_max_dim);

    if (art.background && art.background_key == key && art.background_w == width &&
        art.background_h == height)
        return art.background;

    // The source cover has to be resident before a background can be derived from it.
    if (cache_find(key))
        queue_request(track_path, media_profile()->artwork_max_dim, true, width, height);
    else
        media_artwork_get(track_path, media_profile()->artwork_max_dim, true);

    return NULL;
}

void media_artwork_request(const char *track_path, int max_dim)
{
    if (!track_path || !*track_path || !art.lock)
        return;
    if (strlen(track_path) > MEDIA_MAX_PATH)
        return;

    if (!rg_mutex_take(art.lock, 1000))
        return;

    if (!cache_find(make_key(track_path, max_dim)))
        queue_request(track_path, max_dim, false, 0, 0);

    rg_mutex_give(art.lock);
}

bool media_artwork_take_tags(const char *track_path, media_metadata_t *out)
{
    if (!track_path || !out || !art.lock)
        return false;

    bool taken = false;

    if (rg_mutex_take(art.lock, 100))
    {
        if (art.tags_ready && strcmp(art.tags_path, track_path) == 0)
        {
            *out = art.tags;
            art.tags_ready = false;
            taken = true;
        }
        rg_mutex_give(art.lock);
    }

    return taken;
}

bool media_artwork_busy(void)
{
    return art.request_count > 0;
}

void media_artwork_forget(const char *track_path)
{
    if (!track_path || !*track_path || !art.lock)
        return;

    uint32_t path_hash = rg_hash(track_path, strlen(track_path));

    if (!rg_mutex_take(art.lock, 1000))
        return;

    // Every size variant of the same path, so a failed load cannot leave a negative entry
    // behind that blocks the retry.
    for (int i = art.entry_count - 1; i >= 0; --i)
    {
        if (art.entries[i].path_hash == path_hash)
            cache_drop(i);
    }

    rg_mutex_give(art.lock);
}

void media_artwork_cancel(void)
{
    if (!art.lock)
        return;

    if (rg_mutex_take(art.lock, 1000))
    {
        art.request_count = 0;
        rg_mutex_give(art.lock);
    }

    // A request already in flight owns no lock while it decodes, so dropping the queue is not
    // enough: wait for it to finish before the caller starts freeing what it may be using.
    for (int i = 0; i < 300 && art.busy; ++i)
        rg_task_delay(10);

    if (art.busy)
        RG_LOGW("Artwork worker is still busy after 3 s");
}

/** Flush with `art.lock` already held. */
static void flush_locked(void)
{
    art.tags_ready = false;
    art.tags_path[0] = 0;

    while (art.entry_count > 0)
        cache_drop(art.entry_count - 1);

    rg_surface_free(art.background);
    art.background = NULL;
    art.background_key = 0;
    art.request_count = 0;
    art.bytes_used = 0;
}

void media_artwork_flush(void)
{
    if (!art.lock)
        return;

    // The worker is still running when the player is left playing in the background, and it
    // holds this lock while it inserts into the cache. Flushing without it freed surfaces the
    // worker was about to publish and corrupted the entry count.
    if (!rg_mutex_take(art.lock, 2000))
    {
        RG_LOGE("Could not take the artwork lock to flush; keeping the cache");
        return;
    }

    flush_locked();
    rg_mutex_give(art.lock);
}

void media_artwork_set_pressure_source(int (*cb)(void))
{
    art.pressure_cb = cb;
}

void media_artwork_set_ready_callback(void (*cb)(void))
{
    art.ready_cb = cb;
}

size_t media_artwork_bytes_used(void)
{
    return art.bytes_used;
}
