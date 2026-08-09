#include <rg_system.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "media_artwork.h"
#include "media_audio.h"
#include "media_decoder.h"
#include "media_eq.h"
#include "media_fft.h"
#include "media_library.h"
#include "media_lyrics.h"
#include "media_metadata.h"
#include "media_net.h"
#include "media_player.h"
#include "media_queue.h"
#include "media_settings.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

/** Commands posted from the UI task and executed by the decode task. */
typedef enum
{
    CMD_NONE = 0,
    CMD_OPEN,       // Load whatever the queue currently points at
    CMD_SEEK,
    CMD_STOP,
} command_t;

static struct
{
    bool initialized;

    rg_task_t *task;
    volatile bool running;
    volatile bool stop;

    rg_mutex_t *lock;

    volatile command_t command;
    volatile uint32_t command_seek_ms;
    volatile uint32_t command_serial;   // Bumped per command so stale work can be discarded
    volatile uint32_t handled_serial;

    media_decoder_t *decoder;
    int16_t *block;

    media_state_t state;
    media_err_t last_error;
    char error_text[64];

    char path[MEDIA_MAX_PATH + 1];
    media_track_t track;
    bool track_valid;
    uint32_t generation;

    media_lyrics_t *lyrics;
    bool lyrics_loaded;

    uint32_t consecutive_failures;

    /* Sleep timer */
    int sleep_minutes;
    int64_t sleep_deadline_us;
    bool sleep_end_of_track;
    bool sleep_end_of_album;

    /* Statistics */
    bool play_counted;
    int64_t last_position_save_us;

    media_event_cb_t event_cb;
    void *event_user;
} player;

/* -------------------------------------------------------------------------------------- */
/* Helpers                                                                                  */
/* -------------------------------------------------------------------------------------- */

static void emit(media_event_t event, intptr_t arg)
{
    if (player.event_cb)
        player.event_cb(event, arg, player.event_user);
}

static void set_state(media_state_t state)
{
    if (player.state == state)
        return;

    RG_LOGD("State %s -> %s", media_state_name(player.state), media_state_name(state));
    player.state = state;
    emit(MEDIA_EVENT_STATE_CHANGED, (intptr_t)state);
}

static void set_error(media_err_t err, const char *detail)
{
    player.last_error = err;
    snprintf(player.error_text, sizeof(player.error_text), "%s%s%s", media_error_name(err),
             detail && *detail ? ": " : "", detail ? detail : "");
    set_state(MEDIA_STATE_ERROR);
    emit(MEDIA_EVENT_ERROR, (intptr_t)err);
    RG_LOGW("Playback error: %s", player.error_text);
}

static void post_command(command_t command, uint32_t arg)
{
    player.command_seek_ms = arg;
    player.command_serial++;
    player.command = command;
}

/** ReplayGain in linear gain, honouring the Normalization setting. */
static float compute_gain(const media_track_t *track)
{
    const media_settings_t *cfg = media_settings();

    if (cfg->normalization == MEDIA_NORMALIZE_OFF || !track)
        return 1.0f;

    int16_t db100 = cfg->normalization == MEDIA_NORMALIZE_ALBUM ? track->replaygain_album
                                                                : track->replaygain_track;
    // Fall back to the other tag rather than doing nothing when only one is present.
    if (db100 == MEDIA_REPLAYGAIN_NONE)
        db100 = cfg->normalization == MEDIA_NORMALIZE_ALBUM ? track->replaygain_track
                                                            : track->replaygain_album;
    if (db100 == MEDIA_REPLAYGAIN_NONE)
        return 1.0f;

    float db = (float)db100 / 100.0f;
    // Never let a tag ask for more than +6 dB; the limiter would just eat it anyway.
    db = media_clampf(db, -20.0f, 6.0f);
    return powf(10.0f, db / 20.0f);
}

/* -------------------------------------------------------------------------------------- */
/* Track loading (decode task)                                                              */
/* -------------------------------------------------------------------------------------- */

static void release_track_resources(void)
{
    if (player.decoder)
    {
        media_decoder_close(player.decoder);
        player.decoder = NULL;
    }
    if (player.lyrics)
    {
        media_lyrics_free(player.lyrics);
        player.lyrics = NULL;
    }
    player.lyrics_loaded = false;
}

/** Read tags for the current path so the UI has something to show immediately. */
static void load_track_metadata(const char *path, uint32_t track_id)
{
    memset(&player.track, 0, sizeof(player.track));
    player.track_valid = false;

    if (media_net_is_url(path))
    {
        // Nothing to parse yet: the tag readers work on files, and a stream has no tags at
        // all. The URL gives a usable name straight away and Icecast titles replace it once
        // the connection is up (see media_player_tick).
        media_utf8_copy(player.track.path, sizeof(player.track.path), path);
        player.track.path_hash = rg_hash(path, strlen(path));
        media_net_display_name(player.track.title, sizeof(player.track.title), path);
        media_utf8_copy(player.track.album, sizeof(player.track.album), "Network stream");
        player.track.codec = (uint8_t)media_codec_from_path(path);
        player.track_valid = true;
        player.track.id = 0;
        player.track.favorite = false;
        emit(MEDIA_EVENT_METADATA_READY, 0);
        return;
    }

    if (track_id && media_library_get_track(track_id, &player.track))
    {
        player.track_valid = true;
    }
    else
    {
        media_metadata_t *meta = calloc(1, sizeof(media_metadata_t));
        media_utf8_copy(player.track.path, sizeof(player.track.path), path);
        player.track.path_hash = rg_hash(path, strlen(path));

        if (meta && media_metadata_read(path, meta))
        {
            media_metadata_apply(&player.track, meta);
            player.track_valid = true;
        }
        else
        {
            media_path_stem(player.track.title, sizeof(player.track.title), path);
            media_utf8_copy(player.track.album, sizeof(player.track.album),
                            rg_basename(rg_dirname(path)));
            player.track.codec = (uint8_t)media_codec_from_path(path);
            player.track_valid = true;
        }
        free(meta);
    }

    player.track.id = track_id;
    player.track.favorite = track_id ? media_library_is_favorite(track_id) : false;

    emit(MEDIA_EVENT_METADATA_READY, (intptr_t)track_id);
}

static bool open_current(void)
{
    char path_copy[MEDIA_MAX_PATH + 1] = {0};
    uint32_t queue_track_id = 0;

    // Copy out under the lock: the UI can add to or reorder the queue at any moment, which
    // may move the string pool the returned pointer points into.
    media_queue_lock();
    {
        const char *current = media_queue_current();
        if (current)
        {
            media_utf8_copy(path_copy, sizeof(path_copy), current);
            queue_track_id = media_queue_id(media_queue_index());
        }
    }
    media_queue_unlock();

    const char *path = path_copy[0] ? path_copy : NULL;

    release_track_resources();
    media_audio_set_draining(false);

    if (!path)
    {
        set_state(MEDIA_STATE_STOPPED);
        return false;
    }

    if (!rg_storage_ready())
    {
        set_error(MEDIA_ERR_IO, "SD card removed");
        emit(MEDIA_EVENT_SD_REMOVED, 0);
        return false;
    }

    media_utf8_copy(player.path, sizeof(player.path), path);
    player.generation++;
    player.play_counted = false;
    player.last_position_save_us = rg_system_timer();

    set_state(MEDIA_STATE_LOADING);
    emit(MEDIA_EVENT_TRACK_CHANGED, (intptr_t)player.generation);

    load_track_metadata(path, queue_track_id);

    media_err_t err = MEDIA_OK;
    player.decoder = NULL;

    // A station URL is very often a .m3u/.pls holding the real address (sometimes a handful
    // of mirrors). Resolve it here so every entry point -- typed URL, bookmark, playlist
    // line, remote folder -- benefits, and try the mirrors in turn.
    if (media_net_is_url(path) && media_net_url_is_playlist(path))
    {
        char (*streams)[MEDIA_MAX_PATH + 1] = calloc(4, MEDIA_MAX_PATH + 1);
        int found = streams ? media_net_fetch_playlist(path, streams, 4) : -1;

        for (int i = 0; i < found && !player.decoder; ++i)
            player.decoder = media_decoder_open(streams[i], media_profile()->source_buffer, &err);

        free(streams);

        if (!player.decoder && found == 0)
            err = MEDIA_ERR_UNSUPPORTED;
        else if (!player.decoder && found < 0)
            err = MEDIA_ERR_IO;
    }
    else
    {
        player.decoder = media_decoder_open(path, media_profile()->source_buffer, &err);
    }

    if (!player.decoder)
    {
        set_error(err, rg_basename(path));
        player.consecutive_failures++;
        return false;
    }

    player.consecutive_failures = 0;

    // Fill in anything the tag parser could not determine from the decoder itself.
    if (!player.track.duration_ms)
        player.track.duration_ms = player.decoder->duration_ms;
    if (!player.track.sample_rate)
        player.track.sample_rate = player.decoder->sample_rate;
    if (!player.track.channels)
        player.track.channels = player.decoder->channels;
    if (!player.track.bits_per_sample)
        player.track.bits_per_sample = player.decoder->bits_per_sample;
    if (!player.track.bitrate)
        player.track.bitrate = player.decoder->bitrate;

    media_audio_set_sample_rate(player.decoder->sample_rate);
    media_audio_flush(0);
    media_audio_set_gain(compute_gain(&player.track));

    // Resume long files where the listener left off, but never ordinary songs.
    uint32_t resume_ms = 0;
    const media_settings_t *cfg = media_settings();
    if (cfg->resume == MEDIA_RESUME_POSITION && player.track.id)
    {
        resume_ms = media_library_get_position(player.track.id);
        if (resume_ms && player.decoder->duration_ms &&
            resume_ms + 5000 < player.decoder->duration_ms)
        {
            if (media_decoder_seek(player.decoder, resume_ms))
                media_audio_flush(resume_ms);
            else
                resume_ms = 0;
        }
        else
        {
            resume_ms = 0;
        }
    }

    set_state(MEDIA_STATE_BUFFERING);
    RG_LOGI("Playing '%s'", rg_basename(path));
    return true;
}

/** Choose and open the next track, or stop. Runs on the decode task. */
static void advance_track(bool manual)
{
    if (player.sleep_end_of_track)
    {
        RG_LOGI("Sleep timer: stopping at the end of the track");
        player.sleep_end_of_track = false;
        player.sleep_minutes = 0;
        media_player_stop();
        return;
    }

    uint32_t previous_album = player.track.album_hash;

    media_queue_lock();
    int next = media_queue_advance(manual);
    media_queue_unlock();

    if (next < 0)
    {
        set_state(MEDIA_STATE_ENDED);
        media_audio_set_paused(true);
        return;
    }

    if (player.sleep_end_of_album)
    {
        // Peek at the album of the track we just moved to; if it changed, we are done.
        media_queue_lock();
        uint32_t id = media_queue_id(media_queue_index());
        media_queue_unlock();
        media_track_t probe;
        if (id && media_library_get_track(id, &probe) && probe.album_hash != previous_album)
        {
            RG_LOGI("Sleep timer: album finished");
            player.sleep_end_of_album = false;
            player.sleep_minutes = 0;
            media_player_stop();
            return;
        }
    }

    if (!open_current())
    {
        // A single unreadable file must not end the session or spin forever.
        if (media_settings()->skip_on_error && player.consecutive_failures < 8)
        {
            RG_LOGW("Skipping past a failed track (%u in a row)",
                    (unsigned)player.consecutive_failures);
            rg_task_delay(200);
            advance_track(false);
        }
        else if (player.consecutive_failures >= 8)
        {
            RG_LOGE("Too many consecutive failures, stopping");
            media_player_stop();
        }
    }
}

static void decode_task(void *arg)
{
    (void)arg;
    player.running = true;

    while (!player.stop)
    {
        /* --- Commands ---------------------------------------------------------------- */
        command_t command = player.command;
        if (command != CMD_NONE)
        {
            uint32_t serial = player.command_serial;
            player.command = CMD_NONE;

            switch (command)
            {
            case CMD_OPEN:
                open_current();
                break;

            case CMD_SEEK:
                if (player.decoder)
                {
                    uint32_t target = player.command_seek_ms;
                    set_state(MEDIA_STATE_SEEKING);
                    if (media_decoder_seek(player.decoder, target))
                    {
                        media_audio_flush(target);
                        set_state(MEDIA_STATE_BUFFERING);
                    }
                    else
                    {
                        // Not seekable (or the seek failed): carry on from where we are.
                        RG_LOGW("Seek to %u ms failed", (unsigned)target);
                        set_state(media_audio_get_paused() ? MEDIA_STATE_PAUSED
                                                           : MEDIA_STATE_PLAYING);
                    }
                }
                break;

            case CMD_STOP:
                release_track_resources();
                media_audio_flush(0);
                media_audio_set_draining(false);
                set_state(MEDIA_STATE_STOPPED);
                break;

            default:
                break;
            }

            player.handled_serial = serial;
            continue;
        }

        /* --- Decode ------------------------------------------------------------------ */
        if (!player.decoder || player.state == MEDIA_STATE_STOPPED ||
            player.state == MEDIA_STATE_ERROR || player.state == MEDIA_STATE_ENDED)
        {
            rg_task_delay(20);
            continue;
        }

        // While paused we stop decoding once the buffer is comfortably full, so a paused
        // player costs nothing but keeps an instant resume.
        if (media_audio_get_paused() && media_audio_fill_percent() > 70)
        {
            rg_task_delay(30);
            continue;
        }

        int frames = media_decoder_decode(player.decoder, player.block, MEDIA_DECODE_BLOCK_FRAMES);

        if (frames > 0)
        {
            size_t written = 0;
            while (written < (size_t)frames && !player.stop && player.command == CMD_NONE)
            {
                size_t n = media_audio_write(player.block + written * MEDIA_PCM_CHANNELS,
                                             (size_t)frames - written, 200);
                if (n == 0)
                    break; // Ring full and the consumer is paused; retry next iteration
                written += n;
            }

            if (player.state == MEDIA_STATE_BUFFERING)
            {
                size_t buffered = media_audio_buffered_frames();
                if (buffered >= media_profile()->prebuffer_frames || player.decoder->eos)
                    set_state(media_audio_get_paused() ? MEDIA_STATE_PAUSED : MEDIA_STATE_PLAYING);
                else
                {
                    emit(MEDIA_EVENT_BUFFERING, (intptr_t)media_audio_fill_percent());
                }
            }
            else if (player.state == MEDIA_STATE_PLAYING && media_audio_fill_percent() < 5 &&
                     !player.decoder->eos)
            {
                // Ran dry: go back to buffering rather than stuttering along the floor.
                set_state(MEDIA_STATE_BUFFERING);
            }
        }
        else
        {
            // End of stream: let the hardware play out what is already buffered before we
            // move on, otherwise the last fraction of a second is lost.
            media_audio_set_draining(true);

            if (media_audio_drained())
            {
                media_audio_set_draining(false);
                advance_track(false);
            }
            else
            {
                rg_task_delay(20);
            }
        }
    }

    release_track_resources();

#ifdef ESP_PLATFORM
    RG_LOGI("Decode task exiting, stack headroom was %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif

    player.running = false;
}

/* -------------------------------------------------------------------------------------- */
/* Lifecycle                                                                                */
/* -------------------------------------------------------------------------------------- */

static void artwork_ready(void)
{
    emit(MEDIA_EVENT_ARTWORK_READY, 0);
}

bool media_player_init(void)
{
    if (player.initialized)
        return true;

    memset(&player, 0, sizeof(player));

    const media_settings_t *cfg = media_settings();

    player.lock = rg_mutex_create();
    player.block = rg_alloc(MEDIA_DECODE_BLOCK_FRAMES * MEDIA_PCM_CHANNELS * sizeof(int16_t),
                            MEM_SLOW | MEM_8BIT | MEM_NOPANIC);

    if (!player.lock || !player.block)
    {
        RG_LOGE("Failed to allocate the playback controller");
        free(player.block), player.block = NULL;
        if (player.lock)
            rg_mutex_free(player.lock), player.lock = NULL;
        return false;
    }

    if (!media_audio_acquire(MEDIA_AUDIO_OWNER_PLAYER))
    {
        RG_LOGE("Audio focus is held by another subsystem");
        free(player.block), player.block = NULL;
        rg_mutex_free(player.lock), player.lock = NULL;
        return false;
    }

    media_eq_init();
    media_eq_set_enabled(cfg->eq_enabled);
    media_eq_set_preset(cfg->eq_preset);
    if (cfg->eq_preset == MEDIA_EQ_PRESET_CUSTOM)
    {
        for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
            media_eq_set_gain(i, cfg->eq_gains[i]);
    }

    if (!media_audio_start())
    {
        media_audio_release(MEDIA_AUDIO_OWNER_PLAYER);
        free(player.block), player.block = NULL;
        rg_mutex_free(player.lock), player.lock = NULL;
        return false;
    }

    media_artwork_init();
    media_artwork_set_pressure_source(&media_player_pressure);
    media_artwork_set_ready_callback(&artwork_ready);
    media_library_set_pressure_source(&media_player_pressure);

    player.state = MEDIA_STATE_STOPPED;
    player.stop = false;

    // minimp3 puts a mp3dec_scratch_t on the stack inside mp3dec_decode_frame: 2.8 KB of
    // bit reservoir, 4.6 KB of granule buffers and 8.4 KB of synthesis state, ~16 KB in
    // total. Everything else in this task's tree is small by comparison, so the stack is
    // sized around that one frame plus room for logging.
    player.task = rg_task_create("media_dec", &decode_task, NULL, 22 * 1024, RG_TASK_PRIORITY_5,
                                 RG_TASK_AFFINITY_AUDIO);
    if (!player.task)
    {
        RG_LOGE("Failed to start the decode task");
        media_audio_stop();
        media_audio_release(MEDIA_AUDIO_OWNER_PLAYER);
        free(player.block), player.block = NULL;
        rg_mutex_free(player.lock), player.lock = NULL;
        return false;
    }

    media_player_set_sleep_timer(cfg->sleep_timer_minutes);

    player.initialized = true;
    RG_LOGI("Playback controller ready");
    return true;
}

void media_player_shutdown(bool keep_playing)
{
    if (!player.initialized)
        return;

    // Persist before anything is torn down, while the state is still coherent.
    if (player.track.id && player.state != MEDIA_STATE_STOPPED)
        media_library_note_position(player.track.id, media_audio_position_ms(),
                                    player.track.duration_ms);
    media_library_commit();

    if (keep_playing && player.state == MEDIA_STATE_PLAYING)
    {
        // Background playback: the decode and audio tasks keep running, only the UI-side
        // resources go away.
        if (player.lyrics)
        {
            media_lyrics_free(player.lyrics);
            player.lyrics = NULL;
            player.lyrics_loaded = false;
        }
        media_artwork_flush();
        RG_LOGI("Player left running in the background");
        return;
    }

    player.stop = true;
    post_command(CMD_STOP, 0);

    for (int i = 0; i < 500 && player.running; ++i)
        rg_task_delay(10);

    if (player.running)
    {
        // Never delete a task that might still hold the SD lock; leaking is the safe choice.
        RG_LOGE("Decode task did not stop; leaving the player resident");
        return;
    }

    media_audio_stop();
    media_audio_release(MEDIA_AUDIO_OWNER_PLAYER);

    media_artwork_set_ready_callback(NULL);
    media_artwork_set_pressure_source(NULL);
    media_library_set_pressure_source(NULL);
    media_artwork_deinit();
    media_fft_deinit();

    release_track_resources();
    free(player.block), player.block = NULL;
    if (player.lock)
        rg_mutex_free(player.lock), player.lock = NULL;

    player.task = NULL;
    player.initialized = false;
    player.state = MEDIA_STATE_STOPPED;

    RG_LOGI("Playback controller stopped");
}

bool media_player_active(void)
{
    return player.initialized && (player.state == MEDIA_STATE_PLAYING ||
                                  player.state == MEDIA_STATE_BUFFERING ||
                                  player.state == MEDIA_STATE_PAUSED ||
                                  player.state == MEDIA_STATE_SEEKING);
}

/* -------------------------------------------------------------------------------------- */
/* Transport                                                                                */
/* -------------------------------------------------------------------------------------- */

void media_player_play_index(int index)
{
    if (!player.initialized)
        return;
    if (index < 0 || index >= media_queue_count())
        return;

    media_queue_set_index(index);
    media_audio_set_paused(false);
    post_command(CMD_OPEN, 0);
}

void media_player_play_path(const char *path, uint32_t track_id)
{
    if (!player.initialized || !path)
        return;

    media_queue_clear();
    if (!media_queue_add(path, track_id))
        return;
    media_player_play_index(0);
}

void media_player_play(void)
{
    if (!player.initialized)
        return;

    if (player.state == MEDIA_STATE_STOPPED || player.state == MEDIA_STATE_ENDED ||
        player.state == MEDIA_STATE_ERROR)
    {
        if (media_queue_count() > 0)
        {
            if (media_queue_index() < 0)
                media_queue_set_index(0);
            media_audio_set_paused(false);
            post_command(CMD_OPEN, 0);
        }
        return;
    }

    media_audio_set_paused(false);
    if (player.state == MEDIA_STATE_PAUSED)
        set_state(MEDIA_STATE_PLAYING);
}

void media_player_pause(void)
{
    if (!player.initialized)
        return;
    if (player.state != MEDIA_STATE_PLAYING && player.state != MEDIA_STATE_BUFFERING)
        return;

    media_audio_set_paused(true);
    set_state(MEDIA_STATE_PAUSED);

    // Pausing is a natural place to persist where we are.
    if (player.track.id)
        media_library_note_position(player.track.id, media_audio_position_ms(),
                                    player.track.duration_ms);
}

void media_player_toggle_pause(void)
{
    if (player.state == MEDIA_STATE_PLAYING || player.state == MEDIA_STATE_BUFFERING)
        media_player_pause();
    else
        media_player_play();
}

void media_player_stop(void)
{
    if (!player.initialized)
        return;

    if (player.track.id)
        media_library_note_position(player.track.id, media_audio_position_ms(),
                                    player.track.duration_ms);

    media_audio_set_paused(true);
    post_command(CMD_STOP, 0);
}

void media_player_next(void)
{
    if (!player.initialized || media_queue_count() == 0)
        return;

    // A skip inside the first third of a track counts against it for statistics.
    if (player.track.id && player.track.duration_ms &&
        media_audio_position_ms() * 3 < player.track.duration_ms)
        media_library_note_skipped(player.track.id);

    media_queue_lock();
    int next = media_queue_advance(true);
    media_queue_unlock();

    if (next < 0)
        return;

    media_audio_set_paused(false);
    post_command(CMD_OPEN, 0);
}

void media_player_previous(void)
{
    if (!player.initialized || media_queue_count() == 0)
        return;

    // Standard behaviour: restart the current track unless we are near its start.
    if (media_audio_position_ms() > 3000 && player.decoder)
    {
        media_player_seek_to(0);
        return;
    }

    media_queue_lock();
    int previous = media_queue_retreat();
    media_queue_unlock();

    if (previous < 0)
        return;

    media_audio_set_paused(false);
    post_command(CMD_OPEN, 0);
}

void media_player_seek(int32_t delta_ms)
{
    if (!player.initialized || !player.decoder)
        return;

    int64_t target = (int64_t)media_audio_position_ms() + delta_ms;
    uint32_t duration = player.track.duration_ms ? player.track.duration_ms
                                                 : player.decoder->duration_ms;

    if (target < 0)
        target = 0;
    if (duration && target > (int64_t)duration)
        target = duration;

    media_player_seek_to((uint32_t)target);
}

void media_player_seek_to(uint32_t position_ms)
{
    if (!player.initialized || !player.decoder)
        return;
    if (!player.decoder->seekable)
        return;

    post_command(CMD_SEEK, position_ms);
}

void media_player_set_shuffle(bool shuffle)
{
    media_queue_set_shuffle(shuffle);
    media_settings()->shuffle = shuffle;
}

void media_player_set_repeat(media_repeat_t repeat)
{
    media_queue_set_repeat(repeat);
    media_settings()->repeat = repeat;
}

void media_player_toggle_favorite(void)
{
    if (!player.track.id)
        return;
    bool favorite = !media_library_is_favorite(player.track.id);
    media_library_set_favorite(player.track.id, favorite);
    player.track.favorite = favorite;
}

/* -------------------------------------------------------------------------------------- */
/* State                                                                                    */
/* -------------------------------------------------------------------------------------- */

media_snapshot_t media_player_snapshot(void)
{
    media_snapshot_t snapshot = {0};
    const media_spectrum_t *spectrum = media_fft_spectrum();

    snapshot.state = player.state;
    snapshot.last_error = player.last_error;
    snapshot.track_id = player.track.id;
    snapshot.generation = player.generation;
    snapshot.position_ms = media_audio_position_ms();
    snapshot.duration_ms = player.track.duration_ms;
    snapshot.sample_rate = player.track.sample_rate;
    snapshot.bitrate = player.track.bitrate;
    snapshot.channels = player.track.channels;
    snapshot.bits_per_sample = player.track.bits_per_sample;
    snapshot.codec = player.track.codec;
    snapshot.volume = (uint8_t)media_clampi(rg_audio_get_volume(), 0, 100);
    snapshot.brightness = (uint8_t)media_clampi(rg_display_get_backlight(), 0, 100);
    snapshot.muted = rg_audio_get_mute();
    snapshot.shuffle = media_queue_get_shuffle();
    snapshot.repeat = media_queue_get_repeat();
    snapshot.favorite = player.track.favorite;
    snapshot.live = player.decoder && media_source_is_live(player.decoder->source);
    snapshot.network = media_net_is_url(player.path);
    snapshot.pcm_fill_pct = (uint8_t)media_clampi(media_audio_fill_percent(), 0, 100);
    snapshot.src_fill_pct = (uint8_t)media_clampi(
        player.decoder ? media_source_fill_percent(player.decoder->source) : 0, 0, 100);
    snapshot.underruns = media_audio_underruns();
    snapshot.rms_left = spectrum->rms_left;
    snapshot.rms_right = spectrum->rms_right;
    snapshot.peak_left = spectrum->peak_left;
    snapshot.peak_right = spectrum->peak_right;
    snapshot.queue_index = media_queue_index();
    snapshot.queue_length = media_queue_count();
    snapshot.sleep_remaining_s = media_player_sleep_remaining_s();

    // Clamp a duration-less stream so the progress bar never runs past its end.
    if (snapshot.duration_ms && snapshot.position_ms > snapshot.duration_ms)
        snapshot.position_ms = snapshot.duration_ms;

    media_queue_lock();
    int next = media_queue_next_index(false);
    snapshot.next_track_id = next >= 0 ? media_queue_id(next) : 0;
    media_queue_unlock();

    return snapshot;
}

const media_track_t *media_player_track(void)
{
    return player.track_valid ? &player.track : NULL;
}

const char *media_player_path(void)
{
    return player.path[0] ? player.path : NULL;
}

const media_lyrics_t *media_player_lyrics(void)
{
    return player.lyrics;
}

bool media_player_lyrics_available(void)
{
    return player.lyrics != NULL;
}

const char *media_player_last_error(void)
{
    return player.error_text[0] ? player.error_text : NULL;
}

/* -------------------------------------------------------------------------------------- */
/* Periodic work                                                                            */
/* -------------------------------------------------------------------------------------- */

/** Load lyrics for the current track. Runs on the UI task, off the critical path. */
static void ensure_lyrics(void)
{
    if (player.lyrics_loaded || !player.path[0] || !media_settings()->lyrics_enabled)
        return;

    player.lyrics_loaded = true;

    media_lyrics_t *lyrics = media_lyrics_load_sidecar(player.path);

    if (!lyrics && player.track.has_lyrics)
    {
        media_metadata_t *meta = calloc(1, sizeof(media_metadata_t));
        if (meta && media_metadata_read(player.path, meta))
        {
            bool synced = false;
            char *text = media_metadata_read_lyrics(player.path, meta, &synced);
            if (text)
            {
                lyrics = media_lyrics_parse(text, strlen(text));
                free(text);
            }
        }
        free(meta);
    }

    if (lyrics)
    {
        player.lyrics = lyrics;
        emit(MEDIA_EVENT_LYRICS_READY, 0);
    }
}

void media_player_tick(void)
{
    if (!player.initialized)
        return;

    static uint32_t last_generation;
    if (last_generation != player.generation)
    {
        last_generation = player.generation;
        player.lyrics_loaded = false;
        if (player.lyrics)
        {
            media_lyrics_free(player.lyrics);
            player.lyrics = NULL;
        }
    }

    ensure_lyrics();

    /* Play count: credited once the listener is a third of the way in (or 60 seconds). */
    if (!player.play_counted && player.track.id && player.state == MEDIA_STATE_PLAYING)
    {
        uint32_t position = media_audio_position_ms();
        uint32_t threshold = player.track.duration_ms ? player.track.duration_ms / 3 : 60000;
        if (threshold > 60000)
            threshold = 60000;
        if (position >= threshold)
        {
            player.play_counted = true;
            media_library_note_played(player.track.id);
        }
    }

    /* Resume position: saved periodically rather than every second. */
    if (player.track.id && player.state == MEDIA_STATE_PLAYING &&
        (rg_system_timer() - player.last_position_save_us) > 45 * 1000000LL)
    {
        player.last_position_save_us = rg_system_timer();
        media_library_note_position(player.track.id, media_audio_position_ms(),
                                    player.track.duration_ms);
    }

    media_library_commit();

    /* Live stream titles. Polled rather than pushed so nothing from the IO task reaches
     * player state directly. */
    if (player.decoder && player.decoder->source)
    {
        char title[160];
        if (media_source_take_stream_title(player.decoder->source, title, sizeof(title)))
        {
            // Stations almost always send "Artist - Title"; split it so the Now Playing
            // screen lays out the same as it does for a file.
            char *separator = strstr(title, " - ");
            if (separator)
            {
                *separator = 0;
                media_utf8_copy(player.track.artist, sizeof(player.track.artist), title);
                media_utf8_copy(player.track.title, sizeof(player.track.title), separator + 3);
            }
            else
            {
                media_utf8_copy(player.track.title, sizeof(player.track.title), title);
                player.track.artist[0] = 0;
            }

            const char *station = media_source_station_name(player.decoder->source);
            if (station)
                media_utf8_copy(player.track.album, sizeof(player.track.album), station);

            emit(MEDIA_EVENT_METADATA_READY, 0);
        }
    }

    /* Sleep timer */
    if (player.sleep_deadline_us && rg_system_timer() >= player.sleep_deadline_us)
    {
        RG_LOGI("Sleep timer expired");
        player.sleep_deadline_us = 0;
        player.sleep_minutes = 0;
        media_settings()->sleep_timer_minutes = 0;
        media_player_pause();
    }
    else if (player.sleep_deadline_us)
    {
        // Fade the last ten seconds so it does not simply cut out.
        int64_t remaining = player.sleep_deadline_us - rg_system_timer();
        if (remaining < 10 * 1000000LL)
        {
            float ratio = (float)remaining / (10.0f * 1000000.0f);
            media_audio_set_gain(compute_gain(&player.track) * media_clampf(ratio, 0.0f, 1.0f));
        }
    }

    /* SD card disappeared underneath us. */
    if (player.state != MEDIA_STATE_STOPPED && player.state != MEDIA_STATE_ERROR &&
        !rg_storage_ready())
    {
        RG_LOGE("Storage went away, stopping playback");
        media_player_stop();
        set_error(MEDIA_ERR_IO, "SD card removed");
        emit(MEDIA_EVENT_SD_REMOVED, 0);
    }
}

int media_player_pressure(void)
{
    if (!player.initialized)
        return 0;

    if (player.state != MEDIA_STATE_PLAYING && player.state != MEDIA_STATE_BUFFERING)
        return 0;

    int pcm = media_audio_fill_percent();
    int src = player.decoder ? media_source_fill_percent(player.decoder->source) : 100;

    // Either buffer running low means the SD card and the CPU are needed for audio.
    if (pcm < 35 || src < 25 || player.state == MEDIA_STATE_BUFFERING)
        return 2;

    return 1;
}

/* -------------------------------------------------------------------------------------- */
/* Extras                                                                                   */
/* -------------------------------------------------------------------------------------- */

void media_player_set_sleep_timer(int minutes)
{
    player.sleep_minutes = minutes;
    player.sleep_end_of_track = false;
    player.sleep_end_of_album = false;
    player.sleep_deadline_us = 0;

    if (minutes > 0)
        player.sleep_deadline_us = rg_system_timer() + (int64_t)minutes * 60 * 1000000LL;
    else if (minutes == -1)
        player.sleep_end_of_track = true;
    else if (minutes == -2)
        player.sleep_end_of_album = true;
    else
        media_audio_set_gain(compute_gain(&player.track)); // Cancel any in-progress fade

    media_settings()->sleep_timer_minutes = minutes;
}

int media_player_get_sleep_timer(void)
{
    return player.sleep_minutes;
}

uint32_t media_player_sleep_remaining_s(void)
{
    if (!player.sleep_deadline_us)
        return 0;
    int64_t remaining = player.sleep_deadline_us - rg_system_timer();
    return remaining > 0 ? (uint32_t)(remaining / 1000000) : 0;
}

void media_player_release_audio(void)
{
    if (!player.initialized)
        return;

    RG_LOGI("Releasing audio focus");
    media_player_stop();
    for (int i = 0; i < 50 && player.state != MEDIA_STATE_STOPPED; ++i)
        rg_task_delay(10);

    media_audio_stop();
    media_audio_release(MEDIA_AUDIO_OWNER_PLAYER);
}

void media_player_set_event_callback(media_event_cb_t cb, void *user)
{
    player.event_cb = cb;
    player.event_user = user;
}
