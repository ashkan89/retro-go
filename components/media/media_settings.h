/**
 * Retro-Go media player - persistent settings.
 *
 * Everything here is small and lives in retro-go's own settings store (NVS-backed JSON).
 * Anything large - the index, artwork, waveforms - is kept on the SD card instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "media_eq.h"
#include "media_types.h"

typedef enum
{
    MEDIA_VIZ_SPECTRUM = 0,
    MEDIA_VIZ_MIRRORED,
    MEDIA_VIZ_WAVEFORM,
    MEDIA_VIZ_CIRCULAR,
    MEDIA_VIZ_VU,
    MEDIA_VIZ_PEAK,
    MEDIA_VIZ_OSCILLOSCOPE,
    MEDIA_VIZ_PARTICLES,
    MEDIA_VIZ_ART_PULSE,
    MEDIA_VIZ_VINYL,
    MEDIA_VIZ_CLOCK,
    MEDIA_VIZ_MINIMAL,
    MEDIA_VIZ_COUNT,
} media_viz_t;

typedef enum
{
    MEDIA_PAGE_LIBRARY = 0,
    MEDIA_PAGE_NOW_PLAYING,
    MEDIA_PAGE_LYRICS,
    MEDIA_PAGE_VISUALIZER,
    MEDIA_PAGE_QUEUE,
    MEDIA_PAGE_INFO,
    MEDIA_PAGE_COUNT,
} media_page_t;

typedef struct
{
    char root[MEDIA_MAX_PATH + 1];

    media_background_t background_playback;
    media_resume_t resume;
    int default_browser_view;       // media_browser_view_t
    media_page_t default_page;

    bool shuffle;
    media_repeat_t repeat;

    bool eq_enabled;
    media_eq_preset_t eq_preset;
    int eq_gains[MEDIA_EQ_BANDS];

    media_viz_t visualizer;
    int visualizer_fps;

    bool lyrics_enabled;
    int32_t lyrics_offset_ms;

    media_normalize_t normalization;
    bool gapless;
    int crossfade_s;

    bool artwork_background;
    bool dynamic_theme;
    bool low_effects;

    /**
     * Seconds of a live broadcast to bank before the first note is played, 0 to play at the
     * live edge. Trading latency for immunity to network stalls: at 15 s the stream has to be
     * unreachable for that long before anything is audible.
     */
    int stream_delay_s;

    int sleep_timer_minutes;        // 0 = off, -1 = end of track, -2 = end of album
    bool scan_on_startup;
    bool remember_queue;
    bool pause_on_unplug;
    bool skip_on_error;
    bool show_debug;
} media_settings_t;

void media_settings_load(void);
void media_settings_save(void);
media_settings_t *media_settings(void);

const char *media_viz_name(media_viz_t viz);
const char *media_page_name(media_page_t page);
const char *media_repeat_name(media_repeat_t repeat);

/** True when the visualiser is too expensive for the current memory profile. */
bool media_viz_available(media_viz_t viz);
