#include <rg_system.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

/* -------------------------------------------------------------------------------------- */
/* UTF-8                                                                                    */
/* -------------------------------------------------------------------------------------- */

// Length in bytes of the sequence introduced by `c`, or 0 if `c` is not a valid lead byte.
static inline int utf8_seq_len(uint8_t c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0;
}

size_t media_utf8_clip(const char *str, size_t max_bytes)
{
    if (!str)
        return 0;

    size_t pos = 0;
    while (str[pos])
    {
        int len = utf8_seq_len((uint8_t)str[pos]);
        if (len < 1)
            len = 1; // Broken byte: treat as a single unit so we still make progress
        if (pos + (size_t)len > max_bytes)
            break;
        // A truncated trailing sequence must not be counted as complete
        for (int i = 1; i < len; ++i)
        {
            if (((uint8_t)str[pos + i] & 0xC0) != 0x80)
            {
                len = 1;
                break;
            }
        }
        if (pos + (size_t)len > max_bytes)
            break;
        pos += len;
    }
    return pos;
}

size_t media_utf8_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return 0;
    if (!src)
    {
        dst[0] = 0;
        return 0;
    }

    size_t len = media_utf8_clip(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
    return len;
}

size_t media_utf8_sanitize(char *str, size_t size)
{
    if (!str || size == 0)
        return 0;

    // Walk in place. A replacement is 3 bytes (EF BF BD) and a bad byte is 1, so we can only
    // grow; when there is no room we simply drop the offending byte instead.
    char tmp[512];
    size_t out = 0;
    size_t in = 0;
    size_t limit = strnlen(str, size);
    size_t cap = sizeof(tmp) - 1;

    while (in < limit && out < cap)
    {
        uint8_t c = (uint8_t)str[in];
        int len = utf8_seq_len(c);
        bool ok = len > 0 && (in + (size_t)len) <= limit;

        for (int i = 1; ok && i < len; ++i)
            ok = ((uint8_t)str[in + i] & 0xC0) == 0x80;

        if (!ok || c == 0)
        {
            if (out + 3 <= cap)
            {
                tmp[out++] = (char)0xEF;
                tmp[out++] = (char)0xBF;
                tmp[out++] = (char)0xBD;
            }
            in += 1;
            continue;
        }

        if (out + (size_t)len > cap)
            break;
        memcpy(tmp + out, str + in, len);
        out += len;
        in += len;
    }

    tmp[out] = 0;
    return media_utf8_copy(str, size, tmp);
}

size_t media_latin1_to_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    if (!dst || dst_size == 0)
        return 0;

    size_t out = 0;
    for (size_t i = 0; i < src_len && src[i]; ++i)
    {
        uint8_t c = src[i];
        if (c < 0x80)
        {
            if (out + 1 >= dst_size)
                break;
            dst[out++] = (char)c;
        }
        else
        {
            if (out + 2 >= dst_size)
                break;
            dst[out++] = (char)(0xC0 | (c >> 6));
            dst[out++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[out] = 0;
    return out;
}

size_t media_utf16_to_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t src_len, bool be)
{
    if (!dst || dst_size == 0)
        return 0;

    size_t pos = 0;
    if (src_len >= 2)
    {
        if (src[0] == 0xFF && src[1] == 0xFE)
            be = false, pos = 2;
        else if (src[0] == 0xFE && src[1] == 0xFF)
            be = true, pos = 2;
    }

    size_t out = 0;
    while (pos + 1 < src_len)
    {
        uint32_t cp = be ? ((uint32_t)src[pos] << 8 | src[pos + 1])
                         : ((uint32_t)src[pos + 1] << 8 | src[pos]);
        pos += 2;

        if (cp == 0)
            break;

        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < src_len)
        {
            uint32_t lo = be ? ((uint32_t)src[pos] << 8 | src[pos + 1])
                             : ((uint32_t)src[pos + 1] << 8 | src[pos]);
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                pos += 2;
            }
            else
            {
                cp = 0xFFFD;
            }
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            cp = 0xFFFD; // Unpaired low surrogate
        }

        char enc[4];
        size_t n = rg_utf8_encode(enc, (int)cp);
        if (n == 0 || out + n >= dst_size)
            break;
        memcpy(dst + out, enc, n);
        out += n;
    }

    dst[out] = 0;
    return out;
}

char *media_str_trim(char *str)
{
    if (!str)
        return NULL;

    char *start = str;
    while (*start && (uint8_t)*start <= ' ')
        start++;

    size_t len = strlen(start);
    while (len && (uint8_t)start[len - 1] <= ' ')
        len--;

    if (start != str)
        memmove(str, start, len);
    str[len] = 0;
    return str;
}

int media_strnatcasecmp(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";

    while (*a && *b)
    {
        if (isdigit((uint8_t)*a) && isdigit((uint8_t)*b))
        {
            // Compare full runs of digits numerically, ignoring leading zeroes
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;

            const char *da = a, *db = b;
            while (isdigit((uint8_t)*a))
                a++;
            while (isdigit((uint8_t)*b))
                b++;

            ptrdiff_t la = a - da, lb = b - db;
            if (la != lb)
                return la < lb ? -1 : 1;
            for (ptrdiff_t i = 0; i < la; ++i)
            {
                if (da[i] != db[i])
                    return da[i] < db[i] ? -1 : 1;
            }
            continue;
        }

        int ca = tolower((uint8_t)*a);
        int cb = tolower((uint8_t)*b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        a++, b++;
    }

    if (*a == *b)
        return 0;
    return *a ? 1 : -1;
}

bool media_str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return needle && !*needle;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p)
    {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((uint8_t)p[i]) == tolower((uint8_t)needle[i]))
            i++;
        if (i == nlen)
            return true;
    }
    return false;
}

/* -------------------------------------------------------------------------------------- */
/* Paths                                                                                    */
/* -------------------------------------------------------------------------------------- */

typedef struct
{
    const char *ext;
    media_codec_t codec;
} media_ext_map_t;

// The single authority on which extensions the player recognises.
static const media_ext_map_t audio_extensions[] = {
    {"mp3",  MEDIA_CODEC_TYPE_MP3},
    {"wav",  MEDIA_CODEC_TYPE_WAV},
    {"wave", MEDIA_CODEC_TYPE_WAV},
    {"flac", MEDIA_CODEC_TYPE_FLAC},
    {"m4a",  MEDIA_CODEC_TYPE_AAC},
    {"aac",  MEDIA_CODEC_TYPE_AAC},
    {"mp4",  MEDIA_CODEC_TYPE_AAC},
    {"ogg",  MEDIA_CODEC_TYPE_OGG},
    {"oga",  MEDIA_CODEC_TYPE_OGG},
    {"opus", MEDIA_CODEC_TYPE_OPUS},
};

static bool ext_equals(const char *ext, const char *want)
{
    if (!ext)
        return false;
    while (*ext && *want)
    {
        if (tolower((uint8_t)*ext) != *want)
            return false;
        ext++, want++;
    }
    return !*ext && !*want;
}

media_codec_t media_codec_from_path(const char *path)
{
    const char *ext = rg_extension(path);
    if (!ext)
        return MEDIA_CODEC_NONE;

    // "…/hot108?aw_0_req.gdpr=true" would otherwise yield an extension of "gdpr=true".
    char trimmed[16];
    if (strchr(ext, '?') || strchr(ext, '#'))
    {
        size_t len = 0;
        while (ext[len] && ext[len] != '?' && ext[len] != '#' && len < sizeof(trimmed) - 1)
            len++;
        memcpy(trimmed, ext, len);
        trimmed[len] = 0;
        ext = trimmed;
    }

    for (size_t i = 0; i < RG_COUNT(audio_extensions); ++i)
    {
        if (ext_equals(ext, audio_extensions[i].ext))
            return audio_extensions[i].codec;
    }
    return MEDIA_CODEC_NONE;
}

typedef struct
{
    const char *mime;
    media_codec_t codec;
} media_mime_map_t;

// Matched as a prefix, so "audio/mpeg;charset=utf-8" resolves the same as "audio/mpeg".
static const media_mime_map_t mime_types[] = {
    {"audio/mpeg",       MEDIA_CODEC_TYPE_MP3},
    {"audio/mp3",        MEDIA_CODEC_TYPE_MP3},
    {"audio/mpeg3",      MEDIA_CODEC_TYPE_MP3},
    {"audio/x-mpeg",     MEDIA_CODEC_TYPE_MP3},
    {"audio/wav",        MEDIA_CODEC_TYPE_WAV},
    {"audio/wave",       MEDIA_CODEC_TYPE_WAV},
    {"audio/x-wav",      MEDIA_CODEC_TYPE_WAV},
    {"audio/flac",       MEDIA_CODEC_TYPE_FLAC},
    {"audio/x-flac",     MEDIA_CODEC_TYPE_FLAC},
    {"audio/aac",        MEDIA_CODEC_TYPE_AAC},
    {"audio/aacp",       MEDIA_CODEC_TYPE_AAC},
    {"audio/x-aac",      MEDIA_CODEC_TYPE_AAC},
    {"audio/mp4",        MEDIA_CODEC_TYPE_AAC},
    {"audio/m4a",        MEDIA_CODEC_TYPE_AAC},
    {"audio/opus",       MEDIA_CODEC_TYPE_OPUS},
    {"audio/ogg",        MEDIA_CODEC_TYPE_OGG},
    {"application/ogg",  MEDIA_CODEC_TYPE_OGG},
    {"audio/vorbis",     MEDIA_CODEC_TYPE_OGG},
};

static const char *playlist_mime_types[] = {
    "audio/x-mpegurl",
    "audio/mpegurl",
    "application/x-mpegurl",
    "application/vnd.apple.mpegurl",
    "audio/x-scpls",
    "application/pls+xml",
    "audio/scpls",
};

media_codec_t media_codec_from_mime(const char *mime)
{
    if (!mime || !*mime)
        return MEDIA_CODEC_NONE;

    for (size_t i = 0; i < RG_COUNT(mime_types); ++i)
    {
        size_t len = strlen(mime_types[i].mime);
        if (strncasecmp(mime, mime_types[i].mime, len) == 0 &&
            (mime[len] == 0 || mime[len] == ';' || mime[len] == ' '))
            return mime_types[i].codec;
    }

    return MEDIA_CODEC_NONE;
}

bool media_mime_is_playlist(const char *mime)
{
    if (!mime || !*mime)
        return false;

    for (size_t i = 0; i < RG_COUNT(playlist_mime_types); ++i)
    {
        size_t len = strlen(playlist_mime_types[i]);
        if (strncasecmp(mime, playlist_mime_types[i], len) == 0 &&
            (mime[len] == 0 || mime[len] == ';' || mime[len] == ' '))
            return true;
    }

    return false;
}

bool media_path_is_audio(const char *path)
{
    return media_codec_from_path(path) != MEDIA_CODEC_NONE;
}

bool media_path_is_playlist(const char *path)
{
    const char *ext = rg_extension(path);
    return ext_equals(ext, "m3u") || ext_equals(ext, "m3u8");
}

bool media_path_is_image(const char *path)
{
    const char *ext = rg_extension(path);
    return ext_equals(ext, "jpg") || ext_equals(ext, "jpeg") || ext_equals(ext, "png");
}

bool media_path_is_hidden(const char *name)
{
    if (!name || !name[0])
        return true;
    if (name[0] == '.')
        return true;
    // Windows/macOS droppings that would otherwise be indexed as tracks
    if (strcasecmp(name, "System Volume Information") == 0)
        return true;
    return false;
}

bool media_path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0)
        return false;

    size_t dlen = dir ? strlen(dir) : 0;
    while (dlen > 1 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'))
        dlen--;

    while (name && (*name == '/' || *name == '\\'))
        name++;

    size_t nlen = name ? strlen(name) : 0;
    if (dlen + 1 + nlen + 1 > out_size)
    {
        out[0] = 0;
        return false;
    }

    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen);
    out[dlen + 1 + nlen] = 0;
    return true;
}

/** Collapse ".", "..", duplicate separators and backslashes. Operates in place. */
static void path_normalize(char *path)
{
    char *out = path;
    const char *in = path;
    bool absolute = (*in == '/' || *in == '\\');

    if (absolute)
    {
        *out++ = '/';
        in++;
    }

    char *segments[MEDIA_MAX_SCAN_DEPTH + 16];
    int depth = 0;
    char *seg_start = out;

    while (true)
    {
        if (*in == '/' || *in == '\\' || *in == 0)
        {
            size_t len = (size_t)(out - seg_start);
            if (len == 0)
            {
                // Skip empty segment
            }
            else if (len == 1 && seg_start[0] == '.')
            {
                out = seg_start;
            }
            else if (len == 2 && seg_start[0] == '.' && seg_start[1] == '.')
            {
                out = seg_start;
                if (depth > 0)
                    out = segments[--depth];
                else if (!absolute)
                {
                    // Keep leading ".." on relative paths; the caller validates the root anyway
                    *out++ = '.';
                    *out++ = '.';
                    if (depth < (int)RG_COUNT(segments))
                        segments[depth++] = seg_start;
                }
            }
            else if (depth < (int)RG_COUNT(segments))
            {
                segments[depth++] = seg_start;
            }

            if (*in == 0)
                break;

            in++;
            if (out > path && out[-1] != '/')
                *out++ = '/';
            seg_start = out;
            continue;
        }
        *out++ = *in++;
    }

    // Strip a trailing separator (but keep a bare "/")
    while (out > path + 1 && out[-1] == '/')
        out--;
    *out = 0;
}

bool media_path_resolve(char *out, size_t out_size, const char *base_dir, const char *ref,
                        const char *root)
{
    if (!out || out_size == 0 || !ref)
        return false;

    char buffer[MEDIA_MAX_PATH * 2 + 4];
    bool absolute = ref[0] == '/' || ref[0] == '\\';

    if (absolute)
    {
        if (strlen(ref) >= sizeof(buffer))
            return false;
        strcpy(buffer, ref);
    }
    else if (base_dir)
    {
        if (!media_path_join(buffer, sizeof(buffer), base_dir, ref))
            return false;
    }
    else
    {
        return false;
    }

    path_normalize(buffer);

    if (root && *root)
    {
        size_t rlen = strlen(root);
        while (rlen > 1 && root[rlen - 1] == '/')
            rlen--;
        if (strncmp(buffer, root, rlen) != 0 || (buffer[rlen] != '/' && buffer[rlen] != 0))
        {
            RG_LOGW("Rejected path outside of '%s': '%s'", root, buffer);
            return false;
        }
    }

    size_t len = strlen(buffer);
    if (len == 0 || len >= out_size)
        return false;

    memcpy(out, buffer, len + 1);
    return true;
}

void media_path_stem(char *out, size_t out_size, const char *path)
{
    if (!out || out_size == 0)
        return;

    const char *base = rg_basename(path);
    const char *dot = strrchr(base, '.');
    size_t len = dot && dot != base ? (size_t)(dot - base) : strlen(base);

    if (len >= out_size)
        len = media_utf8_clip(base, out_size - 1);
    memcpy(out, base, len);
    out[len] = 0;
}

bool media_path_swap_ext(char *out, size_t out_size, const char *path, const char *ext)
{
    if (!out || out_size == 0 || !path || !ext)
        return false;

    const char *base = rg_basename(path);
    const char *dot = strrchr(base, '.');
    size_t keep = dot && dot != base ? (size_t)(dot - path) : strlen(path);

    if (keep + 1 + strlen(ext) + 1 > out_size)
        return false;

    memcpy(out, path, keep);
    out[keep] = '.';
    strcpy(out + keep + 1, ext);
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* Formatting                                                                               */
/* -------------------------------------------------------------------------------------- */

void media_format_time(char *out, size_t out_size, uint32_t ms)
{
    if (!out || out_size == 0)
        return;

    uint32_t total = ms / 1000;
    uint32_t h = total / 3600;
    uint32_t m = (total / 60) % 60;
    uint32_t s = total % 60;

    if (h)
        snprintf(out, out_size, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else
        snprintf(out, out_size, "%u:%02u", (unsigned)m, (unsigned)s);
}

void media_format_delta(char *out, size_t out_size, int32_t delta_ms)
{
    if (!out || out_size == 0)
        return;

    char sign = delta_ms < 0 ? '-' : '+';
    uint32_t total = (uint32_t)(delta_ms < 0 ? -(int64_t)delta_ms : delta_ms) / 1000;
    snprintf(out, out_size, "%c%02u:%02u", sign, (unsigned)(total / 60), (unsigned)(total % 60));
}

void media_format_size(char *out, size_t out_size, uint64_t bytes)
{
    if (!out || out_size == 0)
        return;

    if (bytes >= 1024ULL * 1024)
        snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(out, out_size, "%u KB", (unsigned)(bytes / 1024));
    else
        snprintf(out, out_size, "%u B", (unsigned)bytes);
}

/* -------------------------------------------------------------------------------------- */
/* Animation                                                                                */
/* -------------------------------------------------------------------------------------- */

void media_anim_start(media_anim_t *anim, int32_t from, int32_t to, uint32_t duration_ms)
{
    if (!anim)
        return;

    if (duration_ms == 0 || from == to)
    {
        *anim = (media_anim_t){.from = to, .to = to, .active = false};
        return;
    }

    anim->start_us = rg_system_timer();
    anim->duration_us = duration_ms * 1000;
    anim->from = from;
    anim->to = to;
    anim->active = true;
}

int32_t media_anim_value(media_anim_t *anim, int64_t now_us)
{
    if (!anim)
        return 0;
    if (!anim->active)
        return anim->to;

    int64_t elapsed = now_us - anim->start_us;
    if (elapsed <= 0)
        return anim->from;
    if ((uint32_t)elapsed >= anim->duration_us)
    {
        anim->active = false;
        return anim->to;
    }

    int32_t t = (int32_t)((elapsed * 65536) / anim->duration_us);
    int32_t eased = media_ease_out(t);
    return anim->from + (int32_t)(((int64_t)(anim->to - anim->from) * eased) >> 16);
}

bool media_anim_running(const media_anim_t *anim)
{
    return anim && anim->active;
}

int32_t media_ease_out(int32_t t)
{
    // 1 - (1-t)^3, in 16.16
    int64_t inv = 65536 - t;
    int64_t cube = (inv * inv) >> 16;
    cube = (cube * inv) >> 16;
    return (int32_t)(65536 - cube);
}

int32_t media_ease_smoothstep(int32_t t)
{
    int64_t x = t;
    int64_t sq = (x * x) >> 16;
    int64_t cu = (sq * x) >> 16;
    return (int32_t)((3 * sq - 2 * cu));
}

uint32_t media_rand(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* -------------------------------------------------------------------------------------- */
/* Enum names                                                                               */
/* -------------------------------------------------------------------------------------- */

const char *media_codec_name(media_codec_t codec)
{
    switch (codec)
    {
    case MEDIA_CODEC_TYPE_WAV:  return "WAV";
    case MEDIA_CODEC_TYPE_MP3:  return "MP3";
    case MEDIA_CODEC_TYPE_FLAC: return "FLAC";
    case MEDIA_CODEC_TYPE_AAC:  return "AAC";
    case MEDIA_CODEC_TYPE_OGG:  return "Vorbis";
    case MEDIA_CODEC_TYPE_OPUS: return "Opus";
    default:                    return "-";
    }
}

const char *media_state_name(media_state_t state)
{
    switch (state)
    {
    case MEDIA_STATE_STOPPED:   return "Stopped";
    case MEDIA_STATE_LOADING:   return "Loading";
    case MEDIA_STATE_BUFFERING: return "Buffering";
    case MEDIA_STATE_PLAYING:   return "Playing";
    case MEDIA_STATE_PAUSED:    return "Paused";
    case MEDIA_STATE_SEEKING:   return "Seeking";
    case MEDIA_STATE_ENDED:     return "Ended";
    case MEDIA_STATE_ERROR:     return "Error";
    default:                    return "?";
    }
}

const char *media_error_name(media_err_t err)
{
    switch (err)
    {
    case MEDIA_OK:              return "OK";
    case MEDIA_ERR_NOTFOUND:    return "File not found";
    case MEDIA_ERR_UNSUPPORTED: return "Unsupported format";
    case MEDIA_ERR_CORRUPT:     return "Corrupt file";
    case MEDIA_ERR_IO:          return "Read error";
    case MEDIA_ERR_NOMEM:       return "Out of memory";
    case MEDIA_ERR_BUSY:        return "Busy";
    case MEDIA_ERR_ABORTED:     return "Aborted";
    default:                    return "Error";
    }
}
