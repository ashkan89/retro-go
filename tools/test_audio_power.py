"""Compile production audio controls with mocked output/RTOS to check zero-volume lifecycle."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    return source[start:end] + "\n"


SYSTEM = r'''
#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rg_audio.h"
#define RG_AUDIO_USE_EXT_DAC 1
#define RG_AUDIO_USE_HEADPHONE_JACK 1
#define RG_AUDIO_HP_DEFAULT_VOLUME 20
#define RG_COUNT(x) (sizeof(x)/sizeof((x)[0]))
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
#define RG_MAX(a,b) ((a)>(b)?(a):(b))
#define RG_ASSERT(condition, ...) assert(condition)
#define RG_LOGI(...) ((void)0)
#define RG_LOGE(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define NS_GLOBAL "global"
typedef struct { bool held; } rg_mutex_t;
static rg_mutex_t test_mutex;
static int64_t clock_us = 1;
static unsigned sleeps;
static rg_mutex_t *rg_mutex_create(void) { test_mutex.held=true; return &test_mutex; }
static bool rg_mutex_take(rg_mutex_t *m, int timeout) {
    assert(m); if (!timeout && m->held) return false;
    assert(!m->held); m->held=true; return true;
}
static bool rg_mutex_give(rg_mutex_t *m) { assert(m && m->held); m->held=false; return true; }
static int64_t rg_system_timer(void) { return clock_us; }
static void rg_task_delay(uint32_t ms) {
    assert(!test_mutex.held); sleeps++; clock_us += ((ms+9)/10)*10000;
}
static int saved_speaker, saved_hp=20, saved_device=1;
static char *rg_settings_get_string(const char *ns, const char *key, const char *fallback) {
    (void)ns; (void)key; (void)fallback; char *s=malloc(4); memcpy(s,"i2s",4); return s;
}
static int rg_settings_get_number(const char *ns, const char *key, int fallback) {
    (void)ns;
    if (!strcmp(key,"Volume")) return saved_speaker;
    if (!strcmp(key,"VolumeHP")) return saved_hp;
    if (!strcmp(key,"AudioDevice")) return saved_device;
    return fallback;
}
static void rg_settings_set_string(const char *ns,const char *key,const char *value) {
    (void)ns; (void)key; (void)value;
}
static void rg_settings_set_number(const char *ns,const char *key,int value) {
    (void)ns;
    if (!strcmp(key,"Volume")) saved_speaker=value;
    if (!strcmp(key,"VolumeHP")) saved_hp=value;
    if (!strcmp(key,"AudioDevice")) saved_device=value;
}
'''

RTOS = r'''
#pragma once
#include <assert.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef uint32_t *EventGroupHandle_t;
#define BIT0 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)((TickType_t)(ms) / 10U))
static uint32_t event_bits, last_timeout;
static unsigned event_waits;
static void (*wake_hook)(void);
static EventGroupHandle_t xEventGroupCreate(void) { return &event_bits; }
static EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t b) { return *e |= b; }
static EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t b) { return *e &= ~b; }
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t b,
                                     int clear, int all, TickType_t timeout) {
    assert(!clear && !all && b==BIT0 && !test_mutex.held);
    event_waits++; last_timeout=timeout;
    if (wake_hook) { void (*hook)(void)=wake_hook; wake_hook=NULL; hook(); }
    return *e;
}
'''

SERVICE_TEST = r'''
static bool backend_enabled, backend_muted, fail_start;
static unsigned starts, stops, submissions, volume_updates;
static int backend_rate, backend_volume;
static rg_audio_route_t backend_route=RG_AUDIO_ROUTE_SPEAKER;
static bool backend_init(int device,int rate) {
    (void)device; assert(test_mutex.held); backend_rate=rate; backend_enabled=false; return true;
}
static bool backend_deinit(void) { assert(test_mutex.held); backend_enabled=false; return true; }
static bool backend_enable(bool enabled) {
    assert(test_mutex.held);
    if (enabled && fail_start) { fail_start=false; return false; }
    if (enabled != backend_enabled) { if (enabled) starts++; else stops++; }
    backend_enabled=enabled; return true;
}
static bool backend_submit(const rg_audio_frame_t *f,size_t count) {
    assert(f && count && test_mutex.held && backend_enabled); submissions++; return true;
}
static bool backend_mute(bool muted) { assert(test_mutex.held); backend_muted=muted; return true; }
static bool backend_set_volume(int percent) {
    assert(test_mutex.held); volume_updates++; backend_volume=percent; return true;
}
static bool backend_set_rate(int rate) { assert(test_mutex.held); backend_rate=rate; return true; }
static rg_audio_route_t backend_get_route(void) { assert(test_mutex.held); return backend_route; }
static bool backend_set_device(int device) {
    backend_route=device==3?RG_AUDIO_ROUTE_HEADPHONES:RG_AUDIO_ROUTE_SPEAKER; return true;
}
const rg_audio_driver_t rg_audio_driver_i2s = {
    .name="i2s", .init=backend_init, .deinit=backend_deinit, .submit=backend_submit,
    .set_volume=backend_set_volume, .set_mute=backend_mute, .set_sample_rate=backend_set_rate,
    .get_route=backend_get_route, .set_device=backend_set_device, .set_enabled=backend_enable,
};
const rg_audio_driver_t rg_audio_driver_dummy = {
    .name="dummy", .init=backend_init, .deinit=backend_deinit, .submit=backend_submit,
};
static void raise_volume(void) { rg_audio_set_volume(25); }
int main(void) {
    rg_audio_init(32000); // Saved 0% must start disabled, including effective hardware mute.
    assert(!rg_audio_is_enabled() && !backend_enabled && backend_muted && !starts);
    assert(!rg_audio_wait_enabled(1) && last_timeout==1 && event_waits==1);
    rg_audio_set_mute(false);
    assert(backend_muted && !rg_audio_get_mute());
    rg_audio_frame_t frame={1000,-1000};
    unsigned before=submissions;
    audio.sampleRate=0;
    rg_audio_submit(&frame,1);
    assert(submissions==before); // An unavailable clock must not submit to a stopped backend.
    audio.sampleRate=32000;
    int64_t time_before=clock_us;
    for (unsigned n=0;n<32000;n++) rg_audio_submit(&frame,1);
    assert(submissions==before && sleeps<=101 && clock_us-time_before==1000000);
    assert(audio.silent_remainder==0); // Exact 32 kHz timing, including tiny submissions.
    wake_hook=raise_volume;
    assert(rg_audio_wait_enabled(-1) && last_timeout==portMAX_DELAY);
    assert(starts==1 && backend_enabled && backend_volume==25 && !backend_muted && event_bits==BIT0);
    test_mutex.held=true;
    assert(rg_audio_get_route()==RG_AUDIO_ROUTE_SPEAKER); // Cached status under contention.
    test_mutex.held=false;
    rg_audio_submit(&frame,1);
    assert(submissions==before+1);
    rg_audio_set_volume(60);
    assert(starts==1 && stops==0); // Ordinary volume updates do not restart output.
    rg_audio_set_volume(-10);
    assert(!rg_audio_is_enabled() && stops==1 && saved_speaker==0 && event_bits==0);
    rg_audio_set_sample_rate(48000);
    assert(backend_rate==48000 && !backend_enabled);
    audio.silent_deadline=0; audio.silent_remainder=0;
    time_before=clock_us;
    for (unsigned n=0;n<48000;n++) rg_audio_submit(NULL,1);
    assert(clock_us-time_before==1000000 && audio.silent_remainder==0);
    // Auto switching uses the destination's volume, including waking/stopping all waiters.
    backend_route=RG_AUDIO_ROUTE_HEADPHONES;
    assert(rg_audio_get_route()==RG_AUDIO_ROUTE_HEADPHONES);
    assert(rg_audio_is_enabled() && backend_volume==20 && starts==2);
    rg_audio_set_volume(0);
    assert(saved_hp==0 && stops==2);
    backend_route=RG_AUDIO_ROUTE_SPEAKER;
    rg_audio_get_route();
    assert(!rg_audio_is_enabled());
    rg_audio_set_mute(true);
    rg_audio_set_volume(101);
    assert(rg_audio_is_enabled() && backend_muted && saved_speaker==100);
    rg_audio_set_mute(false);
    assert(!backend_muted);
    rg_audio_set_sink("i2s",3);
    assert(!rg_audio_is_enabled() && rg_audio_get_volume()==0);
    fail_start=true;
    rg_audio_set_volume(10);
    assert(!rg_audio_is_enabled() && backend_muted && event_bits==0);
    rg_audio_set_volume(10);
    assert(rg_audio_is_enabled() && backend_enabled);
    rg_audio_deinit();
    assert(!rg_audio_is_enabled() && !backend_enabled && !event_bits);
    // Reinitialization and sample-rate changes must not lose the zero-volume state.
    saved_hp=0; rg_audio_init(22050);
    assert(!rg_audio_is_enabled());
    rg_audio_deinit();
    puts("PASS: saved zero, silent timing at 32/48 kHz, worker wake, route/sink switching, mute, rates, restart failure and reinit");
}
'''

I2S_TEST = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#define I2S_NUM_0 0
#define ESP_OK 0
#define RG_AUDIO_USE_HEADPHONE_JACK 1
#define RG_GPIO_SND_AMP_ENABLE 1
#define RG_GPIO_SND_HP_ENABLE 2
#define AMP_ON 1
#define AMP_OFF 0
#define HP_ON 1
#define HP_OFF 0
typedef enum { RG_AUDIO_ROUTE_UNKNOWN, RG_AUDIO_ROUTE_SPEAKER, RG_AUDIO_ROUTE_HEADPHONES } rg_audio_route_t;
static struct {
    bool output_enabled, running, muted;
    int sample_rate, ramp_gain, ramp_target, ramp_step, ramp_remaining;
    const char *last_error;
    rg_audio_route_t route;
} state;
static bool peripheral_running, allocated, fail_task, fail_dma;
static unsigned starts, stops, polls, clears;
static bool amp_on, hp_on;
static void gpio_set_level(int pin,int on) { if (pin==1) amp_on=on; else hp_on=on; }
static bool start_audio_task(void) {
    assert(peripheral_running && !allocated && !state.running);
    if (fail_task) { state.last_error="allocation failure"; return false; }
    starts++; allocated=true; state.running=true; return true;
}
static void stop_audio_task(void) {
    assert(!amp_on && !hp_on && allocated && state.running);
    stops++; allocated=false; state.running=false;
}
static int i2s_start(int port) { (void)port; if (fail_dma) return -1; peripheral_running=true; return ESP_OK; }
static int i2s_stop(int port) { (void)port; assert(!state.running); peripheral_running=false; return ESP_OK; }
static int i2s_zero_dma_buffer(int port) { (void)port; clears++; return ESP_OK; }
static int i2s_set_sample_rates(int port,int rate) { (void)port; (void)rate; peripheral_running=true; return ESP_OK; }
static void poll_jack(void) { polls++; }
'''

I2S_RUN = r'''
int main(void) {
    state.route=RG_AUDIO_ROUTE_SPEAKER;
    assert(driver_set_enabled(true));
    assert(starts==1 && allocated && peripheral_running && amp_on && !hp_on);
    assert(driver_set_enabled(true) && starts==1);
    assert(driver_set_enabled(false));
    assert(stops==1 && !allocated && !peripheral_running && !amp_on && !hp_on);
    driver_get_route(); assert(polls==1);
    assert(driver_set_sample_rates(48000) && !peripheral_running);
    state.route=RG_AUDIO_ROUTE_HEADPHONES;
    assert(driver_set_enabled(true) && starts==2 && hp_on && !amp_on);
    assert(driver_set_sample_rates(22050) && peripheral_running);
    driver_get_route(); assert(polls==1);
    assert(driver_set_enabled(false));
    fail_task=true;
    assert(!driver_set_enabled(true) && !allocated && !peripheral_running && !state.output_enabled);
    fail_task=false; fail_dma=true;
    assert(!driver_set_enabled(true) && !allocated && !state.output_enabled);
    fail_dma=false;
    assert(driver_set_enabled(true) && starts==3);
    assert(driver_set_enabled(false) && stops==3);
    puts("PASS: I2S writer allocation/cleanup, DMA stop/restart, amplifiers, route polling and failure recovery");
}
'''

MSX_TEST = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define AUDIO_SAMPLE_RATE 32000
#define RG_LOGI(...) ((void)0)
typedef int rg_task_t;
typedef struct { unsigned dataInt; } rg_task_msg_t;
static rg_task_t *audioQueue;
static uint64_t audio_remainder, sent_samples, silent_frames;
static int64_t FrameStartTime;
static bool enabled;
static unsigned pending, renders;
static bool rg_audio_is_enabled(void) { return enabled; }
static int64_t rg_system_timer(void) { return 0; }
static void rg_audio_submit(const void *frames,size_t count) {
    assert(!enabled && !frames); silent_frames += count;
}
static bool rg_task_send(rg_task_t *task,const rg_task_msg_t *msg) {
    (void)task; assert(enabled && !(msg->dataInt&1)); sent_samples += msg->dataInt; return true;
}
static bool rg_task_peek(rg_task_msg_t *msg) { msg->dataInt=32; return pending>0; }
static bool rg_task_receive(rg_task_msg_t *msg) { (void)msg; pending--; return true; }
static void RenderAndPlayAudio(unsigned count) { assert(enabled && count==32); renders++; }
'''

MSX_RUN = r'''
int main(void) {
    for (unsigned on=0;on<2;on++) {
        enabled=on; audio_remainder=sent_samples=silent_frames=0;
        PlayAllSound(0); PlayAllSound(-1);
        for (unsigned n=0;n<10000;n++) PlayAllSound(509);
        uint64_t frames=(uint64_t)509*10000*AUDIO_SAMPLE_RATE/1000000;
        assert(enabled ? (sent_samples==frames*2 && !silent_frames)
                       : (silent_frames==frames && !sent_samples));
        pending=10; renders=0; audioTask(NULL);
        assert(!pending && renders==(enabled?10:0));
    }
    puts("PASS: MSX fractional timing, silent main-task pacing, disabled request suppression and queued-work draining");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    compiler = [args.cc] + (["cc"] if Path(args.cc).stem.lower() == "zig" else [])
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "rg_system.h").write_text(SYSTEM)
        (folder / "rg_audio.h").write_text((ROOT / "components/retro-go/rg_audio.h").read_text())
        (folder / "freertos").mkdir()
        (folder / "freertos/FreeRTOS.h").write_text(RTOS)
        (folder / "freertos/event_groups.h").write_text('#include "FreeRTOS.h"\n')
        service = (ROOT / "components/retro-go/rg_audio.c").read_text()
        i2s = (ROOT / "components/retro-go/drivers/audio/i2s.c").read_text()
        lifecycle = "\n".join(function(i2s, signature) for signature in [
            "static void apply_route(", "static bool driver_set_sample_rates(",
            "static rg_audio_route_t driver_get_route(", "static bool driver_set_enabled("])
        cases = [(service + SERVICE_TEST, ["-DESP_PLATFORM"])]
        cases += [(I2S_TEST + lifecycle + I2S_RUN, [f"-DRG_AUDIO_VOLUME_RAMP_MS={ramp}"])
                  for ramp in (0, 20)]
        msx = (ROOT / "fmsx/main/main.c").read_text()
        cases += [(MSX_TEST + function(msx, "void PlayAllSound(") +
                   function(msx, "static void audioTask(") + MSX_RUN, ["-Wno-unused-parameter"])]
        for source, defines in cases:
            path = folder / "audio_power.c"
            path.write_text(source)
            executable = folder / "audio_power.exe"
            subprocess.run(compiler + ["-std=gnu11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                                      "-I", str(folder), *defines, str(path), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == "__main__":
    main()
