#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_settings.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

#define NS NS_APP
#define KEY(name) "Media." name

static media_settings_t settings;
static bool loaded;

void media_settings_load(void)
{
    const media_profile_t *profile = media_profile();

    char *root = rg_settings_get_string(NS, KEY("Root"), RG_STORAGE_ROOT "/media");
    media_utf8_copy(settings.root, sizeof(settings.root), root ? root : RG_STORAGE_ROOT "/media");
    free(root);

    settings.background_playback =
        (media_background_t)media_clampi((int)rg_settings_get_number(NS, KEY("Background"),
                                                                    MEDIA_BACKGROUND_LAUNCHER),
                                         0, MEDIA_BACKGROUND_COUNT - 1);
    settings.resume = (media_resume_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("Resume"), MEDIA_RESUME_TRACK), 0, MEDIA_RESUME_COUNT - 1);
    settings.default_browser_view = (int)rg_settings_get_number(NS, KEY("BrowserView"), 0);
    settings.default_page = (media_page_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("Page"), MEDIA_PAGE_NOW_PLAYING), 0, MEDIA_PAGE_COUNT - 1);

    settings.shuffle = rg_settings_get_boolean(NS, KEY("Shuffle"), false);
    settings.repeat = (media_repeat_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("Repeat"), MEDIA_REPEAT_ALL), 0, MEDIA_REPEAT_COUNT - 1);

    settings.eq_enabled = rg_settings_get_boolean(NS, KEY("EQ"), false);
    settings.eq_preset = (media_eq_preset_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("EQPreset"), MEDIA_EQ_PRESET_FLAT), 0,
        MEDIA_EQ_PRESET_COUNT - 1);

    for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
    {
        char key[32];
        snprintf(key, sizeof(key), KEY("EQBand%d"), i);
        settings.eq_gains[i] = media_clampi((int)rg_settings_get_number(NS, key, 0),
                                            MEDIA_EQ_GAIN_MIN, MEDIA_EQ_GAIN_MAX);
    }

    settings.visualizer = (media_viz_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("Viz"), MEDIA_VIZ_SPECTRUM), 0, MEDIA_VIZ_COUNT - 1);
    settings.visualizer_fps =
        media_clampi((int)rg_settings_get_number(NS, KEY("VizFPS"), profile->target_fps), 10, 60);

    settings.lyrics_enabled = rg_settings_get_boolean(NS, KEY("Lyrics"), true);
    settings.lyrics_offset_ms =
        media_clampi((int)rg_settings_get_number(NS, KEY("LyricsOffset"), 0), -10000, 10000);

    settings.normalization = (media_normalize_t)media_clampi(
        (int)rg_settings_get_number(NS, KEY("Normalize"), MEDIA_NORMALIZE_OFF), 0,
        MEDIA_NORMALIZE_COUNT - 1);
    settings.gapless = rg_settings_get_boolean(NS, KEY("Gapless"), true);
    settings.crossfade_s = media_clampi((int)rg_settings_get_number(NS, KEY("Crossfade"), 0), 0, 5);

    settings.artwork_background =
        rg_settings_get_boolean(NS, KEY("ArtBackground"), profile->background_blur);
    settings.dynamic_theme = rg_settings_get_boolean(NS, KEY("DynamicTheme"), true);
    settings.low_effects =
        rg_settings_get_boolean(NS, KEY("LowEffects"), profile->profile == MEDIA_MEMORY_LOW);

    settings.sleep_timer_minutes = (int)rg_settings_get_number(NS, KEY("SleepTimer"), 0);
    settings.scan_on_startup = rg_settings_get_boolean(NS, KEY("ScanOnStartup"), true);
    settings.remember_queue = rg_settings_get_boolean(NS, KEY("RememberQueue"), true);
    settings.pause_on_unplug = rg_settings_get_boolean(NS, KEY("PauseOnUnplug"), true);
    settings.skip_on_error = rg_settings_get_boolean(NS, KEY("SkipOnError"), true);
    settings.show_debug = rg_settings_get_boolean(NS, KEY("Debug"), false);

    // Crossfade needs a second decoder's worth of buffers; refuse it where it would not fit.
    if (!profile->crossfade_allowed)
        settings.crossfade_s = 0;

    // A visualiser saved on a bigger device must not brick the UI on a smaller one.
    if (!media_viz_available(settings.visualizer))
        settings.visualizer = MEDIA_VIZ_SPECTRUM;

    loaded = true;
}

void media_settings_save(void)
{
    if (!loaded)
        return;

    rg_settings_set_string(NS, KEY("Root"), settings.root);
    rg_settings_set_number(NS, KEY("Background"), settings.background_playback);
    rg_settings_set_number(NS, KEY("Resume"), settings.resume);
    rg_settings_set_number(NS, KEY("BrowserView"), settings.default_browser_view);
    rg_settings_set_number(NS, KEY("Page"), settings.default_page);
    rg_settings_set_boolean(NS, KEY("Shuffle"), settings.shuffle);
    rg_settings_set_number(NS, KEY("Repeat"), settings.repeat);
    rg_settings_set_boolean(NS, KEY("EQ"), settings.eq_enabled);
    rg_settings_set_number(NS, KEY("EQPreset"), settings.eq_preset);

    for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
    {
        char key[32];
        snprintf(key, sizeof(key), KEY("EQBand%d"), i);
        rg_settings_set_number(NS, key, settings.eq_gains[i]);
    }

    rg_settings_set_number(NS, KEY("Viz"), settings.visualizer);
    rg_settings_set_number(NS, KEY("VizFPS"), settings.visualizer_fps);
    rg_settings_set_boolean(NS, KEY("Lyrics"), settings.lyrics_enabled);
    rg_settings_set_number(NS, KEY("LyricsOffset"), settings.lyrics_offset_ms);
    rg_settings_set_number(NS, KEY("Normalize"), settings.normalization);
    rg_settings_set_boolean(NS, KEY("Gapless"), settings.gapless);
    rg_settings_set_number(NS, KEY("Crossfade"), settings.crossfade_s);
    rg_settings_set_boolean(NS, KEY("ArtBackground"), settings.artwork_background);
    rg_settings_set_boolean(NS, KEY("DynamicTheme"), settings.dynamic_theme);
    rg_settings_set_boolean(NS, KEY("LowEffects"), settings.low_effects);
    rg_settings_set_number(NS, KEY("SleepTimer"), settings.sleep_timer_minutes);
    rg_settings_set_boolean(NS, KEY("ScanOnStartup"), settings.scan_on_startup);
    rg_settings_set_boolean(NS, KEY("RememberQueue"), settings.remember_queue);
    rg_settings_set_boolean(NS, KEY("PauseOnUnplug"), settings.pause_on_unplug);
    rg_settings_set_boolean(NS, KEY("SkipOnError"), settings.skip_on_error);
    rg_settings_set_boolean(NS, KEY("Debug"), settings.show_debug);

    rg_settings_commit();
}

media_settings_t *media_settings(void)
{
    if (!loaded)
        media_settings_load();
    return &settings;
}

const char *media_viz_name(media_viz_t viz)
{
    switch (viz)
    {
    case MEDIA_VIZ_SPECTRUM:     return "Spectrum";
    case MEDIA_VIZ_MIRRORED:     return "Mirrored";
    case MEDIA_VIZ_WAVEFORM:     return "Waveform";
    case MEDIA_VIZ_CIRCULAR:     return "Circular";
    case MEDIA_VIZ_VU:           return "Stereo VU";
    case MEDIA_VIZ_PEAK:         return "Peak / RMS";
    case MEDIA_VIZ_OSCILLOSCOPE: return "Oscilloscope";
    case MEDIA_VIZ_PARTICLES:    return "Particles";
    case MEDIA_VIZ_ART_PULSE:    return "Art Pulse";
    case MEDIA_VIZ_VINYL:        return "Vinyl";
    case MEDIA_VIZ_CLOCK:        return "Clock";
    case MEDIA_VIZ_MINIMAL:      return "Minimal";
    default:                     return "?";
    }
}

const char *media_page_name(media_page_t page)
{
    switch (page)
    {
    case MEDIA_PAGE_LIBRARY:     return "Library";
    case MEDIA_PAGE_NOW_PLAYING: return "Now Playing";
    case MEDIA_PAGE_LYRICS:      return "Lyrics";
    case MEDIA_PAGE_VISUALIZER:  return "Visualizer";
    case MEDIA_PAGE_QUEUE:       return "Queue";
    case MEDIA_PAGE_INFO:        return "Track Info";
    default:                     return "?";
    }
}

const char *media_repeat_name(media_repeat_t repeat)
{
    switch (repeat)
    {
    case MEDIA_REPEAT_OFF:    return "Off";
    case MEDIA_REPEAT_TRACK:  return "Track";
    case MEDIA_REPEAT_FOLDER: return "Album";
    case MEDIA_REPEAT_ALL:    return "All";
    default:                  return "?";
    }
}

bool media_viz_available(media_viz_t viz)
{
    const media_profile_t *profile = media_profile();

    if (viz < 0 || viz >= MEDIA_VIZ_COUNT)
        return false;

    // Particles keep hundreds of live points and a trail buffer; only the high profile has
    // both the memory and the CPU headroom to run it without hurting the audio.
    if (viz == MEDIA_VIZ_PARTICLES && !profile->particles)
        return false;

    return true;
}
