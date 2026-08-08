#include "media_metadata.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t le16(const uint8_t *p)
{
    return ((uint32_t)p[1] << 8) | p[0];
}

/*******************************************************************************
 * Format detection
 ******************************************************************************/

media_format_t media_format_from_path(const char *path)
{
    if (rg_extension_match(path, "mp3"))
        return MEDIA_FORMAT_MP3;
#if MEDIA_ENABLE_EXTRA_CODECS
    if (rg_extension_match(path, "m4a mp4 m4b"))
        return MEDIA_FORMAT_M4A;
    if (rg_extension_match(path, "aac adts"))
        return MEDIA_FORMAT_AAC;
    if (rg_extension_match(path, "flac"))
        return MEDIA_FORMAT_FLAC;
    if (rg_extension_match(path, "wav wave"))
        return MEDIA_FORMAT_WAV;
#endif
    return MEDIA_FORMAT_UNKNOWN;
}

const char *media_format_name(media_format_t format)
{
    switch (format) {
    case MEDIA_FORMAT_MP3: return "MP3";
    case MEDIA_FORMAT_AAC: return "AAC";
    case MEDIA_FORMAT_M4A: return "M4A";
    case MEDIA_FORMAT_FLAC: return "FLAC";
    case MEDIA_FORMAT_WAV: return "WAV";
    default: return "?";
    }
}

const char *media_format_extensions(void)
{
#if MEDIA_ENABLE_EXTRA_CODECS
    return "mp3 m4a mp4 m4b aac adts flac wav wave";
#else
    return "mp3";
#endif
}

bool media_metadata_seekable(const media_metadata_t *meta)
{
    /* The container formats need their header to make sense of the stream, so
       we can only restart them from the beginning. MP3 is a bare frame stream
       and the decoder resynchronises wherever we drop it. */
    return meta && meta->format == MEDIA_FORMAT_MP3 && meta->duration_ms > 0;
}

uint32_t media_metadata_seek_offset(const media_metadata_t *meta, uint32_t position_ms)
{
    if (!meta || !position_ms || !meta->duration_ms)
        return meta ? meta->audio_offset : 0;

    if (position_ms > meta->duration_ms)
        position_ms = meta->duration_ms;

    if (meta->has_toc) {
        /* The Xing TOC maps 100 evenly spaced time points onto a 0-255 range
           of the file size. Interpolating between entries keeps VBR seeks
           within a frame or two of the requested position. */
        float percent = 100.0f * position_ms / meta->duration_ms;
        if (percent > 99.0f)
            percent = 99.0f;
        int index = (int)percent;
        float fraction = percent - index;
        float a = meta->toc[index];
        float b = index < 99 ? meta->toc[index + 1] : 256.0f;
        float point = a + (b - a) * fraction;
        return meta->audio_offset + (uint32_t)(point / 256.0f * meta->audio_size);
    }

    return meta->audio_offset + (uint32_t)((uint64_t)meta->audio_size * position_ms / meta->duration_ms);
}

/*******************************************************************************
 * Text decoding
 ******************************************************************************/

static size_t utf8_put(char *out, size_t room, uint32_t cp)
{
    if (cp < 0x80 && room >= 1) out[0] = cp, out[1] = 0, room = 1;
    else if (cp < 0x800 && room >= 2) out[0] = 0xc0 | (cp >> 6), out[1] = 0x80 | (cp & 63), out[2] = 0, room = 2;
    else if (cp < 0x10000 && room >= 3) out[0] = 0xe0 | (cp >> 12), out[1] = 0x80 | ((cp >> 6) & 63), out[2] = 0x80 | (cp & 63), out[3] = 0, room = 3;
    else if (room >= 4) out[0] = 0xf0 | (cp >> 18), out[1] = 0x80 | ((cp >> 12) & 63), out[2] = 0x80 | ((cp >> 6) & 63), out[3] = 0x80 | (cp & 63), out[4] = 0, room = 4;
    else return 0;
    return room;
}

static void trim_trailing(char *out, size_t used)
{
    while (used && (out[used - 1] == ' ' || out[used - 1] == '\r' || out[used - 1] == '\n' || out[used - 1] == '\t'))
        out[--used] = 0;
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
        memcpy(out, data, copy);
        out[copy] = 0;
        /* Frames are often NUL padded; strlen gives the real length so the
           trailing whitespace trim below actually has something to work on. */
        used = strlen(out);
    } else {
        /* ISO-8859-1 */
        while (len-- && used + 2 < out_size && *data) {
            uint32_t cp = *data++;
            used += utf8_put(out + used, out_size - used - 1, cp);
        }
    }
    trim_trailing(out, used);
}

static void set_text(char *out, size_t out_size, const char *value, size_t len)
{
    size_t copy = RG_MIN(len, out_size - 1);
    memcpy(out, value, copy);
    out[copy] = 0;
    trim_trailing(out, strlen(out));
}

/*******************************************************************************
 * ID3v2
 ******************************************************************************/

static void parse_apic(media_metadata_t *m, const uint8_t *p, size_t n, uint32_t file_offset, int version)
{
    const uint8_t *base = p;
    if (n < 5) return;
    int enc = *p++; n--;
    const uint8_t *mime = p;
    size_t mime_len = version == 2 ? RG_MIN(n, (size_t)3) : strnlen((const char *)p, n);
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
    m->cover_offset = file_offset + (uint32_t)(p - base);
    m->cover_size = n;
}

static void parse_frames(FILE *fp, media_metadata_t *m, uint32_t tag_size, int version, uint8_t tag_flags)
{
    uint32_t pos = 10, end = 10 + tag_size;
    uint8_t head[10];

    if ((tag_flags & 0x40) && pos + 4 < end) {
        if (!fseek(fp, pos, SEEK_SET) && fread(head, 1, 4, fp) == 4) {
            uint32_t extended = version == 4 ? synchsafe(head) : be32(head) + 4;
            if (extended < tag_size) pos += extended;
        }
    }

    while (pos + (version == 2 ? 6u : 10u) <= end) {
        int hs = version == 2 ? 6 : 10;
        if (fseek(fp, pos, SEEK_SET) || fread(head, 1, hs, fp) != (size_t)hs || head[0] == 0) break;
        char id[5] = {0};
        memcpy(id, head, version == 2 ? 3 : 4);
        uint32_t size = version == 2 ? ((uint32_t)head[3] << 16) | ((uint32_t)head[4] << 8) | head[5]
                                     : (version == 4 ? synchsafe(head + 4) : be32(head + 4));
        uint8_t frame_flags = version == 2 ? 0 : head[9];
        pos += hs;
        if (!size || size > end - pos) break;

        bool wanted = !strcmp(id, "TIT2") || !strcmp(id, "TT2") || !strcmp(id, "TPE1") || !strcmp(id, "TP1") ||
                      !strcmp(id, "TALB") || !strcmp(id, "TAL") || !strcmp(id, "TCON") || !strcmp(id, "TCO") ||
                      !strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TRCK") ||
                      !strcmp(id, "TRK") || !strcmp(id, "COMM") || !strcmp(id, "COM") || !strcmp(id, "USLT") ||
                      !strcmp(id, "ULT") || !strcmp(id, "APIC") || !strcmp(id, "PIC");
        /* Compressed (0x08) and encrypted (0x04) frames are not something we
           can make sense of, so skip them rather than emit garbage. */
        bool readable = !(frame_flags & 0x0C);

        if (wanted && readable && size <= 1024 * 1024) {
            uint8_t *buf = malloc(size + 1);
            if (buf && fread(buf, 1, size, fp) == size) {
                uint8_t *data = buf;
                uint32_t length = size;
                buf[size] = 0;

                /* ID3v2.4 data length indicator: four extra bytes in front of
                   the payload that used to be decoded as text. */
                if ((frame_flags & 0x01) && length > 4) {
                    data += 4;
                    length -= 4;
                }
                /* Per-frame unsynchronisation: 0xFF 0x00 pairs collapse to 0xFF. */
                if (frame_flags & 0x02) {
                    uint32_t out = 0;
                    for (uint32_t i = 0; i < length; i++) {
                        data[out++] = data[i];
                        if (data[i] == 0xFF && i + 1 < length && data[i + 1] == 0x00)
                            i++;
                    }
                    length = out;
                }

                if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) decode_text(m->title, sizeof(m->title), data, length);
                else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) decode_text(m->artist, sizeof(m->artist), data, length);
                else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) decode_text(m->album, sizeof(m->album), data, length);
                else if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) decode_text(m->genre, sizeof(m->genre), data, length);
                else if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) decode_text(m->year, sizeof(m->year), data, length);
                else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) decode_text(m->track, sizeof(m->track), data, length);
                else if (!strcmp(id, "APIC") || !strcmp(id, "PIC")) parse_apic(m, data, length, pos + (uint32_t)(data - buf), version);
                else if (length > 4) {
                    /* COMM/USLT: encoding + language + description + value */
                    size_t skip = 4;
                    if (data[0] == 1 || data[0] == 2) while (skip + 1 < length && (data[skip] || data[skip + 1])) skip += 2;
                    else while (skip < length && data[skip]) skip++;
                    skip += (data[0] == 1 || data[0] == 2) ? 2 : 1;
                    if (skip < length) {
                        uint8_t *text = malloc(length - skip + 1);
                        if (text) {
                            bool is_lyrics = !strcmp(id, "USLT") || !strcmp(id, "ULT");
                            text[0] = data[0];
                            memcpy(text + 1, data + skip, length - skip);
                            decode_text(is_lyrics ? m->lyrics : m->comment,
                                        is_lyrics ? sizeof(m->lyrics) : sizeof(m->comment),
                                        text, length - skip + 1);
                            free(text);
                        }
                    }
                }
            }
            free(buf);
        }
        pos += size;
    }
}

/*******************************************************************************
 * MP3
 ******************************************************************************/

static bool parse_mp3_header(const uint8_t *h, uint32_t *rate, uint32_t *bitrate, uint8_t *channels)
{
    static const uint16_t br1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const uint32_t rates[3] = {44100,48000,32000};
    /* Layer bits live in h[1] & 0x06: 0 reserved, 2 = Layer III, 4 = II, 6 = I.
       Both bitrate tables below are Layer III tables, so Layer III is the only
       thing we can decode -- and it is what "MP3" means. This used to reject
       ==2, i.e. exactly the layer we want, so no frame ever matched and every
       file ended up with a zero bitrate and a zero duration. */
    if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || (h[1] & 6) != 2) return false;
    int ver = (h[1] >> 3) & 3, bri = h[2] >> 4, sri = (h[2] >> 2) & 3;
    if (ver == 1 || !bri || bri == 15 || sri == 3) return false;
    *rate = rates[sri] >> (ver == 2 ? 1 : ver == 0 ? 2 : 0);
    *bitrate = 1000u * (ver == 3 ? br1[bri] : br2[bri]);
    *channels = ((h[3] >> 6) == 3) ? 1 : 2;
    return true;
}

#define MP3_SCAN_WINDOW 4096

static void scan_mp3_stream(FILE *fp, media_metadata_t *m)
{
    /* Deliberately not on the stack. This runs deep inside the browser call
       chain (tab event -> directory scan -> scroll event -> preview load) on a
       task with a 16 KB stack that also has to fit a JPEG decode afterwards.
       Four kilobytes of locals here was enough to blow the canary.
       Static is safe because every media_metadata_read() caller is the
       launcher UI thread; the player's worker tasks never parse tags. */
    static uint8_t window[MP3_SCAN_WINDOW];

    fseek(fp, m->audio_offset, SEEK_SET);
    size_t got = fread(window, 1, sizeof(window), fp);

    for (size_t i = 0; i + 4 <= got; i++) {
        if (!parse_mp3_header(window + i, &m->sample_rate, &m->bitrate, &m->channels))
            continue;

        int version = (window[i + 1] >> 3) & 3;
        int crc = (window[i + 1] & 1) ? 0 : 2;
        int side = version == 3 ? (m->channels == 1 ? 17 : 32) : (m->channels == 1 ? 9 : 17);
        uint32_t samples_per_frame = version == 3 ? 1152 : 576;
        uint32_t frame_bytes = m->sample_rate ? (samples_per_frame / 8 * m->bitrate / m->sample_rate) : 0;

        size_t xing = i + 4 + crc + side;
        if (xing + 12 <= got && (!memcmp(window + xing, "Xing", 4) || !memcmp(window + xing, "Info", 4))) {
            uint32_t flags = be32(window + xing + 4), cursor = xing + 8;
            uint32_t frames = 0;
            if ((flags & 0x0001) && cursor + 4 <= got) { frames = be32(window + cursor); cursor += 4; }
            if ((flags & 0x0002) && cursor + 4 <= got) { cursor += 4; /* byte count */ }
            if ((flags & 0x0004) && cursor + 100 <= got) {
                memcpy(m->toc, window + cursor, 100);
                m->has_toc = true;
                cursor += 100;
            }
            if (frames && m->sample_rate)
                m->duration_ms = (uint64_t)frames * samples_per_frame * 1000 / m->sample_rate;
            /* The Xing/Info frame itself decodes to silence, so hand the
               decoder the frame after it. */
            if (frame_bytes && i + frame_bytes < got) {
                m->audio_offset += i + frame_bytes;
                m->audio_size = m->audio_size > i + frame_bytes ? m->audio_size - (i + frame_bytes) : 0;
            }
        } else {
            size_t vbri = i + 4 + 32;
            if (vbri + 26 <= got && !memcmp(window + vbri, "VBRI", 4)) {
                uint32_t frames = be32(window + vbri + 14);
                if (frames && m->sample_rate)
                    m->duration_ms = (uint64_t)frames * samples_per_frame * 1000 / m->sample_rate;
                if (frame_bytes && i + frame_bytes < got) {
                    m->audio_offset += i + frame_bytes;
                    m->audio_size = m->audio_size > i + frame_bytes ? m->audio_size - (i + frame_bytes) : 0;
                }
            } else if (i) {
                /* Junk before the first frame confuses byte-ratio seeking. */
                m->audio_offset += i;
                m->audio_size = m->audio_size > i ? m->audio_size - i : 0;
            }
        }

        if (m->duration_ms) {
            /* Report the average rate for VBR files; the per-frame value we
               read above is only true for the first frame. */
            m->bitrate = (uint64_t)m->audio_size * 8000 / m->duration_ms;
        } else if (m->bitrate) {
            /* Constant bitrate estimate. Never let this overwrite a duration
               we already derived from a real frame count. */
            m->duration_ms = (uint64_t)m->audio_size * 8000 / m->bitrate;
        }
        return;
    }
}

/*******************************************************************************
 * FLAC
 ******************************************************************************/

static void parse_vorbis_comment(media_metadata_t *m, const uint8_t *data, size_t len)
{
    if (len < 4) return;
    uint32_t vendor = le32(data);
    size_t pos = 4;
    if (vendor > len - pos) return;
    pos += vendor;
    if (pos + 4 > len) return;
    uint32_t count = le32(data + pos);
    pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= len; i++) {
        uint32_t length = le32(data + pos);
        pos += 4;
        if (length > len - pos) break;
        const char *entry = (const char *)data + pos;
        const char *equals = memchr(entry, '=', length);
        pos += length;
        if (!equals) continue;
        size_t key_len = equals - entry;
        const char *value = equals + 1;
        size_t value_len = length - key_len - 1;
        if (!value_len) continue;
        #define FIELD(name, dest) (key_len == strlen(name) && !strncasecmp(entry, name, key_len) ? \
                                   (set_text(dest, sizeof(dest), value, value_len), true) : false)
        if (FIELD("TITLE", m->title)) continue;
        if (FIELD("ARTIST", m->artist)) continue;
        if (FIELD("ALBUM", m->album)) continue;
        if (FIELD("GENRE", m->genre)) continue;
        if (FIELD("DATE", m->year)) continue;
        if (FIELD("TRACKNUMBER", m->track)) continue;
        if (FIELD("COMMENT", m->comment)) continue;
        #undef FIELD
    }
}

static void read_flac(FILE *fp, media_metadata_t *m, long total)
{
    uint8_t head[4];
    uint32_t pos = 4;
    m->audio_offset = 4;

    while (fseek(fp, pos, SEEK_SET) == 0 && fread(head, 1, 4, fp) == 4) {
        bool last = head[0] & 0x80;
        int type = head[0] & 0x7f;
        uint32_t size = ((uint32_t)head[1] << 16) | ((uint32_t)head[2] << 8) | head[3];
        pos += 4;
        if (size > (uint32_t)total)
            break;

        if (type == 0 && size >= 18) { /* STREAMINFO */
            uint8_t info[18];
            if (fread(info, 1, sizeof(info), fp) == sizeof(info)) {
                m->sample_rate = ((uint32_t)info[10] << 12) | ((uint32_t)info[11] << 4) | (info[12] >> 4);
                m->channels = ((info[12] >> 1) & 7) + 1;
                m->bits_per_sample = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1;
                uint64_t samples = ((uint64_t)(info[13] & 0x0f) << 32) | ((uint64_t)info[14] << 24) |
                                   ((uint64_t)info[15] << 16) | ((uint64_t)info[16] << 8) | info[17];
                if (samples && m->sample_rate)
                    m->duration_ms = samples * 1000 / m->sample_rate;
            }
        } else if (type == 4 && size <= 256 * 1024) { /* VORBIS_COMMENT */
            uint8_t *buffer = malloc(size);
            if (buffer && fread(buffer, 1, size, fp) == size)
                parse_vorbis_comment(m, buffer, size);
            free(buffer);
        } else if (type == 6 && size >= 32) { /* PICTURE */
            /* type(4) mime_len(4) mime desc_len(4) desc w(4) h(4) depth(4)
               colours(4) data_len(4) data -- all offsets relative to `pos`. */
            uint8_t field[4];
            uint32_t cursor = 4;
            if (fseek(fp, pos + cursor, SEEK_SET) == 0 && fread(field, 1, 4, fp) == 4) {
                uint32_t mime_len = be32(field);
                cursor += 4;
                if (mime_len < 64 && cursor + mime_len + 4 < size) {
                    char mime[64] = "";
                    if (fread(mime, 1, mime_len, fp) == mime_len) {
                        mime[mime_len] = 0;
                        snprintf(m->cover_mime, sizeof(m->cover_mime), "%s", mime);
                    }
                    cursor += mime_len;
                    if (fread(field, 1, 4, fp) == 4) {
                        uint32_t desc_len = be32(field);
                        cursor += 4 + desc_len + 16;
                        if (cursor + 4 < size && fseek(fp, pos + cursor, SEEK_SET) == 0 &&
                            fread(field, 1, 4, fp) == 4) {
                            uint32_t data_len = be32(field);
                            if (data_len && cursor + 4 + data_len <= size) {
                                m->cover_size = data_len;
                                m->cover_offset = pos + cursor + 4;
                            }
                        }
                    }
                }
            }
        }

        pos += size;
        m->audio_offset = pos;
        if (last)
            break;
    }

    m->audio_size = total > (long)m->audio_offset ? total - m->audio_offset : 0;
    if (m->duration_ms && m->audio_size)
        m->bitrate = (uint64_t)m->audio_size * 8000 / m->duration_ms;
    /* The container carries its own header, so playback always starts at 0. */
    m->audio_offset = 0;
    m->audio_size = total;
}

/*******************************************************************************
 * WAV
 ******************************************************************************/

static void read_wav(FILE *fp, media_metadata_t *m, long total)
{
    uint8_t header[12];
    if (fseek(fp, 0, SEEK_SET) || fread(header, 1, sizeof(header), fp) != sizeof(header))
        return;
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
        return;

    uint32_t pos = 12, byte_rate = 0, data_size = 0;
    uint8_t chunk[8];
    while (pos + 8 <= (uint32_t)total && fseek(fp, pos, SEEK_SET) == 0 && fread(chunk, 1, 8, fp) == 8) {
        uint32_t size = le32(chunk + 4);
        pos += 8;
        if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), fp) == sizeof(fmt)) {
                m->channels = le16(fmt + 2);
                m->sample_rate = le32(fmt + 4);
                byte_rate = le32(fmt + 8);
                m->bits_per_sample = le16(fmt + 14);
            }
        } else if (!memcmp(chunk, "data", 4)) {
            data_size = size;
            if (!size || size > (uint32_t)total - pos)
                data_size = (uint32_t)total - pos;
            break;
        } else if (!memcmp(chunk, "LIST", 4) && size <= 64 * 1024 && size >= 4) {
            uint8_t *list = malloc(size);
            if (list && fread(list, 1, size, fp) == size && !memcmp(list, "INFO", 4)) {
                uint32_t cursor = 4;
                while (cursor + 8 <= size) {
                    uint32_t entry = le32(list + cursor + 4);
                    const char *value = (const char *)list + cursor + 8;
                    if (entry > size - cursor - 8) break;
                    if (!memcmp(list + cursor, "INAM", 4)) set_text(m->title, sizeof(m->title), value, entry);
                    else if (!memcmp(list + cursor, "IART", 4)) set_text(m->artist, sizeof(m->artist), value, entry);
                    else if (!memcmp(list + cursor, "IPRD", 4)) set_text(m->album, sizeof(m->album), value, entry);
                    else if (!memcmp(list + cursor, "IGNR", 4)) set_text(m->genre, sizeof(m->genre), value, entry);
                    else if (!memcmp(list + cursor, "ICRD", 4)) set_text(m->year, sizeof(m->year), value, entry);
                    else if (!memcmp(list + cursor, "ICMT", 4)) set_text(m->comment, sizeof(m->comment), value, entry);
                    cursor += 8 + entry + (entry & 1);
                }
            }
            free(list);
        }
        pos += size + (size & 1);
    }

    if (!byte_rate && m->sample_rate && m->channels && m->bits_per_sample)
        byte_rate = m->sample_rate * m->channels * (m->bits_per_sample / 8);
    if (byte_rate && data_size)
        m->duration_ms = (uint64_t)data_size * 1000 / byte_rate;
    m->bitrate = byte_rate * 8;
    m->audio_offset = 0;
    m->audio_size = total;
}

/*******************************************************************************
 * MP4 / M4A
 ******************************************************************************/

static void read_mp4_ilst(FILE *fp, media_metadata_t *m, uint32_t pos, uint32_t end)
{
    uint8_t head[8];
    while (pos + 8 <= end && fseek(fp, pos, SEEK_SET) == 0 && fread(head, 1, 8, fp) == 8) {
        uint32_t size = be32(head);
        if (size < 8 || pos + size > end)
            break;
        /* Each item wraps its value in a 'data' atom: 8 byte header, 4 byte
           type, 4 byte locale, then the payload. */
        uint32_t payload = size - 8;
        if (payload > 16 && payload <= 64 * 1024) {
            uint8_t *buffer = malloc(payload);
            if (buffer && fread(buffer, 1, payload, fp) == payload && !memcmp(buffer + 4, "data", 4)) {
                uint32_t type = be32(buffer + 8);
                const char *value = (const char *)buffer + 16;
                uint32_t length = payload - 16;
                /* The "\xa9" prefix is split from the name so the compiler
                   does not swallow following hex digits into the escape. */
                if (!memcmp(head + 4, "\xa9" "nam", 4)) set_text(m->title, sizeof(m->title), value, length);
                else if (!memcmp(head + 4, "\xa9" "ART", 4)) set_text(m->artist, sizeof(m->artist), value, length);
                else if (!memcmp(head + 4, "\xa9" "alb", 4)) set_text(m->album, sizeof(m->album), value, length);
                else if (!memcmp(head + 4, "\xa9" "gen", 4)) set_text(m->genre, sizeof(m->genre), value, length);
                else if (!memcmp(head + 4, "\xa9" "day", 4)) set_text(m->year, sizeof(m->year), value, length);
                else if (!memcmp(head + 4, "trkn", 4) && length >= 4)
                    snprintf(m->track, sizeof(m->track), "%u", ((unsigned)value[2] << 8) | (unsigned char)value[3]);
                else if (!memcmp(head + 4, "covr", 4) && length > 8) {
                    /* type 13 = JPEG, 14 = PNG */
                    m->cover_offset = pos + 8 + 16;
                    m->cover_size = length;
                    snprintf(m->cover_mime, sizeof(m->cover_mime), "%s", type == 14 ? "image/png" : "image/jpeg");
                }
            }
            free(buffer);
        }
        pos += size;
    }
}

static void read_mp4_atoms(FILE *fp, media_metadata_t *m, uint32_t pos, uint32_t end, int depth)
{
    if (depth > 6)
        return;
    uint8_t head[8];
    while (pos + 8 <= end && fseek(fp, pos, SEEK_SET) == 0 && fread(head, 1, 8, fp) == 8) {
        uint64_t size = be32(head);
        if (size == 1) {
            uint8_t large[8];
            if (fread(large, 1, 8, fp) != 8) break;
            size = ((uint64_t)be32(large) << 32) | be32(large + 4);
        }
        if (size < 8 || pos + size > end)
            break;

        if (!memcmp(head + 4, "moov", 4) || !memcmp(head + 4, "udta", 4) || !memcmp(head + 4, "trak", 4) ||
            !memcmp(head + 4, "mdia", 4)) {
            read_mp4_atoms(fp, m, pos + 8, pos + (uint32_t)size, depth + 1);
        } else if (!memcmp(head + 4, "meta", 4)) {
            /* 'meta' carries a 4 byte version/flags field before children. */
            read_mp4_atoms(fp, m, pos + 12, pos + (uint32_t)size, depth + 1);
        } else if (!memcmp(head + 4, "ilst", 4)) {
            read_mp4_ilst(fp, m, pos + 8, pos + (uint32_t)size);
        } else if (!memcmp(head + 4, "mvhd", 4) && size >= 32) {
            /* version 0: flags(4) created(4) modified(4) timescale(4) duration(4)
               version 1: flags(4) created(8) modified(8) timescale(4) duration(8) */
            uint8_t mvhd[32];
            if (fread(mvhd, 1, sizeof(mvhd), fp) == sizeof(mvhd)) {
                int version = mvhd[0];
                uint32_t timescale = version ? be32(mvhd + 20) : be32(mvhd + 12);
                uint64_t duration = version ? (((uint64_t)be32(mvhd + 24) << 32) | be32(mvhd + 28))
                                            : be32(mvhd + 16);
                if (timescale && (!version || size >= 40))
                    m->duration_ms = duration * 1000 / timescale;
            }
        }
        pos += (uint32_t)size;
    }
}

static void read_mp4(FILE *fp, media_metadata_t *m, long total)
{
    read_mp4_atoms(fp, m, 0, (uint32_t)total, 0);
    m->audio_offset = 0;
    m->audio_size = total;
    if (m->duration_ms)
        m->bitrate = (uint64_t)total * 8000 / m->duration_ms;
}

/*******************************************************************************
 * Entry point
 ******************************************************************************/

bool media_metadata_read(const char *path, media_metadata_t *m, bool scan_audio)
{
    memset(m, 0, sizeof(*m));
    m->format = media_format_from_path(path);

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;

    fseek(fp, 0, SEEK_END);
    long total = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (total <= 0) {
        fclose(fp);
        return false;
    }

    if (m->format == MEDIA_FORMAT_FLAC) {
        uint8_t magic[4];
        if (fread(magic, 1, 4, fp) == 4 && !memcmp(magic, "fLaC", 4))
            read_flac(fp, m, total);
    } else if (m->format == MEDIA_FORMAT_WAV) {
        read_wav(fp, m, total);
    } else if (m->format == MEDIA_FORMAT_M4A) {
        read_mp4(fp, m, total);
    } else {
        /* MP3 and raw AAC both start with an optional ID3v2 tag. */
        uint8_t h[10];
        if (fread(h, 1, sizeof(h), fp) == sizeof(h) && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
            m->id3_version = h[3];
            uint32_t tag = synchsafe(h + 6);
            m->audio_offset = 10 + tag + ((h[5] & 0x10) ? 10 : 0);
            parse_frames(fp, m, tag, h[3], h[5]);
        }
        if (m->audio_offset > (uint32_t)total)
            m->audio_offset = 0;
        m->audio_size = total > (long)m->audio_offset ? total - m->audio_offset : 0;

        if (total >= 128) {
            uint8_t tag[128];
            fseek(fp, total - 128, SEEK_SET);
            if (fread(tag, 1, sizeof(tag), fp) == sizeof(tag) && !memcmp(tag, "TAG", 3)) {
                char temp[64];
#define ID3V1_COPY(dest, offset, length) do { memcpy(temp, tag + (offset), (length)); temp[(length)] = 0; \
                for (int z = (length) - 1; z >= 0 && (temp[z] == 0 || temp[z] == ' '); z--) temp[z] = 0; \
                if (!(dest)[0]) { size_t copy = RG_MIN(strlen(temp), sizeof(dest) - 1); \
                    memcpy((dest), temp, copy); (dest)[copy] = 0; } } while (0)
                ID3V1_COPY(m->title, 3, 30); ID3V1_COPY(m->artist, 33, 30); ID3V1_COPY(m->album, 63, 30); ID3V1_COPY(m->year, 93, 4);
#undef ID3V1_COPY
                /* An almost-empty file with only a tag would wrap around. */
                m->audio_size = m->audio_size > 128 ? m->audio_size - 128 : 0;
            }
        }

        if (scan_audio && m->format == MEDIA_FORMAT_MP3)
            scan_mp3_stream(fp, m);
    }

    fclose(fp);

    if (!m->title[0]) {
        snprintf(m->title, sizeof(m->title), "%s", rg_basename(path));
        char *dot = strrchr(m->title, '.');
        if (dot) *dot = 0;
    }
    return true;
}

/*******************************************************************************
 * Cover art
 ******************************************************************************/

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
    /* A large cover must never take the whole launcher down with it. */
    cfg.outbuf = rg_alloc(info.output_len, MEM_SLOW | MEM_NOPANIC);
    cfg.outbuf_size = info.output_len;
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
        uint8_t *data = rg_alloc(m->cover_size, MEM_SLOW | MEM_NOPANIC);
        if (fp && data && !fseek(fp, m->cover_offset, SEEK_SET) && fread(data, 1, m->cover_size, fp) == m->cover_size) {
            rg_surface_t *img = load_cover_data(data, m->cover_size, w, h);
            free(data);
            fclose(fp);
            if (img) return img;
        } else {
            free(data);
            if (fp) fclose(fp);
        }
    }
    char dir[RG_PATH_MAX + 1], candidate[RG_PATH_MAX + 1];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    static const char *names[] = {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.png",
                                  "album.jpg", "album.png", "front.jpg", "front.png"};
    for (size_t i = 0; i < RG_COUNT(names); i++) {
        if (!join_path(candidate, sizeof(candidate), dir, names[i])) continue;
        void *data; size_t len;
        if (rg_storage_read_file(candidate, &data, &len, 0)) {
            rg_surface_t *img = load_cover_data(data, len, w, h);
            free(data);
            if (img) return img;
        }
    }
    return NULL;
}

/*******************************************************************************
 * Lyrics
 ******************************************************************************/

static int lyric_compare(const void *a, const void *b)
{
    const media_lyric_line_t *aa = a, *bb = b;
    return aa->time_ms < bb->time_ms ? -1 : aa->time_ms > bb->time_ms;
}

bool media_lyrics_load(const char *media_path, media_lyrics_t *l)
{
    media_lyrics_free(l);
    char path[RG_PATH_MAX + 1];
    snprintf(path, sizeof(path), "%s", media_path);
    char *dot = strrchr(path, '.');
    if (!dot) return false;
    snprintf(dot, path + sizeof(path) - dot, ".lrc");
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
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else
        snprintf(out, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}
