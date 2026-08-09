/**
 * Retro-Go media player - buffered file source.
 *
 * A source owns the FILE handle and a dedicated prefetch task that keeps a compressed-data
 * ring full. Decoders only ever touch the ring, so a slow SD read never stalls the decoder
 * for longer than the reserve lasts, and the decoder never stalls the audio task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

typedef struct media_source_s media_source_t;

typedef struct
{
    uint32_t reads;
    uint32_t avg_latency_us;
    uint32_t max_latency_us;
    uint32_t stalls;         // Times the decoder found the ring empty before EOF
} media_source_stats_t;

/**
 * Open `path` and start prefetching. `buffer_bytes` is the compressed reserve; it is placed
 * in PSRAM. Returns NULL on failure (file missing, out of memory).
 */
media_source_t *media_source_open(const char *path, size_t buffer_bytes);
void media_source_close(media_source_t *source);

/** Blocking read from the prefetch ring. Returns bytes read (0 = EOF or aborted). */
size_t media_source_read(media_source_t *source, void *buffer, size_t len, int timeout_ms);

/** Discard `len` bytes. Returns bytes actually skipped. */
size_t media_source_skip(media_source_t *source, size_t len);

/** Byte offset of the next byte the consumer will read. */
uint64_t media_source_tell(const media_source_t *source);
uint64_t media_source_size(const media_source_t *source);

/**
 * Reposition. Flushes the ring and re-points the file. Must be called from the consumer
 * (decoder) task only. Returns false if the offset is out of range or the file is gone.
 */
bool media_source_seek(media_source_t *source, uint64_t offset);

/** True once the file has been fully read AND the ring has been drained. */
bool media_source_eof(const media_source_t *source);

/** Ring occupancy 0..100, used by the buffering state machine and the resource manager. */
int media_source_fill_percent(const media_source_t *source);

/** Unblock a consumer parked in media_source_read(). Used during teardown. */
void media_source_abort(media_source_t *source);

media_source_stats_t media_source_get_stats(const media_source_t *source);

const char *media_source_path(const media_source_t *source);
