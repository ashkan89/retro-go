#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "media_artwork.h"
#include "media_audio.h"
#include "media_fft.h"
#include "media_library.h"
#include "media_player.h"
#include "media_queue.h"
#include "media_ui_internal.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

static bool initialized;
static bool player_started;
static bool foreground;

void media_clear_cache(void)
{
    // Safe before media_init(): the library knows whether it has ever been given a root
    media_library_clear_cache();
}

void media_init(void)
{
    if (initialized)
        return;

    const media_profile_t *profile = media_profile();
    media_settings_t *cfg = media_settings();

    RG_LOGI("Media subsystem starting (profile %s)", profile->name);

    media_library_init(cfg->root);
    media_queue_init();

    if (!media_library_load() && cfg->scan_on_startup)
    {
        // First run: build the library without asking. The browser shows live progress and
        // the user can walk away from the screen while it works.
        if (rg_storage_exists(media_library_root()))
            media_library_scan_start(false);
        else
            RG_LOGW("Media root '%s' is missing", media_library_root());
    }

    if (cfg->remember_queue)
        media_queue_restore(media_library_root());

    media_queue_set_shuffle(cfg->shuffle);
    media_queue_set_repeat(cfg->repeat);

    initialized = true;
}

/** Bring up the playback pipeline on demand; the launcher does not pay for it until used. */
static bool ensure_player(void)
{
    if (player_started && media_audio_running())
        return true;

    if (!media_fft_init(media_profile()->fft_size, media_profile()->fft_bands))
        RG_LOGW("Running without a visualizer");

    if (!media_player_init())
    {
        rg_gui_alert("Media Player", "Could not start audio.\nAnother app may be using it.");
        return false;
    }

    player_started = true;
    return true;
}

void media_run(void)
{
    media_run_at(-1);
}

void media_run_at(int browse_mode)
{
    media_init();

    if (!ensure_player())
        return;

    // Keep the launcher's own screen state out of the way; the player owns the display until
    // it returns.
    rg_display_sync(true);

    if (browse_mode >= 0 && browse_mode < MEDIA_BROWSE_COUNT)
        media_ui_set_pending_view((media_browse_mode_t)browse_mode);

    foreground = true;
    media_ui_run();
    foreground = false;

    media_settings_t *cfg = media_settings();

    if (cfg->remember_queue)
        media_queue_save(media_library_root());
    media_settings_save();

    bool keep_playing = cfg->background_playback != MEDIA_BACKGROUND_OFF &&
                        media_is_playing();

    media_player_shutdown(keep_playing);
    if (!keep_playing)
        player_started = false;

    // The launcher redraws itself on the next loop iteration.
    rg_display_clear(C_BLACK);
}

bool media_is_playing(void)
{
    if (!player_started)
        return false;
    media_snapshot_t snapshot = media_player_snapshot();
    return snapshot.state == MEDIA_STATE_PLAYING || snapshot.state == MEDIA_STATE_BUFFERING;
}

bool media_has_library(void)
{
    return media_library_ready();
}

bool media_is_foreground(void)
{
    return foreground;
}

void media_status_line(char *out, size_t size)
{
    if (!out || !size)
        return;

    out[0] = 0;

    if (!player_started)
    {
        media_scan_status_t scan = media_library_scan_status();
        if (scan.scanning)
            snprintf(out, size, "Scanning: %u tracks", (unsigned)scan.tracks_found);
        return;
    }

    media_snapshot_t snapshot = media_player_snapshot();
    const media_track_t *track = media_player_track();

    if (!track || snapshot.state == MEDIA_STATE_STOPPED)
    {
        snprintf(out, size, "Stopped");
        return;
    }

    const char *marker = snapshot.state == MEDIA_STATE_PAUSED ? "||" : ">";
    if (track->artist[0])
        snprintf(out, size, "%s %.28s - %.20s", marker, track->title, track->artist);
    else
        snprintf(out, size, "%s %.40s", marker, track->title);
}

void media_library_line(char *out, size_t size)
{
    if (!out || !size)
        return;

    media_scan_status_t scan = media_library_scan_status();

    if (scan.scanning)
        snprintf(out, size, "Scanning... %u tracks", (unsigned)scan.tracks_found);
    else if (media_library_ready())
        snprintf(out, size, "%u tracks, %u albums", (unsigned)media_library_track_count(),
                 (unsigned)media_library_group_count(MEDIA_VIEW_ALBUMS));
    else
        snprintf(out, size, "No library");
}

void media_suspend_for_app(void)
{
    if (!player_started)
        return;

    // Emulator audio always wins: two subsystems must never drive I2S at once.
    RG_LOGI("Suspending playback for another application");

    media_settings_t *cfg = media_settings();
    if (cfg->remember_queue)
        media_queue_save(media_library_root());

    media_player_release_audio();
    media_player_shutdown(false);
    player_started = false;
}

void media_shutdown(void)
{
    if (!initialized)
        return;

    media_suspend_for_app();
    media_audio_deinit();
    media_library_deinit();
    media_queue_deinit();
    media_artwork_deinit();
    media_fft_deinit();

    initialized = false;
}
