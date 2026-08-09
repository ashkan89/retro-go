#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_ring.h"
#include "media_source.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_IO"

// Large aligned reads keep the SD card in multi-block mode. 16 KB measured best on the
// reference boards; smaller reads roughly double the per-byte cost.
#define IO_CHUNK 16384
// Don't bother issuing a read for a sliver of free space, it wastes a full SD transaction.
#define IO_MIN_CHUNK 4096

struct media_source_s
{
    FILE *fp;
    char path[MEDIA_MAX_PATH + 1];
    uint64_t size;
    uint64_t file_pos;      // Next byte the IO task will read
    uint64_t consumer_pos;  // Next byte the decoder will see
    media_ring_t *ring;
    rg_mutex_t *lock;
    rg_task_t *task;
    uint8_t *scratch;
    volatile bool running;
    volatile bool stop;
    volatile bool eof;      // File fully read into the ring
    volatile bool stopped;
    media_source_stats_t stats;
    uint64_t latency_total_us;
};

static void io_task(void *arg)
{
    media_source_t *src = arg;

    while (!src->stop)
    {
        bool idle = true;

        if (rg_mutex_take(src->lock, 50))
        {
            if (!src->stop && src->fp && !src->eof)
            {
                size_t space = media_ring_free_space(src->ring);
                size_t chunk = space < IO_CHUNK ? space : IO_CHUNK;

                // Only read a partial chunk when we're close to the end of the file, otherwise
                // wait for the consumer to free a worthwhile amount of space.
                uint64_t remaining = src->size > src->file_pos ? src->size - src->file_pos : 0;
                if (chunk >= IO_MIN_CHUNK || (chunk > 0 && remaining <= chunk))
                {
                    int64_t start = rg_system_timer();
                    size_t got = fread(src->scratch, 1, chunk, src->fp);
                    uint32_t latency = (uint32_t)(rg_system_timer() - start);

                    src->stats.reads++;
                    src->latency_total_us += latency;
                    src->stats.avg_latency_us = (uint32_t)(src->latency_total_us / src->stats.reads);
                    if (latency > src->stats.max_latency_us)
                        src->stats.max_latency_us = latency;

                    if (got > 0)
                    {
                        // Space was reserved above while holding the lock, so this cannot block.
                        media_ring_write(src->ring, src->scratch, got, 0);
                        src->file_pos += got;
                        idle = false;
                    }

                    if (got < chunk)
                        src->eof = true;
                }
            }
            rg_mutex_give(src->lock);
        }

        if (idle)
            rg_task_delay(src->eof ? 20 : 4);
    }

    src->stopped = true;
    src->running = false;
}

media_source_t *media_source_open(const char *path, size_t buffer_bytes)
{
    RG_ASSERT_ARG(path);

    if (strlen(path) > MEDIA_MAX_PATH)
    {
        RG_LOGE("Path too long");
        return NULL;
    }

    media_source_t *src = calloc(1, sizeof(media_source_t));
    if (!src)
        return NULL;

    strcpy(src->path, path);

    src->fp = fopen(path, "rb");
    if (!src->fp)
    {
        RG_LOGE("Failed to open '%s'", path);
        free(src);
        return NULL;
    }

    if (fseek(src->fp, 0, SEEK_END) == 0)
    {
        long end = ftell(src->fp);
        src->size = end > 0 ? (uint64_t)end : 0;
    }
    fseek(src->fp, 0, SEEK_SET);

    if (buffer_bytes < 16 * 1024)
        buffer_bytes = 16 * 1024;

    src->ring = media_ring_create("src", buffer_bytes, true);
    src->scratch = rg_alloc(IO_CHUNK, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    src->lock = rg_mutex_create();

    if (!src->ring || !src->scratch || !src->lock)
    {
        RG_LOGE("Out of memory opening '%s'", path);
        media_source_close(src);
        return NULL;
    }

    src->running = true;
    src->task = rg_task_create("media_io", &io_task, src, 3 * 1024, RG_TASK_PRIORITY_4,
                               RG_TASK_AFFINITY_IO);
    if (!src->task)
    {
        src->running = false;
        media_source_close(src);
        return NULL;
    }

    RG_LOGI("Opened '%s' (%u KB)", rg_basename(path), (unsigned)(src->size / 1024));
    return src;
}

void media_source_close(media_source_t *source)
{
    if (!source)
        return;

    source->stop = true;
    if (source->ring)
        media_ring_abort(source->ring);

    // The IO task can be inside a single fread; give it room to finish before we free anything.
    for (int i = 0; i < 200 && source->running; ++i)
        rg_task_delay(10);

    if (source->running)
        RG_LOGE("IO task did not stop, leaking source to stay safe");
    else
    {
        if (source->fp)
            fclose(source->fp);
        media_ring_free(source->ring);
        free(source->scratch);
        if (source->lock)
            rg_mutex_free(source->lock);
        free(source);
    }
}

size_t media_source_read(media_source_t *source, void *buffer, size_t len, int timeout_ms)
{
    if (!source || !buffer || !len)
        return 0;

    size_t total = 0;
    while (total < len)
    {
        size_t got = media_ring_read(source->ring, (uint8_t *)buffer + total, len - total,
                                     timeout_ms < 0 ? 20 : timeout_ms);
        if (got == 0)
        {
            // Nothing available: either the file is done or the card is being slow.
            if (source->eof && media_ring_used(source->ring) == 0)
                break;
            if (media_ring_aborted(source->ring))
                break;
            if (timeout_ms >= 0)
                break;
            source->stats.stalls++;
            continue;
        }
        total += got;
    }

    source->consumer_pos += total;
    return total;
}

size_t media_source_skip(media_source_t *source, size_t len)
{
    if (!source || !len)
        return 0;

    uint8_t dump[512];
    size_t total = 0;
    while (total < len)
    {
        size_t want = len - total;
        if (want > sizeof(dump))
            want = sizeof(dump);
        size_t got = media_source_read(source, dump, want, 2000);
        if (!got)
            break;
        total += got;
    }
    return total;
}

uint64_t media_source_tell(const media_source_t *source)
{
    return source ? source->consumer_pos : 0;
}

uint64_t media_source_size(const media_source_t *source)
{
    return source ? source->size : 0;
}

bool media_source_seek(media_source_t *source, uint64_t offset)
{
    if (!source || !source->fp)
        return false;
    if (source->size && offset > source->size)
        offset = source->size;

    bool ok = false;
    if (rg_mutex_take(source->lock, 3000))
    {
        if (fseek(source->fp, (long)offset, SEEK_SET) == 0)
        {
            media_ring_reset(source->ring);
            source->file_pos = offset;
            source->consumer_pos = offset;
            source->eof = source->size && offset >= source->size;
            ok = true;
        }
        else
        {
            RG_LOGE("fseek to %u failed", (unsigned)offset);
        }
        rg_mutex_give(source->lock);
    }
    else
    {
        RG_LOGE("Timed out waiting for the IO lock");
    }

    return ok;
}

bool media_source_eof(const media_source_t *source)
{
    return !source || (source->eof && media_ring_used(source->ring) == 0);
}

int media_source_fill_percent(const media_source_t *source)
{
    if (!source)
        return 0;
    if (source->eof)
        return 100; // Nothing more is coming, so we are by definition not starved
    return media_ring_fill_percent(source->ring);
}

void media_source_abort(media_source_t *source)
{
    if (source)
        media_ring_abort(source->ring);
}

media_source_stats_t media_source_get_stats(const media_source_t *source)
{
    return source ? source->stats : (media_source_stats_t){0};
}

const char *media_source_path(const media_source_t *source)
{
    return source ? source->path : "";
}
