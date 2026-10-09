#include <rg_system.h>
#include <stdlib.h>
#include "media_lighting.h"
#include "media_audio.h"
#include "media_util.h"

static struct
{
    volatile bool running, stop;
    uint32_t levels, stamp_ms;
    int low;
} lighting;

const char *media_lighting_name(media_light_t effect)
{
    static const char *names[] = {"Off", "Rainbow", "Music sync", "Bass pulse", "Beat dance", "Spectrum"};
    return names[effect >= 0 && effect < MEDIA_LIGHT_COUNT ? effect : 0];
}

bool media_lighting_available(void)
{
#if defined(RG_GPIO_LED_WS2812)
    return true;
#else
    return false;
#endif
}

void media_lighting_feed(const int16_t *pcm, size_t frames)
{
    if (!frames || !lighting.running) return;
    uint32_t energy = 0, bass = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        int left = pcm[i * 2], right = pcm[i * 2 + 1];
        int mono = (left + right) / 2;
        // Low-pass envelope estimates bass without sharing mutable FFT state with the UI.
        lighting.low += (mono - lighting.low) / 32;
        energy += (abs(left) + abs(right)) / 2;
        bass += abs(lighting.low);
    }
    uint32_t values = (energy / frames) | ((bass / frames) << 16);
    __atomic_store_n(&lighting.levels, values, __ATOMIC_RELEASE);
    __atomic_store_n(&lighting.stamp_ms, (uint32_t)(rg_system_timer() / 1000), __ATOMIC_RELEASE);
}

uint16_t media_lighting_color(media_light_t effect, int brightness, int level, int bass,
                              bool beat, unsigned hue)
{
    level = media_clampi(level, 0, 255);
    bass = media_clampi(bass, 0, 255);
    unsigned sector = (hue % 1536) / 256, ramp = hue % 256;
    int r = 0, g = 0, b = 0;
    switch (sector)
    {
    case 0: r = 255; g = ramp; break;
    case 1: r = 255 - ramp; g = 255; break;
    case 2: g = 255; b = ramp; break;
    case 3: g = 255 - ramp; b = 255; break;
    case 4: b = 255; r = ramp; break;
    default: b = 255 - ramp; r = 255; break;
    }
    int intensity = level;
    if (effect == MEDIA_LIGHT_MUSIC)
    {
        r = bass; g = level / 2; b = 255 - bass / 2;
        if (beat) r = g = b = 255;
    }
    else if (effect == MEDIA_LIGHT_BASS)
    {
        r = 255; g = beat ? 80 : 0; b = 180;
        intensity = bass;
    }
    else if (effect == MEDIA_LIGHT_SPECTRUM)
    {
        r = bass; g = level; b = RG_MAX(level - bass, 0) * 2;
        if (b > 255) b = 255;
        intensity = 255;
    }
    if (effect == MEDIA_LIGHT_OFF) return 0;
    intensity = intensity * media_clampi(brightness, 0, 100) / 100;
    return C_RGB(r * intensity / 255, g * intensity / 255, b * intensity / 255);
}

static void lighting_task(void *arg)
{
    (void)arg;
    unsigned hue = 0;
    int average = 1024, envelope = 0, bass_envelope = 0;
    uint32_t last_beat = 0;
    while (!lighting.stop)
    {
        const media_settings_t *cfg = media_settings();
        uint32_t now = (uint32_t)(rg_system_timer() / 1000);
        uint32_t levels = __atomic_load_n(&lighting.levels, __ATOMIC_ACQUIRE);
        bool active = media_audio_running() && !media_audio_get_paused() && !rg_audio_get_mute() &&
            now - __atomic_load_n(&lighting.stamp_ms, __ATOMIC_ACQUIRE) < 150;
        if (cfg->lighting == MEDIA_LIGHT_OFF || !active || !cfg->lighting_brightness)
        {
            rg_system_set_led_override(C_NONE);
            envelope = bass_envelope = 0;
        }
        else
        {
            int level = levels & 0xFFFF, bass = levels >> 16;
            bool beat = bass > average * 3 / 2 && bass > 450 && now - last_beat > 180;
            average += (bass - average) / 24;
            if (average < 150) average = 150;
            int target = level < 60 ? 0 : media_clampi(level * 180 / RG_MAX(average * 2, 1000), 0, 255);
            int bass_target = bass < 60 ? 0 : media_clampi(bass * 200 / RG_MAX(average * 2, 1000), 0, 255);
            envelope += (target - envelope) / (target > envelope ? 2 : 5);
            bass_envelope += (bass_target - bass_envelope) / (bass_target > bass_envelope ? 2 : 6);
            if (beat) { last_beat = now; if (cfg->lighting == MEDIA_LIGHT_BEAT) hue += 237; }
            if (cfg->lighting != MEDIA_LIGHT_BEAT) hue += 8;
            rg_system_set_led_override(media_lighting_color(cfg->lighting, cfg->lighting_brightness,
                envelope, bass_envelope, now - last_beat < 70 && last_beat != 0, hue));
        }
        rg_task_delay(33);
    }
    rg_system_set_led_override(C_NONE);
    __atomic_store_n(&lighting.running, false, __ATOMIC_RELEASE);
}

bool media_lighting_start(void)
{
    if (!media_lighting_available() || lighting.running) return true;
    lighting.stop = false;
    lighting.low = 0;
    lighting.levels = lighting.stamp_ms = 0;
    lighting.running = true;
    if (!rg_task_create("media_light", lighting_task, NULL, 3 * 1024, RG_TASK_PRIORITY_2,
                        RG_TASK_AFFINITY_MAIN))
        lighting.running = false;
    return lighting.running;
}

bool media_lighting_stop(void)
{
    lighting.stop = true;
    for (int i = 0; i < 100 && __atomic_load_n(&lighting.running, __ATOMIC_ACQUIRE); ++i)
        rg_task_delay(10);
    return !lighting.running;
}
