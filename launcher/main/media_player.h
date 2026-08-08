#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "media_metadata.h"

typedef enum {
    MEDIA_STOPPED,
    MEDIA_BUFFERING,
    MEDIA_PLAYING,
    MEDIA_PAUSED,
    MEDIA_ERROR,
} media_playback_state_t;

/* Number of stereo frames the decoder keeps for the visualisers. The analysis
   itself runs on the UI thread so the decode path stays free of DSP work. */
#define MEDIA_SCOPE_FRAMES 512

/* Deliberately small. Callers keep one of these on the stack, sometimes nested
   two deep, on a UI task that also has to fit a JPEG decode. Embedding the full
   media_metadata_t here cost 1.6 KB per copy; the tags the UI wants to draw are
   held by the library layer instead. */
typedef struct {
    media_playback_state_t state;
    char path[RG_PATH_MAX + 1];
    char error[64];
    media_format_t format;
    uint32_t audio_size;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffered_ms;
    uint32_t underruns;
    uint32_t decode_errors;
    uint32_t read_errors;
    /* Live values reported by the decoder, which may differ from the ones the
       metadata scanner guessed from the first frame. */
    uint32_t sample_rate;
    uint32_t bitrate;
    uint8_t channels;
    uint8_t buffer_fill; /* ring occupancy, 0-100 */
    bool seekable;
    bool finished;
} media_player_snapshot_t;

bool media_player_init(void);
bool media_player_play(const char *path, const media_metadata_t *metadata, uint32_t start_ms);
void media_player_toggle_pause(void);
void media_player_set_paused(bool paused);
void media_player_stop(void);
void media_player_seek(int32_t delta_ms);
void media_player_seek_to(uint32_t position_ms);
void media_player_get_snapshot(media_player_snapshot_t *snapshot);
bool media_player_take_finished(void);

/* Visualisers. Enabling the analyzer makes the decoder keep a cheap mono copy
   of the most recent PCM block; it is disabled whenever no visualiser page is
   on screen so that playback never pays for it. */
void media_player_set_analyzer(bool enabled);
/* Copies out up to `max_frames` interleaved stereo frames of recent audio,
   taken after the equalizer so the display matches what is heard. */
size_t media_player_read_scope(int16_t *out, size_t max_frames, uint32_t *sample_rate);
