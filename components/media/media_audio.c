#include <rg_system.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "media_audio.h"
#include "media_config.h"
#include "media_eq.h"
#include "media_fft.h"
#include "media_lighting.h"
#include "media_ring.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_AUDIO"

// The shared writer commits complete DMA blocks. Supply silence after a final partial
// block, then let the bounded software/DMA reserve play before changing track or rate.
#ifndef RG_AUDIO_DMA_BUFFER_COUNT
#define RG_AUDIO_DMA_BUFFER_COUNT 4
#endif
#ifndef RG_AUDIO_DMA_BUFFER_LENGTH
#define RG_AUDIO_DMA_BUFFER_LENGTH 180
#endif
#ifndef RG_AUDIO_QUEUE_LENGTH
#define RG_AUDIO_QUEUE_LENGTH 1536
#endif

static struct
{
    media_audio_owner_t owner;

    media_ring_t *pcm;
    rg_mutex_t *lock;           // Serializes chunk processing with flush/rate changes.
    rg_task_t *task;
    volatile bool running;
    volatile bool stop;
    volatile bool alive;

    volatile bool paused;
    bool buffering;
    volatile bool draining;
    volatile bool drained;
    bool drain_padded;
    int64_t drain_deadline_us;

    uint32_t sample_rate;
    int previous_system_rate;   // What retro-go was configured for before we took over

    float gain;                 // ReplayGain / sleep fade
    float fade;                 // Current click-suppression ramp, 0.0 - 1.0
    float fade_target;

    uint64_t frames_played;
    uint32_t base_ms;           // Position corresponding to frames_played == 0
    uint64_t frames_at_base;
    uint64_t track_boundary;
    uint32_t track_position_ms;
    bool track_marked;

    uint32_t underruns;

    int16_t *chunk;
} audio = {
    .gain = 1.0f,
    .fade = 0.0f,
    .fade_target = 1.0f,
    .sample_rate = 44100,
};

/* -------------------------------------------------------------------------------------- */
/* Focus                                                                                    */
/* -------------------------------------------------------------------------------------- */

bool media_audio_acquire(media_audio_owner_t owner)
{
    if (owner == MEDIA_AUDIO_OWNER_NONE)
        return false;
    media_audio_owner_t expected = MEDIA_AUDIO_OWNER_NONE;
    if (!__atomic_compare_exchange_n(&audio.owner, &expected, owner, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected != owner)
    {
        RG_LOGW("Audio focus denied: already held by %d", audio.owner);
        return false;
    }
    return true;
}

void media_audio_release(media_audio_owner_t owner)
{
    __atomic_compare_exchange_n(&audio.owner, &owner, MEDIA_AUDIO_OWNER_NONE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

media_audio_owner_t media_audio_get_owner(void)
{
    return __atomic_load_n(&audio.owner, __ATOMIC_ACQUIRE);
}

/* -------------------------------------------------------------------------------------- */
/* Output task                                                                              */
/* -------------------------------------------------------------------------------------- */

/** Apply the click-suppression ramp in place and advance it towards its target. */
static void apply_fade(int16_t *pcm, size_t frames)
{
    if (audio.fade == audio.fade_target && audio.fade == 1.0f)
        return;

    // MEDIA_FADE_MS worth of samples to go from one end of the ramp to the other.
    float step = 1000.0f / ((float)MEDIA_FADE_MS * (float)audio.sample_rate);
    if (step <= 0.0f)
        step = 0.001f;

    for (size_t i = 0; i < frames; ++i)
    {
        if (audio.fade < audio.fade_target)
            audio.fade = media_clampf(audio.fade + step, 0.0f, audio.fade_target);
        else if (audio.fade > audio.fade_target)
            audio.fade = media_clampf(audio.fade - step, audio.fade_target, 1.0f);

        pcm[i * 2 + 0] = (int16_t)((float)pcm[i * 2 + 0] * audio.fade);
        pcm[i * 2 + 1] = (int16_t)((float)pcm[i * 2 + 1] * audio.fade);
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    const size_t chunk_frames = MEDIA_AUDIO_CHUNK_FRAMES;
    const size_t chunk_bytes = chunk_frames * MEDIA_PCM_CHANNELS * sizeof(int16_t);

    audio.alive = true;

    while (!audio.stop)
    {
        rg_mutex_take(audio.lock, -1);
        if (audio.buffering || (audio.paused && audio.fade <= 0.0f))
        {
            // Fully faded out: stop consuming so the buffer is intact when we resume.
            rg_mutex_give(audio.lock);
            rg_task_delay(10);
            continue;
        }

        size_t got = media_ring_read(audio.pcm, audio.chunk, chunk_bytes, 0);
        size_t frames = got / (MEDIA_PCM_CHANNELS * sizeof(int16_t));

        // The ring may contain both tracks. Apply the position marker without dropping
        // PCM or restarting the hardware clock (observation is bounded by one chunk).
        if (audio.track_marked && audio.frames_played >= audio.track_boundary)
        {
            audio.frames_at_base = audio.track_boundary;
            audio.base_ms = audio.track_position_ms;
            audio.track_marked = false;
        }

        if (frames == 0)
        {
            if (audio.draining)
            {
                if (!audio.drain_padded)
                {
                    memset(audio.chunk, 0, chunk_bytes);
                    size_t remaining = RG_AUDIO_DMA_BUFFER_LENGTH;
                    while (remaining)
                    {
                        size_t count = remaining < chunk_frames ? remaining : chunk_frames;
                        rg_audio_submit((const rg_audio_frame_t *)audio.chunk, count);
                        remaining -= count;
                    }
                    uint64_t reserve = RG_AUDIO_QUEUE_LENGTH +
                        RG_AUDIO_DMA_BUFFER_COUNT * RG_AUDIO_DMA_BUFFER_LENGTH;
                    audio.drain_deadline_us = rg_system_timer() +
                        (int64_t)((reserve * 1000000ULL + audio.sample_rate - 1) / audio.sample_rate);
                    audio.drain_padded = true;
                }
                audio.drained = rg_system_timer() >= audio.drain_deadline_us;
            }
            else if (!audio.paused)
            {
                // A genuine underrun. The hardware keeps playing its own DMA reserve, so this
                // is recoverable; we just note it and let the controller rebuffer.
                // Count an episode, not every empty poll, and bank PCM before resuming.
                audio.underruns++;
                audio.buffering = true;
                audio.fade = 0.0f;
            }
            else
            {
                // An empty paused ring cannot finish a sample-driven fade.
                audio.fade = 0.0f;
            }
            rg_mutex_give(audio.lock);
            if (audio.draining || audio.paused)
                rg_task_delay(10);
            else
                media_ring_wait_readable(audio.pcm, 20);
            continue;
        }

        audio.drained = false;

        // EQ, ReplayGain and the limiter all run here, on the way out, so the same block is
        // touched exactly once.
        media_eq_process(audio.chunk, frames, audio.gain);
        apply_fade(audio.chunk, frames);

        // Non-blocking tap for the visualiser: it sees exactly what is being played.
        media_fft_feed(audio.chunk, frames);
        media_lighting_feed(audio.chunk, frames);

        rg_audio_submit((const rg_audio_frame_t *)audio.chunk, frames);
        audio.frames_played += frames;
        if (audio.track_marked && audio.frames_played >= audio.track_boundary)
        {
            audio.frames_at_base = audio.track_boundary;
            audio.base_ms = audio.track_position_ms;
            audio.track_marked = false;
        }
        rg_mutex_give(audio.lock);
    }

#ifdef ESP_PLATFORM
    RG_LOGI("Audio task exiting, stack headroom was %u bytes",
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif

    audio.alive = false;
    audio.running = false; // Last access to audio state; stop() waits for this.
}

bool media_audio_start(void)
{
    if (audio.running)
        return true;

    const media_profile_t *profile = media_profile();

    if (!audio.lock)
        audio.lock = rg_mutex_create();
    if (!audio.lock)
        return false;

    if (!audio.pcm)
    {
        size_t bytes = profile->pcm_buffer_frames * MEDIA_PCM_CHANNELS * sizeof(int16_t);
        audio.pcm = media_ring_create("pcm", bytes, true);
        if (!audio.pcm)
            return false;
    }
    media_ring_resume(audio.pcm);
    media_ring_reset(audio.pcm);

    if (!audio.chunk)
    {
        // The output chunk is small and touched every few milliseconds: keep it internal.
        audio.chunk = rg_alloc(MEDIA_AUDIO_CHUNK_FRAMES * MEDIA_PCM_CHANNELS * sizeof(int16_t),
                               MEM_FAST | MEM_8BIT | MEM_NOPANIC);
        if (!audio.chunk)
        {
            RG_LOGE("Failed to allocate the output chunk");
            return false;
        }
    }

    audio.previous_system_rate = rg_audio_get_sample_rate();
    audio.stop = false;
    audio.paused = false;
    audio.buffering = true;
    audio.draining = false;
    audio.drained = false;
    audio.drain_padded = false;
    audio.fade = 0.0f;
    audio.fade_target = 1.0f;
    audio.frames_played = 0;
    audio.frames_at_base = 0;
    audio.track_marked = false;
    audio.base_ms = 0;
    audio.underruns = 0;
    audio.running = true;

    audio.task = rg_task_create("media_audio", &audio_task, NULL, 4 * 1024, MEDIA_AUDIO_TASK_PRIORITY,
                                RG_TASK_AFFINITY_AUDIO);
    if (!audio.task)
    {
        audio.running = false;
        RG_LOGE("Failed to start the audio task");
        return false;
    }

    RG_LOGI("Audio output started");
    return true;
}

void media_audio_stop(void)
{
    if (!audio.running && !audio.alive)
        return;

    // Fade out before tearing anything down, otherwise the amplifier clicks.
    rg_mutex_take(audio.lock, -1);
    audio.fade_target = 0.0f;
    rg_mutex_give(audio.lock);
    for (int i = 0; i < 10 && audio.fade > 0.0f && audio.alive; ++i)
        rg_task_delay(MEDIA_FADE_MS / 4 + 1);

    audio.stop = true;
    if (audio.pcm)
        media_ring_abort(audio.pcm);

    for (int i = 0; i < 200 && audio.running; ++i)
        rg_task_delay(5);

    if (audio.running)
    {
        RG_LOGE("Audio task did not stop");
        return;
    }

    audio.task = NULL;
    audio.running = false;

    // Hand the hardware back exactly as we found it, so the launcher's own sounds and the
    // next emulator start from a sane configuration.
    if (audio.previous_system_rate > 0 && rg_audio_get_sample_rate() != audio.previous_system_rate)
        rg_audio_set_sample_rate(audio.previous_system_rate);

    RG_LOGI("Audio output stopped (%u underruns)", (unsigned)audio.underruns);
}

bool media_audio_running(void)
{
    return audio.running;
}

void media_audio_deinit(void)
{
    // The buffers are deliberately kept across start/stop cycles so re-entering the player
    // is instant; this is the only place that actually gives them back.
    if (audio.running || audio.alive)
        media_audio_stop();

    if (audio.running || audio.alive)
    {
        RG_LOGE("Refusing to free buffers while the audio task is alive");
        return;
    }

    media_ring_free(audio.pcm);
    audio.pcm = NULL;
    free(audio.chunk);
    audio.chunk = NULL;
    if (audio.lock)
        rg_mutex_free(audio.lock);
    audio.lock = NULL;
}

bool media_audio_set_sample_rate(uint32_t sample_rate)
{
    if (!audio.lock || sample_rate < MEDIA_PCM_SAMPLE_RATE_MIN || sample_rate > MEDIA_PCM_SAMPLE_RATE_MAX)
        return false;

    rg_mutex_take(audio.lock, -1);
    if (sample_rate == audio.sample_rate && (int)sample_rate == rg_audio_get_sample_rate())
    {
        rg_mutex_give(audio.lock);
        return true;
    }

    // Reconfiguring I2S restarts the clock; mute across it so the transient never reaches
    // the speaker, and fade back in afterwards.
    bool was_muted = rg_audio_get_mute();
    audio.fade = 0.0f;
    audio.fade_target = 0.0f;

    rg_audio_set_mute(true);
    rg_audio_set_sample_rate((int)sample_rate);
    if (!was_muted)
        rg_audio_set_mute(false);

    audio.sample_rate = sample_rate;
    media_eq_set_sample_rate(sample_rate);
    audio.fade_target = audio.paused ? 0.0f : 1.0f;
    rg_mutex_give(audio.lock);

    RG_LOGI("Output rate is now %u Hz", (unsigned)sample_rate);
    return true;
}

uint32_t media_audio_get_sample_rate(void)
{
    return audio.sample_rate;
}

size_t media_audio_write(const int16_t *pcm, size_t frames, int timeout_ms)
{
    if (!audio.pcm || !pcm || !frames)
        return 0;

    size_t bytes = frames * MEDIA_PCM_CHANNELS * sizeof(int16_t);
    size_t written = media_ring_write(audio.pcm, pcm, bytes, timeout_ms);
    return written / (MEDIA_PCM_CHANNELS * sizeof(int16_t));
}

void media_audio_flush(uint32_t position_ms)
{
    if (!audio.pcm)
        return;

    rg_mutex_take(audio.lock, -1);
    // Ramp down first so the discontinuity at the seek point is not audible.
    audio.fade = 0.0f;
    media_ring_reset(audio.pcm);

    audio.base_ms = position_ms;
    audio.frames_at_base = audio.frames_played;
    audio.track_marked = false;
    audio.drained = false;
    audio.drain_padded = false;
    audio.fade_target = audio.paused ? 0.0f : 1.0f;

    media_eq_flush();
    media_fft_reset();
    rg_mutex_give(audio.lock);
}

void media_audio_set_buffering(bool buffering)
{
    if (!audio.lock)
        return;
    rg_mutex_take(audio.lock, -1);
    audio.buffering = buffering;
    if (buffering)
        audio.fade = 0.0f;
    rg_mutex_give(audio.lock);
}

bool media_audio_get_buffering(void)
{
    if (!audio.lock)
        return true;
    rg_mutex_take(audio.lock, -1);
    bool buffering = audio.buffering;
    rg_mutex_give(audio.lock);
    return buffering;
}

size_t media_audio_capacity_frames(void)
{
    return media_ring_capacity(audio.pcm) / (MEDIA_PCM_CHANNELS * sizeof(int16_t));
}

void media_audio_set_paused(bool paused)
{
    if (!audio.lock)
        return;
    rg_mutex_take(audio.lock, -1);
    if (audio.paused == paused)
    {
        rg_mutex_give(audio.lock);
        return;
    }
    audio.paused = paused;
    audio.fade_target = paused ? 0.0f : 1.0f;
    rg_mutex_give(audio.lock);
}

bool media_audio_get_paused(void)
{
    return audio.paused;
}

void media_audio_set_gain(float gain)
{
    if (!(gain > 0.0f) || !isfinite(gain))
        gain = 0.0f;
    if (!audio.lock)
        return;
    rg_mutex_take(audio.lock, -1);
    audio.gain = media_clampf(gain, 0.0f, 4.0f);
    rg_mutex_give(audio.lock);
}

float media_audio_get_gain(void)
{
    return audio.gain;
}

void media_audio_set_draining(bool draining)
{
    if (!audio.lock)
        return;
    rg_mutex_take(audio.lock, -1);
    audio.draining = draining;
    // Short files and the final partial chunk must play even below the prebuffer target.
    if (draining)
        audio.buffering = false;
    if (!draining)
    {
        audio.drained = false;
        audio.drain_padded = false;
    }
    rg_mutex_give(audio.lock);
}

bool media_audio_drained(void)
{
    return audio.drained;
}

uint32_t media_audio_position_ms(void)
{
    if (!audio.lock)
        return 0;
    rg_mutex_take(audio.lock, -1);
    uint64_t frames = audio.frames_played - audio.frames_at_base;
    uint32_t position = audio.base_ms + (audio.sample_rate ?
        (uint32_t)((frames * 1000ULL) / audio.sample_rate) : 0);
    rg_mutex_give(audio.lock);
    return position;
}

void media_audio_mark_track(uint64_t frame, uint32_t position_ms)
{
    if (!audio.lock) return;
    rg_mutex_take(audio.lock, -1);
    audio.track_boundary = frame;
    audio.track_position_ms = position_ms;
    audio.track_marked = true;
    rg_mutex_give(audio.lock);
}

int media_audio_fill_percent(void)
{
    return media_ring_fill_percent(audio.pcm);
}

size_t media_audio_buffered_frames(void)
{
    if (!audio.pcm)
        return 0;
    return media_ring_used(audio.pcm) / (MEDIA_PCM_CHANNELS * sizeof(int16_t));
}

uint32_t media_audio_underruns(void)
{
    return audio.underruns;
}

uint64_t media_audio_frames_played(void)
{
    if (!audio.lock)
        return 0;
    rg_mutex_take(audio.lock, -1);
    uint64_t frames = audio.frames_played;
    rg_mutex_give(audio.lock);
    return frames;
}
