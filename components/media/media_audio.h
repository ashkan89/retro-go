/**
 * Retro-Go media player - PCM output stage and audio focus.
 *
 * Owns the decoded-PCM ring and the task that drains it into rg_audio_submit(). Playback
 * position is derived from frames actually handed to the hardware, not from a wall clock, so
 * the progress bar and the lyrics can never drift away from what is being heard.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

/* -------------------------------------------------------------------------------------- */
/* Audio focus                                                                              */
/* -------------------------------------------------------------------------------------- */

/**
 * Claim the shared I2S output. Fails if another subsystem holds it. Emulators are never
 * pre-empted: the player releases focus before an emulator is launched.
 */
bool media_audio_acquire(media_audio_owner_t owner);
void media_audio_release(media_audio_owner_t owner);
media_audio_owner_t media_audio_get_owner(void);

/* -------------------------------------------------------------------------------------- */
/* Output stage                                                                             */
/* -------------------------------------------------------------------------------------- */

/** Allocate the PCM ring and start the output task. Idempotent. */
bool media_audio_start(void);
void media_audio_stop(void);
/** Release the ring and the output chunk. Only call once the task has stopped. */
void media_audio_deinit(void);
bool media_audio_running(void);

/**
 * Reconfigure the hardware for `sample_rate`. Output is muted across the change and a short
 * fade is applied afterwards, so switching between tracks of different rates never pops.
 */
bool media_audio_set_sample_rate(uint32_t sample_rate);
uint32_t media_audio_get_sample_rate(void);

/**
 * Producer side, called from the decode task. Blocks until there is room or `timeout_ms`
 * elapses. Returns frames accepted.
 */
size_t media_audio_write(const int16_t *pcm, size_t frames, int timeout_ms);

/** Drop everything buffered and resynchronise the frame counter to `position_ms`. */
void media_audio_flush(uint32_t position_ms);

/** Fade out and stop consuming, or fade back in. The ring contents are preserved. */
void media_audio_set_paused(bool paused);
bool media_audio_get_paused(void);

/** Linear gain applied before the limiter (ReplayGain, sleep-timer fade). 1.0 = unity. */
void media_audio_set_gain(float gain);
float media_audio_get_gain(void);

/** Marks the end of the stream: the task drains the ring, then reports drained. */
void media_audio_set_draining(bool draining);
bool media_audio_drained(void);

uint32_t media_audio_position_ms(void);
int media_audio_fill_percent(void);
size_t media_audio_buffered_frames(void);
uint32_t media_audio_underruns(void);
uint64_t media_audio_frames_played(void);
