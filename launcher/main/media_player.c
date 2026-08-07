#include "media_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include <esp_audio_simple_dec.h>
#include <esp_audio_dec_reg.h>
#include <esp_mp3_dec.h>
#endif

#define STREAM_BUFFER_LARGE (256 * 1024)
#define STREAM_BUFFER_SMALL (64 * 1024)
#define INPUT_CHUNK 4096
#define PCM_BUFFER 9216

typedef struct {
    rg_mutex_t *lock;
    uint8_t *ring;
    size_t ring_size;
    size_t read_pos;
    size_t write_pos;
    size_t used;
    FILE *file;
    uint32_t generation;
    uint32_t io_generation;
    uint32_t seek_offset;
    bool eof;
    bool initialized;
    bool paused;
    bool stop;
    media_player_snapshot_t view;
} player_context_t;

static player_context_t player;

static void lock(void) { rg_mutex_take(player.lock, 1000); }
static void unlock(void) { rg_mutex_give(player.lock); }

static void set_error(const char *message)
{
    lock();
    player.view.state = MEDIA_ERROR;
    snprintf(player.view.error, sizeof(player.view.error), "%s", message);
    unlock();
}

static size_t ring_read(uint8_t *dest, size_t max, uint32_t generation)
{
    lock();
    if (generation != player.generation) { unlock(); return 0; }
    size_t count = RG_MIN(max, player.used);
    size_t first = RG_MIN(count, player.ring_size - player.read_pos);
    memcpy(dest, player.ring + player.read_pos, first);
    memcpy(dest + first, player.ring, count - first);
    player.read_pos = (player.read_pos + count) % player.ring_size;
    player.used -= count;
    unlock();
    return count;
}

static void io_task(void *arg)
{
    uint32_t active_generation = 0;
    while (true) {
        lock();
        uint32_t generation = player.generation;
        char path[RG_PATH_MAX + 1]; snprintf(path, sizeof(path), "%s", player.view.path);
        uint32_t offset = player.seek_offset;
        bool stopped = player.stop;
        size_t free_space = player.ring_size - player.used;
        size_t write_pos = player.write_pos;
        unlock();

        if (!generation || stopped) { rg_task_delay(20); continue; }
        if (generation != active_generation) {
            if (player.file) fclose(player.file), player.file = NULL;
            player.file = fopen(path, "rb");
            if (!player.file || fseek(player.file, offset, SEEK_SET)) {
                if (player.file) fclose(player.file), player.file = NULL;
                set_error("Unable to read MP3 from SD card");
                rg_task_delay(50); continue;
            }
            lock();
            player.read_pos = player.write_pos = player.used = 0;
            player.eof = false;
            player.io_generation = generation;
            unlock();
            active_generation = generation;
            free_space = player.ring_size;
            write_pos = 0;
        }
        if (free_space < INPUT_CHUNK || !player.file) { rg_task_delay(2); continue; }
        size_t request = RG_MIN(free_space, player.ring_size - write_pos);
        request = RG_MIN(request, 16 * 1024);
        size_t got = fread(player.ring + write_pos, 1, request, player.file);
        lock();
        if (generation == player.generation && active_generation == player.io_generation) {
            player.write_pos = (player.write_pos + got) % player.ring_size;
            player.used += got;
            if (got < request) player.eof = true;
        }
        unlock();
        if (!got) rg_task_delay(10);
    }
}

static void update_spectrum(const int16_t *pcm, size_t samples, int channels, int sample_rate)
{
    if (!samples || !sample_rate) return;
    static const uint16_t frequencies[16] = {60,90,130,190,270,390,560,800,1150,1650,2350,3350,4800,6800,9700,13700};
    uint8_t bars[16] = {0};
    size_t frames = samples / channels;
    size_t count = RG_MIN(frames, (size_t)256), step = RG_MAX(1, frames / count);
    for (int band = 0; band < 16; band++) {
        if (frequencies[band] >= sample_rate / 2) continue;
        float coeff = 2.0f * cosf(2.0f * 3.14159265f * frequencies[band] / sample_rate);
        float q1 = 0, q2 = 0;
        for (size_t n = 0, index = 0; n < count && index < frames; n++, index += step) {
            int32_t v = pcm[index * channels]; if (channels > 1) v = (v + pcm[index * channels + 1]) / 2;
            float q0 = v / 32768.0f + coeff * q1 - q2; q2 = q1; q1 = q0;
        }
        float magnitude = sqrtf(RG_MAX(0.0f, q1 * q1 + q2 * q2 - coeff * q1 * q2)) / count;
        bars[band] = RG_MIN(100, (int)(magnitude * 900.0f));
    }
    lock();
    for (int i = 0; i < 16; i++) player.view.spectrum[i] = (player.view.spectrum[i] * 3 + bars[i]) / 4;
    unlock();
}

static void submit_pcm(const uint8_t *data, size_t bytes, int channels, int sample_rate)
{
    const int16_t *pcm = (const int16_t *)data;
    size_t samples = bytes / 2;
    update_spectrum(pcm, samples, channels, sample_rate);
    if (channels == 2) {
        rg_audio_submit((const rg_audio_frame_t *)pcm, samples / 2);
    } else {
        rg_audio_frame_t frames[256];
        size_t done = 0;
        while (done < samples) {
            size_t count = RG_MIN(RG_COUNT(frames), samples - done);
            for (size_t i = 0; i < count; i++) frames[i].left = frames[i].right = pcm[done + i];
            rg_audio_submit(frames, count); done += count;
        }
    }
}

static void decoder_task(void *arg)
{
#ifdef ESP_PLATFORM
    esp_mp3_dec_register();
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *input = rg_alloc(INPUT_CHUNK, MEM_FAST);
    uint8_t *pcm = rg_alloc(PCM_BUFFER, MEM_FAST);
    uint32_t active_generation = 0;
    uint64_t played_samples = 0;
    uint32_t position_base = 0;
    int current_rate = 0, channels = 2;
    while (input && pcm) {
        lock();
        uint32_t generation = player.generation;
        bool stopped = player.stop;
        bool paused = player.paused;
        uint32_t io_generation = player.io_generation;
        bool eof = player.eof;
        size_t buffered = player.used;
        unlock();
        if (!generation || stopped || paused || io_generation != generation) { rg_task_delay(5); continue; }
        if (generation != active_generation) {
            if (decoder) esp_audio_simple_dec_close(decoder), decoder = NULL;
            esp_audio_simple_dec_cfg_t cfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, .use_frame_dec = false};
            if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) { set_error("MP3 decoder could not start"); rg_task_delay(50); continue; }
            lock(); position_base = player.view.position_ms; player.view.state = MEDIA_BUFFERING; unlock();
            played_samples = 0; current_rate = 0; active_generation = generation;
        }
        size_t got = ring_read(input, INPUT_CHUNK, generation);
        if (!got) {
            if (eof && !buffered) {
                lock(); player.view.state = MEDIA_STOPPED; player.view.finished = true; player.stop = true; unlock();
            } else {
                lock(); if (player.view.state == MEDIA_PLAYING) player.view.underruns++; player.view.state = MEDIA_BUFFERING; unlock();
                rg_task_delay(2);
            }
            continue;
        }
        esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = got, .eos = eof && got == buffered};
        while (raw.len && generation == player.generation) {
            esp_audio_simple_dec_out_t out = {.buffer = pcm, .len = PCM_BUFFER};
            raw.consumed = 0;
            esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &out);
            if (result != ESP_AUDIO_ERR_OK) {
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) set_error("MP3 frame exceeds PCM buffer");
                else set_error("Invalid or unsupported MP3 stream");
                break;
            }
            if (!raw.consumed && !out.decoded_size) break;
            raw.buffer += raw.consumed; raw.len -= raw.consumed;
            if (out.decoded_size) {
                esp_audio_simple_dec_info_t info = {0}; esp_audio_simple_dec_get_info(decoder, &info);
                if (info.sample_rate && current_rate != (int)info.sample_rate) { rg_audio_set_sample_rate(info.sample_rate); current_rate = info.sample_rate; }
                channels = info.channel ?: 2;
                submit_pcm(out.buffer, out.decoded_size, channels, current_rate);
                played_samples += out.decoded_size / (2 * channels);
                lock();
                player.view.state = MEDIA_PLAYING;
                if (current_rate) player.view.position_ms = position_base + played_samples * 1000 / current_rate;
                if (info.bitrate) player.view.metadata.bitrate = info.bitrate;
                uint32_t br = player.view.metadata.bitrate;
                player.view.buffered_ms = player.view.position_ms + (br ? (uint64_t)player.used * 8000 / br : 0);
                if (player.view.duration_ms && player.view.buffered_ms > player.view.duration_ms) player.view.buffered_ms = player.view.duration_ms;
                unlock();
            }
        }
    }
    if (decoder) esp_audio_simple_dec_close(decoder);
    free(input); free(pcm);
#else
    set_error("MP3 playback is not available on this platform");
#endif
}

bool media_player_init(void)
{
    if (player.initialized) return true;
    memset(&player, 0, sizeof(player));
    player.lock = rg_mutex_create();
    player.ring = rg_alloc(STREAM_BUFFER_LARGE, MEM_SLOW);
    player.ring_size = STREAM_BUFFER_LARGE;
    if (!player.ring) player.ring = rg_alloc(STREAM_BUFFER_SMALL, MEM_SLOW), player.ring_size = STREAM_BUFFER_SMALL;
    if (!player.lock || !player.ring) return false;
    player.initialized = true; player.stop = true;
    if (!rg_task_create("media_sd", io_task, NULL, 4 * 1024, RG_TASK_PRIORITY_4, RG_TASK_AFFINITY_IO) ||
        !rg_task_create("media_dec", decoder_task, NULL, 20 * 1024, RG_TASK_PRIORITY_7, RG_TASK_AFFINITY_AUDIO)) {
        player.initialized = false; return false;
    }
    return true;
}

bool media_player_play(const char *path, const media_metadata_t *meta, uint32_t start_ms)
{
    if (!player.initialized && !media_player_init()) return false;
    media_metadata_t local;
    if (!meta) { if (!media_metadata_read(path, &local, true)) return false; meta = &local; }
    if (start_ms > meta->duration_ms && meta->duration_ms) start_ms = meta->duration_ms;
    uint32_t offset = meta->audio_offset;
    if (meta->duration_ms && start_ms) offset += (uint64_t)meta->audio_size * start_ms / meta->duration_ms;
    lock();
    player.generation++;
    player.seek_offset = offset;
    player.stop = false; player.paused = false; player.eof = false;
    player.read_pos = player.write_pos = player.used = 0;
    memset(&player.view, 0, sizeof(player.view));
    player.view.state = MEDIA_BUFFERING; player.view.metadata = *meta;
    player.view.duration_ms = meta->duration_ms; player.view.position_ms = start_ms;
    snprintf(player.view.path, sizeof(player.view.path), "%s", path);
    unlock();
    return true;
}

void media_player_toggle_pause(void)
{
    if (!player.initialized || !player.lock) return;
    lock();
    if (!player.stop && player.view.state != MEDIA_ERROR) {
        player.paused = !player.paused;
        player.view.state = player.paused ? MEDIA_PAUSED : MEDIA_BUFFERING;
    }
    unlock();
}

void media_player_set_paused(bool paused)
{
    if (!player.initialized || !player.lock) return;
    lock();
    if (!player.stop && player.view.state != MEDIA_ERROR) {
        player.paused = paused;
        player.view.state = paused ? MEDIA_PAUSED : MEDIA_BUFFERING;
    }
    unlock();
}

void media_player_stop(void)
{
    if (!player.initialized || !player.lock) return;
    lock(); player.stop = true; player.paused = false; player.used = 0; player.eof = false;
    player.view.state = MEDIA_STOPPED; player.view.position_ms = 0; player.view.buffered_ms = 0; unlock();
}

void media_player_seek_to(uint32_t ms)
{
    if (!player.initialized || !player.lock) return;
    lock(); uint32_t duration = player.view.duration_ms; char path[RG_PATH_MAX + 1];
    media_metadata_t meta = player.view.metadata; snprintf(path, sizeof(path), "%s", player.view.path); unlock();
    if (duration && ms > duration) ms = duration;
    if (path[0]) media_player_play(path, &meta, ms);
}

void media_player_seek(int32_t delta)
{
    media_player_snapshot_t s; media_player_get_snapshot(&s);
    int64_t target = (int64_t)s.position_ms + delta; media_player_seek_to(target < 0 ? 0 : target);
}

void media_player_get_snapshot(media_player_snapshot_t *s)
{
    if (!s) return;
    if (!player.initialized || !player.lock) {
        memset(s, 0, sizeof(*s));
        s->state = MEDIA_STOPPED;
        return;
    }
    lock(); *s = player.view; unlock();
}

bool media_player_take_finished(void)
{
    if (!player.initialized || !player.lock) return false;
    lock(); bool finished = player.view.finished; player.view.finished = false; unlock(); return finished;
}
