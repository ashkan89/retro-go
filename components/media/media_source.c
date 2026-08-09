#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "media_net.h"
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

// Network reads come back in whatever size the socket has ready, so there is no equivalent
// alignment win; a smaller floor just keeps the ring topped up more smoothly.
#define NET_MIN_CHUNK 1024
#define NET_TIMEOUT_MS 6000
#define NET_MAX_RECONNECTS 4
// Shoutcast metadata blocks are a length byte times 16, so 255 * 16 is the hard maximum.
#define ICY_MAX_META 4080

struct media_source_s
{
    bool is_url;

    /* Local file backend */
    FILE *fp;

    /* Network backend */
    rg_http_req_t *req;
    char url[MEDIA_MAX_PATH + 1];
    bool accept_ranges;
    bool live;              // No content length: a continuous broadcast rather than a file
    int reconnects;
    uint32_t icy_metaint;   // 0 when the server sends no inline metadata
    uint32_t icy_remaining; // Audio bytes still to come before the next metadata block
    char icy_title[160];
    char icy_station[96];
    char content_type[64];
    volatile bool icy_updated;

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
    volatile bool eof;      // Source fully read into the ring
    volatile bool stopped;
    media_source_stats_t stats;
    uint64_t latency_total_us;
};

/* -------------------------------------------------------------------------------------- */
/* Network helpers                                                                          */
/* -------------------------------------------------------------------------------------- */

#ifdef RG_ENABLE_NETWORKING

static void on_response_header(const char *name, const char *value, void *arg)
{
    media_source_t *src = arg;

    if (strcasecmp(name, "Content-Type") == 0)
        media_utf8_copy(src->content_type, sizeof(src->content_type), value);
    else if (strcasecmp(name, "Accept-Ranges") == 0)
        src->accept_ranges = strcasestr(value, "bytes") != NULL;
    else if (strcasecmp(name, "icy-metaint") == 0)
        src->icy_metaint = (uint32_t)media_clampi(atoi(value), 0, 1024 * 1024);
    else if (strcasecmp(name, "icy-name") == 0)
        media_utf8_copy(src->icy_station, sizeof(src->icy_station), value);
    else if (strcasecmp(name, "icy-description") == 0 && !src->icy_station[0])
        media_utf8_copy(src->icy_station, sizeof(src->icy_station), value);
}

/** Open (or reopen) the HTTP request, optionally resuming at `offset`. */
static bool net_open(media_source_t *src, uint64_t offset)
{
    char range[48];

    if (src->req)
    {
        rg_network_http_close(src->req);
        src->req = NULL;
    }

    if (!media_net_available())
    {
        RG_LOGE("Network is not available");
        return false;
    }

    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.timeout_ms = NET_TIMEOUT_MS;
    cfg.on_header = &on_response_header;
    cfg.on_header_arg = src;

    rg_http_header_t headers[3];
    int n = 0;

    // Ask for inline titles. Servers that do not speak Icecast simply ignore it.
    headers[n++] = (rg_http_header_t){"Icy-MetaData", "1"};

    if (offset > 0)
    {
        snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)offset);
        headers[n++] = (rg_http_header_t){"Range", range};
    }

    headers[n] = (rg_http_header_t){NULL, NULL};
    cfg.headers = headers;

    // These are re-learned from the response each time.
    src->accept_ranges = false;
    src->icy_metaint = 0;
    src->content_type[0] = 0;

    src->req = rg_network_http_open(src->url, &cfg);
    if (!src->req)
        return false;

    if (src->req->status_code < 200 || src->req->status_code >= 300)
    {
        RG_LOGE("HTTP %d for '%s'", src->req->status_code, src->url);
        rg_network_http_close(src->req);
        src->req = NULL;
        return false;
    }

    // A 206 means the Range was honoured; a 200 in response to a Range means it was not and
    // the server is starting over, which would silently corrupt the seek.
    if (offset > 0 && src->req->status_code != 206)
    {
        RG_LOGW("Server ignored the range request");
        rg_network_http_close(src->req);
        src->req = NULL;
        return false;
    }

    if (src->req->content_length > 0)
        src->size = offset + (uint64_t)src->req->content_length;
    else
        src->size = 0;

    src->live = src->size == 0;
    src->icy_remaining = src->icy_metaint;
    src->file_pos = offset;

    return true;
}

/** Read exactly `len` bytes, looping over short reads. Returns bytes read. */
static size_t net_read_exact(media_source_t *src, uint8_t *buffer, size_t len)
{
    size_t total = 0;
    while (total < len && !src->stop)
    {
        int got = rg_network_http_read(src->req, buffer + total, len - total);
        if (got <= 0)
            break;
        total += (size_t)got;
    }
    return total;
}

/** Parse a Shoutcast metadata block: StreamTitle='Artist - Title'; */
static void icy_parse(media_source_t *src, const char *block)
{
    const char *key = strcasestr(block, "StreamTitle=");
    if (!key)
        return;

    const char *value = key + 12;
    char quote = 0;
    if (*value == '\'' || *value == '"')
        quote = *value++;

    char title[sizeof(src->icy_title)];
    size_t i = 0;

    while (*value && i < sizeof(title) - 1)
    {
        if (quote && *value == quote && (value[1] == ';' || value[1] == 0))
            break;
        if (!quote && *value == ';')
            break;
        title[i++] = *value++;
    }
    title[i] = 0;

    media_str_trim(title);
    media_utf8_sanitize(title, sizeof(title));

    // Stations repeat the same title between songs; only publish real changes.
    if (title[0] && strcmp(title, src->icy_title) != 0)
    {
        media_utf8_copy(src->icy_title, sizeof(src->icy_title), title);
        src->icy_updated = true;
        RG_LOGI("Stream title: %s", src->icy_title);
    }
}

/** Consume one inline metadata block. Returns false if the stream ended mid-block. */
static bool icy_consume(media_source_t *src)
{
    uint8_t length_byte = 0;
    if (net_read_exact(src, &length_byte, 1) != 1)
        return false;

    size_t length = (size_t)length_byte * 16;
    if (length == 0)
        return true;

    // A single length byte cannot ask for more than this, but the bound is explicit so the
    // stack buffer below can never be overrun regardless of what the server sends.
    if (length > ICY_MAX_META)
        return false;

    char block[ICY_MAX_META + 1];
    if (net_read_exact(src, (uint8_t *)block, length) != length)
        return false;

    block[length] = 0;
    icy_parse(src, block);
    return true;
}

/**
 * Pull up to `want` bytes of audio, transparently stepping over Icecast metadata blocks.
 * Returns bytes of audio placed in `buffer`.
 */
static size_t net_read_audio(media_source_t *src, uint8_t *buffer, size_t want)
{
    if (src->icy_metaint == 0)
    {
        int got = rg_network_http_read(src->req, buffer, want);
        return got > 0 ? (size_t)got : 0;
    }

    size_t total = 0;

    while (total < want && !src->stop)
    {
        if (src->icy_remaining == 0)
        {
            if (!icy_consume(src))
                break;
            src->icy_remaining = src->icy_metaint;
            continue;
        }

        size_t chunk = want - total;
        if (chunk > src->icy_remaining)
            chunk = src->icy_remaining;

        int got = rg_network_http_read(src->req, buffer + total, chunk);
        if (got <= 0)
            break;

        total += (size_t)got;
        src->icy_remaining -= (uint32_t)got;

        // One socket read is enough per pass unless we landed exactly on a metadata
        // boundary; the caller comes straight back for more either way.
        if (src->icy_remaining > 0)
            break;
    }

    return total;
}

#endif /* RG_ENABLE_NETWORKING */

/* -------------------------------------------------------------------------------------- */
/* IO task                                                                                  */
/* -------------------------------------------------------------------------------------- */

static void io_task(void *arg)
{
    media_source_t *src = arg;

    while (!src->stop)
    {
        bool idle = true;

        if (rg_mutex_take(src->lock, 50))
        {
            if (!src->stop && !src->eof)
            {
                size_t space = media_ring_free_space(src->ring);
                size_t chunk = space < IO_CHUNK ? space : IO_CHUNK;
                size_t floor = src->is_url ? NET_MIN_CHUNK : IO_MIN_CHUNK;

                // Only read a partial chunk when we're close to the end, otherwise wait for
                // the consumer to free a worthwhile amount of space.
                uint64_t remaining = src->size > src->file_pos ? src->size - src->file_pos : 0;
                bool worth_reading = chunk >= floor || (chunk > 0 && src->size && remaining <= chunk);

                if (worth_reading)
                {
                    int64_t start = rg_system_timer();
                    size_t got = 0;
                    bool failed;

                    if (!src->is_url)
                    {
                        got = fread(src->scratch, 1, chunk, src->fp);
                        failed = got < chunk;
                    }
#ifdef RG_ENABLE_NETWORKING
                    else if (src->req)
                    {
                        got = net_read_audio(src, src->scratch, chunk);
                        failed = got == 0;
                    }
#endif
                    else
                    {
                        failed = true;
                    }

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

                    if (failed)
                    {
#ifdef RG_ENABLE_NETWORKING
                        // A dropped connection is normal on a long-running stream (server
                        // restarts, roaming Wi-Fi). Reconnect a bounded number of times, and
                        // resume mid-file when the server supports ranges.
                        if (src->is_url && src->reconnects < NET_MAX_RECONNECTS && !src->stop)
                        {
                            src->reconnects++;
                            RG_LOGW("Stream interrupted, reconnecting (%d/%d)", src->reconnects,
                                    NET_MAX_RECONNECTS);

                            uint64_t resume = (!src->live && src->accept_ranges) ? src->file_pos : 0;
                            bool reopened = net_open(src, resume);

                            rg_mutex_give(src->lock);
                            if (!reopened)
                                rg_task_delay(400 * src->reconnects);
                            continue;
                        }
#endif
                        src->eof = true;
                    }
                }
            }
            rg_mutex_give(src->lock);
        }

        if (idle)
            rg_task_delay(src->eof ? 20 : 4);
    }

#ifdef ESP_PLATFORM
    RG_LOGD("IO task exiting, stack headroom was %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif

    src->stopped = true;
    src->running = false;
}

/* -------------------------------------------------------------------------------------- */
/* Public API                                                                               */
/* -------------------------------------------------------------------------------------- */

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
    src->is_url = media_net_is_url(path);

    if (src->is_url)
    {
#ifdef RG_ENABLE_NETWORKING
        strcpy(src->url, path);
        if (!net_open(src, 0))
        {
            free(src);
            return NULL;
        }
#else
        RG_LOGE("This build has no networking");
        free(src);
        return NULL;
#endif
    }
    else
    {
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
    }

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
    // FatFs read paths can nest a few levels; 4 KB leaves room for that plus logging. The
    // network path additionally puts a 4 KB Icecast metadata block on the stack.
    src->task = rg_task_create("media_io", &io_task, src, src->is_url ? 8 * 1024 : 4 * 1024,
                               RG_TASK_PRIORITY_4, RG_TASK_AFFINITY_IO);
    if (!src->task)
    {
        src->running = false;
        media_source_close(src);
        return NULL;
    }

    if (src->is_url)
        RG_LOGI("Streaming '%s' (%s%s)", src->url, src->live ? "live" : "file",
                src->icy_metaint ? ", icy" : "");
    else
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

    // The IO task can be inside a single read; give it room to finish before we free
    // anything. A stalled socket read is bounded by NET_TIMEOUT_MS.
    for (int i = 0; i < 800 && source->running; ++i)
        rg_task_delay(10);

    if (source->running)
        RG_LOGE("IO task did not stop, leaking source to stay safe");
    else
    {
        if (source->fp)
            fclose(source->fp);
#ifdef RG_ENABLE_NETWORKING
        if (source->req)
            rg_network_http_close(source->req);
#endif
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
            // Nothing available: either the source is done or it is being slow.
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

size_t media_source_peek(media_source_t *source, void *buffer, size_t len, int timeout_ms)
{
    if (!source || !buffer || !len)
        return 0;

    int64_t deadline = rg_system_timer() + (int64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;

    while (media_ring_used(source->ring) < len)
    {
        if (source->eof || media_ring_aborted(source->ring))
            break;
        if (rg_system_timer() >= deadline)
            break;
        rg_task_delay(5);
    }

    return media_ring_peek(source->ring, buffer, len);
}

uint64_t media_source_tell(const media_source_t *source)
{
    return source ? source->consumer_pos : 0;
}

uint64_t media_source_size(const media_source_t *source)
{
    return source ? source->size : 0;
}

bool media_source_seekable(const media_source_t *source)
{
    if (!source)
        return false;
    if (!source->is_url)
        return true;
    // A range-capable HTTP file can be seeked by reopening; a live broadcast cannot.
    return source->accept_ranges && source->size > 0;
}

bool media_source_is_live(const media_source_t *source)
{
    return source && source->is_url && source->live;
}

bool media_source_seek(media_source_t *source, uint64_t offset)
{
    if (!source)
        return false;
    if (source->size && offset > source->size)
        offset = source->size;

    bool ok = false;

    if (rg_mutex_take(source->lock, 8000))
    {
        if (!source->is_url)
        {
            if (source->fp && fseek(source->fp, (long)offset, SEEK_SET) == 0)
                ok = true;
            else
                RG_LOGE("fseek to %u failed", (unsigned)offset);
        }
#ifdef RG_ENABLE_NETWORKING
        else if (media_source_seekable(source))
        {
            // Seeking an HTTP file means a fresh request with a Range header. The reconnect
            // budget resets because this is a deliberate action, not a failure.
            source->reconnects = 0;
            ok = net_open(source, offset);
        }
#endif

        if (ok)
        {
            media_ring_reset(source->ring);
            source->file_pos = offset;
            source->consumer_pos = offset;
            source->eof = source->size && offset >= source->size;
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

bool media_source_take_stream_title(media_source_t *source, char *out, size_t out_size)
{
    if (!source || !out || !out_size || !source->icy_updated)
        return false;

    // Cleared before the copy so a title arriving during it is not lost, only re-reported.
    source->icy_updated = false;
    media_utf8_copy(out, out_size, source->icy_title);
    return out[0] != 0;
}

const char *media_source_station_name(const media_source_t *source)
{
    return (source && source->icy_station[0]) ? source->icy_station : NULL;
}

const char *media_source_content_type(const media_source_t *source)
{
    return (source && source->content_type[0]) ? source->content_type : NULL;
}
