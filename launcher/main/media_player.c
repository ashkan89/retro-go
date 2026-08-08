#include "media_player.h"
#include "media_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_audio_simple_dec.h>
#include <esp_audio_dec_reg.h>
#include <esp_mp3_dec.h>
#if MEDIA_ENABLE_EXTRA_CODECS
#include <esp_aac_dec.h>
#include <esp_flac_dec.h>
/* The WAV and M4A container parsers live under simple_dec/impl, which is not
   on the include path; the default header pulls them in for us. */
#include <esp_audio_simple_dec_default.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

/* The ring is the only thing standing between a slow SD read and an audible
   gap, so we take as much PSRAM as we can reasonably get. At 320 kbit/s
   (the worst realistic MP3) 512 KB is roughly thirteen seconds of audio, which
   comfortably covers a directory scan or a cover-art decode on the UI thread. */
static const size_t ring_sizes[] = {512 * 1024, 320 * 1024, 128 * 1024, 64 * 1024};

/* Read in fairly large blocks: FATFS/SDSPI overhead per call is significant
   and the bus is idle most of the time anyway. */
#define IO_CHUNK (32 * 1024)
/* Refill as soon as a chunk fits, rather than waiting for the ring to drain. */
#define IO_REFILL_THRESHOLD IO_CHUNK

/* The SD driver needs a DMA capable (internal RAM) destination. Reading
   straight into the PSRAM ring makes it allocate a bounce buffer for every
   single call, and those allocations fail once the launcher has filled
   internal RAM -- which is exactly when the card driver starts misbehaving.
   Staging through a small permanent internal buffer avoids that entirely and
   is measurably faster as a bonus. */
#define IO_STAGING (16 * 1024)

/* How many consecutive failed reads before we give up on the file. The card
   driver already retries internally, so reaching this means something is
   genuinely wrong rather than a passing glitch. */
#define MEDIA_READ_ERROR_LIMIT 8

/* Must comfortably hold one compressed frame of any supported format; FLAC
   frames are the largest and routinely pass 4 KB. */
#define INPUT_CHUNK 8192
#define PCM_BUFFER_INITIAL (16 * 1024)
#define PCM_BUFFER_MAX (64 * 1024)

/* Decoding must not start before the ring holds a healthy reserve, otherwise
   the very first seconds of every track fight the SD card for data. */
#define PREBUFFER_START(ring) RG_MIN((size_t)((ring) / 2), (size_t)(128 * 1024))
/* After an underrun we rebuild a smaller reserve so recovery stays quick. */
#define PREBUFFER_RESUME(ring) RG_MIN((size_t)((ring) / 4), (size_t)(48 * 1024))

/* A handful of corrupt frames is normal in the wild (tag junk, torn files).
   Only give up when the stream is hopeless. */
#define DECODE_ERROR_LIMIT 64

#ifdef ESP_PLATFORM
typedef SemaphoreHandle_t event_t;
static event_t event_create(void) { return xSemaphoreCreateBinary(); }
static void event_signal(event_t event) { if (event) xSemaphoreGive(event); }
static void event_wait(event_t event, uint32_t ms)
{
    if (event) xSemaphoreTake(event, pdMS_TO_TICKS(ms));
    else rg_task_delay(ms);
}
#else
typedef void *event_t;
static event_t event_create(void) { return NULL; }
static void event_signal(event_t event) { (void)event; }
static void event_wait(event_t event, uint32_t ms) { (void)event; rg_task_delay(ms); }
#endif

typedef struct {
    rg_mutex_t *lock;
    event_t io_wake;
    event_t decoder_wake;

    uint8_t *ring;
    size_t ring_size;
    size_t read_pos;
    size_t write_pos;
    size_t used;

    /* Bumped by every play/seek request. Both worker tasks compare it against
       their own copy to discover that the stream underneath them changed. */
    uint32_t generation;
    uint32_t io_generation;
    uint32_t seek_offset;

    bool eof;
    bool initialized;
    bool paused;
    bool stop;
    bool analyzer;

    int16_t scope[MEDIA_SCOPE_FRAMES * 2];
    size_t scope_count;
    uint32_t scope_rate;

    /* Kept out of the snapshot: only seeking needs the full tag block, and
       copying it to every caller was a kilobyte and a half of stack each. */
    media_metadata_t seek_meta;

    media_player_snapshot_t view;
} player_context_t;

static player_context_t player;

/* A failed take would mean the mutex is gone or a task deadlocked; carrying on
   without the lock would corrupt the ring, so wait as long as it takes. */
static void lock(void)
{
    while (!rg_mutex_take(player.lock, 1000))
        RG_LOGW("Media player lock is stuck");
}
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
    /* Space just became available: let the reader top the ring back up now
       instead of on its next timed poll. */
    if (count)
        event_signal(player.io_wake);
    return count;
}

/*******************************************************************************
 * SD reader
 ******************************************************************************/

static void io_task(void *arg)
{
    uint32_t active_generation = 0;
    char open_path[RG_PATH_MAX + 1] = "";
    FILE *file = NULL;
    /* Our own idea of where the read cursor is. After a failed fread() the C
       stream position is not defined, so we seek back here rather than trust
       it. */
    uint32_t stream_pos = 0;
    int read_errors = 0;
    uint8_t *staging = rg_alloc(IO_STAGING, MEM_FAST | MEM_DMA | MEM_NOPANIC);

    if (!staging) {
        /* Falling back to the ring directly still works, it just makes the
           driver bounce every read itself. */
        RG_LOGW("Media reader could not reserve a DMA staging buffer");
    }

    while (true) {
        lock();
        uint32_t generation = player.generation;
        bool stopped = player.stop;
        uint32_t offset = player.seek_offset;
        char path[RG_PATH_MAX + 1];
        snprintf(path, sizeof(path), "%s", player.view.path);
        unlock();

        if (!generation || stopped) {
            /* Holding a FILE open across a stop keeps FATFS structures and an
               SD sector cache alive for no reason. */
            if (file) { fclose(file); file = NULL; open_path[0] = 0; }
            active_generation = 0;
            event_wait(player.io_wake, 200);
            continue;
        }

        if (generation != active_generation) {
            /* Seeking inside the current file only needs an fseek. Opening a
               file on FATFS costs tens of milliseconds, which is exactly the
               kind of stall that produces an audible gap. */
            if (file && strcmp(open_path, path) != 0) {
                fclose(file);
                file = NULL;
                open_path[0] = 0;
            }
            if (!file) {
                file = fopen(path, "rb");
                if (file)
                    snprintf(open_path, sizeof(open_path), "%s", path);
            }
            if (!file || fseek(file, offset, SEEK_SET) != 0) {
                if (file) { fclose(file); file = NULL; open_path[0] = 0; }
                set_error("Unable to read this file from the SD card");
                lock();
                if (generation == player.generation)
                    player.stop = true;
                unlock();
                event_wait(player.io_wake, 200);
                continue;
            }
            clearerr(file);
            stream_pos = offset;
            read_errors = 0;

            lock();
            if (generation != player.generation) { unlock(); continue; }
            player.read_pos = player.write_pos = player.used = 0;
            player.eof = false;
            player.io_generation = generation;
            unlock();

            active_generation = generation;
            event_signal(player.decoder_wake);
        }

        lock();
        size_t free_space = player.ring_size - player.used;
        size_t write_pos = player.write_pos;
        bool eof = player.eof;
        unlock();

        if (eof) { event_wait(player.io_wake, 200); continue; }
        if (free_space < IO_REFILL_THRESHOLD) { event_wait(player.io_wake, 50); continue; }

        size_t request = RG_MIN(RG_MIN(free_space, player.ring_size - write_pos), (size_t)IO_CHUNK);
        size_t got = 0;
        if (staging) {
            while (got < request) {
                size_t slice = RG_MIN(request - got, (size_t)IO_STAGING);
                size_t read = fread(staging, 1, slice, file);
                if (read) {
                    memcpy(player.ring + write_pos + got, staging, read);
                    got += read;
                }
                if (read < slice)
                    break;
            }
        } else {
            got = fread(player.ring + write_pos, 1, request, file);
        }
        stream_pos += got;

        /* A short read means one of two very different things. Treating an I/O
           error as end of stream is what made a flaky card look like a track
           that finished early: the decoder drained the ring, reported the track
           complete, and the playlist restarted it. Only a genuine EOF ends the
           stream; an error is retried from the position we know is correct. */
        bool truncated = got < request;
        bool failed = truncated && ferror(file);
        bool at_end = truncated && !failed;

        if (failed) {
            clearerr(file);
            read_errors++;
            RG_LOGW("Media read failed at offset %lu (attempt %d)", (unsigned long)stream_pos, read_errors);
            if (fseek(file, stream_pos, SEEK_SET) != 0)
                read_errors = MEDIA_READ_ERROR_LIMIT;
        } else if (got) {
            read_errors = 0;
        }

        lock();
        bool still_current = generation == player.generation && active_generation == player.io_generation;
        if (still_current) {
            player.write_pos = (player.write_pos + got) % player.ring_size;
            player.used += got;
            if (at_end)
                player.eof = true;
            player.view.read_errors = read_errors;
            player.view.buffer_fill = player.used * 100 / player.ring_size;
        }
        unlock();

        if (still_current && read_errors >= MEDIA_READ_ERROR_LIMIT) {
            set_error("SD card read errors, playback stopped");
            lock();
            if (generation == player.generation)
                player.stop = true;
            unlock();
            continue;
        }

        if (still_current && got)
            event_signal(player.decoder_wake);
        if (failed)
            /* Give the card a moment before hammering it again. */
            event_wait(player.io_wake, 50);
        else if (!got)
            event_wait(player.io_wake, 20);
    }
}

/*******************************************************************************
 * Decoder
 ******************************************************************************/

#ifdef ESP_PLATFORM
/* `pcm` is always interleaved stereo by the time this is called. */
static void capture_scope(const int16_t *pcm, size_t frames, uint32_t rate)
{
    if (!frames)
        return;
    /* Keep the newest window; a straight memcpy costs almost nothing even for
       a large FLAC frame. */
    size_t count = RG_MIN(frames, (size_t)MEDIA_SCOPE_FRAMES);
    lock();
    memcpy(player.scope, pcm + (frames - count) * 2, count * 2 * sizeof(int16_t));
    player.scope_count = count;
    player.scope_rate = rate;
    unlock();
}

/* Normalises whatever the decoder produced (8/16/24/32 bit, 1..N channels)
   into the interleaved stereo int16 frames the audio driver expects, runs the
   equalizer over it, and hands it to the audio driver. */
static void submit_pcm(uint8_t *data, size_t bytes, int channels, int bits, uint32_t rate)
{
    static rg_audio_frame_t frames[512];
    if (channels < 1)
        channels = 2;

    if (bits == 16 && channels == 2) {
        /* Already in the target layout, so filter it where it sits. */
        size_t count = bytes / 4;
        media_eq_process((int16_t *)data, count, rate);
        if (player.analyzer)
            capture_scope((const int16_t *)data, count, rate);
        rg_audio_submit((const rg_audio_frame_t *)data, count);
        return;
    }

    int bytes_per_sample = bits / 8;
    if (bytes_per_sample < 1 || bytes_per_sample > 4)
        return;
    size_t total_frames = bytes / (bytes_per_sample * channels);
    size_t done = 0;

    while (done < total_frames) {
        size_t count = RG_MIN(RG_COUNT(frames), total_frames - done);
        for (size_t i = 0; i < count; i++) {
            const uint8_t *src = data + (done + i) * bytes_per_sample * channels;
            int32_t sample[2] = {0, 0};
            for (int c = 0; c < channels; c++) {
                const uint8_t *p = src + c * bytes_per_sample;
                int32_t value;
                switch (bytes_per_sample) {
                case 1: value = ((int32_t)p[0] - 128) << 8; break;
                case 2: value = (int16_t)(p[0] | (p[1] << 8)); break;
                case 3: value = (int16_t)((p[1] | (p[2] << 8))); break;
                default: value = (int16_t)(p[2] | (p[3] << 8)); break;
                }
                /* Anything past the second channel is folded into both sides
                   so surround sources stay audible instead of vanishing. */
                if (c == 0) sample[0] += value;
                else if (c == 1) sample[1] += value;
                else { sample[0] += value / 2; sample[1] += value / 2; }
            }
            if (channels == 1)
                sample[1] = sample[0];
            frames[i].left = RG_MIN(RG_MAX(sample[0], -32768), 32767);
            frames[i].right = RG_MIN(RG_MAX(sample[1], -32768), 32767);
        }
        media_eq_process((int16_t *)frames, count, rate);
        if (player.analyzer && done == 0)
            capture_scope((const int16_t *)frames, count, rate);
        rg_audio_submit(frames, count);
        done += count;
    }
}

static void register_decoders(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    esp_mp3_dec_register();
#if MEDIA_ENABLE_EXTRA_CODECS
    esp_aac_dec_register();
    esp_flac_dec_register();
    esp_wav_dec_register();
    esp_m4a_dec_register();
#endif
}

static esp_audio_simple_dec_type_t decoder_type_for(media_format_t format)
{
    switch (format) {
    case MEDIA_FORMAT_MP3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
#if MEDIA_ENABLE_EXTRA_CODECS
    case MEDIA_FORMAT_AAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case MEDIA_FORMAT_M4A: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case MEDIA_FORMAT_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case MEDIA_FORMAT_WAV: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
#endif
    default: return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static void decoder_task(void *arg)
{
    register_decoders();

    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_type_t decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    uint8_t *input = rg_alloc(INPUT_CHUNK, MEM_FAST | MEM_NOPANIC);
    size_t pcm_size = PCM_BUFFER_INITIAL;
    uint8_t *pcm = rg_alloc(pcm_size, MEM_FAST | MEM_NOPANIC);
    if (!pcm)
        pcm = rg_alloc(pcm_size, MEM_SLOW | MEM_NOPANIC);

    if (!input || !pcm) {
        free(input);
        free(pcm);
        set_error("Not enough memory for the decoder");
        return;
    }

    uint32_t active_generation = 0;
    uint64_t played_samples = 0;
    uint32_t position_base = 0;
    uint32_t current_rate = 0;
    size_t prebuffer = 0;
    /* Bytes left over from the previous pass. A parser that needs a whole
       frame may consume nothing, and throwing those bytes away is what makes
       a stream stutter or lose sync. */
    size_t input_fill = 0;
    int channels = 2, bits = 16;
    int consecutive_errors = 0;

    while (true) {
        lock();
        uint32_t generation = player.generation;
        bool stopped = player.stop;
        bool paused = player.paused;
        uint32_t io_generation = player.io_generation;
        bool eof = player.eof;
        size_t buffered = player.used;
        media_format_t format = player.view.format;
        unlock();

        if (!generation || stopped || paused || io_generation != generation) {
            event_wait(player.decoder_wake, 100);
            continue;
        }

        if (generation != active_generation) {
            esp_audio_simple_dec_type_t type = decoder_type_for(format);
            if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE ||
                esp_audio_simple_check_audio_type(type) != ESP_AUDIO_ERR_OK) {
                set_error("This file format is not supported");
                lock();
                if (generation == player.generation) player.stop = true;
                unlock();
                continue;
            }
            /* Reusing the handle across seeks in the same stream avoids the
               allocation churn (and the click) of a full teardown. */
            bool reused = false;
            if (decoder && decoder_type == type && esp_audio_simple_dec_reset(decoder) == ESP_AUDIO_ERR_OK)
                reused = true;
            if (!reused) {
                if (decoder) { esp_audio_simple_dec_close(decoder); decoder = NULL; }
                esp_audio_simple_dec_cfg_t cfg = {.dec_type = type, .use_frame_dec = false};
                if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
                    decoder = NULL;
                    set_error("The decoder could not start");
                    lock();
                    if (generation == player.generation) player.stop = true;
                    unlock();
                    continue;
                }
                decoder_type = type;
            }
            lock();
            position_base = player.view.position_ms;
            player.view.state = MEDIA_BUFFERING;
            unlock();
            played_samples = 0;
            current_rate = 0;
            consecutive_errors = 0;
            input_fill = 0;
            /* Stale filter history from the previous position would ring
               through as a click on the first frames of the new one. */
            media_eq_reset();
            prebuffer = PREBUFFER_START(player.ring_size);
            active_generation = generation;
        }

        if (prebuffer) {
            if (buffered < prebuffer && !eof) {
                event_signal(player.io_wake);
                event_wait(player.decoder_wake, 50);
                continue;
            }
            prebuffer = 0;
        }

        size_t got = ring_read(input + input_fill, INPUT_CHUNK - input_fill, generation);
        input_fill += got;
        if (!got && input_fill < INPUT_CHUNK) {
            if (eof && !buffered) {
                lock();
                player.view.state = MEDIA_STOPPED;
                player.view.finished = true;
                player.stop = true;
                unlock();
            } else {
                lock();
                if (player.view.state == MEDIA_PLAYING)
                    player.view.underruns++;
                player.view.state = MEDIA_BUFFERING;
                unlock();
                prebuffer = PREBUFFER_RESUME(player.ring_size);
                event_signal(player.io_wake);
                event_wait(player.decoder_wake, 50);
            }
            continue;
        }

        esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = input_fill, .eos = eof && !buffered};
        while (raw.len && generation == player.generation) {
            esp_audio_simple_dec_out_t out = {.buffer = pcm, .len = pcm_size};
            raw.consumed = 0;
            esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &out);

            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                /* Grow instead of failing: FLAC and 24-bit sources routinely
                   produce frames larger than a conservative default. */
                size_t needed = RG_MAX((size_t)out.needed_size, pcm_size * 2);
                uint8_t *grown = needed <= PCM_BUFFER_MAX ? rg_alloc(needed, MEM_SLOW | MEM_NOPANIC) : NULL;
                if (!grown) {
                    /* Retrying would just fail identically on the next frame,
                       so end the track instead of looping on the error. */
                    set_error(needed > PCM_BUFFER_MAX ? "Audio frame is too large to decode"
                                                      : "Not enough memory to decode this file");
                    lock();
                    if (generation == player.generation) player.stop = true;
                    unlock();
                    break;
                }
                free(pcm);
                pcm = grown;
                pcm_size = needed;
                continue;
            }

            if (result != ESP_AUDIO_ERR_OK) {
                lock();
                player.view.decode_errors++;
                unlock();
                if (++consecutive_errors >= DECODE_ERROR_LIMIT) {
                    set_error("Invalid or unsupported audio stream");
                    lock();
                    if (generation == player.generation) player.stop = true;
                    unlock();
                    raw.len = 0;
                    break;
                }
                /* Step over one byte and let the parser resynchronise on the
                   next frame header rather than killing playback outright. */
                raw.buffer++;
                raw.len--;
                continue;
            }

            if (!raw.consumed && !out.decoded_size)
                break;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;

            if (out.decoded_size) {
                consecutive_errors = 0;
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(decoder, &info);
                if (info.sample_rate && current_rate != info.sample_rate) {
                    rg_audio_set_sample_rate(info.sample_rate);
                    current_rate = info.sample_rate;
                }
                channels = info.channel ? info.channel : 2;
                bits = info.bits_per_sample ? info.bits_per_sample : 16;
                submit_pcm(out.buffer, out.decoded_size, channels, bits, current_rate);

                size_t frame_bytes = (size_t)channels * RG_MAX(1, bits / 8);
                if (frame_bytes)
                    played_samples += out.decoded_size / frame_bytes;

                lock();
                if (generation == player.generation) {
                    player.view.state = MEDIA_PLAYING;
                    player.view.sample_rate = current_rate;
                    player.view.channels = channels;
                    if (info.bitrate)
                        player.view.bitrate = info.bitrate;
                    if (current_rate)
                        player.view.position_ms = position_base + played_samples * 1000 / current_rate;
                    uint32_t bitrate = player.view.bitrate ? player.view.bitrate : player.seek_meta.bitrate;
                    player.view.buffered_ms = player.view.position_ms +
                        (bitrate ? (uint64_t)player.used * 8000 / bitrate : 0);
                    if (player.view.duration_ms && player.view.buffered_ms > player.view.duration_ms)
                        player.view.buffered_ms = player.view.duration_ms;
                    player.view.buffer_fill = player.used * 100 / player.ring_size;
                }
                unlock();
            }
        }

        /* Whatever the parser has not taken yet moves to the front so the next
           read tops it up into a complete frame. */
        if (raw.len && raw.buffer != input)
            memmove(input, raw.buffer, raw.len);
        input_fill = raw.len;
        if (input_fill >= INPUT_CHUNK) {
            /* A full buffer the parser refuses to touch means the stream is
               damaged here; skip a byte so it can find the next header. */
            memmove(input, input + 1, INPUT_CHUNK - 1);
            input_fill = INPUT_CHUNK - 1;
        }
    }
}
#else
static void decoder_task(void *arg)
{
    set_error("Audio playback is not available on this platform");
    while (true)
        rg_task_delay(1000);
}
#endif

/*******************************************************************************
 * Public API
 ******************************************************************************/

bool media_player_init(void)
{
    if (player.initialized)
        return true;

    memset(&player, 0, sizeof(player));
    player.lock = rg_mutex_create();
    player.io_wake = event_create();
    player.decoder_wake = event_create();

    for (size_t i = 0; i < RG_COUNT(ring_sizes) && !player.ring; i++) {
        player.ring = rg_alloc(ring_sizes[i], MEM_SLOW | MEM_NOPANIC);
        player.ring_size = ring_sizes[i];
    }
    if (!player.lock || !player.ring) {
        RG_LOGE("Media player could not reserve its stream buffer");
        free(player.ring);
        player.ring = NULL;
        return false;
    }

    player.initialized = true;
    player.stop = true;

    /* FATFS plus the SDSPI host uses a fair amount of stack; 4 KB was not a
       comfortable margin for a task whose whole job is file I/O. */
    if (!rg_task_create("media_sd", io_task, NULL, 8 * 1024, RG_TASK_PRIORITY_5, RG_TASK_AFFINITY_IO) ||
        !rg_task_create("media_dec", decoder_task, NULL, 24 * 1024, RG_TASK_PRIORITY_7, RG_TASK_AFFINITY_AUDIO)) {
        RG_LOGE("Media player could not start its worker tasks");
        player.initialized = false;
        return false;
    }
    return true;
}

bool media_player_play(const char *path, const media_metadata_t *meta, uint32_t start_ms)
{
    if (!path || !path[0])
        return false;
    if (!player.initialized && !media_player_init())
        return false;

    /* On the heap, not the stack: media_metadata_t is 1.3 KB and this can be
       reached from deep inside the UI call chain. */
    media_metadata_t *local = NULL;
    if (!meta) {
        local = malloc(sizeof(*local));
        if (!local || !media_metadata_read(path, local, true)) {
            free(local);
            return false;
        }
        meta = local;
    }

    if (meta->duration_ms && start_ms > meta->duration_ms)
        start_ms = meta->duration_ms;
    if (!media_metadata_seekable(meta))
        start_ms = 0;

    uint32_t offset = media_metadata_seek_offset(meta, start_ms);

    lock();
    player.generation++;
    player.seek_offset = offset;
    player.stop = false;
    player.paused = false;
    player.eof = false;
    player.read_pos = player.write_pos = player.used = 0;
    player.scope_count = 0;
    memset(&player.view, 0, sizeof(player.view));
    player.seek_meta = *meta;
    player.view.state = MEDIA_BUFFERING;
    player.view.format = meta->format;
    player.view.audio_size = meta->audio_size;
    player.view.duration_ms = meta->duration_ms;
    player.view.position_ms = start_ms;
    player.view.bitrate = meta->bitrate;
    player.view.sample_rate = meta->sample_rate;
    player.view.channels = meta->channels;
    player.view.seekable = media_metadata_seekable(meta);
    snprintf(player.view.path, sizeof(player.view.path), "%s", path);
    unlock();

    free(local);
    event_signal(player.io_wake);
    event_signal(player.decoder_wake);
    return true;
}

void media_player_toggle_pause(void)
{
    if (!player.initialized)
        return;
    lock();
    bool wake = false;
    if (!player.stop && player.view.state != MEDIA_ERROR) {
        player.paused = !player.paused;
        player.view.state = player.paused ? MEDIA_PAUSED : MEDIA_BUFFERING;
        wake = !player.paused;
    }
    unlock();
    if (wake) {
        event_signal(player.decoder_wake);
        event_signal(player.io_wake);
    }
}

void media_player_set_paused(bool paused)
{
    if (!player.initialized)
        return;
    lock();
    bool wake = false;
    if (!player.stop && player.view.state != MEDIA_ERROR) {
        player.paused = paused;
        player.view.state = paused ? MEDIA_PAUSED : MEDIA_BUFFERING;
        wake = !paused;
    }
    unlock();
    if (wake) {
        event_signal(player.decoder_wake);
        event_signal(player.io_wake);
    }
}

void media_player_stop(void)
{
    if (!player.initialized)
        return;
    lock();
    player.stop = true;
    player.paused = false;
    /* Reset all three ring cursors together; leaving them out of step used to
       corrupt the buffer on the next start. */
    player.read_pos = player.write_pos = player.used = 0;
    player.eof = false;
    player.scope_count = 0;
    player.view.state = MEDIA_STOPPED;
    player.view.position_ms = 0;
    player.view.buffered_ms = 0;
    player.view.buffer_fill = 0;
    unlock();
    event_signal(player.io_wake);
    event_signal(player.decoder_wake);
}

void media_player_seek_to(uint32_t ms)
{
    if (!player.initialized)
        return;
    /* The metadata copy is large, so it is taken on the heap rather than the
       caller's stack. */
    media_metadata_t *meta = malloc(sizeof(*meta));
    if (!meta)
        return;
    lock();
    uint32_t duration = player.view.duration_ms;
    bool seekable = player.view.seekable;
    *meta = player.seek_meta;
    char path[RG_PATH_MAX + 1];
    snprintf(path, sizeof(path), "%s", player.view.path);
    unlock();

    if (path[0] && seekable) {
        if (duration && ms > duration)
            ms = duration > 1000 ? duration - 1000 : 0;
        media_player_play(path, meta, ms);
    }
    free(meta);
}

void media_player_seek(int32_t delta)
{
    media_player_snapshot_t s;
    media_player_get_snapshot(&s);
    int64_t target = (int64_t)s.position_ms + delta;
    media_player_seek_to(target < 0 ? 0 : (uint32_t)target);
}

void media_player_get_snapshot(media_player_snapshot_t *s)
{
    if (!s)
        return;
    if (!player.initialized || !player.lock) {
        memset(s, 0, sizeof(*s));
        s->state = MEDIA_STOPPED;
        return;
    }
    lock();
    *s = player.view;
    unlock();
}

bool media_player_take_finished(void)
{
    if (!player.initialized || !player.lock)
        return false;
    lock();
    bool finished = player.view.finished;
    player.view.finished = false;
    unlock();
    return finished;
}

void media_player_set_analyzer(bool enabled)
{
    /* Plain bool write: worst case the decoder captures (or skips) one extra
       block, which is not worth a lock on the audio path. */
    player.analyzer = enabled;
}

size_t media_player_read_scope(int16_t *out, size_t max_frames, uint32_t *sample_rate)
{
    if (!out || !max_frames || !player.initialized || !player.lock)
        return 0;
    lock();
    size_t count = RG_MIN(max_frames, player.scope_count);
    memcpy(out, player.scope, count * 2 * sizeof(int16_t));
    if (sample_rate)
        *sample_rate = player.scope_rate;
    unlock();
    return count;
}
