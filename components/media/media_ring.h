/**
 * Retro-Go media player - lock-free single-producer / single-consumer ring buffer.
 *
 * One writer task and one reader task only. `head` and `tail` are 32-bit and are written by
 * exactly one side each, so no critical section is needed on Xtensa (aligned 32-bit stores
 * are atomic). A semaphore per direction lets either side block instead of spinning.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct media_ring_s media_ring_t;

/** capacity is rounded up to a power of two. `psram` places the data buffer in PSRAM. */
media_ring_t *media_ring_create(const char *name, size_t capacity, bool psram);
void media_ring_free(media_ring_t *ring);

size_t media_ring_capacity(const media_ring_t *ring);
size_t media_ring_used(const media_ring_t *ring);
size_t media_ring_free_space(const media_ring_t *ring);
int media_ring_fill_percent(const media_ring_t *ring);

/**
 * Copy up to `len` bytes in. Blocks up to `timeout_ms` waiting for space (0 = never block,
 * -1 = forever). Returns the number of bytes actually written.
 */
size_t media_ring_write(media_ring_t *ring, const void *data, size_t len, int timeout_ms);

/** Copy up to `len` bytes out. Same blocking rules. Returns bytes read. */
size_t media_ring_read(media_ring_t *ring, void *data, size_t len, int timeout_ms);

/** Read without consuming. Never blocks. */
size_t media_ring_peek(const media_ring_t *ring, void *data, size_t len);

/** Drop `len` bytes from the front. Returns bytes actually dropped. */
size_t media_ring_skip(media_ring_t *ring, size_t len);

/**
 * Discard everything. Must only be called when the producer is known to be parked (during a
 * seek or a track change), otherwise the producer's in-flight copy would be partially kept.
 */
void media_ring_reset(media_ring_t *ring);

/**
 * Wake any task blocked in read/write. Used to unblock a consumer at shutdown; subsequent
 * blocking calls return immediately with 0 until media_ring_resume() is called.
 */
void media_ring_abort(media_ring_t *ring);
void media_ring_resume(media_ring_t *ring);
bool media_ring_aborted(const media_ring_t *ring);
