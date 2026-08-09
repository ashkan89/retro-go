/**
 * Retro-Go media player - album art cache.
 *
 * Loading and decoding happens on a worker task so scrolling never waits for a JPEG. The
 * cache is bounded by both entry count and total bytes and evicts least-recently-used, so
 * browsing a large library cannot consume all of PSRAM.
 *
 * The UI must wrap a frame in media_artwork_lock()/media_artwork_unlock(): that is what
 * makes it safe to hold a returned pointer for the duration of the render.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rg_surface.h>

#include "media_image.h"
#include "media_types.h"

void media_artwork_init(void);
void media_artwork_deinit(void);

void media_artwork_lock(void);
void media_artwork_unlock(void);

/**
 * Cached artwork for `track_path` at roughly `max_dim` pixels on its longest edge.
 * Returns NULL when it is not resident. When `request` is true a background load is queued
 * and MEDIA_EVENT_ARTWORK_READY fires once it lands.
 */
const rg_image_t *media_artwork_get(const char *track_path, int max_dim, bool request);

/** Palette for a track. Falls back to a hash-derived palette when there is no artwork. */
media_palette_t media_artwork_palette(const char *track_path);

/**
 * Blurred, darkened full-screen background derived from the track's artwork.
 * Generated once per track and cached; returns NULL until it is ready.
 */
const rg_image_t *media_artwork_background(const char *track_path, int width, int height);

/** True when the worker still has queued work. */
bool media_artwork_busy(void);

/** Drop everything (SD removal, root change, memory pressure). */
void media_artwork_flush(void);

/** Install the same resource-pressure hook the library scanner uses. */
void media_artwork_set_pressure_source(int (*cb)(void));

/** Called by the player when new artwork becomes available. */
void media_artwork_set_ready_callback(void (*cb)(void));

size_t media_artwork_bytes_used(void);
