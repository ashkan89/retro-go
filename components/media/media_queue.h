/**
 * Retro-Go media player - playback queue.
 *
 * The queue is the single authority on what plays next. Both the UI and the playback
 * controller ask it, so "next track" can never mean two different things.
 *
 * Shuffle is a bag, not a dice roll: a permutation is played through before it is reshuffled,
 * which is what listeners actually expect from a shuffle button.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

void media_queue_init(void);
void media_queue_deinit(void);

/**
 * Hold across a read that must see a consistent queue (path + id + index together). The
 * mutators take the same lock internally, so single calls do not need it.
 */
void media_queue_lock(void);
void media_queue_unlock(void);

void media_queue_clear(void);
int media_queue_count(void);

/** Append. `id` may be 0 when the track is not in the library. */
bool media_queue_add(const char *path, uint32_t id);
/** Insert straight after the current track. */
bool media_queue_add_next(const char *path, uint32_t id);
bool media_queue_remove(int index);
bool media_queue_move(int from, int to);

const char *media_queue_path(int index);
uint32_t media_queue_id(int index);

int media_queue_index(void);
void media_queue_set_index(int index);

/** Path of the current entry, or NULL when the queue is empty. */
const char *media_queue_current(void);

void media_queue_set_shuffle(bool shuffle);
bool media_queue_get_shuffle(void);
void media_queue_set_repeat(media_repeat_t repeat);
media_repeat_t media_queue_get_repeat(void);

/**
 * Index that follows the current one.
 * `manual` is true for a user-pressed Next: repeat-track is then ignored, because pressing
 * next while a track is set to repeat should still move on.
 * Returns -1 when playback should stop.
 */
int media_queue_next_index(bool manual);
int media_queue_prev_index(void);

/** Advance/step back, updating the current index. Returns the new index or -1. */
int media_queue_advance(bool manual);
int media_queue_retreat(void);

/** Rebuild the shuffle order, keeping the current track at the front. */
void media_queue_reshuffle(void);

/** Persist to / restore from `<root>/.retrogo-media/queue.m3u8`. */
bool media_queue_save(const char *root);
bool media_queue_restore(const char *root);

/** Write the queue out as a user-visible playlist. */
bool media_queue_export(const char *path);
