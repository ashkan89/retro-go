#include <rg_system.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "media_transition.h"
#include "media_ui_geometry.h"
#include "media_settings.h"
static media_settings_t settings;
media_settings_t *media_settings(void) { return &settings; }
static const media_profile_t profile = {.profile = MEDIA_MEMORY_LOW};
const media_profile_t *media_profile(void) { return &profile; }
static rg_color_t led_override = C_NONE;
#define ledOverride led_override
#define C_RED C_RGB(255,0,0)
#define C_GREEN C_RGB(0,255,0)
#define C_BLUE C_RGB(0,0,255)
enum { RG_INDICATOR_ACTIVITY_DISK, RG_INDICATOR_ACTIVITY_DISK_READ, RG_INDICATOR_ACTIVITY_DISK_WRITE,
       RG_INDICATOR_POWER_LOW, RG_INDICATOR_CRITICAL };
static uint32_t indicators;
static struct { uint32_t indicatorsMask; } app = {.indicatorsMask = UINT32_MAX};
static rg_color_t ledColor;
static bool rg_system_set_led_color(rg_color_t color) { ledColor = color; return true; }
#include "media_led_under_test.h"
static bool output_running = true, output_paused;
bool media_audio_running(void) { return output_running; }
bool media_audio_get_paused(void) { return output_paused; }
static void rg_audio_submit(const rg_audio_frame_t *frames, size_t count) { (void)frames; (void)count; }

typedef struct { size_t cursor, count; int fail; double frequency; } fixture_t;
int media_decoder_decode(media_decoder_t *d, int16_t *pcm, size_t capacity)
{
    fixture_t *f = d->state;
    if (f->cursor == f->count) return f->fail ? -1 : 0;
    size_t count = RG_MIN(capacity, RG_MIN(f->count - f->cursor, 137));
    for (size_t i = 0; i < count; ++i)
    {
        pcm[i * 2] = f->frequency ? (int16_t)(12000 * sin(2 * M_PI * f->frequency *
            (f->cursor + i) / d->sample_rate)) : (int16_t)((f->cursor + i) % 20000);
        pcm[i * 2 + 1] = -pcm[i * 2];
    }
    f->cursor += count;
    d->frames_decoded += count;
    return (int)count;
}
#include "media_transition.c"
struct media_source_s { FILE *file; uint64_t size; };
size_t media_source_read(media_source_t *source, void *buf, size_t len, int timeout)
{ (void)timeout; return fread(buf, 1, len, source->file); }
uint64_t media_source_size(const media_source_t *source) { return source->size; }
bool media_source_eof(const media_source_t *source) { return feof(source->file); }
bool media_source_seek(media_source_t *source, uint64_t offset)
{ return fseek(source->file, (long)offset, SEEK_SET) == 0; }
#include "codecs/codec_mp3.c"
#define RG_GPIO_LED_WS2812 1
#include "media_lighting.c"

#include "media_prepared_type.h"
static struct
{
    media_decoder_t *decoder;
    media_pcm_reader_t reader;
    media_tail_t tail;
    prepared_track_t *prepared;
    rg_mutex_t *lock;
    int16_t *block, *mix_block;
    media_track_t track, announcement_track;
    char path[MEDIA_MAX_PATH + 1], announcement_path[MEDIA_MAX_PATH + 1], stream_art[MEDIA_MAX_PATH + 1];
    bool stop, command, finishing, overlapping, announcement_pending, track_valid, play_counted;
    bool prepare_attempted, sleep_end_of_track, sleep_end_of_album, continuous;
    bool net_tags_requested, net_tags_applied;
    uint32_t command_serial, generation;
    int net_tags_attempts;
    int64_t last_position_save_us;
    size_t overlap_frames, overlap_done;
    uint64_t written_frames, announcement_frame;
} player;
enum { CMD_NONE };
static int16_t delivered[20000 * 2];
static size_t delivered_count;
static uint64_t played, marked_frame;
static uint32_t marked_position;
static int advance_calls, queue_advances;
static bool draining;
static int queue_next = 1;
static const char *queue_path = "next.wav";
size_t media_audio_write(const int16_t *pcm, size_t frames, int timeout)
{
    (void)timeout;
    size_t count = RG_MIN(frames, 67); // Force partial writes through actual write_pcm().
    assert(delivered_count + count <= RG_COUNT(delivered) / 2);
    memcpy(delivered + delivered_count * 2, pcm, count * 4);
    delivered_count += count;
    return count;
}
uint64_t media_audio_frames_played(void) { return played; }
uint32_t media_audio_get_sample_rate(void) { return 44100; }
void media_audio_mark_track(uint64_t frame, uint32_t position) { marked_frame = frame; marked_position = position; }
void media_audio_set_draining(bool enabled) { draining = enabled; }
bool media_audio_drained(void) { return true; }
static void service_buffering(void) {}
static void prepare_next(void) {}
static bool prepared_done(void) { return player.prepared && player.prepared->done; }
void media_decoder_close(media_decoder_t *decoder) { free(decoder); }
static void discard_prepared(void)
{
    if (!prepared_done()) return;
    media_decoder_close(player.prepared->decoder);
    free(player.prepared);
    player.prepared = NULL;
}
static void advance_track(bool manual) { (void)manual; advance_calls++; }
static float compute_gain(const media_track_t *track) { (void)track; return 1; }
static media_event_t last_event;
static void emit(media_event_t event, intptr_t arg) { last_event = event; (void)arg; }
size_t media_utf8_copy(char *dst, size_t capacity, const char *src)
{
    snprintf(dst, capacity, "%s", src);
    return strlen(dst);
}
void media_queue_lock(void) {}
void media_queue_unlock(void) {}
int media_queue_next_index(bool manual) { (void)manual; return queue_next; }
const char *media_queue_path(int index) { return index < 0 ? NULL : queue_path; }
int media_queue_advance(bool manual) { (void)manual; queue_advances++; return queue_next; }
#include "media_continuous_under_test.h"

static struct
{
    int count, index, order_count, order_position;
    bool shuffle;
    media_repeat_t repeat;
    uint16_t *order;
    uint32_t rand_state;
} q;
#include "media_rand_under_test.h"
#define media_queue_reshuffle queue_actual_shuffle
#define media_queue_next_index queue_actual_next
#define media_queue_advance queue_actual_advance
#include "media_queue_under_test.h"
#undef media_queue_reshuffle
#undef media_queue_next_index
#undef media_queue_advance

typedef struct { void (*func)(void *); void *arg; uintptr_t handle; char name[16]; } task_slot_t;
static task_slot_t tasks[20];
static int occupied[20];
#define rg_task_t task_slot_t
#include "media_task_slots_under_test.h"
#undef rg_task_t
static void slot_dummy(void *arg) { (void)arg; }
#ifdef _WIN32
static DWORD WINAPI slot_stress(void *arg)
#else
static void *slot_stress(void *arg)
#endif
{
    for (int i = 0; i < 10000; ++i)
    {
        task_slot_t *slot;
        while (!(slot = task_reserve(slot_dummy))) {}
        size_t index = (size_t)(slot - tasks);
        assert(__atomic_exchange_n(&occupied[index], 1, __ATOMIC_ACQ_REL) == 0);
        slot->arg = arg; strcpy(slot->name, "owned"); slot->handle = 123;
        assert(__atomic_exchange_n(&occupied[index], 0, __ATOMIC_ACQ_REL) == 1);
        task_release(slot);
    }
    return 0;
}
static void test_task_slots(void)
{
    for (int i = 1; i < 20; ++i) assert(task_reserve(slot_dummy) == &tasks[i]);
    assert(!task_reserve(slot_dummy));
    for (int i = 1; i < 20; ++i) task_release(&tasks[i]);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, slot_stress, (void *)1, 0, NULL);
    assert(thread);
    slot_stress((void *)2);
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, slot_stress, (void *)1) == 0);
    slot_stress((void *)2);
    assert(pthread_join(thread, NULL) == 0);
#endif
    for (int i = 1; i < 20; ++i) assert(!tasks[i].func && !tasks[i].name[0] && !tasks[i].handle);
}

static void test_shuffle_wrap(void)
{
    uint16_t order[7];
    q.count = 7; q.index = 0; q.order = order; q.rand_state = 1;
    q.shuffle = true; q.repeat = MEDIA_REPEAT_ALL;
    queue_actual_shuffle();
    for (int i = 0; i < 70; ++i)
    {
        int old = q.index;
        int next = queue_actual_next(false);
        assert(next >= 0 && next < 7 && next == queue_actual_next(false));
        assert(queue_actual_advance(false) == next && next != old);
    }
    q.repeat = MEDIA_REPEAT_TRACK;
    assert(queue_actual_next(false) == q.index && queue_actual_advance(false) == q.index);
    q.repeat = MEDIA_REPEAT_OFF; q.order_position = 6;
    assert(queue_actual_next(false) == -1 && queue_actual_advance(false) == -1);
    assert(queue_actual_advance(true) >= 0);
}

static void test_controller_transition(void)
{
    for (int mode = 0; mode < 6; ++mode)
    {
        memset(&player, 0, sizeof(player));
        delivered_count = played = marked_frame = marked_position = 0;
        advance_calls = queue_advances = 0;
        queue_path = mode == 3 ? "edited.wav" : "next.wav";
        player.lock = rg_mutex_create();
        player.block = malloc(MEDIA_DECODE_BLOCK_FRAMES * 4);
        player.mix_block = malloc(MEDIA_DECODE_BLOCK_FRAMES * 4);
        player.decoder = calloc(1, sizeof(media_decoder_t));
        strcpy(player.path, "old.wav");
        player.track.album_hash = 123;
        player.sleep_end_of_track = mode == 2;
        player.sleep_end_of_album = mode == 4;
        settings.crossfade_s = mode == 0 ? 0 : 1;
        settings.gapless = player.continuous = true;
        assert(media_tail_init(&player.tail, 100));
        int16_t pcm[800 * 2];
        for (int i = 0; i < 1600; ++i) pcm[i] = 1000;
        write_pcm(pcm, media_tail_push(&player.tail, pcm, 800));
        fixture_t incoming = {.count = 600};
        player.prepared = calloc(1, sizeof(prepared_track_t));
        prepared_track_t *job = player.prepared;
        job->done = true;
        job->index = 1;
        job->rate = 44100;
        strcpy(job->path, "next.wav"); strcpy(job->from, "old.wav");
        strcpy(job->track.title, "Next song");
        job->track.album_hash = mode == 4 ? 456 : 123;
        job->decoder = calloc(1, sizeof(media_decoder_t));
        job->decoder->state = &incoming;
        job->decoder->sample_rate = 44100;
        job->decoder->total_frames = mode == 5 ? 0 : incoming.count;
        media_pcm_reader_init(&job->reader, job->decoder, 44100);
        player.finishing = true;
        for (int i = 0; i < 20 && player.finishing && !advance_calls; ++i) finish_continuous();
        if (mode >= 2 && mode <= 4)
        {
            assert(advance_calls == 1 && queue_advances == 0);
            assert(delivered_count == 800);
            for (size_t i = 0; i < delivered_count * 2; ++i) assert(delivered[i] == 1000);
            discard_prepared();
        }
        else
        {
            assert(queue_advances == 1 && advance_calls == 0);
            assert(player.announcement_pending && !strcmp(player.path, "old.wav"));
            assert(marked_frame == 800 && marked_position == (mode == 1 ? 100000 / 44100 : 0));
            played = marked_frame;
            announce_transition();
            assert(!player.announcement_pending && !strcmp(player.path, "next.wav") && player.generation == 1);
            int count;
            while ((count = media_pcm_reader_read(&player.reader, pcm, 800)) > 0)
                write_pcm(pcm, media_tail_push(&player.tail, pcm, count));
            player.finishing = true;
            while (!advance_calls) finish_continuous();
            assert(delivered_count == 800 + 600 - (mode == 1 ? 100 : 0));
            assert(delivered[delivered_count * 2 - 2] == 599); // Last incoming frame survives.
            if (mode != 1) assert(delivered[800 * 2] == 0); // Exact gapless join; no padding/silence inserted.
            else assert(delivered[799 * 2] == 99); // Fade ends at incoming frame 99.
        }
        media_decoder_close(player.decoder);
        media_tail_free(&player.tail);
        free(player.block); free(player.mix_block);
        rg_mutex_free(player.lock);
    }
    assert(!draining);
}

static void test_resample(void)
{
    const unsigned rates[] = {8000, 22050, 32000, 44100, 48000, 96000};
    for (size_t r = 0; r < RG_COUNT(rates); ++r)
        for (size_t n = 1; n < 600; n += 43)
        {
            fixture_t fixture = {.count = n};
            media_decoder_t decoder = {.state = &fixture, .sample_rate = rates[r]};
            media_pcm_reader_t reader;
            media_pcm_reader_init(&reader, &decoder, 44100);
            int16_t pcm[79 * 2];
            size_t total = 0;
            int count;
            while ((count = media_pcm_reader_read(&reader, pcm, 79)) > 0)
            {
                for (int i = 0; i < count; ++i)
                {
                    assert(abs(pcm[i * 2] + pcm[i * 2 + 1]) <= 1);
                    if (rates[r] == 44100) assert(pcm[i * 2] == (int)(total + i));
                }
                total += count;
            }
            assert(count == 0);
            assert(total == (n * 44100ULL + rates[r] - 1) / rates[r]);
        }
    fixture_t fixture = {.count = 45, .fail = 1};
    media_decoder_t decoder = {.state = &fixture, .sample_rate = 44100};
    media_pcm_reader_t reader;
    media_pcm_reader_init(&reader, &decoder, 44100);
    int16_t pcm[64 * 2];
    assert(media_pcm_reader_read(&reader, pcm, 64) == 45);
    assert(media_pcm_reader_read(&reader, pcm, 64) == -1);

    double energy[2] = {0};
    for (int tone = 0; tone < 2; ++tone)
    {
        fixture_t wave = {.count = 9600, .frequency = tone ? 30000 : 1000};
        decoder = (media_decoder_t){.state = &wave, .sample_rate = 96000};
        media_pcm_reader_init(&reader, &decoder, 44100);
        size_t total = 0;
        int count;
        while ((count = media_pcm_reader_read(&reader, pcm, 64)) > 0)
        {
            for (int i = 0; i < count; ++i)
                if (total + i > 100) energy[tone] += (double)pcm[i * 2] * pcm[i * 2];
            total += count;
        }
    }
    assert(energy[0] > 1e10 && energy[1] < energy[0] * 0.09); // Reject out-of-band aliases.
}

static void test_transition_memory(void)
{
    player.tail = (media_tail_t){0}; player.continuous = true;
    settings.crossfade_s = 5;
    fail_alloc = true;
    configure_tail(44100);
    assert(player.tail.capacity == 0 && last_event == MEDIA_EVENT_TRANSITION_FALLBACK);
    fail_alloc = false;
    configure_tail(44100);
    assert(player.tail.capacity == 3 * 44100); // LOW profile duration limit.
    settings.crossfade_s = 0;
    configure_tail(44100);
    assert(!player.tail.pcm && !player.tail.capacity);
}

static void test_tail_and_mix(void)
{
    for (size_t window = 1; window <= 307; window += 17)
    {
        media_tail_t tail = {0};
        assert(media_tail_init(&tail, window));
        size_t emitted = 0;
        int16_t pcm[83 * 2];
        for (size_t start = 0; start < 1000;)
        {
            size_t n = RG_MIN(83, 1000 - start);
            for (size_t i = 0; i < n; ++i) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(start + i);
            size_t got = media_tail_push(&tail, pcm, n);
            for (size_t i = 0; i < got; ++i) assert(pcm[i * 2] == (int)(emitted + i));
            emitted += got;
            start += n;
        }
        assert(emitted == 1000 - window);
        size_t got;
        while ((got = media_tail_pop(&tail, pcm, 83)))
        {
            for (size_t i = 0; i < got; ++i) assert(pcm[i * 2] == (int)(emitted + i));
            emitted += got;
        }
        assert(emitted == 1000);
        media_tail_free(&tail);
    }
    int16_t out[400], next[400];
    for (int i = 0; i < 400; ++i) { out[i] = 32767; next[i] = -32768; }
    media_crossfade_mix(out, next, 200, 0, 200);
    assert(out[0] == 32767 && out[398] == -32768);
    for (int i = 2; i < 400; i += 2) assert(out[i] <= out[i - 2]);
    for (int i = 0; i < 400; ++i) out[i] = next[i] = 32767;
    media_crossfade_mix(out, next, 200, 0, 200);
    for (int i = 0; i < 400; ++i) assert(out[i] == 32767); // Correlated signals retain headroom.
    int16_t split[400];
    for (int i = 0; i < 400; ++i) out[i] = split[i] = (int16_t)(i * 53 - 10000);
    media_crossfade_mix(out, next, 200, 0, 200);
    for (size_t i = 0; i < 200; i += 31)
        media_crossfade_mix(split + i * 2, next + i * 2, RG_MIN(31, 200 - i), i, 200);
    assert(!memcmp(out, split, sizeof(out))); // Ramps remain sample-exact across block boundaries.
}

static void test_mp3_trimming(void)
{
    uint8_t frame[200] = {0xFF, 0xFB, 0x90, 0};
    memcpy(frame + 36, "Xing", 4);
    frame[43] = 1; frame[47] = 3; // 3 audio frames, no optional Xing fields.
    memcpy(frame + 48, "LAME3.100", 9);
    frame[69] = 0x24; frame[70] = 0x03; frame[71] = 0xE8; // Delay 576, padding 1000.
    mp3dec_frame_info_t info = {.channels = 2, .hz = 44100};
    mp3_state_t state = {0};
    media_decoder_t decoder = {.state = &state, .channels = 2, .sample_rate = 44100};
    mp3_parse_vbr_header(&state, frame, sizeof(frame), &info);
    assert(state.trim_valid && state.start_delay == 1105 && state.end_padding == 471);
    decoder.total_frames = 1880; // 3456 - 1105 - 471.
    state.pending_frames = 1152;
    for (int i = 0; i < MINIMP3_MAX_SAMPLES_PER_FRAME; ++i) state.pending[i] = (int16_t)(i / 2);
    int16_t pcm[1152 * 2];
    assert(mp3_emit(&decoder, &state, pcm, 1152) == 47);
    assert(pcm[0] == 1105 && state.delay_remaining == 0);
    decoder.frames_decoded = decoder.total_frames - 20;
    state.pending_read = 0;
    assert(mp3_emit(&decoder, &state, pcm, 1152) == 20);
    state = (mp3_state_t){0};
    mp3_parse_vbr_header(&state, frame, 65, &info);
    assert(!state.trim_valid); // Never read delay/padding outside the metadata frame.
}

static void test_mp3_files(void)
{
    const int rates[] = {44100,48000,22050};
    for (size_t r = 0; r < RG_COUNT(rates); ++r)
    {
        int rate = rates[r];
        char path[128];
        snprintf(path, sizeof(path), "tools/tests/data/gapless-%d.mp3", rate);
        FILE *file = fopen(path, "rb");
        assert(file);
        fseek(file, 0, SEEK_END);
        media_source_t source = {.file = file, .size = (uint64_t)ftell(file)};
        rewind(file);
        media_decoder_t decoder = {.source = &source};
        assert(mp3_open(&decoder) == MEDIA_OK && decoder.gapless);
        assert(decoder.total_frames == (uint64_t)rate && decoder.duration_ms == 1000);
        for (int replay = 0; replay < 2; ++replay)
        {
            if (replay)
            {
                assert(mp3_seek(&decoder, 0));
                decoder.frames_decoded = 0;
            }
            int16_t pcm[257 * 2];
            int count;
            uint64_t energy = 0;
            while ((count = mp3_decode(&decoder, pcm, 257)) > 0)
            {
                for (int i = 0; i < count; ++i)
                {
                    energy += abs(pcm[i * 2]);
                    if (rate != 44100) assert(pcm[i * 2] == pcm[i * 2 + 1]);
                }
                decoder.frames_decoded += count;
            }
            assert(count == 0 && decoder.frames_decoded == (uint64_t)rate && energy > 1000000);
        }
        mp3_close(&decoder);
        fclose(file);
    }
}

static void test_geometry(void)
{
    const int sizes[][2] = {{320,240},{480,320},{320,480},{240,320},{240,240},{160,128}};
    for (size_t i = 0; i < RG_COUNT(sizes); ++i)
        for (int line = 10; line <= 24; line += 2)
        {
            if (sizes[i][1] < 200 && line > 10) continue;
            int width = sizes[i][0], pad = RG_MAX(width / 60, 3);
            int top = line + pad * 2, bottom = sizes[i][1] - line - pad * 2;
            media_player_geometry_t g = media_player_geometry(width, top, bottom, line, pad);
            assert(g.status_y + line <= g.hero_y);
            assert(g.text_y >= g.hero_y);
            assert(g.text_y + g.title_h + line + (g.album ? line : 0) <= g.hero_y + g.hero_h);
            assert(g.art_y + g.art_size <= g.hero_y + g.hero_h || !g.art_size);
            assert(g.text_x + g.text_w <= width - pad * 2);
            if (g.art_size && width >= 280) assert(g.art_x + g.art_size < g.text_x);
            assert(g.hero_y + g.hero_h < g.bar_y);
            if (g.quality_y >= 0) assert(g.hero_y + g.hero_h < g.quality_y);
            if (g.next_y >= 0) assert(g.quality_y + line <= g.next_y && g.next_y + line < g.bar_y);
            assert(g.bar_y + 5 <= g.time_y);
            assert(g.time_y + line <= g.transport_y);
            assert(g.transport_y + g.transport_h <= bottom);
        }
}

static void stop_light(void) { lighting.stop = true; }
static void test_lighting(void)
{
    for (int effect = 0; effect < MEDIA_LIGHT_COUNT; ++effect)
        for (unsigned hue = 0; hue < 3072; hue += 47)
        {
            assert(media_lighting_color(effect, 0, 255, 255, true, hue) == 0);
            assert(media_lighting_color(effect, 100, 0, 0, false, hue) == 0);
        }
    assert(media_lighting_color(MEDIA_LIGHT_RAINBOW, 100, 255, 255, false, 0) == C_RGB(255,0,0));
    assert(media_lighting_available());
    assert(media_lighting_start());
    int16_t pcm[1024];
    for (int i = 0; i < 1024; ++i) pcm[i] = 4000;
    clock_us = 1000000;
    media_lighting_feed(pcm, 512);
    assert((lighting.levels & 0xFFFF) == 4000 && (lighting.levels >> 16) > 3000);
    settings.lighting = MEDIA_LIGHT_MUSIC;
    settings.lighting_brightness = 65;
    delay_hook = stop_light;
    lighting_task(NULL);
    assert(led_override == C_NONE && !lighting.running);
    assert(media_lighting_stop());
    delay_hook = NULL;
    clock_us = 2000000;
    indicators = (1u << RG_INDICATOR_ACTIVITY_DISK_READ);
    rg_system_set_led_override(C_RGB(0,200,200));
    assert(ledColor == C_RGB(0,200,200)); // Disk activity cannot overwrite music lighting.
    indicators |= 1u << RG_INDICATOR_POWER_LOW;
    rg_system_set_led_override(C_BLUE);
    assert(ledColor == C_RED);
    clock_us += 500000;
    rg_system_set_led_override(C_BLUE);
    assert(ledColor == 0); // Warning cadence depends on time, not LED/SD update count.
    indicators = 1u << RG_INDICATOR_CRITICAL;
    clock_us = 2000000;
    rg_system_set_led_override(C_BLUE);
    assert(ledColor == C_RED);
    clock_us += 250000;
    rg_system_set_led_override(C_BLUE);
    assert(ledColor == 0);
    indicators = 0;
    rg_system_set_led_override(C_NONE);
    assert(ledColor == 0);
}

int main(void)
{
    current_route = RG_AUDIO_ROUTE_SPEAKER;
    test_resample();
    test_tail_and_mix();
    test_mp3_trimming();
    test_mp3_files();
    test_geometry();
    test_lighting();
    test_controller_transition();
    test_shuffle_wrap();
    test_task_slots();
    test_transition_memory();
    puts("Media feature tests passed: exact PCM counts at 6 rates, MP3 delay/padding and replay at 3 rates, tail FIFO/crossfade headroom, controller joins/queue edits/sleep boundaries, shuffle lookahead, adaptive UI at 6 sizes, lighting modes/envelope/warning priority/cleanup.");
    return 0;
}
