#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "media_metadata.h"

typedef enum {
    MEDIA_STOPPED,
    MEDIA_BUFFERING,
    MEDIA_PLAYING,
    MEDIA_PAUSED,
    MEDIA_ERROR,
} media_playback_state_t;

typedef struct {
    media_playback_state_t state;
    char path[RG_PATH_MAX + 1];
    char error[64];
    media_metadata_t metadata;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffered_ms;
    uint32_t underruns;
    uint8_t spectrum[16];
    bool finished;
} media_player_snapshot_t;

bool media_player_init(void);
bool media_player_play(const char *path, const media_metadata_t *metadata, uint32_t start_ms);
void media_player_toggle_pause(void);
void media_player_stop(void);
void media_player_seek(int32_t delta_ms);
void media_player_seek_to(uint32_t position_ms);
void media_player_get_snapshot(media_player_snapshot_t *snapshot);
bool media_player_take_finished(void);

