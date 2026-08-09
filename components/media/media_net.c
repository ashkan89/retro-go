#include <rg_system.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_library.h"
#include "media_net.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_NET"

// A directory listing is HTML or XML; anything past this is not a listing we can use, and
// holding it would cost more than the whole artwork cache.
#define LISTING_MAX_BYTES (192 * 1024)
#define BOOKMARK_FILE "network.txt"

/* -------------------------------------------------------------------------------------- */
/* Basics                                                                                   */
/* -------------------------------------------------------------------------------------- */

bool media_net_is_url(const char *path)
{
    if (!path)
        return false;
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

bool media_net_available(void)
{
#ifdef RG_ENABLE_NETWORKING
    return rg_network_get_info().state == RG_NETWORK_CONNECTED;
#else
    return false;
#endif
}

const char *media_net_status(void)
{
#ifdef RG_ENABLE_NETWORKING
    switch (rg_network_get_info().state)
    {
    case RG_NETWORK_CONNECTED:    return NULL;
    case RG_NETWORK_CONNECTING:   return "Connecting to Wi-Fi...";
    case RG_NETWORK_DISCONNECTED: return "Wi-Fi is not connected.";
    default:                      return "Wi-Fi is disabled.";
    }
#else
    return "This build has networking disabled.";
#endif
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/** Percent-decode in place. Server listings are full of %20 and UTF-8 escapes. */
static void url_decode(char *text)
{
    char *out = text;
    for (const char *in = text; *in; ++in)
    {
        if (*in == '%' && hex_value(in[1]) >= 0 && hex_value(in[2]) >= 0)
        {
            *out++ = (char)((hex_value(in[1]) << 4) | hex_value(in[2]));
            in += 2;
        }
        else
        {
            *out++ = *in;
        }
    }
    *out = 0;
}

void media_net_display_name(char *out, size_t out_size, const char *url)
{
    if (!out || !out_size)
        return;
    out[0] = 0;
    if (!url || !*url)
        return;

    // Skip the scheme, then find the last non-empty path segment.
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;

    const char *host = p;
    const char *end = p + strlen(p);

    // Drop the query and fragment before looking at the path
    for (const char *q = p; q < end; ++q)
    {
        if (*q == '?' || *q == '#')
        {
            end = q;
            break;
        }
    }

    while (end > p && end[-1] == '/')
        end--;

    const char *start = end;
    while (start > p && start[-1] != '/')
        start--;

    if (start >= end)
    {
        // Bare host, e.g. "http://stream.example/" - use the host itself
        const char *host_end = host;
        while (*host_end && *host_end != '/' && *host_end != ':')
            host_end++;
        size_t len = (size_t)(host_end - host);
        if (len >= out_size)
            len = out_size - 1;
        memcpy(out, host, len);
        out[len] = 0;
    }
    else
    {
        size_t len = (size_t)(end - start);
        if (len >= out_size)
            len = out_size - 1;
        memcpy(out, start, len);
        out[len] = 0;
    }

    url_decode(out);
    media_utf8_sanitize(out, out_size);
    media_str_trim(out);
}

bool media_net_resolve(char *out, size_t out_size, const char *base_url, const char *href)
{
    if (!out || !out_size || !href || !*href)
        return false;

    if (media_net_is_url(href))
    {
        if (strlen(href) >= out_size)
            return false;
        strcpy(out, href);
        return true;
    }

    if (!base_url || !media_net_is_url(base_url))
        return false;

    // Locate the end of the scheme and the authority so "/absolute" and "relative" can both
    // be resolved without a full URL parser.
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end)
        return false;
    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');

    if (href[0] == '/')
    {
        size_t root_len = path ? (size_t)(path - base_url) : strlen(base_url);
        if (root_len + strlen(href) + 1 > out_size)
            return false;
        memcpy(out, base_url, root_len);
        strcpy(out + root_len, href);
        return true;
    }

    // Relative: append to the base's directory (everything up to and including the last '/')
    size_t base_len = strlen(base_url);
    const char *last_slash = path ? strrchr(base_url, '/') : NULL;
    size_t dir_len = last_slash && last_slash > authority ? (size_t)(last_slash - base_url) + 1
                                                          : base_len;

    if (dir_len + strlen(href) + 2 > out_size)
        return false;

    memcpy(out, base_url, dir_len);
    out[dir_len] = 0;
    if (dir_len && out[dir_len - 1] != '/')
        strcat(out, "/");
    strcat(out, href);
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* Fetching                                                                                 */
/* -------------------------------------------------------------------------------------- */

#ifdef RG_ENABLE_NETWORKING

/** Download a whole document into a PSRAM buffer. Returns NULL on any failure. */
static char *fetch_document(const char *url, const char *method_header, size_t *len_out)
{
    if (len_out)
        *len_out = 0;

    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.timeout_ms = 8000;

    // WebDAV needs a PROPFIND with Depth: 1. rg_network only issues GET/POST, so the depth
    // header is sent on a GET; servers that speak WebDAV answer a plain GET on a collection
    // with an HTML index anyway, which the HTML parser below then handles. This keeps us to
    // one HTTP verb without losing either dialect.
    const rg_http_header_t headers[] = {
        {"Depth", "1"},
        {"Accept", "text/html, application/xml;q=0.9, */*;q=0.5"},
        {NULL, NULL},
    };
    cfg.headers = headers;
    (void)method_header;

    rg_http_req_t *req = rg_network_http_open(url, &cfg);
    if (!req)
    {
        RG_LOGW("Could not open '%s'", url);
        return NULL;
    }

    if (req->status_code < 200 || req->status_code >= 300)
    {
        RG_LOGW("HTTP %d for '%s'", req->status_code, url);
        rg_network_http_close(req);
        return NULL;
    }

    size_t capacity = 8192;
    if (req->content_length > 0 && (size_t)req->content_length + 1 < LISTING_MAX_BYTES)
        capacity = (size_t)req->content_length + 1;

    char *buffer = rg_alloc(capacity, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    size_t used = 0;

    while (buffer)
    {
        if (used + 1 >= capacity)
        {
            if (capacity >= LISTING_MAX_BYTES)
            {
                RG_LOGW("Listing exceeded %d bytes, truncating", LISTING_MAX_BYTES);
                break;
            }
            size_t grown = capacity * 2;
            if (grown > LISTING_MAX_BYTES)
                grown = LISTING_MAX_BYTES;
            char *bigger = rg_alloc(grown, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
            if (!bigger)
                break;
            memcpy(bigger, buffer, used);
            free(buffer);
            buffer = bigger;
            capacity = grown;
        }

        int got = rg_network_http_read(req, buffer + used, capacity - used - 1);
        if (got <= 0)
            break;
        used += (size_t)got;
    }

    rg_network_http_close(req);

    if (!buffer)
        return NULL;

    buffer[used] = 0;
    if (len_out)
        *len_out = used;

    if (used == 0)
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}

/* ---- WebDAV ------------------------------------------------------------------------- */

/** Extract the text between <tag ...> and </tag>, ignoring any namespace prefix. */
static const char *xml_find_tag(const char *xml, const char *local_name, size_t *len_out)
{
    size_t name_len = strlen(local_name);

    for (const char *p = xml; (p = strchr(p, '<')) != NULL; ++p)
    {
        const char *name = p + 1;
        if (*name == '/' || *name == '?' || *name == '!')
            continue;

        // Skip a namespace prefix such as "d:" or "D:"
        const char *colon = name;
        while (*colon && *colon != '>' && *colon != ' ' && *colon != ':' && *colon != '/')
            colon++;
        const char *local = (*colon == ':') ? colon + 1 : name;

        if (strncasecmp(local, local_name, name_len) != 0)
            continue;

        char after = local[name_len];
        if (after != '>' && after != ' ' && after != '/' && after != '\t')
            continue;

        const char *close = strchr(local, '>');
        if (!close)
            return NULL;
        if (close[-1] == '/') // Self-closing, no content
            return NULL;

        const char *value = close + 1;
        const char *end = strstr(value, "</");
        if (!end)
            return NULL;

        if (len_out)
            *len_out = (size_t)(end - value);
        return value;
    }

    return NULL;
}

static int parse_webdav(const char *xml, const char *base_url, media_net_entry_t *out, int max)
{
    int count = 0;
    const char *cursor = xml;

    // Each child is a <response> holding an <href> and a <resourcetype>.
    while (count < max)
    {
        const char *response = strcasestr(cursor, "<d:response");
        if (!response)
            response = strcasestr(cursor, "<response");
        if (!response)
            response = strcasestr(cursor, ":response");
        if (!response)
            break;

        const char *next = strcasestr(response + 1, "response>");
        const char *block_end = next ? next : response + strlen(response);

        size_t href_len = 0;
        const char *href = xml_find_tag(response, "href", &href_len);

        if (href && href < block_end && href_len && href_len < MEDIA_MAX_PATH)
        {
            char raw[MEDIA_MAX_PATH + 1];
            memcpy(raw, href, href_len);
            raw[href_len] = 0;
            media_str_trim(raw);

            char absolute[MEDIA_MAX_PATH + 1];
            if (media_net_resolve(absolute, sizeof(absolute), base_url, raw))
            {
                // A <collection/> marker inside this response means it is a folder.
                size_t scope = (size_t)(block_end - response);
                char *collection = NULL;
                for (const char *p = response; p < block_end - 10; ++p)
                {
                    if (strncasecmp(p, "collection", 10) == 0)
                    {
                        collection = (char *)p;
                        break;
                    }
                }
                (void)scope;

                // The first response is the collection itself; skip it.
                bool is_self = strcmp(absolute, base_url) == 0;
                if (!is_self)
                {
                    size_t alen = strlen(absolute);
                    bool is_dir = collection != NULL || (alen && absolute[alen - 1] == '/');

                    media_net_entry_t *entry = &out[count];
                    memset(entry, 0, sizeof(*entry));
                    strcpy(entry->url, absolute);
                    entry->is_dir = is_dir;
                    media_net_display_name(entry->name, sizeof(entry->name), absolute);

                    if (entry->name[0] && (is_dir || media_path_is_audio(entry->name) ||
                                           media_path_is_playlist(entry->name)))
                        count++;
                }
            }
        }

        cursor = block_end;
    }

    return count;
}

/* ---- HTML directory index ------------------------------------------------------------ */

static int parse_html_index(const char *html, const char *base_url, media_net_entry_t *out, int max)
{
    int count = 0;

    for (const char *p = html; count < max && (p = strcasestr(p, "<a ")) != NULL; ++p)
    {
        const char *href = strcasestr(p, "href=");
        const char *tag_end = strchr(p, '>');
        if (!href || !tag_end || href > tag_end)
            continue;

        href += 5;
        char quote = 0;
        if (*href == '"' || *href == '\'')
            quote = *href++;

        const char *end = href;
        while (*end && end < tag_end + 1 && ((quote && *end != quote) || (!quote && *end != ' ' && *end != '>')))
            end++;

        size_t len = (size_t)(end - href);
        if (!len || len >= MEDIA_MAX_PATH)
            continue;

        char raw[MEDIA_MAX_PATH + 1];
        memcpy(raw, href, len);
        raw[len] = 0;

        // Navigation and sorting links that autoindex pages are full of
        if (raw[0] == '#' || raw[0] == '?' || strncmp(raw, "..", 2) == 0 ||
            strncasecmp(raw, "mailto:", 7) == 0)
            continue;

        char absolute[MEDIA_MAX_PATH + 1];
        if (!media_net_resolve(absolute, sizeof(absolute), base_url, raw))
            continue;

        // Only descend into the tree we were pointed at; an autoindex page links out to all
        // sorts of things and following them would wander off the server.
        if (strncmp(absolute, base_url, strlen(base_url)) != 0)
            continue;
        if (strcmp(absolute, base_url) == 0)
            continue;

        size_t alen = strlen(absolute);
        bool is_dir = alen && absolute[alen - 1] == '/';

        if (!is_dir && !media_path_is_audio(absolute) && !media_path_is_playlist(absolute))
            continue;

        // Skip duplicates: many templates emit both an icon link and a text link per row.
        bool duplicate = false;
        for (int i = 0; i < count; ++i)
            duplicate = duplicate || strcmp(out[i].url, absolute) == 0;
        if (duplicate)
            continue;

        media_net_entry_t *entry = &out[count];
        memset(entry, 0, sizeof(*entry));
        strcpy(entry->url, absolute);
        entry->is_dir = is_dir;
        media_net_display_name(entry->name, sizeof(entry->name), absolute);

        if (entry->name[0])
            count++;
    }

    return count;
}

#endif /* RG_ENABLE_NETWORKING */

static int compare_entries(const void *a, const void *b)
{
    const media_net_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1;
    return media_strnatcasecmp(ea->name, eb->name);
}

int media_net_list(const char *url, media_net_entry_t *out, int max)
{
#ifdef RG_ENABLE_NETWORKING
    if (!url || !out || max <= 0 || !media_net_is_url(url))
        return -1;
    if (!media_net_available())
        return -1;

    size_t len = 0;
    char *document = fetch_document(url, "PROPFIND", &len);
    if (!document)
        return -1;

    int count = 0;

    // WebDAV replies are XML with <multistatus>; everything else is treated as HTML.
    if (strcasestr(document, "multistatus") || strcasestr(document, "<D:response") ||
        strcasestr(document, "<d:response"))
        count = parse_webdav(document, url, out, max);

    if (count == 0)
        count = parse_html_index(document, url, out, max);

    free(document);

    if (count > 1)
        qsort(out, (size_t)count, sizeof(media_net_entry_t), compare_entries);

    RG_LOGI("Listed %d entries from '%s'", count, url);
    return count;
#else
    (void)url, (void)out, (void)max;
    return -1;
#endif
}

/* -------------------------------------------------------------------------------------- */
/* Byte ranges                                                                              */
/* -------------------------------------------------------------------------------------- */

int media_net_fetch_range(const char *url, uint64_t offset, size_t length, void *buffer,
                          uint64_t *total_size_out)
{
#ifdef RG_ENABLE_NETWORKING
    if (total_size_out)
        *total_size_out = 0;
    if (!url || !buffer || !length || !media_net_is_url(url))
        return -1;
    if (!media_net_available())
        return -1;

    char range[64];
    snprintf(range, sizeof(range), "bytes=%llu-%llu", (unsigned long long)offset,
             (unsigned long long)(offset + length - 1));

    const rg_http_header_t headers[] = {
        {"Range", range},
        {NULL, NULL},
    };

    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.timeout_ms = 8000;
    cfg.headers = headers;

    rg_http_req_t *req = rg_network_http_open(url, &cfg);
    if (!req)
        return -1;

    if (req->status_code < 200 || req->status_code >= 300)
    {
        rg_network_http_close(req);
        return -1;
    }

    // A 200 means the server ignored the range and is sending from the start. That is usable
    // when we wanted the start anyway, and useless otherwise.
    if (offset > 0 && req->status_code != 206)
    {
        RG_LOGD("Server ignored the range for '%s'", url);
        rg_network_http_close(req);
        return -1;
    }

    if (total_size_out && req->content_length > 0)
        *total_size_out = (req->status_code == 206) ? offset + (uint64_t)req->content_length
                                                    : (uint64_t)req->content_length;

    size_t got = 0;
    while (got < length)
    {
        int n = rg_network_http_read(req, (uint8_t *)buffer + got, length - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }

    rg_network_http_close(req);
    return (int)got;
#else
    (void)url, (void)offset, (void)length, (void)buffer, (void)total_size_out;
    return -1;
#endif
}

uint8_t *media_net_fetch_file(const char *url, size_t max_bytes, size_t *len_out)
{
#ifdef RG_ENABLE_NETWORKING
    if (len_out)
        *len_out = 0;
    if (!url || !max_bytes || !media_net_is_url(url) || !media_net_available())
        return NULL;

    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.timeout_ms = 8000;

    rg_http_req_t *req = rg_network_http_open(url, &cfg);
    if (!req)
        return NULL;

    if (req->status_code < 200 || req->status_code >= 300)
    {
        rg_network_http_close(req);
        return NULL;
    }

    if (req->content_length > 0 && (size_t)req->content_length > max_bytes)
    {
        RG_LOGD("'%s' is %d bytes, over the %u limit", url, req->content_length,
                (unsigned)max_bytes);
        rg_network_http_close(req);
        return NULL;
    }

    size_t capacity = req->content_length > 0 ? (size_t)req->content_length : 16384;
    uint8_t *buffer = rg_alloc(capacity, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
    size_t used = 0;

    while (buffer)
    {
        if (used >= capacity)
        {
            if (capacity >= max_bytes)
                break;
            size_t grown = capacity * 2 > max_bytes ? max_bytes : capacity * 2;
            uint8_t *bigger = rg_alloc(grown, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
            if (!bigger)
                break;
            memcpy(bigger, buffer, used);
            free(buffer);
            buffer = bigger;
            capacity = grown;
        }

        int n = rg_network_http_read(req, buffer + used, capacity - used);
        if (n <= 0)
            break;
        used += (size_t)n;
    }

    rg_network_http_close(req);

    if (!buffer)
        return NULL;

    if (used < 16)
    {
        free(buffer);
        return NULL;
    }

    if (len_out)
        *len_out = used;
    return buffer;
#else
    (void)url, (void)max_bytes, (void)len_out;
    return NULL;
#endif
}

/* -------------------------------------------------------------------------------------- */
/* Remote playlists                                                                         */
/* -------------------------------------------------------------------------------------- */

bool media_net_url_is_playlist(const char *url)
{
    if (!media_net_is_url(url))
        return false;

    // Strip the query before looking at the extension: ".../stream.m3u?token=x" is still a
    // playlist, and ".../hot108?aw_0_req.gdpr=true" is still not one.
    char path[MEDIA_MAX_PATH + 1];
    media_utf8_copy(path, sizeof(path), url);

    char *cut = strpbrk(path, "?#");
    if (cut)
        *cut = 0;

    const char *ext = strrchr(path, '.');
    if (!ext || strchr(ext, '/'))
        return false;

    return strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0 ||
           strcasecmp(ext, ".pls") == 0;
}

int media_net_fetch_playlist(const char *url, char (*out)[MEDIA_MAX_PATH + 1], int max)
{
#ifdef RG_ENABLE_NETWORKING
    if (!url || !out || max <= 0 || !media_net_is_url(url))
        return -1;
    if (!media_net_available())
        return -1;

    size_t len = 0;
    char *document = fetch_document(url, "GET", &len);
    if (!document)
        return -1;

    // An HLS manifest looks like an M3U but describes segments and codecs we do not
    // implement; playing its first line would produce noise rather than music.
    if (strstr(document, "#EXT-X-"))
    {
        RG_LOGW("'%s' is an HLS manifest, which is not supported", url);
        free(document);
        return 0;
    }

    int count = 0;
    char *cursor = document;

    while (count < max && cursor && *cursor)
    {
        char *line = cursor;
        char *newline = strpbrk(cursor, "\r\n");

        if (newline)
        {
            *newline = 0;
            cursor = newline + 1;
            while (*cursor == '\r' || *cursor == '\n')
                cursor++;
        }
        else
        {
            cursor = NULL;
        }

        media_str_trim(line);
        if (!line[0])
            continue;

        // PLS puts the address after "FileN="; M3U comments start with '#'.
        if (line[0] == '#')
            continue;

        char *value = line;
        if (strncasecmp(line, "File", 4) == 0)
        {
            char *equals = strchr(line, '=');
            if (!equals)
                continue;
            value = media_str_trim(equals + 1);
        }
        else if (strchr(line, '=') && !media_net_is_url(line))
        {
            continue; // Some other PLS key (Title1, Length1, NumberOfEntries, ...)
        }

        if (!media_net_is_url(value) || strlen(value) > MEDIA_MAX_PATH)
            continue;

        // A playlist that lists itself would loop forever.
        if (strcmp(value, url) == 0)
            continue;

        strcpy(out[count++], value);
    }

    free(document);

    RG_LOGI("Playlist '%s' resolved to %d stream(s)", url, count);
    return count;
#else
    (void)url, (void)out, (void)max;
    return -1;
#endif
}

/* -------------------------------------------------------------------------------------- */
/* Bookmarks                                                                                */
/* -------------------------------------------------------------------------------------- */

static void bookmark_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/.retrogo-media/%s", media_library_root(), BOOKMARK_FILE);
}

int media_net_bookmarks_load(media_net_entry_t *out, int max)
{
    if (!out || max <= 0)
        return 0;

    char path[MEDIA_MAX_PATH + 64];
    bookmark_path(path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;

    char line[MEDIA_MAX_PATH + MEDIA_NET_NAME_LEN + 16];
    int count = 0;

    while (count < max && fgets(line, sizeof(line), fp))
    {
        media_str_trim(line);
        if (!line[0] || line[0] == '#')
            continue;

        // type <tab> name <tab> url
        char *tab1 = strchr(line, '\t');
        if (!tab1)
            continue;
        *tab1 = 0;
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2)
            continue;
        *tab2 = 0;

        const char *type = media_str_trim(line);
        const char *name = media_str_trim(tab1 + 1);
        const char *url = media_str_trim(tab2 + 1);

        if (!media_net_is_url(url) || strlen(url) > MEDIA_MAX_PATH)
            continue;

        media_net_entry_t *entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        strcpy(entry->url, url);
        entry->is_dir = strcasecmp(type, "dir") == 0;
        entry->is_stream = strcasecmp(type, "radio") == 0;

        if (name[0])
            media_utf8_copy(entry->name, sizeof(entry->name), name);
        else
            media_net_display_name(entry->name, sizeof(entry->name), url);
    }

    fclose(fp);
    return count;
}

static bool bookmarks_save(const media_net_entry_t *entries, int count)
{
    char path[MEDIA_MAX_PATH + 64];
    char dir[MEDIA_MAX_PATH + 32];

    bookmark_path(path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s/.retrogo-media", media_library_root());
    rg_storage_mkdir(dir);

    size_t capacity = 128;
    for (int i = 0; i < count; ++i)
        capacity += strlen(entries[i].name) + strlen(entries[i].url) + 16;

    char *buffer = malloc(capacity);
    if (!buffer)
        return false;

    size_t used = (size_t)snprintf(buffer, capacity,
                                   "# Retro-Go media player network bookmarks\n"
                                   "# type<TAB>name<TAB>url   (type: dir, radio or url)\n");

    for (int i = 0; i < count; ++i)
    {
        const char *type = entries[i].is_dir ? "dir" : (entries[i].is_stream ? "radio" : "url");
        int n = snprintf(buffer + used, capacity - used, "%s\t%s\t%s\n", type, entries[i].name,
                         entries[i].url);
        if (n < 0 || (size_t)n >= capacity - used)
            break;
        used += (size_t)n;
    }

    bool ok = rg_storage_write_file(path, buffer, used, RG_FILE_ATOMIC_WRITE);
    free(buffer);
    return ok;
}

bool media_net_bookmark_add(const char *name, const char *url, bool is_dir, bool is_stream)
{
    if (!media_net_is_url(url) || strlen(url) > MEDIA_MAX_PATH)
        return false;

    media_net_entry_t *entries = calloc(MEDIA_NET_MAX_ENTRIES, sizeof(media_net_entry_t));
    if (!entries)
        return false;

    int count = media_net_bookmarks_load(entries, MEDIA_NET_MAX_ENTRIES - 1);

    // Adding the same URL twice just updates its name rather than growing the list.
    for (int i = 0; i < count; ++i)
    {
        if (strcmp(entries[i].url, url) == 0)
        {
            if (name && *name)
                media_utf8_copy(entries[i].name, sizeof(entries[i].name), name);
            entries[i].is_dir = is_dir;
            entries[i].is_stream = is_stream;
            bool ok = bookmarks_save(entries, count);
            free(entries);
            return ok;
        }
    }

    media_net_entry_t *entry = &entries[count++];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->url, url);
    entry->is_dir = is_dir;
    entry->is_stream = is_stream;

    if (name && *name)
        media_utf8_copy(entry->name, sizeof(entry->name), name);
    else
        media_net_display_name(entry->name, sizeof(entry->name), url);

    bool ok = bookmarks_save(entries, count);
    free(entries);
    return ok;
}

bool media_net_bookmark_remove(const char *url)
{
    if (!url)
        return false;

    media_net_entry_t *entries = calloc(MEDIA_NET_MAX_ENTRIES, sizeof(media_net_entry_t));
    if (!entries)
        return false;

    int count = media_net_bookmarks_load(entries, MEDIA_NET_MAX_ENTRIES);
    int keep = 0;

    for (int i = 0; i < count; ++i)
    {
        if (strcmp(entries[i].url, url) == 0)
            continue;
        if (keep != i)
            entries[keep] = entries[i];
        keep++;
    }

    bool ok = keep == count ? true : bookmarks_save(entries, keep);
    free(entries);
    return ok;
}
