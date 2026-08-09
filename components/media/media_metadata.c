#include <rg_system.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_metadata.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_META"

/* -------------------------------------------------------------------------------------- */
/* Small file reader                                                                        */
/* -------------------------------------------------------------------------------------- */

typedef struct
{
    FILE *fp;
    uint64_t size;
} mfile_t;

static bool mf_open(mfile_t *mf, const char *path)
{
    mf->fp = fopen(path, "rb");
    if (!mf->fp)
        return false;
    if (fseek(mf->fp, 0, SEEK_END) == 0)
    {
        long end = ftell(mf->fp);
        mf->size = end > 0 ? (uint64_t)end : 0;
    }
    else
    {
        mf->size = 0;
    }
    fseek(mf->fp, 0, SEEK_SET);
    return true;
}

static void mf_close(mfile_t *mf)
{
    if (mf->fp)
        fclose(mf->fp);
    mf->fp = NULL;
}

static bool mf_seek(mfile_t *mf, uint64_t offset)
{
    if (offset > mf->size)
        return false;
    return fseek(mf->fp, (long)offset, SEEK_SET) == 0;
}

static size_t mf_read(mfile_t *mf, void *buf, size_t len)
{
    return fread(buf, 1, len, mf->fp);
}

static uint64_t mf_tell(mfile_t *mf)
{
    long p = ftell(mf->fp);
    return p > 0 ? (uint64_t)p : 0;
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/** ID3 syncsafe integer: 7 bits per byte. */
static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/* -------------------------------------------------------------------------------------- */
/* Shared helpers                                                                           */
/* -------------------------------------------------------------------------------------- */

static void set_tag(char *dst, size_t dst_size, const char *value)
{
    if (!value || !*value)
        return;
    // First non-empty value wins; later duplicate frames are ignored.
    if (dst[0])
        return;
    media_utf8_copy(dst, dst_size, value);
    media_str_trim(dst);
    media_utf8_sanitize(dst, dst_size);
}

/** Parse "5" or "5/12" into a track/disc number. */
static uint16_t parse_number(const char *text)
{
    if (!text)
        return 0;
    while (*text && !isdigit((uint8_t)*text))
        text++;
    long v = strtol(text, NULL, 10);
    return (v > 0 && v < 65536) ? (uint16_t)v : 0;
}

static uint16_t parse_year(const char *text)
{
    if (!text)
        return 0;
    // Accept "1973", "1973-03-01" and "P1973"
    for (const char *p = text; *p; ++p)
    {
        if (isdigit((uint8_t)p[0]) && isdigit((uint8_t)p[1]) && isdigit((uint8_t)p[2]) &&
            isdigit((uint8_t)p[3]))
        {
            long v = strtol(p, NULL, 10) % 10000;
            return (v >= 1000 && v <= 2999) ? (uint16_t)v : 0;
        }
        if (!p[1] || !p[2] || !p[3])
            break;
    }
    return 0;
}

/** "-7.30 dB" -> -730 */
static int16_t parse_replaygain(const char *text)
{
    if (!text)
        return MEDIA_REPLAYGAIN_NONE;
    char *end = NULL;
    double db = strtod(text, &end);
    if (end == text || db < -60.0 || db > 60.0)
        return MEDIA_REPLAYGAIN_NONE;
    return (int16_t)(db * 100.0);
}

static bool text_looks_synced(const char *text)
{
    // A single "[mm:ss" is enough to treat the blob as LRC
    for (const char *p = text; *p; ++p)
    {
        if (p[0] == '[' && isdigit((uint8_t)p[1]) && isdigit((uint8_t)p[2]) && p[3] == ':')
            return true;
    }
    return false;
}

/* -------------------------------------------------------------------------------------- */
/* ID3v2                                                                                    */
/* -------------------------------------------------------------------------------------- */

/** Decode an ID3 text payload (leading encoding byte) into UTF-8. */
static void id3_decode_text(char *out, size_t out_size, const uint8_t *data, size_t len)
{
    out[0] = 0;
    if (len < 1)
        return;

    uint8_t encoding = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = len - 1;

    switch (encoding)
    {
    case 0: // ISO-8859-1
        media_latin1_to_utf8(out, out_size, payload, plen);
        break;
    case 1: // UTF-16 with BOM
        media_utf16_to_utf8(out, out_size, payload, plen, false);
        break;
    case 2: // UTF-16BE without BOM
        media_utf16_to_utf8(out, out_size, payload, plen, true);
        break;
    case 3: // UTF-8
    default:
    {
        size_t n = plen;
        if (n >= out_size)
            n = media_utf8_clip((const char *)payload, out_size - 1);
        memcpy(out, payload, n);
        out[n] = 0;
        media_utf8_sanitize(out, out_size);
        break;
    }
    }
}

static void id3_handle_txxx(media_metadata_t *meta, const uint8_t *data, size_t len)
{
    // TXXX = encoding, description, NUL, value. Used by ReplayGain and MusicBrainz.
    char buffer[128];
    id3_decode_text(buffer, sizeof(buffer), data, len);

    // id3_decode_text stops at the first NUL, so re-scan for the value ourselves.
    size_t desc_start = 1;
    uint8_t encoding = data[0];
    size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
    size_t i = desc_start;
    size_t value_start = 0;

    while (i + step <= len)
    {
        bool zero = true;
        for (size_t k = 0; k < step; ++k)
            zero = zero && data[i + k] == 0;
        if (zero)
        {
            value_start = i + step;
            break;
        }
        i += step;
    }

    if (!value_start || value_start >= len)
        return;

    char desc[64];
    uint8_t desc_buf[65];
    size_t desc_len = i - desc_start;
    if (desc_len > sizeof(desc_buf) - 2)
        desc_len = sizeof(desc_buf) - 2;
    desc_buf[0] = encoding;
    memcpy(desc_buf + 1, data + desc_start, desc_len);
    id3_decode_text(desc, sizeof(desc), desc_buf, desc_len + 1);

    char value[64];
    uint8_t value_buf[65];
    size_t value_len = len - value_start;
    if (value_len > sizeof(value_buf) - 2)
        value_len = sizeof(value_buf) - 2;
    value_buf[0] = encoding;
    memcpy(value_buf + 1, data + value_start, value_len);
    id3_decode_text(value, sizeof(value), value_buf, value_len + 1);

    if (strcasecmp(desc, "replaygain_track_gain") == 0)
        meta->replaygain_track = parse_replaygain(value);
    else if (strcasecmp(desc, "replaygain_album_gain") == 0)
        meta->replaygain_album = parse_replaygain(value);
}

static void id3_handle_frame(mfile_t *mf, media_metadata_t *meta, const char *id,
                             uint64_t frame_offset, uint32_t frame_size)
{
    // APIC and USLT can be large; record where they live and read them on demand instead.
    if (strcmp(id, "APIC") == 0 || strcmp(id, "PIC") == 0)
    {
        if (meta->has_embedded_art || frame_size < 4 || frame_size > MEDIA_MAX_ARTWORK_BYTES)
            return;
        meta->has_embedded_art = true;
        meta->art_offset = frame_offset;
        meta->art_length = frame_size;
        return;
    }
    if (strcmp(id, "USLT") == 0 || strcmp(id, "ULT") == 0 || strcmp(id, "SYLT") == 0)
    {
        if (meta->has_embedded_lyrics || frame_size < 5 || frame_size > MEDIA_MAX_LYRICS_BYTES)
            return;
        meta->has_embedded_lyrics = true;
        meta->lyrics_offset = frame_offset;
        meta->lyrics_length = frame_size;
        meta->lyrics_kind = (id[0] == 'S') ? 2 : 0;
        return;
    }

    if (frame_size < 2 || frame_size > 1024)
        return; // Text frames are never legitimately this large

    uint8_t buffer[1025];
    if (!mf_seek(mf, frame_offset) || mf_read(mf, buffer, frame_size) != frame_size)
        return;
    buffer[frame_size] = 0;

    char text[MEDIA_TAG_TITLE_LEN * 2];
    if (strcmp(id, "TXXX") == 0 || strcmp(id, "TXX") == 0)
    {
        id3_handle_txxx(meta, buffer, frame_size);
        return;
    }
    if (strcmp(id, "COMM") == 0 || strcmp(id, "COM") == 0)
    {
        // Skip the 3-byte language code and the description
        if (frame_size < 5)
            return;
        uint8_t encoding = buffer[0];
        size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
        size_t i = 4;
        while (i + step <= frame_size)
        {
            bool zero = true;
            for (size_t k = 0; k < step; ++k)
                zero = zero && buffer[i + k] == 0;
            i += step;
            if (zero)
                break;
        }
        if (i >= frame_size)
            return;
        uint8_t tmp[257];
        size_t n = frame_size - i;
        if (n > sizeof(tmp) - 2)
            n = sizeof(tmp) - 2;
        tmp[0] = encoding;
        memcpy(tmp + 1, buffer + i, n);
        id3_decode_text(text, sizeof(text), tmp, n + 1);
        set_tag(meta->comment, sizeof(meta->comment), text);
        return;
    }

    id3_decode_text(text, sizeof(text), buffer, frame_size);
    media_str_trim(text);
    if (!text[0])
        return;

    if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0)
        set_tag(meta->title, sizeof(meta->title), text);
    else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0)
        set_tag(meta->artist, sizeof(meta->artist), text);
    else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0)
        set_tag(meta->album, sizeof(meta->album), text);
    else if (strcmp(id, "TPE2") == 0 || strcmp(id, "TP2") == 0)
        set_tag(meta->album_artist, sizeof(meta->album_artist), text);
    else if (strcmp(id, "TCON") == 0 || strcmp(id, "TCO") == 0)
    {
        // ID3v1 genre references look like "(17)" or "(17)Rock"
        const char *g = text;
        if (g[0] == '(')
        {
            const char *close = strchr(g, ')');
            if (close && close[1])
                g = close + 1;
        }
        set_tag(meta->genre, sizeof(meta->genre), g);
    }
    else if (strcmp(id, "TCOM") == 0 || strcmp(id, "TCM") == 0)
        set_tag(meta->composer, sizeof(meta->composer), text);
    else if (strcmp(id, "TRCK") == 0 || strcmp(id, "TRK") == 0)
    {
        if (!meta->track_number)
            meta->track_number = parse_number(text);
    }
    else if (strcmp(id, "TPOS") == 0 || strcmp(id, "TPA") == 0)
    {
        if (!meta->disc_number)
            meta->disc_number = parse_number(text);
    }
    else if (strcmp(id, "TYER") == 0 || strcmp(id, "TDRC") == 0 || strcmp(id, "TDRL") == 0 ||
             strcmp(id, "TYE") == 0)
    {
        if (!meta->year)
            meta->year = parse_year(text);
    }
    else if (strcmp(id, "TLEN") == 0)
    {
        long ms = strtol(text, NULL, 10);
        if (ms > 0 && ms < 24 * 3600 * 1000)
            meta->duration_ms = (uint32_t)ms;
    }
}

/** Returns the total size of the ID3v2 container (0 if absent) and fills `meta`. */
static uint32_t id3v2_parse(mfile_t *mf, media_metadata_t *meta)
{
    uint8_t header[10];
    if (!mf_seek(mf, 0) || mf_read(mf, header, 10) != 10)
        return 0;
    if (memcmp(header, "ID3", 3) != 0 || header[3] == 0xFF || header[4] == 0xFF)
        return 0;

    uint8_t version = header[3];
    uint8_t flags = header[5];
    uint32_t tag_size = syncsafe32(header + 6);

    if (tag_size == 0 || tag_size > MEDIA_MAX_METADATA_BLOCK || tag_size > mf->size)
        return 10;

    uint64_t pos = 10;
    uint64_t end = 10 + (uint64_t)tag_size;

    if (flags & 0x40) // Extended header
    {
        uint8_t ext[4];
        if (mf_read(mf, ext, 4) != 4)
            return (uint32_t)end;
        uint32_t ext_size = version >= 4 ? syncsafe32(ext) : be32(ext) + 4;
        if (ext_size > tag_size)
            return (uint32_t)end;
        pos += ext_size;
    }

    const size_t id_len = version <= 2 ? 3 : 4;
    const size_t hdr_len = version <= 2 ? 6 : 10;
    int frames = 0;

    while (pos + hdr_len <= end && frames++ < 256)
    {
        uint8_t fh[10];
        if (!mf_seek(mf, pos) || mf_read(mf, fh, hdr_len) != hdr_len)
            break;
        if (fh[0] == 0) // Padding
            break;

        char id[5] = {0};
        memcpy(id, fh, id_len);
        for (size_t i = 0; i < id_len; ++i)
        {
            if (!isalnum((uint8_t)id[i]))
                return (uint32_t)end; // Not a frame ID: the tag is malformed, stop here
        }

        uint32_t frame_size;
        uint16_t frame_flags = 0;
        if (version <= 2)
        {
            frame_size = be24(fh + 3);
        }
        else if (version == 3)
        {
            frame_size = be32(fh + 4);
            frame_flags = be16(fh + 8);
        }
        else
        {
            // 2.4 is meant to be syncsafe, but plenty of encoders write plain 32-bit sizes.
            frame_size = syncsafe32(fh + 4);
            uint32_t plain = be32(fh + 4);
            if (frame_size > 0x7F && pos + hdr_len + frame_size > end && plain <= end - pos - hdr_len)
                frame_size = plain;
            frame_flags = be16(fh + 8);
        }

        uint64_t frame_data = pos + hdr_len;
        if (frame_size == 0 || frame_data + frame_size > end || frame_size > MEDIA_MAX_TAG_FRAME)
            break;

        // Compressed or encrypted frames are skipped rather than mis-parsed
        bool skip = (version == 3 && (frame_flags & 0x00C0)) || (version >= 4 && (frame_flags & 0x000C));
        if (!skip)
            id3_handle_frame(mf, meta, id, frame_data, frame_size);

        pos = frame_data + frame_size;
    }

    return (uint32_t)end;
}

static void id3v1_parse(mfile_t *mf, media_metadata_t *meta)
{
    if (mf->size < 128)
        return;

    uint8_t tag[128];
    if (!mf_seek(mf, mf->size - 128) || mf_read(mf, tag, 128) != 128)
        return;
    if (memcmp(tag, "TAG", 3) != 0)
        return;

    char text[64];
    media_latin1_to_utf8(text, sizeof(text), tag + 3, 30);
    set_tag(meta->title, sizeof(meta->title), media_str_trim(text));
    media_latin1_to_utf8(text, sizeof(text), tag + 33, 30);
    set_tag(meta->artist, sizeof(meta->artist), media_str_trim(text));
    media_latin1_to_utf8(text, sizeof(text), tag + 63, 30);
    set_tag(meta->album, sizeof(meta->album), media_str_trim(text));
    media_latin1_to_utf8(text, sizeof(text), tag + 93, 4);
    if (!meta->year)
        meta->year = parse_year(media_str_trim(text));
    // ID3v1.1 stores the track number in the last byte of the comment field
    if (!meta->track_number && tag[125] == 0 && tag[126])
        meta->track_number = tag[126];
}

/* -------------------------------------------------------------------------------------- */
/* MPEG frame geometry (for duration when no VBR header exists)                              */
/* -------------------------------------------------------------------------------------- */

static const int mp3_bitrates_v1l3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int mp3_bitrates_v2l3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const int mp3_rates[4] = {44100, 48000, 32000, 0};

static void mp3_parse_stream(mfile_t *mf, media_metadata_t *meta, uint32_t tag_size)
{
    uint8_t buf[2048];
    if (!mf_seek(mf, tag_size))
        return;
    size_t got = mf_read(mf, buf, sizeof(buf));
    if (got < 4)
        return;

    // Find the first frame header
    size_t i = 0;
    while (i + 4 <= got)
    {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0 && (buf[i + 1] & 0x06) != 0 &&
            (buf[i + 2] & 0xF0) != 0xF0 && (buf[i + 2] & 0x0C) != 0x0C)
            break;
        i++;
    }
    if (i + 4 > got)
        return;

    const uint8_t *h = buf + i;
    int version_bits = (h[1] >> 3) & 3;       // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    int rate_index = (h[2] >> 2) & 3;
    int bitrate_index = (h[2] >> 4) & 0x0F;
    int channel_mode = (h[3] >> 6) & 3;

    if (rate_index == 3 || version_bits == 1)
        return;

    int rate = mp3_rates[rate_index];
    if (version_bits == 2)
        rate /= 2;
    else if (version_bits == 0)
        rate /= 4;

    int kbps = version_bits == 3 ? mp3_bitrates_v1l3[bitrate_index] : mp3_bitrates_v2l3[bitrate_index];

    meta->sample_rate = (uint32_t)rate;
    meta->channels = channel_mode == 3 ? 1 : 2;
    meta->bits_per_sample = 16;
    meta->bitrate = (uint32_t)kbps * 1000;

    int samples_per_frame = version_bits == 3 ? 1152 : 576;

    // Xing/Info header inside this frame gives an exact frame count for VBR files
    size_t side_info = version_bits == 3 ? (meta->channels == 1 ? 17 : 32) : (meta->channels == 1 ? 9 : 17);
    size_t xing = i + 4 + side_info;
    uint32_t frame_count = 0;
    uint32_t byte_count = 0;

    if (xing + 12 <= got && (memcmp(buf + xing, "Xing", 4) == 0 || memcmp(buf + xing, "Info", 4) == 0))
    {
        uint32_t flags = be32(buf + xing + 4);
        size_t p = xing + 8;
        if ((flags & 1) && p + 4 <= got)
        {
            frame_count = be32(buf + p);
            p += 4;
        }
        if ((flags & 2) && p + 4 <= got)
            byte_count = be32(buf + p);
    }
    else if (i + 4 + 32 + 18 <= got && memcmp(buf + i + 4 + 32, "VBRI", 4) == 0)
    {
        byte_count = be32(buf + i + 4 + 32 + 10);
        frame_count = be32(buf + i + 4 + 32 + 14);
    }

    uint64_t audio_bytes = mf->size > tag_size ? mf->size - tag_size : 0;

    if (frame_count && rate)
    {
        uint64_t total_samples = (uint64_t)frame_count * samples_per_frame;
        meta->duration_ms = (uint32_t)((total_samples * 1000ULL) / (uint32_t)rate);
        if (byte_count && meta->duration_ms)
            meta->bitrate = (uint32_t)(((uint64_t)byte_count * 8000ULL) / meta->duration_ms);
    }
    else if (kbps > 0)
    {
        meta->duration_ms = (uint32_t)((audio_bytes * 8ULL) / (uint32_t)kbps);
    }
}

/* -------------------------------------------------------------------------------------- */
/* Vorbis comments (shared by FLAC and Ogg)                                                  */
/* -------------------------------------------------------------------------------------- */

static void vorbis_apply(media_metadata_t *meta, const char *key, const char *value)
{
    if (strcasecmp(key, "TITLE") == 0)
        set_tag(meta->title, sizeof(meta->title), value);
    else if (strcasecmp(key, "ARTIST") == 0)
        set_tag(meta->artist, sizeof(meta->artist), value);
    else if (strcasecmp(key, "ALBUM") == 0)
        set_tag(meta->album, sizeof(meta->album), value);
    else if (strcasecmp(key, "ALBUMARTIST") == 0 || strcasecmp(key, "ALBUM ARTIST") == 0)
        set_tag(meta->album_artist, sizeof(meta->album_artist), value);
    else if (strcasecmp(key, "GENRE") == 0)
        set_tag(meta->genre, sizeof(meta->genre), value);
    else if (strcasecmp(key, "COMPOSER") == 0)
        set_tag(meta->composer, sizeof(meta->composer), value);
    else if (strcasecmp(key, "COMMENT") == 0 || strcasecmp(key, "DESCRIPTION") == 0)
        set_tag(meta->comment, sizeof(meta->comment), value);
    else if (strcasecmp(key, "TRACKNUMBER") == 0)
    {
        if (!meta->track_number)
            meta->track_number = parse_number(value);
    }
    else if (strcasecmp(key, "DISCNUMBER") == 0)
    {
        if (!meta->disc_number)
            meta->disc_number = parse_number(value);
    }
    else if (strcasecmp(key, "DATE") == 0 || strcasecmp(key, "YEAR") == 0)
    {
        if (!meta->year)
            meta->year = parse_year(value);
    }
    else if (strcasecmp(key, "REPLAYGAIN_TRACK_GAIN") == 0)
        meta->replaygain_track = parse_replaygain(value);
    else if (strcasecmp(key, "REPLAYGAIN_ALBUM_GAIN") == 0)
        meta->replaygain_album = parse_replaygain(value);
}

/**
 * Walk a Vorbis comment block. `block_offset` points at the vendor-string length field.
 * Lyrics are recorded by position so the (potentially large) text is not held in RAM here.
 */
static void vorbis_comments_parse(mfile_t *mf, media_metadata_t *meta, uint64_t block_offset,
                                  uint32_t block_size)
{
    if (block_size < 8 || block_size > MEDIA_MAX_METADATA_BLOCK)
        return;
    if (!mf_seek(mf, block_offset))
        return;

    uint8_t len_buf[4];
    if (mf_read(mf, len_buf, 4) != 4)
        return;
    uint32_t vendor_len = le32(len_buf);
    uint64_t pos = block_offset + 4;
    if (vendor_len > block_size)
        return;
    pos += vendor_len;

    if (!mf_seek(mf, pos) || mf_read(mf, len_buf, 4) != 4)
        return;
    uint32_t count = le32(len_buf);
    pos += 4;

    if (count > 512)
        count = 512; // Bounded: a legitimate file never has this many comments

    char line[MEDIA_TAG_TITLE_LEN * 2 + 64];

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!mf_seek(mf, pos) || mf_read(mf, len_buf, 4) != 4)
            return;
        uint32_t entry_len = le32(len_buf);
        pos += 4;

        if (entry_len == 0 || pos + entry_len > block_offset + block_size)
            return;

        // Lyrics can be large; note the location and move on.
        if (entry_len > sizeof(line) - 1)
        {
            char probe[24] = {0};
            if (mf_read(mf, probe, sizeof(probe) - 1) == sizeof(probe) - 1)
            {
                char *eq = strchr(probe, '=');
                if (eq)
                {
                    *eq = 0;
                    if ((strcasecmp(probe, "LYRICS") == 0 || strcasecmp(probe, "UNSYNCEDLYRICS") == 0) &&
                        !meta->has_embedded_lyrics && entry_len <= MEDIA_MAX_LYRICS_BYTES)
                    {
                        size_t key_len = strlen(probe) + 1;
                        meta->has_embedded_lyrics = true;
                        meta->lyrics_offset = pos + key_len;
                        meta->lyrics_length = entry_len - (uint32_t)key_len;
                        meta->lyrics_encoding = 0xFF;
                    }
                }
            }
            pos += entry_len;
            continue;
        }

        if (mf_read(mf, line, entry_len) != entry_len)
            return;
        line[entry_len] = 0;
        pos += entry_len;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *value = eq + 1;
        media_str_trim(value);
        if (*value)
            vorbis_apply(meta, line, value);
    }
}

/* -------------------------------------------------------------------------------------- */
/* FLAC                                                                                     */
/* -------------------------------------------------------------------------------------- */

static void flac_parse(mfile_t *mf, media_metadata_t *meta, uint32_t start)
{
    uint8_t marker[4];
    if (!mf_seek(mf, start) || mf_read(mf, marker, 4) != 4 || memcmp(marker, "fLaC", 4) != 0)
        return;

    uint64_t pos = start + 4;
    int blocks = 0;

    while (pos + 4 <= mf->size && blocks++ < 64)
    {
        uint8_t bh[4];
        if (!mf_seek(mf, pos) || mf_read(mf, bh, 4) != 4)
            return;

        bool last = (bh[0] & 0x80) != 0;
        uint8_t type = bh[0] & 0x7F;
        uint32_t size = be24(bh + 1);
        uint64_t body = pos + 4;

        if (body + size > mf->size)
            return;

        if (type == 0 && size >= 34) // STREAMINFO
        {
            uint8_t si[34];
            if (mf_read(mf, si, 34) == 34)
            {
                uint32_t rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4);
                uint8_t channels = (uint8_t)(((si[12] >> 1) & 0x07) + 1);
                uint8_t bits = (uint8_t)((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
                uint64_t total = ((uint64_t)(si[13] & 0x0F) << 32) | ((uint64_t)si[14] << 24) |
                                 ((uint64_t)si[15] << 16) | ((uint64_t)si[16] << 8) | si[17];
                if (rate)
                {
                    meta->sample_rate = rate;
                    meta->channels = channels;
                    meta->bits_per_sample = bits;
                    meta->duration_ms = (uint32_t)((total * 1000ULL) / rate);
                    if (meta->duration_ms)
                        meta->bitrate = (uint32_t)((mf->size * 8000ULL) / meta->duration_ms);
                }
            }
        }
        else if (type == 4) // VORBIS_COMMENT
        {
            vorbis_comments_parse(mf, meta, body, size);
        }
        else if (type == 6 && size > 32 && !meta->has_embedded_art) // PICTURE
        {
            uint8_t head[8];
            if (mf_seek(mf, body) && mf_read(mf, head, 8) == 8)
            {
                uint32_t mime_len = be32(head + 4);
                if (mime_len <= 128)
                {
                    uint64_t p = body + 8 + mime_len;
                    uint8_t buf4[4];
                    if (mf_seek(mf, p) && mf_read(mf, buf4, 4) == 4)
                    {
                        uint32_t desc_len = be32(buf4);
                        if (desc_len <= 512)
                        {
                            // Skip description + width/height/depth/colours (4 x uint32)
                            p += 4 + desc_len + 16;
                            if (mf_seek(mf, p) && mf_read(mf, buf4, 4) == 4)
                            {
                                uint32_t data_len = be32(buf4);
                                if (data_len > 0 && data_len <= MEDIA_MAX_ARTWORK_BYTES &&
                                    p + 4 + data_len <= mf->size)
                                {
                                    meta->has_embedded_art = true;
                                    meta->art_offset = p + 4;
                                    meta->art_length = data_len;
                                }
                            }
                        }
                    }
                }
            }
        }

        pos = body + size;
        if (last)
            break;
    }
}

/* -------------------------------------------------------------------------------------- */
/* WAV                                                                                      */
/* -------------------------------------------------------------------------------------- */

static void wav_parse(mfile_t *mf, media_metadata_t *meta)
{
    uint8_t header[12];
    if (!mf_seek(mf, 0) || mf_read(mf, header, 12) != 12)
        return;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
        return;

    uint64_t pos = 12;
    uint32_t byte_rate = 0;
    int chunks = 0;

    while (pos + 8 <= mf->size && chunks++ < 64)
    {
        uint8_t ch[8];
        if (!mf_seek(mf, pos) || mf_read(mf, ch, 8) != 8)
            return;
        uint32_t size = le32(ch + 4);
        uint64_t body = pos + 8;
        if (body + size > mf->size)
            size = (uint32_t)(mf->size - body);

        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16)
        {
            uint8_t fmt[16];
            if (mf_read(mf, fmt, 16) == 16)
            {
                meta->channels = (uint8_t)media_clampi(le16(fmt + 2), 0, 255);
                meta->sample_rate = le32(fmt + 4);
                byte_rate = le32(fmt + 8);
                meta->bits_per_sample = (uint8_t)media_clampi(le16(fmt + 14), 0, 64);
            }
        }
        else if (memcmp(ch, "data", 4) == 0)
        {
            if (byte_rate)
                meta->duration_ms = (uint32_t)(((uint64_t)size * 1000ULL) / byte_rate);
            meta->bitrate = byte_rate * 8;
        }
        else if (memcmp(ch, "LIST", 4) == 0 && size >= 4)
        {
            uint8_t kind[4];
            if (mf_read(mf, kind, 4) == 4 && memcmp(kind, "INFO", 4) == 0)
            {
                uint64_t p = body + 4;
                uint64_t list_end = body + size;
                int entries = 0;
                while (p + 8 <= list_end && entries++ < 32)
                {
                    uint8_t ih[8];
                    if (!mf_seek(mf, p) || mf_read(mf, ih, 8) != 8)
                        break;
                    uint32_t ilen = le32(ih + 4);
                    if (ilen > 256 || p + 8 + ilen > list_end)
                        break;
                    char raw[257];
                    if (mf_read(mf, raw, ilen) != ilen)
                        break;
                    raw[ilen] = 0;
                    char text[128];
                    media_latin1_to_utf8(text, sizeof(text), (const uint8_t *)raw, ilen);
                    media_str_trim(text);

                    if (memcmp(ih, "INAM", 4) == 0)
                        set_tag(meta->title, sizeof(meta->title), text);
                    else if (memcmp(ih, "IART", 4) == 0)
                        set_tag(meta->artist, sizeof(meta->artist), text);
                    else if (memcmp(ih, "IPRD", 4) == 0)
                        set_tag(meta->album, sizeof(meta->album), text);
                    else if (memcmp(ih, "IGNR", 4) == 0)
                        set_tag(meta->genre, sizeof(meta->genre), text);
                    else if (memcmp(ih, "ICRD", 4) == 0 && !meta->year)
                        meta->year = parse_year(text);
                    else if (memcmp(ih, "ITRK", 4) == 0 && !meta->track_number)
                        meta->track_number = parse_number(text);

                    p += 8 + ilen + (ilen & 1);
                }
            }
        }

        pos = body + size + (size & 1);
    }
}

/* -------------------------------------------------------------------------------------- */
/* MP4 / M4A                                                                                */
/* -------------------------------------------------------------------------------------- */

static void mp4_read_ilst_value(mfile_t *mf, media_metadata_t *meta, const char *name,
                                uint64_t offset, uint32_t size)
{
    if (size < 16 || size > MEDIA_MAX_TAG_FRAME)
        return;

    uint8_t head[16];
    if (!mf_seek(mf, offset) || mf_read(mf, head, 16) != 16)
        return;
    if (memcmp(head + 4, "data", 4) != 0)
        return;

    uint32_t data_size = be32(head);
    uint32_t data_type = be32(head + 8) & 0x00FFFFFF;
    if (data_size < 16 || data_size > size)
        return;

    uint32_t payload = data_size - 16;
    uint64_t payload_offset = offset + 16;

    if (memcmp(name, "covr", 4) == 0)
    {
        if (!meta->has_embedded_art && payload > 0 && payload <= MEDIA_MAX_ARTWORK_BYTES)
        {
            meta->has_embedded_art = true;
            meta->art_offset = payload_offset;
            meta->art_length = payload;
        }
        return;
    }

    if (data_type == 0 && payload >= 2) // Binary: track/disc number pairs
    {
        uint8_t nums[8] = {0};
        uint32_t want = payload < sizeof(nums) ? payload : (uint32_t)sizeof(nums);
        if (mf_read(mf, nums, want) != want)
            return;
        uint16_t value = want >= 4 ? be16(nums + 2) : nums[0];
        if (memcmp(name, "trkn", 4) == 0 && !meta->track_number)
            meta->track_number = value;
        else if (memcmp(name, "disk", 4) == 0 && !meta->disc_number)
            meta->disc_number = value;
        return;
    }

    char text[MEDIA_TAG_TITLE_LEN * 2];
    uint32_t want = payload < sizeof(text) - 1 ? payload : (uint32_t)sizeof(text) - 1;
    if (mf_read(mf, text, want) != want)
        return;
    text[want] = 0;
    media_str_trim(text);
    media_utf8_sanitize(text, sizeof(text));
    if (!text[0])
        return;

    if (memcmp(name, "\xA9nam", 4) == 0)
        set_tag(meta->title, sizeof(meta->title), text);
    else if (memcmp(name, "\xA9""ART", 4) == 0)
        set_tag(meta->artist, sizeof(meta->artist), text);
    else if (memcmp(name, "\xA9""alb", 4) == 0)
        set_tag(meta->album, sizeof(meta->album), text);
    else if (memcmp(name, "aART", 4) == 0)
        set_tag(meta->album_artist, sizeof(meta->album_artist), text);
    else if (memcmp(name, "\xA9gen", 4) == 0)
        set_tag(meta->genre, sizeof(meta->genre), text);
    else if (memcmp(name, "\xA9wrt", 4) == 0)
        set_tag(meta->composer, sizeof(meta->composer), text);
    else if (memcmp(name, "\xA9""cmt", 4) == 0)
        set_tag(meta->comment, sizeof(meta->comment), text);
    else if (memcmp(name, "\xA9""day", 4) == 0 && !meta->year)
        meta->year = parse_year(text);
    else if (memcmp(name, "\xA9lyr", 4) == 0 && !meta->has_embedded_lyrics)
    {
        meta->has_embedded_lyrics = true;
        meta->lyrics_offset = payload_offset;
        meta->lyrics_length = payload;
        meta->lyrics_encoding = 0xFF;
        meta->lyrics_kind = text_looks_synced(text) ? 1 : 0;
    }
}

/** Recursively walk MP4 atoms, depth limited. */
static void mp4_walk(mfile_t *mf, media_metadata_t *meta, uint64_t start, uint64_t end, int depth)
{
    if (depth > 6)
        return;

    uint64_t pos = start;
    int atoms = 0;

    while (pos + 8 <= end && atoms++ < 128)
    {
        uint8_t hdr[8];
        if (!mf_seek(mf, pos) || mf_read(mf, hdr, 8) != 8)
            return;

        uint64_t size = be32(hdr);
        uint64_t body = pos + 8;

        if (size == 1) // 64-bit size
        {
            uint8_t ext[8];
            if (mf_read(mf, ext, 8) != 8)
                return;
            size = ((uint64_t)be32(ext) << 32) | be32(ext + 4);
            body += 8;
        }
        else if (size == 0)
        {
            size = end - pos;
        }

        if (size < (body - pos) || pos + size > end)
            return;

        const char *type = (const char *)hdr + 4;
        uint64_t body_end = pos + size;

        if (memcmp(type, "moov", 4) == 0 || memcmp(type, "udta", 4) == 0 ||
            memcmp(type, "trak", 4) == 0 || memcmp(type, "mdia", 4) == 0 ||
            memcmp(type, "minf", 4) == 0 || memcmp(type, "stbl", 4) == 0)
        {
            mp4_walk(mf, meta, body, body_end, depth + 1);
        }
        else if (memcmp(type, "meta", 4) == 0)
        {
            mp4_walk(mf, meta, body + 4, body_end, depth + 1); // 4-byte version/flags
        }
        else if (memcmp(type, "ilst", 4) == 0)
        {
            uint64_t p = body;
            int items = 0;
            while (p + 8 <= body_end && items++ < 64)
            {
                uint8_t ih[8];
                if (!mf_seek(mf, p) || mf_read(mf, ih, 8) != 8)
                    break;
                uint32_t isize = be32(ih);
                if (isize < 8 || p + isize > body_end)
                    break;
                mp4_read_ilst_value(mf, meta, (const char *)ih + 4, p + 8, isize - 8);
                p += isize;
            }
        }
        else if (memcmp(type, "mvhd", 4) == 0 && size >= 32)
        {
            uint8_t mv[28];
            if (mf_read(mf, mv, sizeof(mv)) == sizeof(mv))
            {
                uint32_t timescale, duration;
                if (mv[0] == 0) // version 0: 32-bit fields
                {
                    timescale = be32(mv + 12);
                    duration = be32(mv + 16);
                }
                else
                {
                    timescale = be32(mv + 20);
                    duration = be32(mv + 28 - 4);
                }
                if (timescale)
                    meta->duration_ms = (uint32_t)(((uint64_t)duration * 1000ULL) / timescale);
            }
        }
        else if (memcmp(type, "stsd", 4) == 0 && size >= 8 + 8 + 28)
        {
            uint8_t sd[44];
            if (mf_read(mf, sd, sizeof(sd)) == sizeof(sd))
            {
                // entry starts at +8; the audio sample entry has channels/bits/rate at +16
                meta->channels = (uint8_t)media_clampi(be16(sd + 8 + 16), 0, 255);
                meta->bits_per_sample = (uint8_t)media_clampi(be16(sd + 8 + 18), 0, 64);
                meta->sample_rate = be16(sd + 8 + 24); // 16.16 fixed, integer part
            }
        }

        pos = body_end;
    }
}

/* -------------------------------------------------------------------------------------- */
/* Public entry points                                                                      */
/* -------------------------------------------------------------------------------------- */

bool media_metadata_read(const char *path, media_metadata_t *out)
{
    RG_ASSERT_ARG(path && out);

    memset(out, 0, sizeof(*out));
    out->replaygain_track = MEDIA_REPLAYGAIN_NONE;
    out->replaygain_album = MEDIA_REPLAYGAIN_NONE;
    out->lyrics_encoding = 0xFF;
    out->codec = media_codec_from_path(path);

    mfile_t mf = {0};
    if (!mf_open(&mf, path))
        return false;

    if (mf.size < 16)
    {
        mf_close(&mf);
        return false;
    }

    uint32_t id3_size = id3v2_parse(&mf, out);

    switch (out->codec)
    {
    case MEDIA_CODEC_TYPE_MP3:
        mp3_parse_stream(&mf, out, id3_size);
        id3v1_parse(&mf, out);
        break;

    case MEDIA_CODEC_TYPE_FLAC:
        flac_parse(&mf, out, id3_size);
        break;

    case MEDIA_CODEC_TYPE_WAV:
        wav_parse(&mf, out);
        break;

    case MEDIA_CODEC_TYPE_AAC:
        mp4_walk(&mf, out, 0, mf.size, 0);
        break;

    case MEDIA_CODEC_TYPE_OGG:
    case MEDIA_CODEC_TYPE_OPUS:
        // The Ogg page structure is only walked far enough to find the comment header, which
        // lives in the second page of the logical stream.
        {
            uint8_t page[64];
            uint64_t pos = 0;
            for (int i = 0; i < 8 && pos + 27 < mf.size; ++i)
            {
                if (!mf_seek(&mf, pos) || mf_read(&mf, page, 27) != 27)
                    break;
                if (memcmp(page, "OggS", 4) != 0)
                    break;
                uint8_t segments = page[26];
                uint8_t table[255];
                if (mf_read(&mf, table, segments) != segments)
                    break;
                uint32_t body = 0;
                for (int s = 0; s < segments; ++s)
                    body += table[s];
                uint64_t body_offset = pos + 27 + segments;

                uint8_t probe[8] = {0};
                if (mf_seek(&mf, body_offset) && mf_read(&mf, probe, 8) == 8)
                {
                    if (memcmp(probe + 1, "vorbis", 6) == 0 && probe[0] == 3)
                        vorbis_comments_parse(&mf, out, body_offset + 7, body);
                    else if (memcmp(probe, "OpusTags", 8) == 0)
                        vorbis_comments_parse(&mf, out, body_offset + 8, body);
                }
                pos = body_offset + body;
            }
        }
        break;

    default:
        break;
    }

    mf_close(&mf);

    // Sanity: a duration longer than a day means we mis-parsed something.
    if (out->duration_ms > 24u * 3600u * 1000u)
        out->duration_ms = 0;

    return true;
}

void media_metadata_apply(media_track_t *track, const media_metadata_t *meta)
{
    RG_ASSERT_ARG(track && meta);

    // Title: tag, then the filename without its extension.
    if (meta->title[0])
        media_utf8_copy(track->title, sizeof(track->title), meta->title);
    else
        media_path_stem(track->title, sizeof(track->title), track->path);

    // Artist: tag, then album artist, then "Unknown Artist" (left empty; the UI substitutes).
    if (meta->artist[0])
        media_utf8_copy(track->artist, sizeof(track->artist), meta->artist);
    else if (meta->album_artist[0])
        media_utf8_copy(track->artist, sizeof(track->artist), meta->album_artist);
    else
        track->artist[0] = 0;

    // Album: tag, then the parent folder name.
    if (meta->album[0])
    {
        media_utf8_copy(track->album, sizeof(track->album), meta->album);
    }
    else
    {
        const char *dir = rg_dirname(track->path);
        media_utf8_copy(track->album, sizeof(track->album), rg_basename(dir));
    }

    media_utf8_copy(track->album_artist, sizeof(track->album_artist),
                    meta->album_artist[0] ? meta->album_artist : track->artist);
    media_utf8_copy(track->genre, sizeof(track->genre), meta->genre);

    track->year = meta->year;
    track->track_number = meta->track_number;
    track->disc_number = meta->disc_number;
    track->duration_ms = meta->duration_ms;
    track->sample_rate = meta->sample_rate;
    track->bitrate = meta->bitrate;
    track->channels = meta->channels;
    track->bits_per_sample = meta->bits_per_sample;
    track->codec = (uint8_t)meta->codec;
    track->replaygain_track = meta->replaygain_track;
    track->replaygain_album = meta->replaygain_album;
    track->has_embedded_art = meta->has_embedded_art;
    track->has_lyrics = meta->has_embedded_lyrics;
    track->metadata_parsed = 1;
    track->gapless_ok = (meta->codec == MEDIA_CODEC_TYPE_FLAC || meta->codec == MEDIA_CODEC_TYPE_WAV);

    // A stable album identity even when the album artist is missing
    char key[MEDIA_TAG_ALBUM_LEN + MEDIA_TAG_ARTIST_LEN + 2];
    int n = snprintf(key, sizeof(key), "%s|%s", track->album_artist, track->album);
    track->album_hash = (n > 0) ? rg_hash(key, (size_t)n) : 0;
}

uint8_t *media_metadata_read_artwork(const char *path, const media_metadata_t *meta, size_t *len_out)
{
    if (len_out)
        *len_out = 0;
    if (!path || !meta || !meta->has_embedded_art)
        return NULL;
    if (meta->art_length == 0 || meta->art_length > MEDIA_MAX_ARTWORK_BYTES)
        return NULL;

    mfile_t mf = {0};
    if (!mf_open(&mf, path))
        return NULL;

    uint8_t *data = NULL;
    uint32_t length = meta->art_length;
    uint64_t offset = meta->art_offset;

    if (offset + length <= mf.size && mf_seek(&mf, offset))
    {
        // ID3 APIC frames carry a variable-length header before the image bytes.
        if (meta->codec == MEDIA_CODEC_TYPE_MP3)
        {
            uint8_t head[256];
            size_t got = mf_read(&mf, head, length < sizeof(head) ? length : sizeof(head));
            size_t skip = 0;
            if (got > 4)
            {
                uint8_t encoding = head[0];
                size_t i = 1;
                // MIME type (Latin-1, NUL terminated) or a 3-byte image format in ID3v2.2
                while (i < got && head[i])
                    i++;
                i++; // NUL
                if (i < got)
                    i++; // picture type
                // Description, in the frame's text encoding
                size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
                while (i + step <= got)
                {
                    bool zero = true;
                    for (size_t k = 0; k < step; ++k)
                        zero = zero && head[i + k] == 0;
                    i += step;
                    if (zero)
                        break;
                }
                skip = i;
            }
            if (skip >= length)
            {
                mf_close(&mf);
                return NULL;
            }
            offset += skip;
            length -= (uint32_t)skip;
            mf_seek(&mf, offset);
        }

        data = rg_alloc(length, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
        if (data)
        {
            size_t got = mf_read(&mf, data, length);
            if (got < 8)
            {
                free(data);
                data = NULL;
            }
            else if (len_out)
            {
                *len_out = got;
            }
        }
    }

    mf_close(&mf);
    return data;
}

char *media_metadata_read_lyrics(const char *path, const media_metadata_t *meta, bool *is_synced_out)
{
    if (is_synced_out)
        *is_synced_out = false;
    if (!path || !meta || !meta->has_embedded_lyrics)
        return NULL;
    if (meta->lyrics_length == 0 || meta->lyrics_length > MEDIA_MAX_LYRICS_BYTES)
        return NULL;

    mfile_t mf = {0};
    if (!mf_open(&mf, path))
        return NULL;

    char *text = NULL;
    uint32_t length = meta->lyrics_length;
    uint64_t offset = meta->lyrics_offset;

    if (offset + length <= mf.size && mf_seek(&mf, offset))
    {
        uint8_t *raw = rg_alloc(length + 2, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
        if (raw)
        {
            size_t got = mf_read(&mf, raw, length);
            raw[got] = 0;
            raw[got + 1] = 0;

            if (meta->lyrics_encoding == 0xFF)
            {
                // Already UTF-8 (Vorbis comment / MP4)
                text = (char *)raw;
                raw = NULL;
                media_utf8_sanitize(text, got + 1);
            }
            else if (got > 5)
            {
                // ID3 USLT: encoding byte, 3-byte language, description, NUL, text
                uint8_t encoding = raw[0];
                size_t step = (encoding == 1 || encoding == 2) ? 2 : 1;
                size_t i = 4;
                while (i + step <= got)
                {
                    bool zero = true;
                    for (size_t k = 0; k < step; ++k)
                        zero = zero && raw[i + k] == 0;
                    i += step;
                    if (zero)
                        break;
                }

                if (i < got)
                {
                    size_t body = got - i;
                    size_t capacity = body * 4 + 4; // Worst case UTF-16 -> UTF-8 expansion
                    text = rg_alloc(capacity, MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
                    if (text)
                    {
                        if (encoding == 0)
                            media_latin1_to_utf8(text, capacity, raw + i, body);
                        else if (encoding == 1)
                            media_utf16_to_utf8(text, capacity, raw + i, body, false);
                        else if (encoding == 2)
                            media_utf16_to_utf8(text, capacity, raw + i, body, true);
                        else
                        {
                            size_t n = body < capacity - 1 ? body : capacity - 1;
                            memcpy(text, raw + i, n);
                            text[n] = 0;
                            media_utf8_sanitize(text, capacity);
                        }
                    }
                }
            }
            free(raw);
        }
    }

    mf_close(&mf);

    if (text && is_synced_out)
        *is_synced_out = meta->lyrics_kind != 0 || text_looks_synced(text);

    return text;
}

media_art_source_t media_metadata_find_artwork(const char *track_path, const media_metadata_t *meta,
                                               char *out_path, size_t out_size)
{
    if (out_path && out_size)
        out_path[0] = 0;

    if (meta && meta->has_embedded_art)
        return MEDIA_ART_EMBEDDED;

    if (!track_path || !out_path || !out_size)
        return MEDIA_ART_NONE;

    static const char *extensions[] = {"jpg", "jpeg", "png"};

    // 1. <track>.jpg beside the file
    for (size_t i = 0; i < RG_COUNT(extensions); ++i)
    {
        char candidate[MEDIA_MAX_PATH + 1];
        if (media_path_swap_ext(candidate, sizeof(candidate), track_path, extensions[i]) &&
            rg_storage_exists(candidate))
        {
            media_utf8_copy(out_path, out_size, candidate);
            return MEDIA_ART_SIDECAR;
        }
    }

    // 2. Well-known folder covers, in the order most players use
    static const char *names[] = {"cover", "folder", "front", "album", "albumart", "AlbumArt"};
    const char *dir = rg_dirname(track_path);

    for (size_t n = 0; n < RG_COUNT(names); ++n)
    {
        for (size_t i = 0; i < RG_COUNT(extensions); ++i)
        {
            char candidate[MEDIA_MAX_PATH + 1];
            char filename[64];
            snprintf(filename, sizeof(filename), "%s.%s", names[n], extensions[i]);
            if (media_path_join(candidate, sizeof(candidate), dir, filename) &&
                rg_storage_exists(candidate))
            {
                media_utf8_copy(out_path, out_size, candidate);
                return MEDIA_ART_FOLDER;
            }
        }
    }

    // 3. <album folder name>.jpg inside the album folder
    {
        char candidate[MEDIA_MAX_PATH + 1];
        char filename[96];
        const char *folder_name = rg_basename(dir);
        for (size_t i = 0; i < RG_COUNT(extensions); ++i)
        {
            snprintf(filename, sizeof(filename), "%s.%s", folder_name, extensions[i]);
            if (media_path_join(candidate, sizeof(candidate), dir, filename) &&
                rg_storage_exists(candidate))
            {
                media_utf8_copy(out_path, out_size, candidate);
                return MEDIA_ART_FOLDER;
            }
        }
    }

    return MEDIA_ART_NONE;
}
