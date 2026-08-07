#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rg_system.h>

#define MEDIA_TEXT_MAX 128
#define MEDIA_LYRIC_LINES_MAX 512

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
    uint8_t channels;
    uint8_t id3_version;
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

bool media_metadata_read(const char *path, media_metadata_t *meta, bool scan_audio);
rg_surface_t *media_metadata_load_cover(const char *mp3_path, const media_metadata_t *meta,
                                        int target_width, int target_height);
bool media_lyrics_load(const char *mp3_path, media_lyrics_t *lyrics);
void media_lyrics_free(media_lyrics_t *lyrics);
int media_lyrics_find(const media_lyrics_t *lyrics, uint32_t position_ms);
void media_format_time(uint32_t milliseconds, char *buffer, size_t size);

