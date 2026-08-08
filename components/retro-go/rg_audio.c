#include "rg_system.h"
#include "rg_audio.h"

#include <stdlib.h>
#include <string.h>

extern const rg_audio_driver_t rg_audio_driver_dummy;
extern const rg_audio_driver_t rg_audio_driver_buzzer;
extern const rg_audio_driver_t rg_audio_driver_i2s;
extern const rg_audio_driver_t rg_audio_driver_sdl2;

// static const rg_audio_driver_t *drivers[] = {
//     NULL,
// };

static const rg_audio_sink_t sinks[] = {
    {&rg_audio_driver_dummy,  0, "Dummy",      false},
#if RG_AUDIO_USE_INT_DAC
    {&rg_audio_driver_i2s,    0, "Speaker",    false},
#endif
#if RG_AUDIO_USE_EXT_DAC
#if RG_AUDIO_USE_HEADPHONE_JACK
    // Device 1 keeps its meaning (the external DAC bus) so existing saved settings land on
    // automatic routing. Devices 2 and 3 pin the output down when the detection is wrong.
    {&rg_audio_driver_i2s,    1, "Auto",       true },
    {&rg_audio_driver_i2s,    2, "Speaker",    false},
    {&rg_audio_driver_i2s,    3, "Headphones", false},
#else
    {&rg_audio_driver_i2s,    1, "Ext DAC",    false},
#endif
#endif
#if RG_AUDIO_USE_SDL2
    {&rg_audio_driver_sdl2,   0, "SDL2",       false},
#endif
#if RG_AUDIO_USE_BUZZER_PIN
    {&rg_audio_driver_buzzer, 0, "Buzzer",     false},
#endif
    // {rg_audio_driver_bt_a2dp, 0, "Bluetooth"},
};

#define ACQUIRE_DEVICE(timeout)                         \
    ({                                                  \
        bool lock = rg_mutex_take(audio.lock, timeout); \
        if (!lock)                                      \
            RG_LOGE("Failed to acquire lock!\n");       \
        lock;                                           \
    })
#define RELEASE_DEVICE() rg_mutex_give(audio.lock)

static struct
{
    const rg_audio_sink_t *sink;
    const rg_audio_driver_t *driver;
    rg_mutex_t *lock;
    int sampleRate;
    int filter;
    int volumeSpeaker;
    int volumeHeadphones;
    rg_audio_route_t route;
    bool muted;
} audio;
static rg_audio_counters_t counters;

static const char *SETTING_DRIVER = "AudioDriver";
static const char *SETTING_DEVICE = "AudioDevice";
static const char *SETTING_VOLUME = "Volume";
static const char *SETTING_VOLUME_HP = "VolumeHP";
static const char *SETTING_FILTER = "AudioFilter";

static const char *get_last_driver_error(void)
{
    if (audio.driver && audio.driver->get_error)
        return audio.driver->get_error();
    return "Unspecified Error";
}

static int *active_volume(void)
{
    return audio.route == RG_AUDIO_ROUTE_HEADPHONES ? &audio.volumeHeadphones : &audio.volumeSpeaker;
}

// The driver may re-route on its own (a plug being inserted), so we poll it rather than
// being called back from an interrupt-adjacent context. Both fields are plain words and
// the worst outcome of a race with the driver is applying the same volume twice.
static void sync_route(void)
{
    // Read the driver once: the getters call this without holding the lock, so rg_audio_deinit
    // could clear audio.driver underneath us.
    const rg_audio_driver_t *driver = audio.driver;
    rg_audio_route_t route = RG_AUDIO_ROUTE_SPEAKER;

    if (!driver)
        return;
    if (driver->get_route)
        route = driver->get_route();

    if (route == audio.route)
        return;

    audio.route = route;
    if (driver->set_volume)
        driver->set_volume(*active_volume());

    RG_LOGI("Audio route is now '%s', volume %d%%\n", rg_audio_route_name(route), *active_volume());
}

static void select_sink(const char *driver_name, int device)
{
    audio.sink = NULL;
    for (size_t i = 0; i < RG_COUNT(sinks); ++i)
    {
        if (strcmp(sinks[i].driver->name, driver_name) == 0 && sinks[i].device == device)
            audio.sink = &sinks[i];
    }
}

void rg_audio_init(int sampleRate)
{
    RG_ASSERT(audio.sink == NULL, "Audio sink already initialized!");

    if (!audio.lock)
    {
        audio.lock = rg_mutex_create();
        RELEASE_DEVICE();
    }
    ACQUIRE_DEVICE(1000);

    char *driver_name = rg_settings_get_string(NS_GLOBAL, SETTING_DRIVER, "DEFAULT");
    int device = rg_settings_get_number(NS_GLOBAL, SETTING_DEVICE, 0);
    select_sink(driver_name, device);
    free(driver_name);

    if (!audio.sink) // Default to first non-dummy if no match found
        audio.sink = &sinks[1 % RG_COUNT(sinks)];

    audio.filter = (int)rg_settings_get_number(NS_GLOBAL, SETTING_FILTER, 0);
    audio.volumeSpeaker = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME, 50);
    audio.volumeHeadphones = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME_HP, RG_AUDIO_HP_DEFAULT_VOLUME);
    audio.route = RG_AUDIO_ROUTE_UNKNOWN;
    audio.sampleRate = sampleRate;
    audio.driver = audio.sink->driver;

    if (audio.driver->init(audio.sink->device, sampleRate))
    {
        // The driver resolved its route during init, so pick up the matching volume before unmuting
        sync_route();

        if (audio.driver->set_mute)
            audio.driver->set_mute(audio.muted);
        if (audio.driver->set_volume)
            audio.driver->set_volume(*active_volume());

        RG_LOGI("Audio ready. sink='%s', route='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, rg_audio_route_name(audio.route), audio.sampleRate, *active_volume());
    }
    else
    {
        RG_LOGE("Failed to initialize audio. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, *active_volume());
        RG_LOGE(" - Error: %s\n", get_last_driver_error());
        audio.sink = &sinks[0]; // Switching to dummy might allow us to at least boot
        audio.driver = audio.sink->driver;
        audio.route = RG_AUDIO_ROUTE_UNKNOWN;
    }

    RELEASE_DEVICE();
}

void rg_audio_deinit(void)
{
    if (!audio.sink)
        return;

    // We'll go ahead even if we can't acquire the lock...
    ACQUIRE_DEVICE(1000);

    audio.driver->deinit();

    RG_LOGI("Audio terminated. sink='%s'\n", audio.sink->name);

    audio.driver = NULL;
    audio.sink = NULL;
    audio.route = RG_AUDIO_ROUTE_UNKNOWN;

    RELEASE_DEVICE();
}

void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    const int64_t time_start = rg_system_timer();

    if (!audio.driver)
        return;

    if (!frames || !count)
        return;

    // A momentary settings operation must not silently discard a whole audio
    // block. A short bounded wait preserves continuity without stalling a frame.
    if (ACQUIRE_DEVICE(10))
    {
        sync_route();
        audio.driver->submit(frames, count);
        RELEASE_DEVICE();
    }

    counters.totalSamples += count;
    counters.busyTime += rg_system_timer() - time_start;
}

rg_audio_counters_t rg_audio_get_counters(void)
{
    return counters;
}

const char *rg_audio_get_driver(void)
{
    if (!audio.driver)
        return NULL;
    return audio.driver->name;
}

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count)
{
    if (count)
        *count = RG_COUNT(sinks);
    return sinks;
}

const rg_audio_sink_t *rg_audio_get_sink(void)
{
    return audio.sink;
}

void rg_audio_set_sink(const char *driver_name, int device)
{
    RG_LOGI("%s %d", driver_name, device);
    rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, driver_name);
    rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, device);

    // Selecting a different output of the same driver (speaker vs headphones) doesn't need the
    // peripheral to be torn down and reinstalled. Scrolling through the menu would otherwise
    // reinstall I2S on every keypress, which drops samples and pops the amplifier.
    const rg_audio_sink_t *previous = audio.sink;
    if (previous && audio.driver && audio.driver->set_device && strcmp(audio.driver->name, driver_name) == 0)
    {
        bool switched = false;

        if (ACQUIRE_DEVICE(1000))
        {
            select_sink(driver_name, device);
            if (audio.sink && (switched = audio.driver->set_device(device)))
                sync_route();
            else
                audio.sink = previous; // Leave the driver's actual state described correctly
            RELEASE_DEVICE();
        }

        if (switched)
            return;
    }

    rg_audio_deinit();
    rg_audio_init(audio.sampleRate);
}

void rg_audio_get_sink_label(char *buffer, size_t length)
{
    if (!buffer || !length)
        return;

    sync_route();

    if (!audio.sink)
        snprintf(buffer, length, "-");
    else if (audio.sink->automatic && audio.route != RG_AUDIO_ROUTE_UNKNOWN)
        snprintf(buffer, length, "%s (%s)", audio.sink->name, rg_audio_route_name(audio.route));
    else
        snprintf(buffer, length, "%s", audio.sink->name);
}

rg_audio_route_t rg_audio_get_route(void)
{
    sync_route();
    return audio.route;
}

const char *rg_audio_route_name(rg_audio_route_t route)
{
    switch (route)
    {
    case RG_AUDIO_ROUTE_SPEAKER:
        return "Speaker";
    case RG_AUDIO_ROUTE_HEADPHONES:
        return "Headphones";
    default:
        return "Unknown";
    }
}

int rg_audio_get_volume(void)
{
    return *active_volume();
}

void rg_audio_set_volume(int percent)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");

    bool headphones = audio.route == RG_AUDIO_ROUTE_HEADPHONES;
    int volume = RG_MIN(RG_MAX(percent, 0), 100);

    *active_volume() = volume;
    if (audio.driver->set_volume)
        audio.driver->set_volume(volume);
    rg_settings_set_number(NS_GLOBAL, headphones ? SETTING_VOLUME_HP : SETTING_VOLUME, volume);
    RG_LOGI("Volume set to %d%% (%s)\n", volume, rg_audio_route_name(audio.route));
}

bool rg_audio_get_mute(void)
{
    return audio.muted;
}

void rg_audio_set_mute(bool mute)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");

    if (!ACQUIRE_DEVICE(1000))
        return;

    if (audio.driver->set_mute)
        audio.driver->set_mute(mute);

    audio.muted = mute;
    RELEASE_DEVICE();
}

int rg_audio_get_sample_rate(void)
{
    return audio.sampleRate;
}

void rg_audio_set_sample_rate(int sampleRate)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");

    if (audio.sampleRate == sampleRate)
        return;

    if (audio.driver->set_sample_rate)
    {
        if (ACQUIRE_DEVICE(1000))
        {
            audio.driver->set_sample_rate(sampleRate);
            audio.sampleRate = sampleRate;
            RG_LOGI("Samplerate set to %d", audio.sampleRate);
            RELEASE_DEVICE();
        }
    }
    else
    {
        rg_audio_deinit();
        rg_audio_init(sampleRate);
    }
}
