/* Include production implementations to exercise private output-loop state as well as APIs. */
#include <rg_system.h>
#include "media_ring.c"
#include "media_eq.c"
#include "media_fft.c"
#include "media_audio.c"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static const media_profile_t profile = {
    .pcm_buffer_frames = 8192, .prebuffer_frames = 2048, .prebuffer_ms = 2500};
const media_profile_t *media_profile(void) { return &profile; }

/* Exercise the controller's actual buffering helpers against the real output stage. */
static struct { size_t bytes, capacity; bool eof; } source;
typedef struct { bool eos; void *source; } test_decoder_t;
static test_decoder_t decoder = {.source = &source};
static struct
{
    test_decoder_t *decoder;
    media_state_t state;
    rg_audio_route_t previous_route;
    char path[256];
    bool preroll_active;
    size_t preroll_bytes, preroll_seen;
    int64_t preroll_stall_us;
} player;
#define PREROLL_STALL_US 8000000LL
static void emit(media_event_t event, intptr_t arg) { (void)event; (void)arg; }
static size_t bytes_for_ms(uint32_t ms) { return (size_t)ms * 16; }
static bool media_net_is_url(const char *path) { return !strncmp(path, "http", 4); }
static bool media_source_eof(const void *src) { (void)src; return source.eof; }
static size_t media_source_buffered(const void *src) { (void)src; return source.bytes; }
static size_t media_ring_capacity_of(const void *src) { (void)src; return source.capacity; }
#include "media_buffering_under_test.h"
static bool player_started, foreground;
static int housekeeping_calls;
static void media_player_tick(void) { housekeeping_calls++; }
#include "media_tick_under_test.h"
static struct { bool pause_on_unplug; } settings = {.pause_on_unplug = true};
#define media_settings() (&settings)
static void media_player_pause(void)
{
    media_audio_set_paused(true);
    set_state(MEDIA_STATE_PAUSED);
}
#include "media_route_under_test.h"

static size_t submitted, stop_after;
static int expected_sample;
static bool check_fifo;
static bool check_silence;
static void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    assert(audio.lock->held); // Flush/rate commands cannot race an in-flight chunk.
    if (check_silence)
        for (size_t i = 0; i < count; ++i)
            assert(frames[i].left == 0 && frames[i].right == 0);
    if (check_fifo)
        for (size_t i = 0; i < count; ++i)
        {
            assert(frames[i].left == expected_sample);
            assert(frames[i].right == -expected_sample);
            expected_sample++;
        }
    submitted += count;
    if (submitted >= stop_after) audio.stop = true;
}

static void start_output(void)
{
    delay_hook = NULL;
    assert(media_audio_start());
    submitted = 0;
    stop_after = 1;
    check_fifo = false;
    check_silence = false;
    media_audio_flush(0);
}

static void test_ring(void)
{
    media_ring_t *ring = media_ring_create("test", 64, false);
    assert(ring && media_ring_capacity(ring) == 64);
    unsigned char input[96], output[96];
    for (int i = 0; i < 96; ++i) input[i] = (unsigned char)i;
    for (int offset = 0; offset < 64; ++offset)
    {
        // Also cross the uint32 cursor wrap, not merely the physical ring wrap.
        ring->head = ring->tail = UINT32_MAX - (uint32_t)offset;
        assert(media_ring_write(ring, input, 48, 0) == 48);
        assert(media_ring_read(ring, output, 31, 0) == 31);
        assert(!memcmp(input, output, 31));
        assert(media_ring_write(ring, input + 48, 48, 0) == 47);
        assert(media_ring_fill_percent(ring) == 100);
        assert(media_ring_peek(ring, output, 64) == 64);
        assert(!memcmp(input + 31, output, 64));
        assert(media_ring_read(ring, output, 64, 0) == 64);
        assert(!memcmp(input + 31, output, 64));
        assert(media_ring_used(ring) == 0);
    }
    assert(!media_ring_wait_readable(ring, 0));
    assert(media_ring_write(ring, input, 48, 0) == 48);
    assert(media_ring_wait_readable(ring, 0));
    media_ring_abort(ring);
    assert(!media_ring_wait_readable(ring, 0));
    assert(media_ring_read(ring, output, 48, 0) == 0);
    media_ring_resume(ring);
    media_ring_reset(ring);
    assert(!media_ring_used(ring));
    media_ring_free(ring);
}

static media_ring_t *stress_ring;
#define STRESS_BYTES 2000000u
#ifdef _WIN32
static DWORD WINAPI stress_producer(void *arg)
#else
static void *stress_producer(void *arg)
#endif
{
    (void)arg;
    unsigned char bytes[197];
    for (size_t sent = 0; sent < STRESS_BYTES;)
    {
        size_t count = STRESS_BYTES - sent;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)((sent + i) * 31u);
        sent += media_ring_write(stress_ring, bytes, count, 0);
    }
    return 0;
}
static void test_ring_concurrency(void)
{
    stress_ring = media_ring_create("stress", 256, false);
    assert(stress_ring);
    stress_ring->head = stress_ring->tail = UINT32_MAX - 127;
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, stress_producer, NULL, 0, NULL);
    assert(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, stress_producer, NULL) == 0);
#endif
    unsigned char bytes[113];
    for (size_t received = 0; received < STRESS_BYTES;)
    {
        size_t count = STRESS_BYTES - received;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        size_t got = media_ring_read(stress_ring, bytes, count, 0);
        for (size_t i = 0; i < got; ++i)
            assert(bytes[i] == (unsigned char)((received + i) * 31u));
        received += got;
        assert(media_ring_fill_percent(stress_ring) >= 0 &&
               media_ring_fill_percent(stress_ring) <= 100);
    }
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread, NULL) == 0);
#endif
    assert(media_ring_used(stress_ring) == 0);
    media_ring_free(stress_ring);
}

static void stop_on_delay(void) { audio.stop = true; }
static void test_controller_buffering(void)
{
    start_output();
    player.decoder = &decoder;
    source.capacity = 65536;
    int16_t pcm[2048 * 2] = {0};
    set_state(MEDIA_STATE_BUFFERING);
    assert(media_audio_get_buffering());
    assert(media_audio_write(pcm, 2047, 0) == 2047);
    service_buffering();
    assert(player.state == MEDIA_STATE_BUFFERING);
    assert(media_audio_write(pcm, 1, 0) == 1);
    service_buffering();
    assert(player.state == MEDIA_STATE_PLAYING && !media_audio_get_buffering());

    set_state(MEDIA_STATE_BUFFERING);
    player.preroll_active = true;
    player.preroll_seen = source.bytes = 12000;
    player.preroll_stall_us = rg_system_timer() + PREROLL_STALL_US;
    media_audio_set_paused(true);
    clock_us += PREROLL_STALL_US;
    service_buffering();
    assert(!player.preroll_active && !media_audio_get_paused()); // Stalled pre-roll is bounded.

    set_state(MEDIA_STATE_BUFFERING);
    player.preroll_active = true;
    media_audio_set_paused(true);
    set_state(MEDIA_STATE_PAUSED);
    service_buffering();
    assert(!player.preroll_active && media_audio_get_paused()); // User pause wins.
    media_audio_set_paused(false);

    strcpy(player.path, "https://radio/test");
    source.bytes = 0;
    set_state(MEDIA_STATE_BUFFERING);
    service_buffering();
    assert(media_audio_get_buffering()); // PCM alone must not start a live stream.
    source.bytes = source.capacity / 2;
    service_buffering();
    assert(player.state == MEDIA_STATE_PLAYING);

    set_state(MEDIA_STATE_BUFFERING);
    player.preroll_active = true;
    player.preroll_bytes = 40000;
    player.preroll_stall_us = rg_system_timer() + PREROLL_STALL_US;
    media_audio_set_paused(true);
    service_buffering();
    assert(player.preroll_active && media_audio_get_paused());
    source.bytes = 40000;
    service_buffering();
    assert(!player.preroll_active && !media_audio_get_paused());
    assert(player.state == MEDIA_STATE_PLAYING && !media_audio_get_buffering());

    // An allocation fallback must not wait for a prebuffer larger than the actual ring.
    player.path[0] = 0;
    media_ring_free(audio.pcm);
    audio.pcm = media_ring_create("fallback", 4096, false); // 1024 stereo frames
    set_state(MEDIA_STATE_BUFFERING);
    assert(media_audio_write(pcm, 512, 0) == 512);
    service_buffering();
    assert(player.state == MEDIA_STATE_PLAYING);
    media_audio_flush(0);
    source.eof = true;
    set_state(MEDIA_STATE_BUFFERING);
    assert(media_audio_write(pcm, 12, 0) == 12);
    service_buffering();
    assert(player.state == MEDIA_STATE_PLAYING); // Short file below the startup target.
    source.eof = false;
    audio.stop = true;
    audio_task(NULL);
    media_audio_deinit();
}

static void test_buffering_pause(void)
{
    int16_t pcm[512] = {0};
    start_output();
    assert(media_audio_get_buffering());
    assert(media_audio_write(pcm, 256, 0) == 256);
    delay_hook = stop_on_delay;
    audio_task(NULL);
    assert(submitted == 0 && media_audio_buffered_frames() == 256);
    assert(media_audio_underruns() == 0); // Idle/loading is not starvation.

    start_output();
    media_audio_set_buffering(false);
    media_audio_set_paused(true);
    audio.fade = 0.5f;
    delay_hook = stop_on_delay;
    audio_task(NULL);
    assert(audio.fade == 0.0f && submitted == 0 && media_audio_underruns() == 0);

    start_output();
    media_audio_set_buffering(false);
    delay_hook = stop_on_delay;
    audio_task(NULL);
    assert(media_audio_underruns() == 1 && media_audio_get_buffering());
}

static void test_fifo_and_position(void)
{
    int16_t pcm[600 * 2];
    for (int i = 0; i < 600; ++i) { pcm[i * 2] = i; pcm[i * 2 + 1] = -i; }
    start_output();
    assert(media_audio_set_sample_rate(48000));
    media_audio_flush(12500);
    audio.fade = audio.fade_target = 1.0f;
    assert(media_audio_write(pcm, 600, 0) == 600);
    stop_after = 600;
    expected_sample = 0;
    check_fifo = true;
    media_audio_set_draining(true); // Includes a final 88-frame partial chunk.
    audio_task(NULL);
    assert(submitted == 600 && media_audio_frames_played() == 600);
    assert(media_audio_position_ms() == 12512);
    assert(media_audio_underruns() == 0);
    media_audio_flush(321);
    assert(media_audio_position_ms() == 321 && media_audio_buffered_frames() == 0);
    assert(!media_audio_set_sample_rate(96000));
    assert(media_audio_get_sample_rate() == 48000);
}

static void stop_when_drained(void)
{
    if (media_audio_drained()) audio.stop = true;
}
static void test_dma_tail(void)
{
    start_output();
    media_audio_set_draining(true);
    stop_after = SIZE_MAX;
    check_silence = true;
    delay_hook = stop_when_drained;
    int64_t before = rg_system_timer();
    audio_task(NULL);
    assert(submitted == RG_AUDIO_DMA_BUFFER_LENGTH);
    assert(media_audio_frames_played() == 0 && media_audio_position_ms() == 0);
    assert(media_audio_drained() && rg_system_timer() > before);
    assert(media_audio_underruns() == 0);
}

static void start_pending_task(void)
{
    delay_hook = NULL;
    audio_task(NULL);
}
static void test_start_stop(void)
{
    start_output();
    assert(audio.running && !audio.alive);
    delay_hook = start_pending_task;
    media_audio_stop(); // Must wait even if the new task has not yet entered its function.
    assert(!audio.running && !audio.alive && audio.task == NULL);
    assert(system_rate == audio.previous_system_rate);
    media_audio_deinit();
    assert(!audio.pcm && !audio.chunk && !audio.lock);
}

static void test_background_tick(void)
{
    media_tick();
    assert(housekeeping_calls == 0);
    player_started = true;
    foreground = true;
    media_tick();
    assert(housekeeping_calls == 0);
    foreground = false;
    for (int i = 0; i < 1500; ++i)
    {
        media_tick();
        clock_us += 1000;
    }
    assert(housekeeping_calls == 15);
    player_started = false;
}

static void test_headphone_unplug(void)
{
    start_output();
    set_state(MEDIA_STATE_PLAYING);
    player.previous_route = RG_AUDIO_ROUTE_HEADPHONES;
    current_route = RG_AUDIO_ROUTE_SPEAKER;
    poll_audio_route();
    assert(player.state == MEDIA_STATE_PAUSED && media_audio_get_paused());
    assert(player.previous_route == RG_AUDIO_ROUTE_SPEAKER);
    settings.pause_on_unplug = false;
    set_state(MEDIA_STATE_PLAYING);
    media_audio_set_paused(false);
    player.previous_route = RG_AUDIO_ROUTE_HEADPHONES;
    poll_audio_route();
    assert(player.state == MEDIA_STATE_PLAYING && !media_audio_get_paused());
    audio.stop = true;
    audio_task(NULL);
    media_audio_deinit();
}

static void test_eq(void)
{
    int16_t pcm[256 * 2];
    media_eq_set_enabled(true);
    media_eq_set_preset(MEDIA_EQ_PRESET_BASS_BOOST);
    assert(pending.dirty);
    for (int rate = 8000; rate <= 48000; rate += 8000)
    {
        media_eq_set_sample_rate(rate);
        media_eq_flush();
        for (int i = 0; i < 512; ++i) pcm[i] = 30000;
        media_eq_process(pcm, 256, 4.0f);
        assert(!pending.dirty && !pending.flush);
        assert(isfinite(eq.limiter_gain));
        for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
            assert(isfinite(eq.state[0][i].y1));
    }
    media_eq_set_enabled(false);
    media_eq_set_sample_rate(44100);
}

static void test_fft(void)
{
    int16_t pcm[1024 * 2];
    for (int size = 128; size <= 512; size *= 2)
    {
        assert(media_fft_init(size, 24));
        assert(fft.band_end[23] == size / 2);
        for (int i = 0; i < 1024; ++i)
            pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(16000 * sin(2 * M_PI * i / 5));
        media_fft_feed(pcm, 1024);
        assert(media_fft_analyze());
        for (int i = 0; i < 24; ++i)
            assert(isfinite(fft.out.value[i]) && fft.out.value[i] >= 0 && fft.out.value[i] <= 1);
        media_fft_reset();
        assert(!media_fft_analyze());
        for (int i = 0; i < 24; ++i) assert(fft.out.value[i] == 0);
        media_fft_deinit();
    }
}

int main(void)
{
    test_ring();
    test_ring_concurrency();
    media_eq_init();
    test_eq();
    assert(media_audio_acquire(MEDIA_AUDIO_OWNER_PLAYER));
    assert(!media_audio_acquire(MEDIA_AUDIO_OWNER_EMULATOR));
    media_audio_release(MEDIA_AUDIO_OWNER_EMULATOR);
    assert(media_audio_get_owner() == MEDIA_AUDIO_OWNER_PLAYER);
    test_buffering_pause();
    test_fifo_and_position();
    test_dma_tail();
    test_start_stop();
    test_controller_buffering();
    test_background_tick();
    test_headphone_unplug();
    media_audio_release(MEDIA_AUDIO_OWNER_PLAYER);
    assert(media_audio_get_owner() == MEDIA_AUDIO_OWNER_NONE);
    test_fft();
    puts("Media audio tests passed: 64 ring wrap cases, 2 MB concurrent FIFO transfer, buffering/pause/underrun, FIFO/DMA-tail/position, startup/shutdown, local/network/pre-roll/fallback/short-file buffering, background polling, EQ at 6 rates, FFT at 3 sizes.");
    return 0;
}
