/* Exercise production frame submission and artwork pinning with a delayed display. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { bool held; } mutex_t;
static mutex_t cache_lock;
static struct { mutex_t *lock; } art = {&cache_lock};
static unsigned takes, releases, draws, submits, sleeps, waiting;
static int display_task_queue;
static int64_t simulated_us;
static unsigned warnings;
static int64_t rg_system_timer(void) { return simulated_us; }
#define RG_LOGW(...) (warnings++)
static struct {
    void *surface;
    struct { int mini_h; } layout;
    int page;
    bool quick_open;
} mui;
enum { MEDIA_PAGE_LIBRARY, MEDIA_PAGE_NOW_PLAYING, MEDIA_PAGE_LYRICS,
       MEDIA_PAGE_VISUALIZER, MEDIA_PAGE_QUEUE, MEDIA_PAGE_INFO };
#define MEDIA_DEBUG_STATS 0
static bool rg_mutex_take(mutex_t *lock, int timeout)
{
    assert(timeout == 0);
    takes++;
    if (lock->held) return false;
    lock->held = true;
    return true;
}
static void rg_mutex_give(mutex_t *lock)
{ assert(lock->held); lock->held = false; releases++; }
static unsigned rg_task_messages_waiting(int task) { (void)task; return waiting; }
static void rg_task_delay(unsigned ms)
{ assert(ms > 0); sleeps++; waiting = 0; }
#include "media_render_locks_under_test.h"
static void rg_gui_set_surface(void *surface) { (void)surface; }
static void media_ui_update_theme(void) { assert(cache_lock.held); }
static void media_ui_clear(void)
{
    assert(!waiting && cache_lock.held); // The display still owns queued pixels.
    draws++;
}
static void media_ui_library_draw(void) { assert(cache_lock.held); }
#define media_ui_nowplaying_draw media_ui_library_draw
#define media_ui_lyrics_draw media_ui_library_draw
#define media_ui_visualizer_draw media_ui_library_draw
#define media_ui_queue_draw media_ui_library_draw
#define media_ui_info_draw media_ui_library_draw
#define media_ui_draw_overlay media_ui_library_draw
#define quick_draw media_ui_library_draw
static void rg_display_submit(void *surface, unsigned flags)
{
    (void)surface; (void)flags;
    assert(!cache_lock.held && !waiting);
    waiting = 1;
    submits++;
}
#include "media_render_frame_under_test.h"
int main(void)
{
    for (unsigned i = 0; i < 10000; i++)
    {
        mui.page = i % 6;
        cache_lock.held = true; // Worker owns cached surfaces.
        unsigned before = releases;
        assert(!draw_frame());
        assert(cache_lock.held && releases == before);
        cache_lock.held = false;
        assert(draw_frame());
        before = draws;
        simulated_us += 2000000;
        assert(!draw_frame());
        simulated_us += 2000000;
        assert(!draw_frame());
        unsigned before_takes = takes;
        for (int busy = 0; busy < 10; busy++) assert(!draw_frame());
        assert(draws == before && takes == before_takes && !cache_lock.held);
        assert(rg_display_sync(true));
        assert(!waiting);
    }
    assert(warnings >= 10000);
    assert(draws == 10000 && submits == 10000 && releases == 10000 && sleeps == 10000);
    puts("Media render tests passed: 10000 delayed-display/contended-artwork frames, framebuffer ownership, nonblocking retries, unlocked submission and blocking sync yields.");
}
