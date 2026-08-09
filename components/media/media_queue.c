#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_playlist.h"
#include "media_queue.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

typedef struct
{
    uint32_t offset;    // Into the path pool
    uint32_t id;
} queue_item_t;

static struct
{
    queue_item_t *items;
    int count;
    int capacity;

    char *pool;
    size_t pool_used;
    size_t pool_capacity;

    int index;          // Currently selected entry, -1 when nothing is selected

    bool shuffle;
    media_repeat_t repeat;

    uint16_t *order;    // Shuffle permutation: order[position] = queue index
    int order_count;
    int order_position;
    uint32_t rand_state;
    rg_mutex_t *lock;
} q;

/* The UI task mutates the queue while the decode task reads it to decide what to play next,
 * so both sides take this lock. It is only ever held for a memcpy or a short scan. */
#define QUEUE_LOCK()   do { if (q.lock) rg_mutex_take(q.lock, 1000); } while (0)
#define QUEUE_UNLOCK() do { if (q.lock) rg_mutex_give(q.lock); } while (0)

void media_queue_lock(void)
{
    QUEUE_LOCK();
}

void media_queue_unlock(void)
{
    QUEUE_UNLOCK();
}

void media_queue_init(void)
{
    q.index = -1;
    q.rand_state = (uint32_t)rg_system_timer() | 1u;
    if (!q.lock)
        q.lock = rg_mutex_create();
}

void media_queue_deinit(void)
{
    media_queue_clear();
    if (q.lock)
        rg_mutex_free(q.lock), q.lock = NULL;
    free(q.items), q.items = NULL;
    free(q.pool), q.pool = NULL;
    free(q.order), q.order = NULL;
    q.capacity = 0;
    q.pool_capacity = 0;
}

void media_queue_clear(void)
{
    QUEUE_LOCK();
    q.count = 0;
    q.pool_used = 0;
    q.index = -1;
    q.order_count = 0;
    q.order_position = 0;
    QUEUE_UNLOCK();
}

int media_queue_count(void)
{
    return q.count;
}

static bool pool_append(const char *path, uint32_t *offset_out)
{
    size_t len = strlen(path);
    if (!len || len > MEDIA_MAX_PATH)
        return false;

    if (q.pool_used + len + 1 > q.pool_capacity)
    {
        size_t capacity = q.pool_capacity ? q.pool_capacity * 2 : 8192;
        while (capacity < q.pool_used + len + 1)
            capacity *= 2;
        if (capacity > (size_t)MEDIA_MAX_QUEUE_ENTRIES * 96)
            return false;
        char *pool = realloc(q.pool, capacity);
        if (!pool)
            return false;
        q.pool = pool;
        q.pool_capacity = capacity;
    }

    *offset_out = (uint32_t)q.pool_used;
    memcpy(q.pool + q.pool_used, path, len + 1);
    q.pool_used += len + 1;
    return true;
}

static bool items_reserve(int needed)
{
    if (needed <= q.capacity)
        return true;
    if (needed > MEDIA_MAX_QUEUE_ENTRIES)
        return false;

    int capacity = q.capacity ? q.capacity * 2 : 64;
    while (capacity < needed)
        capacity *= 2;
    if (capacity > MEDIA_MAX_QUEUE_ENTRIES)
        capacity = MEDIA_MAX_QUEUE_ENTRIES;

    queue_item_t *items = realloc(q.items, (size_t)capacity * sizeof(queue_item_t));
    if (!items)
        return false;
    q.items = items;

    uint16_t *order = realloc(q.order, (size_t)capacity * sizeof(uint16_t));
    if (!order)
        return false;
    q.order = order;

    q.capacity = capacity;
    return true;
}

bool media_queue_add(const char *path, uint32_t id)
{
    if (!path || !*path)
        return false;

    QUEUE_LOCK();

    bool ok = items_reserve(q.count + 1);
    uint32_t offset = 0;

    if (ok)
        ok = pool_append(path, &offset);

    if (ok)
    {
        q.items[q.count].offset = offset;
        q.items[q.count].id = id;
        q.count++;
        // The permutation is now stale; it is rebuilt lazily on the next shuffle step.
        q.order_count = 0;
    }

    QUEUE_UNLOCK();
    return ok;
}

bool media_queue_add_next(const char *path, uint32_t id)
{
    if (!media_queue_add(path, id))
        return false;

    int target = q.index >= 0 ? q.index + 1 : 0;
    if (target >= q.count - 1)
        return true; // Already in the right place

    return media_queue_move(q.count - 1, target);
}

bool media_queue_remove(int index)
{
    QUEUE_LOCK();

    if (index < 0 || index >= q.count)
    {
        QUEUE_UNLOCK();
        return false;
    }

    // The pool is append-only; removing an item just drops its slot. The wasted bytes are
    // reclaimed when the queue is cleared, which keeps removal O(n) and allocation-free.
    memmove(&q.items[index], &q.items[index + 1], (size_t)(q.count - index - 1) * sizeof(queue_item_t));
    q.count--;

    if (q.index > index)
        q.index--;
    else if (q.index == index && q.index >= q.count)
        q.index = q.count ? q.count - 1 : -1;

    q.order_count = 0;
    QUEUE_UNLOCK();
    return true;
}

bool media_queue_move(int from, int to)
{
    QUEUE_LOCK();

    if (from < 0 || from >= q.count || to < 0 || to >= q.count || from == to)
    {
        QUEUE_UNLOCK();
        return false;
    }

    queue_item_t item = q.items[from];

    if (from < to)
        memmove(&q.items[from], &q.items[from + 1], (size_t)(to - from) * sizeof(queue_item_t));
    else
        memmove(&q.items[to + 1], &q.items[to], (size_t)(from - to) * sizeof(queue_item_t));

    q.items[to] = item;

    if (q.index == from)
        q.index = to;
    else if (from < q.index && q.index <= to)
        q.index--;
    else if (to <= q.index && q.index < from)
        q.index++;

    q.order_count = 0;
    QUEUE_UNLOCK();
    return true;
}

const char *media_queue_path(int index)
{
    if (index < 0 || index >= q.count || !q.pool)
        return NULL;
    uint32_t offset = q.items[index].offset;
    if (offset >= q.pool_used)
        return NULL;
    return q.pool + offset;
}

uint32_t media_queue_id(int index)
{
    if (index < 0 || index >= q.count)
        return 0;
    return q.items[index].id;
}

int media_queue_index(void)
{
    return q.index;
}

void media_queue_set_index(int index)
{
    if (index < -1 || index >= q.count)
        return;
    q.index = index;

    // Keep the shuffle cursor in step so "next" after a manual pick stays sensible.
    for (int i = 0; i < q.order_count; ++i)
    {
        if (q.order[i] == index)
        {
            q.order_position = i;
            break;
        }
    }
}

const char *media_queue_current(void)
{
    return media_queue_path(q.index);
}

void media_queue_set_shuffle(bool shuffle)
{
    if (q.shuffle == shuffle)
        return;
    q.shuffle = shuffle;
    if (shuffle)
        media_queue_reshuffle();
}

bool media_queue_get_shuffle(void)
{
    return q.shuffle;
}

void media_queue_set_repeat(media_repeat_t repeat)
{
    if (repeat >= 0 && repeat < MEDIA_REPEAT_COUNT)
        q.repeat = repeat;
}

media_repeat_t media_queue_get_repeat(void)
{
    return q.repeat;
}

void media_queue_reshuffle(void)
{
    if (!q.order || q.count <= 0)
    {
        q.order_count = 0;
        return;
    }

    for (int i = 0; i < q.count; ++i)
        q.order[i] = (uint16_t)i;

    // Fisher-Yates
    for (int i = q.count - 1; i > 0; --i)
    {
        int j = (int)(media_rand(&q.rand_state) % (uint32_t)(i + 1));
        uint16_t tmp = q.order[i];
        q.order[i] = q.order[j];
        q.order[j] = tmp;
    }

    q.order_count = q.count;
    q.order_position = 0;

    // Whatever is playing stays playing: swap it to the front rather than jumping tracks.
    if (q.index >= 0)
    {
        for (int i = 0; i < q.order_count; ++i)
        {
            if (q.order[i] == q.index)
            {
                uint16_t tmp = q.order[0];
                q.order[0] = q.order[i];
                q.order[i] = tmp;
                break;
            }
        }
    }
}

int media_queue_next_index(bool manual)
{
    if (q.count <= 0)
        return -1;
    if (q.index < 0)
        return 0;

    // Repeating a single track only applies when the track ended on its own.
    if (!manual && q.repeat == MEDIA_REPEAT_TRACK)
        return q.index;

    if (q.shuffle)
    {
        if (q.order_count != q.count)
            media_queue_reshuffle();

        if (q.order_position + 1 < q.order_count)
            return q.order[q.order_position + 1];

        // Bag exhausted. Repeat-all (or a manual press) starts a fresh permutation.
        if (q.repeat == MEDIA_REPEAT_ALL || q.repeat == MEDIA_REPEAT_FOLDER || manual)
            return -2; // Signals "reshuffle then take the first entry"
        return -1;
    }

    if (q.index + 1 < q.count)
        return q.index + 1;

    if (q.repeat == MEDIA_REPEAT_ALL || q.repeat == MEDIA_REPEAT_FOLDER || manual)
        return 0;

    return -1;
}

int media_queue_prev_index(void)
{
    if (q.count <= 0)
        return -1;
    if (q.index < 0)
        return 0;

    if (q.shuffle)
    {
        if (q.order_count != q.count)
            media_queue_reshuffle();
        if (q.order_position > 0)
            return q.order[q.order_position - 1];
        return q.index;
    }

    if (q.index > 0)
        return q.index - 1;

    // Wrapping backwards from the first track is only useful when repeating.
    return (q.repeat == MEDIA_REPEAT_ALL) ? q.count - 1 : 0;
}

int media_queue_advance(bool manual)
{
    int next = media_queue_next_index(manual);

    if (next == -2)
    {
        media_queue_reshuffle();
        if (q.order_count <= 0)
            return -1;
        q.order_position = 0;
        q.index = q.order[0];
        return q.index;
    }

    if (next < 0)
        return -1;

    if (q.shuffle && q.order_count == q.count && next != q.index)
        q.order_position++;

    q.index = next;
    return q.index;
}

int media_queue_retreat(void)
{
    int prev = media_queue_prev_index();
    if (prev < 0)
        return -1;

    if (q.shuffle && q.order_position > 0)
        q.order_position--;

    q.index = prev;
    return q.index;
}

/* -------------------------------------------------------------------------------------- */
/* Persistence                                                                              */
/* -------------------------------------------------------------------------------------- */

static void queue_file_path(char *out, size_t out_size, const char *root)
{
    snprintf(out, out_size, "%s/.retrogo-media/queue.m3u8", root ? root : RG_STORAGE_ROOT "/media");
}

bool media_queue_save(const char *root)
{
    char path[MEDIA_MAX_PATH + 64];
    queue_file_path(path, sizeof(path), root);

    if (q.count == 0)
    {
        remove(path);
        return true;
    }

    // The current index is stored as a comment so restoring lands on the right track.
    size_t capacity = 64;
    for (int i = 0; i < q.count; ++i)
    {
        const char *entry = media_queue_path(i);
        capacity += entry ? strlen(entry) + 2 : 0;
    }

    char *buffer = malloc(capacity);
    if (!buffer)
        return false;

    size_t used = (size_t)snprintf(buffer, capacity, "#EXTM3U\n#RGQUEUE:%d\n", q.index);
    for (int i = 0; i < q.count; ++i)
    {
        const char *entry = media_queue_path(i);
        if (!entry)
            continue;
        int n = snprintf(buffer + used, capacity - used, "%s\n", entry);
        if (n < 0 || (size_t)n >= capacity - used)
            break;
        used += (size_t)n;
    }

    bool ok = rg_storage_write_file(path, buffer, used, RG_FILE_ATOMIC_WRITE);
    free(buffer);
    return ok;
}

bool media_queue_restore(const char *root)
{
    char path[MEDIA_MAX_PATH + 64];
    queue_file_path(path, sizeof(path), root);

    media_playlist_t *pl = media_playlist_load(path, root);
    if (!pl)
        return false;

    media_queue_clear();
    for (int i = 0; i < pl->count; ++i)
        media_queue_add(media_playlist_entry(pl, i), 0);
    media_playlist_free(pl);

    // Recover the saved cursor from the #RGQUEUE comment.
    FILE *fp = fopen(path, "rb");
    if (fp)
    {
        char line[64];
        while (fgets(line, sizeof(line), fp))
        {
            if (strncmp(line, "#RGQUEUE:", 9) == 0)
            {
                int index = atoi(line + 9);
                if (index >= 0 && index < q.count)
                    q.index = index;
                break;
            }
            if (line[0] != '#')
                break;
        }
        fclose(fp);
    }

    RG_LOGI("Restored a queue of %d tracks (at %d)", q.count, q.index);
    return q.count > 0;
}

bool media_queue_export(const char *path)
{
    if (!path || q.count <= 0)
        return false;

    const char **paths = calloc((size_t)q.count, sizeof(char *));
    if (!paths)
        return false;

    int count = 0;
    for (int i = 0; i < q.count; ++i)
    {
        const char *entry = media_queue_path(i);
        if (entry)
            paths[count++] = entry;
    }

    bool ok = media_playlist_save(path, paths, count);
    free(paths);
    return ok;
}
