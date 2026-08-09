/**
 * Retro-Go media player - public interface.
 *
 * The launcher only needs these functions: everything else is internal to the component.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_settings.h"
#include "media_types.h"

/**
 * Browser views the player can be opened on. Defined here (rather than internally) so the
 * launcher's shortcuts cannot drift out of step with the UI's own enum.
 */
typedef enum
{
    MEDIA_BROWSE_HOME = 0,
    MEDIA_BROWSE_FOLDERS,
    MEDIA_BROWSE_ALBUMS,
    MEDIA_BROWSE_ALBUM_TRACKS,
    MEDIA_BROWSE_ARTISTS,
    MEDIA_BROWSE_ARTIST_TRACKS,
    MEDIA_BROWSE_GENRES,
    MEDIA_BROWSE_GENRE_TRACKS,
    MEDIA_BROWSE_PLAYLISTS,
    MEDIA_BROWSE_PLAYLIST_TRACKS,
    MEDIA_BROWSE_FAVORITES,
    MEDIA_BROWSE_RECENT,
    MEDIA_BROWSE_ALL_TRACKS,
    MEDIA_BROWSE_COUNT,
} media_browse_mode_t;

/**
 * Prepare the subsystem: load settings, load the library index and optionally kick off a
 * background scan. Cheap and non-blocking; safe to call from the launcher's startup path.
 */
void media_init(void);

/**
 * Enter the player. Blocks until the user leaves, then returns control to the launcher.
 * Playback may continue afterwards depending on the Background Playback setting.
 */
void media_run(void);

/** Open the player directly on a browser view (used by the launcher tab shortcuts). */
void media_run_at(int browse_mode);

bool media_is_playing(void);
bool media_has_library(void);

/** One-line playback state for the launcher tab, e.g. "Breathe - Pink Floyd". */
void media_status_line(char *out, size_t size);

/** Library summary for the launcher tab, e.g. "342 tracks - 29 albums". */
void media_library_line(char *out, size_t size);

/**
 * Called by the launcher immediately before it hands the device to an emulator. Stops
 * playback and releases the audio hardware so the emulator gets a clean I2S device.
 */
void media_suspend_for_app(void);

/** Release everything (shutdown, storage change). Safe to call when never initialised. */
void media_shutdown(void);
