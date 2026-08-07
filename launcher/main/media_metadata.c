#include "media_metadata.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <jpeg_decoder.h>
#endif

static uint32_t synchsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static size_t utf8_put(char *out, size_t room, uint32_t cp)
{
    if (cp < 0x80 && room >= 1) out[0] = cp, out[1] = 0, room = 1;
    else if (cp < 0x800 && room >= 2) out[0] = 0xc0 | (cp >> 6), out[1] = 0x80 | (cp & 63), out[2] = 0, room = 2;
    else if (cp < 0x10000 && room >= 3) out[0] = 0xe0 | (cp >> 12), out[1] = 0x80 | ((cp >> 6) & 63), out[2] = 0x80 | (cp & 63), out[3] = 0, room = 3;
    else if (room >= 4) out[0] = 0xf0 | (cp >> 18), out[1] = 0x80 | ((cp >> 12) & 63), out[2] = 0x80 | ((cp >> 6) & 63), out[3] = 0x80 | (cp & 63), out[4] = 0, room = 4;
    else return 0;
    return room;
}

static void decode_text(char *out, size_t out_size, const uint8_t *data, size_t len)
{
    if (!out_size) return;
    out[0] = 0;
    if (!data || !len) return;
    int encoding = *data++;
    len--;
    size_t used = 0;
    if (encoding == 1 || encoding == 2) {
        bool little = encoding == 1;
        if (len >= 2 && data[0] == 0xff && data[1] == 0xfe) little = true, data += 2, len -= 2;
        else if (len >= 2 && data[0] == 0xfe && data[1] == 0xff) little = false, data += 2, len -= 2;
        while (len >= 2 && used + 4 < out_size) {
            uint32_t cp = little ? data[0] | ((uint32_t)data[1] << 8) : ((uint32_t)data[0] << 8) | data[1];
            data += 2; len -= 2;
            if (!cp) break;
            if (cp >= 0xd800 && cp <= 0xdbff && len >= 2) {
                uint32_t low = little ? data[0] | ((uint32_t)data[1] << 8) : ((uint32_t)data[0] << 8) | data[1];
                if (low >= 0xdc00 && low <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00, data += 2, len -= 2;
            }
            used += utf8_put(out + used, out_size - used - 1, cp);
        }
    } else if (encoding == 3) {
        size_t copy = RG_MIN(len, out_size - 1);
        memcpy(out, data, copy); out[copy] = 0;
    } else {
        while (len-- && used + 2 < out_size && *data) {
            uint32_t cp = *data++;
            used += utf8_put(out + used, out_size - used - 1, cp);
        }
    }
    while (used && (out[used - 1] == ' ' || out[used - 1] == '\r' || out[used - 1] == '\n')) out[--used] = 0;
}

static void parse_apic(media_metadata_t *m, const uint8_t *p, size_t n, uint32_t file_offset, int version)
{
    if (n < 5) return;
    int enc = *p++; n--;
    const uint8_t *mime = p;
    size_t mime_len = version == 2 ? RG_MIN(n, 3) : strnlen((const char *)p, n);
    if (mime_len >= n) return;
    snprintf(m->cover_mime, sizeof(m->cover_mime), "%.*s", (int)mime_len, mime);
    p += mime_len + (version == 2 ? 0 : 1); n -= mime_len + (version == 2 ? 0 : 1);
    if (!n) return;
    p++; n--; /* picture type */
    if (enc == 1 || enc == 2) {
        while (n >= 2 && (p[0] || p[1])) p += 2, n -= 2;
        if (n >= 2) p += 2, n -= 2;
    } else {
        size_t desc = strnlen((const char *)p, n);
        if (desc >= n) return;
        p += desc + 1; n -= desc + 1;
    }
    m->cover_offset = file_offset + (uint32_t)(p - (mime - 1));
    m->cover_size = n;
}

static void parse_frames(FILE *fp, media_metadata_t *m, uint32_t tag_size, int version, uint8_t flags)
{
    uint32_t pos = 10, end = 10 + tag_size;
    uint8_t head[10];
    if ((flags & 0x40) && pos + 4 < end) {
        if (!fseek(fp, pos, SEEK_SET) && fread(head, 1, 4, fp) == 4) {
            uint32_t extended = version == 4 ? synchsafe(head) : be32(head) + 4;
            if (extended < tag_size) pos += extended;
        }
    }
    while (pos + (version == 2 ? 6 : 10) <= end) {
        int hs = version == 2 ? 6 : 10;
        if (fseek(fp, pos, SEEK_SET) || fread(head, 1, hs, fp) != (size_t)hs || head[0] == 0) break;
        char id[5] = {0}; memcpy(id, head, version == 2 ? 3 : 4);
        uint32_t size = version == 2 ? ((uint32_t)head[3] << 16) | ((uint32_t)head[4] << 8) | head[5]
                                     : (version == 4 ? synchsafe(head + 4) : be32(head + 4));
        pos += hs;
        if (!size || size > end - pos) break;
        bool wanted = !strcmp(id, "TIT2") || !strcmp(id, "TT2") || !strcmp(id, "TPE1") || !strcmp(id, "TP1") ||
                      !strcmp(id, "TALB") || !strcmp(id, "TAL") || !strcmp(id, "TCON") || !strcmp(id, "TCO") ||
                      !strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TRCK") ||
                      !strcmp(id, "TRK") || !strcmp(id, "COMM") || !strcmp(id, "COM") || !strcmp(id, "USLT") ||
                      !strcmp(id, "ULT") || !strcmp(id, "APIC") || !strcmp(id, "PIC");
        if (wanted && size <= 1024 * 1024) {
            uint8_t *buf = malloc(size + 1);
            if (buf && fread(buf, 1, size, fp) == size) {
                buf[size] = 0;
                if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) decode_text(m->title, sizeof(m->title), buf, size);
                else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) decode_text(m->artist, sizeof(m->artist), buf, size);
                else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) decode_text(m->album, sizeof(m->album), buf, size);
                else if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) decode_text(m->genre, sizeof(m->genre), buf, size);
                else if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) decode_text(m->year, sizeof(m->year), buf, size);
                else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) decode_text(m->track, sizeof(m->track), buf, size);
                else if (!strcmp(id, "APIC") || !strcmp(id, "PIC")) parse_apic(m, buf, size, pos, version);
                else if (size > 4) {
                    /* COMM/USLT: encoding + language + description + value */
                    size_t skip = 4;
                    if (buf[0] == 1 || buf[0] == 2) while (skip + 1 < size && (buf[skip] || buf[skip + 1])) skip += 2;
                    else while (skip < size && buf[skip]) skip++;
                    skip += (buf[0] == 1 || buf[0] == 2) ? 2 : 1;
                    if (skip < size) {
                        uint8_t *text = malloc(size - skip + 1);
                        if (text) { text[0] = buf[0]; memcpy(text + 1, buf + skip, size - skip);
                            decode_text((!strcmp(id, "USLT") || !strcmp(id, "ULT")) ? m->lyrics : m->comment,
                                        (!strcmp(id, "USLT") || !strcmp(id, "ULT")) ? sizeof(m->lyrics) : sizeof(m->comment), text, size - skip + 1); free(text); }
                    }
                }
            }
            free(buf);
        }
        pos += size;
    }
}

static bool parse_mp3_header(const uint8_t *h, uint32_t *rate, uint32_t *bitrate, uint8_t *channels)
{
    static const uint16_t br1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const uint32_t rates[3] = {44100,48000,32000};
    if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || (h[1] & 6) == 0 || (h[1] & 6) == 2) return false;
    int ver = (h[1] >> 3) & 3, bri = h[2] >> 4, sri = (h[2] >> 2) & 3;
    if (ver == 1 || !bri || bri == 15 || sri == 3) return false;
    *rate = rates[sri] >> (ver == 2 ? 1 : ver == 0 ? 2 : 0);
    *bitrate = 1000u * (ver == 3 ? br1[bri] : br2[bri]);
    *channels = ((h[3] >> 6) == 3) ? 1 : 2;
    return true;
}

bool media_metadata_read(const char *path, media_metadata_t *m, bool scan_audio)
{
    memset(m, 0, sizeof(*m));
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END); long total = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), fp) == sizeof(h) && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
        m->id3_version = h[3];
        uint32_t tag = synchsafe(h + 6);
        m->audio_offset = 10 + tag + ((h[5] & 0x10) ? 10 : 0);
        parse_frames(fp, m, tag, h[3], h[5]);
    }
    if (!m->audio_offset) m->audio_offset = 0;
    m->audio_size = total > (long)m->audio_offset ? total - m->audio_offset : 0;
    if (total >= 128) {
        uint8_t tag[128]; fseek(fp, total - 128, SEEK_SET);
        if (fread(tag, 1, sizeof(tag), fp) == sizeof(tag) && !memcmp(tag, "TAG", 3)) {
            char temp[64];
#define ID3V1_COPY(dest, offset, length) do { memcpy(temp, tag + (offset), (length)); temp[(length)] = 0; \
                for (int z = (length) - 1; z >= 0 && (temp[z] == 0 || temp[z] == ' '); z--) temp[z] = 0; \
                if (!(dest)[0]) { size_t copy = RG_MIN(strlen(temp), sizeof(dest) - 1); \
                    memcpy((dest), temp, copy); (dest)[copy] = 0; } } while (0)
            ID3V1_COPY(m->title, 3, 30); ID3V1_COPY(m->artist, 33, 30); ID3V1_COPY(m->album, 63, 30); ID3V1_COPY(m->year, 93, 4);
#undef ID3V1_COPY
            m->audio_size -= 128;
        }
    }
    if (scan_audio) {
        fseek(fp, m->audio_offset, SEEK_SET);
        uint8_t window[4096]; size_t got = fread(window, 1, sizeof(window), fp);
        for (size_t i = 0; i + 4 <= got; i++) if (parse_mp3_header(window + i, &m->sample_rate, &m->bitrate, &m->channels)) {
            int version = (window[i + 1] >> 3) & 3;
            int crc = (window[i + 1] & 1) ? 0 : 2;
            int side = version == 3 ? (m->channels == 1 ? 17 : 32) : (m->channels == 1 ? 9 : 17);
            size_t xing = i + 4 + crc + side;
            if (xing + 12 <= got && (!memcmp(window + xing, "Xing", 4) || !memcmp(window + xing, "Info", 4))) {
                uint32_t xflags = be32(window + xing + 4), cursor = xing + 8;
                if ((xflags & 1) && cursor + 4 <= got) {
                    uint32_t frames = be32(window + cursor);
                    uint32_t samples_per_frame = version == 3 ? 1152 : 576;
                    m->duration_ms = (uint64_t)frames * samples_per_frame * 1000 / m->sample_rate;
                    if (m->duration_ms) m->bitrate = (uint64_t)m->audio_size * 8000 / m->duration_ms;
                }
            }
            size_t vbri = i + 4 + 32;
            if (!m->duration_ms && vbri + 18 <= got && !memcmp(window + vbri, "VBRI", 4)) {
                uint32_t frames = be32(window + vbri + 14);
                uint32_t samples_per_frame = version == 3 ? 1152 : 576;
                m->duration_ms = (uint64_t)frames * samples_per_frame * 1000 / m->sample_rate;
                if (m->duration_ms) m->bitrate = (uint64_t)m->audio_size * 8000 / m->duration_ms;
            }
            break;
        }
        if (m->bitrate) m->duration_ms = (uint64_t)m->audio_size * 8000 / m->bitrate;
    }
    fclose(fp);
    if (!m->title[0]) {
        snprintf(m->title, sizeof(m->title), "%s", rg_basename(path));
        char *dot = strrchr(m->title, '.'); if (dot) *dot = 0;
    }
    return true;
}

static rg_surface_t *load_jpeg(const uint8_t *data, size_t len, int target_width, int target_height)
{
#ifdef ESP_PLATFORM
    esp_jpeg_image_cfg_t cfg = {.indata = (uint8_t *)data, .indata_size = len, .out_format = JPEG_IMAGE_FORMAT_RGB565};
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return NULL;
    while ((target_width > 0 && info.width > target_width * 2) || (target_height > 0 && info.height > target_height * 2)) {
        if (cfg.out_scale == JPEG_IMAGE_SCALE_1_8) break;
        cfg.out_scale++;
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return NULL;
    }
    cfg.outbuf = rg_alloc(info.output_len, MEM_SLOW); cfg.outbuf_size = info.output_len;
    if (!cfg.outbuf || esp_jpeg_decode(&cfg, &info) != ESP_OK) { free(cfg.outbuf); return NULL; }
    rg_surface_t raw = {.width = info.width, .height = info.height, .stride = info.width * 2,
                        .format = RG_PIXEL_565_LE, .data = cfg.outbuf};
    rg_surface_t *out = rg_surface_convert(&raw, target_width, target_height, RG_PIXEL_565_LE);
    free(cfg.outbuf); return out;
#else
    return NULL;
#endif
}

static rg_surface_t *load_cover_data(const uint8_t *data, size_t len, int w, int h)
{
    if (len >= 4 && !memcmp(data, "\x89PNG", 4)) {
        rg_surface_t *raw = rg_surface_load_image(data, len, 0);
        if (!raw) return NULL;
        rg_surface_t *scaled = rg_surface_convert(raw, w, h, RG_PIXEL_565_LE);
        rg_surface_free(raw); return scaled;
    }
    if (len >= 2 && data[0] == 0xff && data[1] == 0xd8) return load_jpeg(data, len, w, h);
    return NULL;
}

static bool join_path(char *out, size_t size, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b >= size) return false;
    memcpy(out, dir, a); out[a] = '/'; memcpy(out + a + 1, name, b + 1);
    return true;
}

rg_surface_t *media_metadata_load_cover(const char *path, const media_metadata_t *m, int w, int h)
{
    if (m->cover_size && m->cover_size <= 4 * 1024 * 1024) {
        FILE *fp = fopen(path, "rb");
        uint8_t *data = rg_alloc(m->cover_size, MEM_SLOW);
        if (fp && data && !fseek(fp, m->cover_offset, SEEK_SET) && fread(data, 1, m->cover_size, fp) == m->cover_size) {
            rg_surface_t *img = load_cover_data(data, m->cover_size, w, h); free(data); fclose(fp);
            if (img) return img;
        } else { free(data); if (fp) fclose(fp); }
    }
    char dir[RG_PATH_MAX + 1], candidate[RG_PATH_MAX + 1];
    snprintf(dir, sizeof(dir), "%s", path); char *slash = strrchr(dir, '/'); if (slash) *slash = 0;
    static const char *names[] = {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.png", "album.jpg", "album.png"};
    for (size_t i = 0; i < RG_COUNT(names); i++) {
        if (!join_path(candidate, sizeof(candidate), dir, names[i])) continue;
        void *data; size_t len;
        if (rg_storage_read_file(candidate, &data, &len, 0)) {
            rg_surface_t *img = load_cover_data(data, len, w, h); free(data); if (img) return img;
        }
    }
    return NULL;
}

static int lyric_compare(const void *a, const void *b)
{
    const media_lyric_line_t *aa = a, *bb = b;
    return aa->time_ms < bb->time_ms ? -1 : aa->time_ms > bb->time_ms;
}

bool media_lyrics_load(const char *mp3_path, media_lyrics_t *l)
{
    media_lyrics_free(l);
    char path[RG_PATH_MAX + 1]; snprintf(path, sizeof(path), "%s", mp3_path);
    char *dot = strrchr(path, '.'); if (!dot) return false; snprintf(dot, path + sizeof(path) - dot, ".lrc");
    size_t len;
    if (!rg_storage_read_file(path, (void **)&l->storage, &len, 0)) return false;
    char *terminated = realloc(l->storage, len + 1);
    if (!terminated) { media_lyrics_free(l); return false; }
    l->storage = terminated;
    l->storage[len] = 0;
    char *data = l->storage;
    if (len >= 3 && (uint8_t)data[0] == 0xef && (uint8_t)data[1] == 0xbb && (uint8_t)data[2] == 0xbf) data += 3;
    l->lines = calloc(MEDIA_LYRIC_LINES_MAX, sizeof(*l->lines));
    if (!l->lines) { media_lyrics_free(l); return false; }
    for (char *line = data; line && *line;) {
        char *next = strpbrk(line, "\r\n"); if (next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; }
        if (!strncmp(line, "[ti:", 4)) snprintf(l->title, sizeof(l->title), "%.*s", (int)RG_MAX(0, (int)strlen(line) - 5), line + 4);
        else if (!strncmp(line, "[ar:", 4)) snprintf(l->artist, sizeof(l->artist), "%.*s", (int)RG_MAX(0, (int)strlen(line) - 5), line + 4);
        else if (!strncmp(line, "[al:", 4)) snprintf(l->album, sizeof(l->album), "%.*s", (int)RG_MAX(0, (int)strlen(line) - 5), line + 4);
        else if (!strncmp(line, "[offset:", 8)) l->offset_ms = atoi(line + 8);
        else {
            char *text = line;
            size_t first = l->count;
            while (*text == '[') { int min = 0, sec = 0, frac = 0; char *close = strchr(text, ']');
                if (!close || sscanf(text, "[%d:%d.%d", &min, &sec, &frac) < 2) break;
                if (l->count < MEDIA_LYRIC_LINES_MAX) { while (frac > 999) frac /= 10; if (frac < 10) frac *= 100; else if (frac < 100) frac *= 10;
                    l->lines[l->count++] = (media_lyric_line_t){(uint32_t)(min * 60000 + sec * 1000 + frac), NULL}; }
                text = close + 1;
            }
            for (size_t i = first; i < l->count; i++) l->lines[i].text = text;
        }
        line = next;
    }
    for (size_t i = 0; i < l->count; i++) l->lines[i].time_ms = RG_MAX(0, (int64_t)l->lines[i].time_ms + l->offset_ms);
    qsort(l->lines, l->count, sizeof(*l->lines), lyric_compare);
    return l->count > 0;
}

void media_lyrics_free(media_lyrics_t *l)
{
    if (!l) return;
    free(l->lines);
    free(l->storage);
    memset(l, 0, sizeof(*l));
}

int media_lyrics_find(const media_lyrics_t *l, uint32_t ms)
{
    int lo = 0, hi = (int)l->count - 1, found = -1;
    while (lo <= hi) { int mid = lo + (hi - lo) / 2; if (l->lines[mid].time_ms <= ms) found = mid, lo = mid + 1; else hi = mid - 1; }
    return found;
}

void media_format_time(uint32_t ms, char *out, size_t size)
{
    uint32_t s = ms / 1000; snprintf(out, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}
