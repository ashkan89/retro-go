/**
 * Retro-Go media player - playback controller.
 *
 * Owns the decode task and the playback state machine. Every transition goes through here,
 * so stop / seek / next / SD removal / decoder failure cannot race each other.
 *
 * The UI never touches the decoder: it reads a coherent snapshot and posts commands.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "media_lyrics.h"
#include "media_types.h"

bool media_player_init(void);

/**
 * Tear down. When `keep_playing` is true the audio pipeline is left running (background
 * playback) and only the UI-facing resources are released.
 */
void media_player_shutdown(bool keep_playing);

bool media_player_active(void);

/* ---- Transport ---------------------------------------------------------------------- */

/** Start the entry at `index` in the queue. */
void media_player_play_index(int index);

/** Replace the queue with a single track and play it. */
void media_player_play_path(const char *path, uint32_t track_id);

void media_player_play(void);
void media_player_pause(void);
void media_player_toggle_pause(void);
void media_player_stop(void);

void media_player_next(void);
void media_player_previous(void);

/** Relative seek in milliseconds; clamped to the track. */
void media_player_seek(int32_t delta_ms);
void media_player_seek_to(uint32_t position_ms);

void media_player_set_shuffle(bool shuffle);
void media_player_set_repeat(media_repeat_t repeat);
void media_player_toggle_favorite(void);

/* ---- State -------------------------------------------------------------------------- */

media_snapshot_t media_player_snapshot(void);

/** Metadata of the track being played. Valid until the next track change. */
const media_track_t *media_player_track(void);
const char *media_player_path(void);

/**
 * Path to look artwork up by, which is not always the track's own path: a station may name a
 * cover URL in its metadata. NULL for a broadcast that named none -- there is nothing to find,
 * and asking would pull the head of the stream repeatedly.
 */
const char *media_player_art_path(void);

const media_lyrics_t *media_player_lyrics(void);
bool media_player_lyrics_available(void);

/** Human-readable reason for the last failure, or NULL. */
const char *media_player_last_error(void);

/**
 * Must be called regularly from the UI task. Drives the sleep timer, debounced statistics
 * writes and the deferred metadata/lyrics load. Cheap.
 */
void media_player_tick(void);

/**
 * Resource pressure for background work: 0 idle, 1 playing comfortably, 2 buffer low.
 * Installed into the library scanner and the artwork worker.
 */
int media_player_pressure(void);

/* ---- Extras ------------------------------------------------------------------------- */

/** minutes > 0, or 0 = off, -1 = end of track, -2 = end of album. */
void media_player_set_sleep_timer(int minutes);
int media_player_get_sleep_timer(void);
uint32_t media_player_sleep_remaining_s(void);

/** Called by the launcher before an emulator starts: releases audio focus cleanly. */
void media_player_release_audio(void);

void media_player_set_event_callback(media_event_cb_t cb, void *user);
