/**
 * Retro-Go media player - shared value types.
 *
 * These types are copied between tasks, so they hold no pointers into task-owned storage
 * unless explicitly documented. Strings are fixed-size UTF-8 buffers to keep the library
 * index free of per-track allocations.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_config.h"

/* -------------------------------------------------------------------------------------- */

typedef enum
{
    MEDIA_CODEC_NONE = 0,
    MEDIA_CODEC_TYPE_WAV,
    MEDIA_CODEC_TYPE_MP3,
    MEDIA_CODEC_TYPE_FLAC,
    MEDIA_CODEC_TYPE_AAC,
    MEDIA_CODEC_TYPE_OGG,
    MEDIA_CODEC_TYPE_OPUS,
    MEDIA_CODEC_TYPE_COUNT,
} media_codec_t;

typedef enum
{
    MEDIA_STATE_STOPPED = 0,
    MEDIA_STATE_LOADING,
    MEDIA_STATE_BUFFERING,
    MEDIA_STATE_PLAYING,
    MEDIA_STATE_PAUSED,
    MEDIA_STATE_SEEKING,
    MEDIA_STATE_ENDED,
    MEDIA_STATE_ERROR,
    MEDIA_STATE_COUNT,
} media_state_t;

typedef enum
{
    MEDIA_REPEAT_OFF = 0,
    MEDIA_REPEAT_TRACK,
    MEDIA_REPEAT_FOLDER,
    MEDIA_REPEAT_ALL,
    MEDIA_REPEAT_COUNT,
} media_repeat_t;

typedef enum
{
    MEDIA_NORMALIZE_OFF = 0,
    MEDIA_NORMALIZE_TRACK,
    MEDIA_NORMALIZE_ALBUM,
    MEDIA_NORMALIZE_COUNT,
} media_normalize_t;

typedef enum
{
    MEDIA_BACKGROUND_OFF = 0,
    MEDIA_BACKGROUND_LAUNCHER,
    MEDIA_BACKGROUND_ALWAYS,
    MEDIA_BACKGROUND_COUNT,
} media_background_t;

typedef enum
{
    MEDIA_RESUME_OFF = 0,
    MEDIA_RESUME_TRACK,
    MEDIA_RESUME_POSITION,
    MEDIA_RESUME_COUNT,
} media_resume_t;

/** Who currently owns the shared I2S output. */
typedef enum
{
    MEDIA_AUDIO_OWNER_NONE = 0,
    MEDIA_AUDIO_OWNER_PLAYER,
    MEDIA_AUDIO_OWNER_EMULATOR,
    MEDIA_AUDIO_OWNER_SYSTEM,
} media_audio_owner_t;

typedef enum
{
    MEDIA_EVENT_NONE = 0,
    MEDIA_EVENT_TRACK_CHANGED,
    MEDIA_EVENT_STATE_CHANGED,
    MEDIA_EVENT_METADATA_READY,
    MEDIA_EVENT_ARTWORK_READY,
    MEDIA_EVENT_LYRICS_READY,
    MEDIA_EVENT_POSITION,
    MEDIA_EVENT_BUFFERING,
    MEDIA_EVENT_ERROR,
    MEDIA_EVENT_SD_REMOVED,
    MEDIA_EVENT_LIBRARY_PROGRESS,
    MEDIA_EVENT_LIBRARY_READY,
    MEDIA_EVENT_COUNT,
} media_event_t;

typedef enum
{
    MEDIA_OK = 0,
    MEDIA_ERR_NOTFOUND,
    MEDIA_ERR_UNSUPPORTED,
    MEDIA_ERR_CORRUPT,
    MEDIA_ERR_IO,
    MEDIA_ERR_NOMEM,
    MEDIA_ERR_BUSY,
    MEDIA_ERR_ABORTED,
    MEDIA_ERR_COUNT,
} media_err_t;

/* -------------------------------------------------------------------------------------- */
/* Track description                                                                        */
/* -------------------------------------------------------------------------------------- */

#define MEDIA_TAG_TITLE_LEN   96
#define MEDIA_TAG_ARTIST_LEN  72
#define MEDIA_TAG_ALBUM_LEN   72
#define MEDIA_TAG_SHORT_LEN   40

/**
 * One indexed track. Kept deliberately POD and fixed-size: the library writes these straight
 * to /media/.retrogo-media/library.idx and reads them back by record number, so a track can
 * be resolved without holding the whole library in RAM.
 */
typedef struct
{
    uint32_t id;                                // 1-based record number, 0 = invalid
    uint32_t path_hash;                         // rg_hash of the full path, for fast identity
    uint32_t dir_hash;                          // rg_hash of the containing folder
    uint32_t album_hash;                        // rg_hash of "album_artist\0album", 0 if unknown

    char path[MEDIA_MAX_PATH + 1];              // Absolute, UTF-8

    char title[MEDIA_TAG_TITLE_LEN];
    char artist[MEDIA_TAG_ARTIST_LEN];
    char album[MEDIA_TAG_ALBUM_LEN];
    char album_artist[MEDIA_TAG_ARTIST_LEN];
    char genre[MEDIA_TAG_SHORT_LEN];

    uint16_t year;
    uint16_t track_number;
    uint16_t disc_number;

    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t bitrate;                           // Average, bits/s. 0 if unknown.
    uint32_t file_size;
    uint32_t mtime;

    int16_t replaygain_track;                   // dB * 100, INT16_MIN if absent
    int16_t replaygain_album;                   // dB * 100, INT16_MIN if absent

    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t codec;                              // media_codec_t

    uint8_t has_embedded_art : 1;
    uint8_t has_lyrics : 1;
    uint8_t favorite : 1;
    uint8_t metadata_parsed : 1;
    uint8_t gapless_ok : 1;
    uint8_t reserved_bits : 3;

    uint16_t play_count;
    uint16_t skip_count;
    uint32_t last_played;                       // Unix time
    uint32_t last_position_ms;                  // For resume of long files
} media_track_t;

#define MEDIA_REPLAYGAIN_NONE INT16_MIN

/** Coarse identity of a folder/album grouping, produced by the index. */
typedef struct
{
    uint32_t hash;
    char name[MEDIA_TAG_ALBUM_LEN];
    char artist[MEDIA_TAG_ARTIST_LEN];
    uint16_t year;
    uint16_t track_count;
    uint32_t first_track_id;                    // Used to locate cover art cheaply
} media_group_t;

/* -------------------------------------------------------------------------------------- */
/* Player snapshot - the UI's only view of the audio pipeline                                */
/* -------------------------------------------------------------------------------------- */

typedef struct
{
    media_state_t state;
    media_err_t last_error;

    uint32_t track_id;
    uint32_t generation;        // Bumped on every track change; UI uses it to invalidate caches

    uint32_t position_ms;
    uint32_t duration_ms;

    uint32_t sample_rate;
    uint32_t bitrate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t codec;

    uint8_t volume;
    uint8_t brightness;

    bool shuffle;
    media_repeat_t repeat;
    bool favorite;
    bool muted;

    uint8_t pcm_fill_pct;
    uint8_t src_fill_pct;
    uint32_t underruns;

    float rms_left, rms_right;
    float peak_left, peak_right;

    int queue_index;
    int queue_length;
    uint32_t next_track_id;

    uint32_t sleep_remaining_s;
} media_snapshot_t;

/* -------------------------------------------------------------------------------------- */

typedef void (*media_event_cb_t)(media_event_t event, intptr_t arg, void *user);

const char *media_codec_name(media_codec_t codec);
const char *media_state_name(media_state_t state);
const char *media_error_name(media_err_t err);
