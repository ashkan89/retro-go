#include <rg_system.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "media_lyrics.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_LRC"

static bool lyrics_reserve(media_lyrics_t *ly, int needed)
{
    if (needed <= ly->capacity)
        return true;

    int capacity = ly->capacity ? ly->capacity * 2 : 64;
    while (capacity < needed)
        capacity *= 2;
    if (capacity > MEDIA_MAX_LYRICS_LINES)
        capacity = MEDIA_MAX_LYRICS_LINES;
    if (needed > capacity)
        return false;

    media_lyric_line_t *lines = realloc(ly->lines, (size_t)capacity * sizeof(*lines));
    if (!lines)
        return false;

    ly->lines = lines;
    ly->capacity = capacity;
    return true;
}

/** Append `text` to the pool and return its offset, or -1 when full. */
static int pool_add(media_lyrics_t *ly, size_t *pool_used, size_t *pool_capacity, const char *text)
{
    size_t len = strlen(text);
    if (len > MEDIA_MAX_LYRIC_TEXT - 1)
        len = media_utf8_clip(text, MEDIA_MAX_LYRIC_TEXT - 1);

    if (*pool_used + len + 1 > *pool_capacity)
    {
        size_t capacity = *pool_capacity ? *pool_capacity * 2 : 4096;
        while (capacity < *pool_used + len + 1)
            capacity *= 2;
        if (capacity > MEDIA_MAX_LYRICS_BYTES)
            return -1;
        char *pool = realloc(ly->pool, capacity);
        if (!pool)
            return -1;
        ly->pool = pool;
        *pool_capacity = capacity;
    }

    // The line index stores a uint16 offset, so the pool cannot exceed 64 KB.
    if (*pool_used > 0xFFFF - len - 1)
        return -1;

    int offset = (int)*pool_used;
    memcpy(ly->pool + offset, text, len);
    ly->pool[offset + len] = 0;
    *pool_used += len + 1;
    return offset;
}

/**
 * Try to read "[mm:ss.xx]" or "[mm:ss:xx]" at `p`. On success writes the time and returns a
 * pointer past the closing bracket, otherwise returns NULL.
 */
static const char *parse_timestamp(const char *p, uint32_t *time_ms)
{
    if (*p != '[')
        return NULL;

    const char *s = p + 1;
    if (!isdigit((uint8_t)*s))
        return NULL;

    char *end = NULL;
    long minutes = strtol(s, &end, 10);
    if (!end || *end != ':' || minutes < 0 || minutes > 9999)
        return NULL;

    s = end + 1;
    if (!isdigit((uint8_t)*s))
        return NULL;
    long seconds = strtol(s, &end, 10);
    if (!end || seconds < 0 || seconds > 59)
        return NULL;

    long fraction = 0;
    int digits = 0;
    if (*end == '.' || *end == ':')
    {
        s = end + 1;
        const char *frac_start = s;
        while (isdigit((uint8_t)*s))
            s++;
        digits = (int)(s - frac_start);
        if (digits > 0)
        {
            char buf[8] = {0};
            int n = digits < 3 ? digits : 3;
            memcpy(buf, frac_start, n);
            fraction = strtol(buf, NULL, 10);
            if (n == 1)
                fraction *= 100;
            else if (n == 2)
                fraction *= 10;
        }
        end = (char *)s;
    }

    if (*end != ']')
        return NULL;

    *time_ms = (uint32_t)(minutes * 60000 + seconds * 1000 + fraction);
    return end + 1;
}

static int compare_lines(const void *a, const void *b)
{
    const media_lyric_line_t *la = a, *lb = b;
    if (la->time_ms != lb->time_ms)
        return la->time_ms < lb->time_ms ? -1 : 1;
    // Equal timestamps keep their original relative order via the pool offset
    return (int)la->offset - (int)lb->offset;
}

media_lyrics_t *media_lyrics_parse(const char *text, size_t len)
{
    if (!text || !len)
        return NULL;
    if (len > MEDIA_MAX_LYRICS_BYTES)
        len = MEDIA_MAX_LYRICS_BYTES;

    media_lyrics_t *ly = calloc(1, sizeof(media_lyrics_t));
    if (!ly)
        return NULL;

    size_t pool_used = 0, pool_capacity = 0;
    size_t pos = 0;
    char line[MEDIA_MAX_LYRIC_TEXT * 2];
    int unsynced_count = 0;

    // Skip a UTF-8 BOM
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF)
        pos = 3;

    while (pos < len && ly->count < MEDIA_MAX_LYRICS_LINES)
    {
        size_t start = pos;
        while (pos < len && text[pos] != '\n' && text[pos] != '\r')
            pos++;

        size_t raw_len = pos - start;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r'))
            pos++;

        if (raw_len == 0)
            continue;
        if (raw_len > sizeof(line) - 1)
            raw_len = sizeof(line) - 1;

        memcpy(line, text + start, raw_len);
        line[raw_len] = 0;
        media_utf8_sanitize(line, sizeof(line));

        const char *p = line;
        uint32_t stamps[16];
        int stamp_count = 0;

        while (*p == ' ')
            p++;

        // Collect every leading timestamp: "[00:02.50][00:07.50]Repeated line"
        while (stamp_count < (int)RG_COUNT(stamps))
        {
            uint32_t t;
            const char *next = parse_timestamp(p, &t);
            if (!next)
                break;
            stamps[stamp_count++] = t;
            p = next;
        }

        if (stamp_count == 0)
        {
            // ID tag or plain text
            if (line[0] == '[')
            {
                const char *colon = strchr(line, ':');
                const char *close = strchr(line, ']');
                if (colon && close && colon < close)
                {
                    char key[16] = {0};
                    size_t key_len = (size_t)(colon - line - 1);
                    if (key_len < sizeof(key))
                    {
                        memcpy(key, line + 1, key_len);
                        char value[MEDIA_TAG_TITLE_LEN];
                        size_t value_len = (size_t)(close - colon - 1);
                        if (value_len >= sizeof(value))
                            value_len = sizeof(value) - 1;
                        memcpy(value, colon + 1, value_len);
                        value[value_len] = 0;
                        media_str_trim(value);

                        if (strcasecmp(key, "ti") == 0)
                            media_utf8_copy(ly->title, sizeof(ly->title), value);
                        else if (strcasecmp(key, "ar") == 0)
                            media_utf8_copy(ly->artist, sizeof(ly->artist), value);
                        else if (strcasecmp(key, "offset") == 0)
                        {
                            long v = strtol(value, NULL, 10);
                            if (v > -60000 && v < 60000)
                                ly->offset_ms = (int32_t)v;
                        }
                        // al/by/re/ve and anything else are recognised and deliberately dropped
                        continue;
                    }
                }
            }

            // Plain text line: keep it so unsynced lyrics still display
            char *body = media_str_trim(line);
            if (!*body)
                continue;
            if (!lyrics_reserve(ly, ly->count + 1))
                break;
            int offset = pool_add(ly, &pool_used, &pool_capacity, body);
            if (offset < 0)
                break;
            ly->lines[ly->count].time_ms = 0;
            ly->lines[ly->count].offset = (uint16_t)offset;
            ly->count++;
            unsynced_count++;
            continue;
        }

        char body[MEDIA_MAX_LYRIC_TEXT * 2];
        media_utf8_copy(body, sizeof(body), p);
        media_str_trim(body);

        if (!lyrics_reserve(ly, ly->count + stamp_count))
            break;

        int offset = pool_add(ly, &pool_used, &pool_capacity, body);
        if (offset < 0)
            break;

        for (int i = 0; i < stamp_count && ly->count < MEDIA_MAX_LYRICS_LINES; ++i)
        {
            ly->lines[ly->count].time_ms = stamps[i];
            ly->lines[ly->count].offset = (uint16_t)offset;
            ly->count++;
        }
        ly->synced = true;
    }

    if (ly->count == 0)
    {
        media_lyrics_free(ly);
        return NULL;
    }

    ly->pool_size = pool_used;

    if (ly->synced)
    {
        // Files in the wild are not always ordered, and repeated-line stamps never are.
        qsort(ly->lines, (size_t)ly->count, sizeof(*ly->lines), compare_lines);

        // Drop the leading zero-timestamp entries produced by stray plain-text lines mixed
        // into a synced file; they would otherwise all fight for position 0.
        if (unsynced_count && unsynced_count < ly->count)
        {
            int keep = 0;
            for (int i = 0; i < ly->count; ++i)
            {
                if (ly->lines[i].time_ms == 0 && i < unsynced_count)
                    continue;
                ly->lines[keep++] = ly->lines[i];
            }
            if (keep > 0)
                ly->count = keep;
        }
    }

    RG_LOGI("Parsed %d lyric lines (%s, offset %d ms)", ly->count, ly->synced ? "synced" : "plain",
            (int)ly->offset_ms);
    return ly;
}

media_lyrics_t *media_lyrics_load_sidecar(const char *track_path)
{
    if (!track_path)
        return NULL;

    static const char *extensions[] = {"lrc", "LRC", "txt"};

    for (size_t i = 0; i < RG_COUNT(extensions); ++i)
    {
        char path[MEDIA_MAX_PATH + 1];
        if (!media_path_swap_ext(path, sizeof(path), track_path, extensions[i]))
            continue;

        rg_stat_t info = rg_storage_stat(path);
        if (!info.exists || !info.is_file || info.size == 0 || info.size > MEDIA_MAX_LYRICS_BYTES)
            continue;

        void *data = NULL;
        size_t len = 0;
        if (!rg_storage_read_file(path, &data, &len, 0))
            continue;

        media_lyrics_t *lyrics = media_lyrics_parse(data, len);
        free(data);

        if (lyrics)
        {
            RG_LOGI("Loaded lyrics from '%s'", rg_basename(path));
            return lyrics;
        }
    }

    return NULL;
}

void media_lyrics_free(media_lyrics_t *lyrics)
{
    if (!lyrics)
        return;
    free(lyrics->lines);
    free(lyrics->pool);
    free(lyrics);
}

int media_lyrics_find(const media_lyrics_t *lyrics, uint32_t position_ms, int32_t user_offset_ms)
{
    if (!lyrics || lyrics->count <= 0)
        return -1;
    if (!lyrics->synced)
        return -1;

    int64_t target = (int64_t)position_ms - lyrics->offset_ms - user_offset_ms;
    if (target < 0)
        return -1;

    // Last line whose timestamp is <= target
    int lo = 0, hi = lyrics->count - 1, best = -1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        if ((int64_t)lyrics->lines[mid].time_ms <= target)
        {
            best = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return best;
}

const char *media_lyrics_text(const media_lyrics_t *lyrics, int index)
{
    if (!lyrics || index < 0 || index >= lyrics->count || !lyrics->pool)
        return "";
    uint16_t offset = lyrics->lines[index].offset;
    if (offset >= lyrics->pool_size)
        return "";
    return lyrics->pool + offset;
}

uint32_t media_lyrics_time(const media_lyrics_t *lyrics, int index)
{
    if (!lyrics || index < 0 || index >= lyrics->count)
        return 0;
    return lyrics->lines[index].time_ms;
}
