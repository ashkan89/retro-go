#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rg_system.h>

#define MEDIA_TEXT_MAX 128
#define MEDIA_LYRIC_LINES_MAX 512

/* AAC/M4A/FLAC/WAV support pulls extra decoder libraries into the launcher
   image. Set to 0 for an MP3-only build if flash space ever gets tight. */
#ifndef MEDIA_ENABLE_EXTRA_CODECS
#define MEDIA_ENABLE_EXTRA_CODECS 1
#endif

typedef enum {
    MEDIA_FORMAT_UNKNOWN = 0,
    MEDIA_FORMAT_MP3,
    MEDIA_FORMAT_AAC,
    MEDIA_FORMAT_M4A,
    MEDIA_FORMAT_FLAC,
    MEDIA_FORMAT_WAV,
} media_format_t;

typedef struct {
    char title[MEDIA_TEXT_MAX];
    char artist[MEDIA_TEXT_MAX];
    char album[MEDIA_TEXT_MAX];
    char genre[48];
    char year[16];
    char track[16];
    char comment[MEDIA_TEXT_MAX];
    char lyrics[512];
    char cover_mime[24];
    uint32_t cover_offset;
    uint32_t cover_size;
    uint32_t audio_offset;
    uint32_t audio_size;
    uint32_t duration_ms;
    uint32_t bitrate;
    uint32_t sample_rate;
    media_format_t format;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t id3_version;
    /* Xing/Info seek table. Without it, seeking a VBR MP3 by byte ratio can be
       off by tens of seconds. */
    bool has_toc;
    uint8_t toc[100];
} media_metadata_t;

typedef struct {
    uint32_t time_ms;
    char *text;
} media_lyric_line_t;

typedef struct {
    media_lyric_line_t *lines;
    size_t count;
    char *storage;
    char title[MEDIA_TEXT_MAX];
    char artist[MEDIA_TEXT_MAX];
    char album[MEDIA_TEXT_MAX];
    int32_t offset_ms;
} media_lyrics_t;

media_format_t media_format_from_path(const char *path);
const char *media_format_name(media_format_t format);
/* Space separated extension list accepted by the library browser. */
const char *media_format_extensions(void);

bool media_metadata_read(const char *path, media_metadata_t *meta, bool scan_audio);
/* Only formats we can restart mid-stream advertise seek support. */
bool media_metadata_seekable(const media_metadata_t *meta);
/* Byte offset to start reading from for a given playback position. */
uint32_t media_metadata_seek_offset(const media_metadata_t *meta, uint32_t position_ms);

rg_surface_t *media_metadata_load_cover(const char *media_path, const media_metadata_t *meta,
                                        int target_width, int target_height);
bool media_lyrics_load(const char *media_path, media_lyrics_t *lyrics);
void media_lyrics_free(media_lyrics_t *lyrics);
int media_lyrics_find(const media_lyrics_t *lyrics, uint32_t position_ms);
void media_format_time(uint32_t milliseconds, char *buffer, size_t size);
