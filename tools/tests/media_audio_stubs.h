#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RG_LOGD(...) ((void)0)
#define RG_LOGI(...) ((void)0)
#define RG_LOGW(...) ((void)0)
#define RG_LOGE(...) ((void)0)
#define MEM_FAST 1
#define MEM_SLOW 2
#define MEM_ANY 3
#define MEM_8BIT 4
#define MEM_NOPANIC 8
#define RG_TASK_PRIORITY_7 7
#define RG_TASK_PRIORITY_8 8
#define RG_TASK_AFFINITY_AUDIO 1
typedef struct { int unused; } rg_task_t;
typedef struct { bool held; } rg_mutex_t;
typedef struct { int16_t left, right; } rg_audio_frame_t;
typedef enum { RG_AUDIO_ROUTE_UNKNOWN, RG_AUDIO_ROUTE_SPEAKER, RG_AUDIO_ROUTE_HEADPHONES } rg_audio_route_t;
static rg_audio_route_t current_route;
static rg_audio_route_t rg_audio_get_route(void) { return current_route; }
static int64_t clock_us;
static void (*delay_hook)(void);
static void *rg_alloc(size_t bytes, int flags) { (void)flags; return malloc(bytes); }
static int64_t rg_system_timer(void) { return clock_us; }
static void rg_task_delay(uint32_t ms)
{
    clock_us += ms * 1000;
    if (delay_hook) delay_hook();
}
static rg_mutex_t *rg_mutex_create(void) { return calloc(1, sizeof(rg_mutex_t)); }
static void rg_mutex_free(rg_mutex_t *lock) { assert(!lock->held); free(lock); }
static bool rg_mutex_take(rg_mutex_t *lock, int ms)
{
    (void)ms; assert(lock && !lock->held); lock->held = true; return true;
}
static bool rg_mutex_give(rg_mutex_t *lock)
{
    assert(lock && lock->held); lock->held = false; return true;
}
static rg_task_t *rg_task_create(const char *name, void (*fn)(void *), void *arg,
                               size_t stack, int priority, int affinity)
{
    (void)name; (void)fn; (void)arg; (void)stack;
    assert(priority == 8 && affinity == 1);
    static rg_task_t task;
    return &task;
}
static int system_rate = 32000;
static bool system_muted;
static int rg_audio_get_sample_rate(void) { return system_rate; }
static void rg_audio_set_sample_rate(int rate) { system_rate = rate; }
static bool rg_audio_get_mute(void) { return system_muted; }
static void rg_audio_set_mute(bool muted) { system_muted = muted; }
static void rg_audio_submit(const rg_audio_frame_t *frames, size_t count);
