/**
 * Retro-Go media player - build-time configuration and runtime memory profiles.
 *
 * Feature flags may be overridden from the build system (see CMakeLists.txt) or from a
 * target's config.h. Everything defaults to "on" except formats whose decoders are not
 * vendored in the tree.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------------------- */
/* Feature flags                                                                            */
/* -------------------------------------------------------------------------------------- */

#ifndef MEDIA_PLAYER_ENABLE
#define MEDIA_PLAYER_ENABLE 1
#endif

#ifndef MEDIA_CODEC_WAV
#define MEDIA_CODEC_WAV 1
#endif

#ifndef MEDIA_CODEC_MP3
#define MEDIA_CODEC_MP3 1
#endif

#ifndef MEDIA_CODEC_FLAC
#define MEDIA_CODEC_FLAC 1
#endif

/* No decoder is vendored for these yet. The metadata parsers and the codec registry already
 * know about them, so enabling one only requires dropping in a codec_*.c implementation. */
#ifndef MEDIA_CODEC_AAC
#define MEDIA_CODEC_AAC 0
#endif

#ifndef MEDIA_CODEC_OGG
#define MEDIA_CODEC_OGG 0
#endif

#ifndef MEDIA_CODEC_OPUS
#define MEDIA_CODEC_OPUS 0
#endif

#ifndef MEDIA_EQ_ENABLE
#define MEDIA_EQ_ENABLE 1
#endif

#ifndef MEDIA_FFT_ENABLE
#define MEDIA_FFT_ENABLE 1
#endif

#ifndef MEDIA_LYRICS_ENABLE
#define MEDIA_LYRICS_ENABLE 1
#endif

#ifndef MEDIA_ARTWORK_ENABLE
#define MEDIA_ARTWORK_ENABLE 1
#endif

/* Performance overlay (FPS, underruns, buffer fill, heap). Costs ~1 KB of flash. */
#ifndef MEDIA_DEBUG_STATS
#define MEDIA_DEBUG_STATS 1
#endif

/* -------------------------------------------------------------------------------------- */
/* Hard limits - every parser validates against these before allocating                     */
/* -------------------------------------------------------------------------------------- */

#define MEDIA_MAX_PATH              255     // Excluding NUL. Also bounded by RG_PATH_MAX.
#define MEDIA_MAX_TAG_LEN           192     // Bytes kept per text tag (UTF-8, NUL terminated)
#define MEDIA_MAX_METADATA_BLOCK    (1024 * 1024) // Largest tag container we will walk
#define MEDIA_MAX_TAG_FRAME         (256 * 1024)  // Largest single tag frame we will read
#define MEDIA_MAX_ARTWORK_BYTES     (768 * 1024)  // Largest compressed image we will decode
#define MEDIA_MAX_ARTWORK_PIXELS    (4096 * 4096) // Sanity bound on decoded dimensions
#define MEDIA_MAX_LYRICS_BYTES      (192 * 1024)
#define MEDIA_MAX_LYRICS_LINES      2048
#define MEDIA_MAX_LYRIC_TEXT        192
#define MEDIA_MAX_PLAYLIST_ENTRIES  4096
#define MEDIA_MAX_QUEUE_ENTRIES     2048
#define MEDIA_MAX_SCAN_DEPTH        12
#define MEDIA_MAX_LIBRARY_TRACKS    20000

/* -------------------------------------------------------------------------------------- */
/* Audio pipeline constants                                                                 */
/* -------------------------------------------------------------------------------------- */

#define MEDIA_PCM_SAMPLE_RATE_MIN   8000
#define MEDIA_PCM_SAMPLE_RATE_MAX   48000
/* Every decoder normalises to interleaved signed 16-bit stereo at the track's native rate. */
#define MEDIA_PCM_CHANNELS          2
#define MEDIA_DECODE_BLOCK_FRAMES   1152    // One MP3 granule pair; also the EQ/limiter block

/* Number of PCM frames handed to rg_audio_submit() per iteration of the audio task. */
#define MEDIA_AUDIO_CHUNK_FRAMES    256

/* Fade ramp applied on start/stop/seek/pause to avoid clicks (milliseconds). */
#define MEDIA_FADE_MS               12

/* Visualiser sample tap. Power of two, holds the most recent mono samples. */
#define MEDIA_VIZ_TAP_SAMPLES       1024

/* -------------------------------------------------------------------------------------- */
/* Runtime memory profile                                                                   */
/* -------------------------------------------------------------------------------------- */

typedef enum
{
    MEDIA_MEMORY_LOW = 0,   // <= 2 MB PSRAM (N8R2) or no PSRAM
    MEDIA_MEMORY_NORMAL,    // 2 - 6 MB PSRAM
    MEDIA_MEMORY_HIGH,      // > 6 MB PSRAM (N16R8)
    MEDIA_MEMORY_COUNT,
} media_memory_profile_t;

typedef struct
{
    media_memory_profile_t profile;
    const char *name;

    size_t source_buffer;       // Compressed prefetch ring for a file on the card, bytes
    size_t network_buffer;      // Compressed prefetch ring for a URL, bytes
    size_t pcm_buffer_frames;   // Decoded PCM ring, frames
    size_t prebuffer_frames;    // PCM frames required before leaving BUFFERING
    uint32_t prebuffer_ms;      // Compressed audio to bank before starting a network stream

    size_t artwork_cache_bytes; // Total budget for decoded artwork surfaces
    int artwork_cache_entries;
    int artwork_max_dim;        // Longest edge kept for a full-size cover
    int thumbnail_dim;          // Longest edge kept for list thumbnails

    int fft_size;               // 128 / 256 / 512
    int fft_bands;
    int target_fps;

    bool background_blur;       // Album-art derived background on Now Playing
    bool crossfade_allowed;
    bool waveform_overview;
    bool particles;

    int library_cache_tracks;   // Resident index entries (the rest stays on SD)
} media_profile_t;

/** Resolve (once) and return the active memory profile. Safe to call from any task. */
const media_profile_t *media_profile(void);

/** Force a profile (used by the settings screen for testing / user override). */
void media_profile_override(media_memory_profile_t profile);
