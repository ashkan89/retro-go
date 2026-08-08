#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_INT_DAC || RG_AUDIO_USE_EXT_DAC

#include <stdlib.h>
#include <string.h>

#ifndef ESP_PLATFORM
#error "I2S support can only be built inside esp-idf!"
#elif !CONFIG_IDF_TARGET_ESP32 && RG_AUDIO_USE_INT_DAC
#error "Your chip has no DAC! Please set RG_AUDIO_USE_INT_DAC to 0 in your target file."
#endif

#if RG_AUDIO_USE_HEADPHONE_JACK
#if !RG_AUDIO_USE_EXT_DAC
#error "RG_AUDIO_USE_HEADPHONE_JACK requires RG_AUDIO_USE_EXT_DAC, the headphone DAC shares the I2S bus."
#elif RG_AUDIO_USE_INT_DAC
#error "RG_AUDIO_USE_HEADPHONE_JACK cannot be combined with RG_AUDIO_USE_INT_DAC (ambiguous sink names)."
#elif !defined(RG_GPIO_SND_HP_DETECT)
#error "RG_AUDIO_USE_HEADPHONE_JACK requires RG_GPIO_SND_HP_DETECT to be defined in your target file."
#elif !defined(RG_GPIO_SND_AMP_ENABLE)
#error "RG_AUDIO_USE_HEADPHONE_JACK requires RG_GPIO_SND_AMP_ENABLE, the speaker amp must be shut down."
#endif
#endif

#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Speaker amplifier shutdown pin (MAX98357A SD_MODE and friends). Active high unless inverted.
#ifdef RG_GPIO_SND_AMP_ENABLE_INVERT
#define AMP_ON 0
#define AMP_OFF 1
#else
#define AMP_ON 1
#define AMP_OFF 0
#endif

// Headphone path enable pin (PCM5102A XSMT or a headphone amp shutdown). Active high unless inverted.
#ifdef RG_GPIO_SND_HP_ENABLE_INVERT
#define HP_ON 0
#define HP_OFF 1
#else
#define HP_ON 1
#define HP_OFF 0
#endif

// Devices 1-3 all drive the same external I2S bus, they differ only in where the analog
// signal is allowed to leave the board. Device 0 is the ESP32's built-in DAC.
#define DEVICE_INT_DAC 0
#define DEVICE_EXT_AUTO 1
#define DEVICE_EXT_SPEAKER 2
#define DEVICE_EXT_HEADPHONES 3
#define DEVICE_IS_EXTERNAL(dev) ((dev) >= DEVICE_EXT_AUTO && (dev) <= DEVICE_EXT_HEADPHONES)

// We can safely assume that no application will submit more than 640 audio frames per call to
// driver_submit (32000/50). Using a single large buffer risks blocking the call needlessly because
// some apps submit more than once per cycle or there could be occasional jitter (early submission).
#ifndef RG_AUDIO_DMA_BUFFER_COUNT
#define RG_AUDIO_DMA_BUFFER_COUNT 4
#endif

#ifndef RG_AUDIO_DMA_BUFFER_LENGTH
#define RG_AUDIO_DMA_BUFFER_LENGTH 180
#endif

#ifndef RG_AUDIO_I2S_INTR_FLAGS
#define RG_AUDIO_I2S_INTR_FLAGS 0
#endif

#ifndef RG_AUDIO_QUEUE_LENGTH
// This queue decouples emulator/render timing from the blocking I2S DMA writer.
// 1536 stereo frames are 48 ms at 32 kHz, enough to bridge several render
// spikes without building excessive steady-state latency.
#define RG_AUDIO_QUEUE_LENGTH 1536
#endif

#ifndef RG_AUDIO_TASK_STACK_SIZE
#define RG_AUDIO_TASK_STACK_SIZE (4 * 1024)
#endif

#ifndef RG_AUDIO_TASK_PRIORITY
#define RG_AUDIO_TASK_PRIORITY RG_TASK_PRIORITY_9
#endif

#ifndef RG_AUDIO_I2S_WRITE_TIMEOUT_MS
#define RG_AUDIO_I2S_WRITE_TIMEOUT_MS 100
#endif

#define DMA_BUFFER_COUNT RG_AUDIO_DMA_BUFFER_COUNT
#define DMA_BUFFER_LEN RG_AUDIO_DMA_BUFFER_LENGTH

static struct {
    const char *last_error;
    int device;
    int volume;
    bool muted;
    volatile rg_audio_route_t route;
    bool jack_present;
    bool jack_candidate;
    int64_t jack_changed_at;
    rg_audio_frame_t *queue;
    size_t queue_read;
    size_t queue_write;
    size_t queue_count;
    TaskHandle_t task;
    SemaphoreHandle_t queue_space;
    SemaphoreHandle_t task_stopped;
    volatile bool running;
    uint32_t write_errors;
} state;
static portMUX_TYPE queue_lock = portMUX_INITIALIZER_UNLOCKED;

static bool write_frames(const rg_audio_frame_t *frames, size_t count);

// Drive the two analog output stages from the current route and mute state. Exactly one of
// them is ever enabled, so the speaker can't keep playing into the room while someone is
// wearing headphones, and the headphone amp isn't left running into an empty jack.
static void apply_route(void)
{
    // A board with no enable pins at all (amplifier hardwired on) has nothing to do here
    bool headphones = state.route == RG_AUDIO_ROUTE_HEADPHONES;
    bool audible = !state.muted;
    (void)headphones, (void)audible;

    #ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_set_level(RG_GPIO_SND_AMP_ENABLE, (audible && !headphones) ? AMP_ON : AMP_OFF);
    #endif
    #ifdef RG_GPIO_SND_HP_ENABLE
    gpio_set_level(RG_GPIO_SND_HP_ENABLE, (audible && headphones) ? HP_ON : HP_OFF);
    #endif
}

static void resolve_route(void)
{
    if (state.device == DEVICE_EXT_HEADPHONES)
        state.route = RG_AUDIO_ROUTE_HEADPHONES;
    else if (state.device == DEVICE_EXT_AUTO && state.jack_present)
        state.route = RG_AUDIO_ROUTE_HEADPHONES;
    else
        state.route = RG_AUDIO_ROUTE_SPEAKER;
}

#if RG_AUDIO_USE_HEADPHONE_JACK
static bool read_jack(void)
{
    return gpio_get_level(RG_GPIO_SND_HP_DETECT) == RG_GPIO_SND_HP_DETECT_LEVEL;
}

// The detect line is a mechanical contact, it chatters for tens of milliseconds while a plug
// slides in. Acting on the first edge would flap the amplifiers audibly, so a new state has to
// hold for the full debounce window. Timestamps rather than sample counts because this is
// called from the writer task, whose wake-up rate follows the audio load.
static void poll_jack(void)
{
    bool inserted = read_jack();
    int64_t now = rg_system_timer();

    if (inserted != state.jack_candidate)
    {
        state.jack_candidate = inserted;
        state.jack_changed_at = now;
        return;
    }

    if (inserted == state.jack_present)
        return;
    if (now - state.jack_changed_at < RG_AUDIO_HP_DEBOUNCE_MS * 1000)
        return;

    state.jack_present = inserted;
    RG_LOGI("Headphone jack %s\n", inserted ? "connected" : "disconnected");

    if (state.device == DEVICE_EXT_AUTO)
    {
        resolve_route();
        apply_route();
    }
}
#endif

static void audio_task(void *arg)
{
    rg_audio_frame_t frames[DMA_BUFFER_LEN];

    while (true)
    {
        // A timed wait so the jack is still sampled while nothing is being played, otherwise
        // plugging in during a quiet moment wouldn't be noticed until audio resumed.
        #if RG_AUDIO_USE_HEADPHONE_JACK
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RG_AUDIO_HP_POLL_INTERVAL_MS));
        poll_jack();
        #else
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        #endif

        while (true)
        {
            size_t count = 0;
            bool running;

            portENTER_CRITICAL(&queue_lock);
            running = state.running;
            if (running && state.queue_count)
            {
                count = RG_MIN(state.queue_count, RG_COUNT(frames));
                count = RG_MIN(count, RG_AUDIO_QUEUE_LENGTH - state.queue_read);
                memcpy(frames, &state.queue[state.queue_read], count * sizeof(*frames));
                state.queue_read = (state.queue_read + count) % RG_AUDIO_QUEUE_LENGTH;
                state.queue_count -= count;
            }
            else if (!running)
            {
                state.queue_count = 0;
            }
            portEXIT_CRITICAL(&queue_lock);

            if (!running)
                goto stopped;
            if (!count)
                break;

            // Wake a producer which is applying back-pressure because the
            // software queue is full. The writer itself remains the only task
            // allowed to block on I2S DMA.
            xSemaphoreGive(state.queue_space);
            if (!write_frames(frames, count))
                state.write_errors++;
        }
    }

stopped:
    xSemaphoreGive(state.queue_space);
    xSemaphoreGive(state.task_stopped);
    vTaskDelete(NULL);
}

static bool start_audio_task(void)
{
    state.queue = heap_caps_malloc(RG_AUDIO_QUEUE_LENGTH * sizeof(*state.queue),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    state.queue_space = xSemaphoreCreateBinary();
    state.task_stopped = xSemaphoreCreateBinary();
    if (!state.queue || !state.queue_space || !state.task_stopped)
    {
        state.last_error = "Unable to allocate the real-time audio queue";
        goto fail;
    }

    state.queue_read = 0;
    state.queue_write = 0;
    state.queue_count = 0;
    state.write_errors = 0;
    state.running = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        audio_task, "rg_audio", RG_AUDIO_TASK_STACK_SIZE, NULL,
        RG_AUDIO_TASK_PRIORITY, &state.task, RG_TASK_AFFINITY_AUDIO);
    if (result != pdPASS)
    {
        state.last_error = "Unable to create the real-time audio task";
        state.running = false;
        goto fail;
    }

    RG_LOGI("I2S queue ready. frames=%d, task_priority=%d, core=%d\n",
            RG_AUDIO_QUEUE_LENGTH, RG_AUDIO_TASK_PRIORITY, RG_TASK_AFFINITY_AUDIO);
    return true;

fail:
    if (state.queue)
        free(state.queue);
    if (state.queue_space)
        vSemaphoreDelete(state.queue_space);
    if (state.task_stopped)
        vSemaphoreDelete(state.task_stopped);
    state.queue = NULL;
    state.queue_space = NULL;
    state.task_stopped = NULL;
    state.task = NULL;
    return false;
}

static void stop_audio_task(void)
{
    if (!state.task)
        return;

    portENTER_CRITICAL(&queue_lock);
    state.running = false;
    portEXIT_CRITICAL(&queue_lock);

    xTaskNotifyGive(state.task);
    xSemaphoreGive(state.queue_space);
    xSemaphoreTake(state.task_stopped, portMAX_DELAY);

    if (state.write_errors)
        RG_LOGW("I2S writer stopped with %lu submission error(s).\n",
                (unsigned long)state.write_errors);

    state.task = NULL;
    vSemaphoreDelete(state.queue_space);
    vSemaphoreDelete(state.task_stopped);
    free(state.queue);
    state.queue = NULL;
    state.queue_space = NULL;
    state.task_stopped = NULL;
}

static bool driver_init(int device, int sample_rate)
{
    state.last_error = NULL;
    state.device = device;

    // Bring the outputs up shut down: the I2S bus is silent until the writer task runs, and an
    // enabled amplifier would turn that undefined state into a pop.
    #ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
    gpio_set_level(RG_GPIO_SND_AMP_ENABLE, AMP_OFF);
    gpio_set_direction(RG_GPIO_SND_AMP_ENABLE, GPIO_MODE_OUTPUT);
    #endif
    #ifdef RG_GPIO_SND_HP_ENABLE
    gpio_reset_pin(RG_GPIO_SND_HP_ENABLE);
    gpio_set_level(RG_GPIO_SND_HP_ENABLE, HP_OFF);
    gpio_set_direction(RG_GPIO_SND_HP_ENABLE, GPIO_MODE_OUTPUT);
    #endif

    #if RG_AUDIO_USE_HEADPHONE_JACK
    // Pulled up and active low, so an unmodified board (nothing wired to the detect pin) reads
    // "no headphones" and keeps using its speaker.
    gpio_reset_pin(RG_GPIO_SND_HP_DETECT);
    gpio_set_direction(RG_GPIO_SND_HP_DETECT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RG_GPIO_SND_HP_DETECT, GPIO_PULLUP_ONLY);
    rg_task_delay(20); // Let the pull-up charge the line (and any debounce capacitor) before sampling
    state.jack_present = state.jack_candidate = read_jack();
    state.jack_changed_at = rg_system_timer();
    #endif

    resolve_route();

    if (state.device == DEVICE_INT_DAC)
    {
    #if RG_AUDIO_USE_INT_DAC
        esp_err_t ret = i2s_driver_install(I2S_NUM_0, &(i2s_config_t){
            .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
            .sample_rate = sample_rate,
            .bits_per_sample = 16,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_MSB,
            .intr_alloc_flags = RG_AUDIO_I2S_INTR_FLAGS,
            .dma_buf_count = DMA_BUFFER_COUNT,
            .dma_buf_len = DMA_BUFFER_LEN,
            .tx_desc_auto_clear = true,
        }, 0, NULL);
        if (ret == ESP_OK)
            ret = i2s_set_dac_mode(RG_AUDIO_USE_INT_DAC);
        if (ret != ESP_OK)
            state.last_error = esp_err_to_name(ret);
    #else
        state.last_error = "This device does not support internal DAC mode!";
    #endif
    }
    else if (DEVICE_IS_EXTERNAL(state.device))
    {
    #if RG_AUDIO_USE_EXT_DAC
        esp_err_t ret = i2s_driver_install(I2S_NUM_0, &(i2s_config_t){
            .mode = I2S_MODE_MASTER | I2S_MODE_TX,
            .sample_rate = sample_rate,
            .bits_per_sample = 16,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = RG_AUDIO_I2S_INTR_FLAGS,
            .dma_buf_count = DMA_BUFFER_COUNT,
            .dma_buf_len = DMA_BUFFER_LEN,
            .tx_desc_auto_clear = true,
        #if CONFIG_IDF_TARGET_ESP32
            .use_apll = true, // External DAC may care about accuracy
        #endif
        }, 0, NULL);
        if (ret == ESP_OK)
        {
            ret = i2s_set_pin(I2S_NUM_0, &(i2s_pin_config_t) {
                .mck_io_num = GPIO_NUM_NC,
                .bck_io_num = RG_GPIO_SND_I2S_BCK,
                .ws_io_num = RG_GPIO_SND_I2S_WS,
                .data_out_num = RG_GPIO_SND_I2S_DATA,
                .data_in_num = GPIO_NUM_NC
            });
        }
        if (ret != ESP_OK)
            state.last_error = esp_err_to_name(ret);
    #else
        state.last_error = "This device does not support external DAC mode!";
    #endif
    }

    if (!state.last_error && !start_audio_task())
        i2s_driver_uninstall(I2S_NUM_0);

    // The outputs stay shut down until rg_audio_init() applies the unmute, which happens once
    // the writer task is up and the DMA buffers hold real samples.
    return state.last_error == NULL;
}

static bool driver_set_sample_rates(int sampleRate)
{
    return i2s_set_sample_rates(I2S_NUM_0, sampleRate) == ESP_OK;
}

static bool driver_deinit(void)
{
    stop_audio_task();
    i2s_driver_uninstall(I2S_NUM_0);
    if (state.device == DEVICE_INT_DAC)
    {
    #if RG_AUDIO_USE_INT_DAC
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    #endif
    }
    else if (DEVICE_IS_EXTERNAL(state.device))
    {
    #if RG_AUDIO_USE_EXT_DAC
        gpio_reset_pin(RG_GPIO_SND_I2S_BCK);
        gpio_reset_pin(RG_GPIO_SND_I2S_DATA);
        gpio_reset_pin(RG_GPIO_SND_I2S_WS);
    #endif
    }
    #ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
    #endif
    #ifdef RG_GPIO_SND_HP_ENABLE
    gpio_reset_pin(RG_GPIO_SND_HP_ENABLE);
    #endif
    #if RG_AUDIO_USE_HEADPHONE_JACK
    gpio_reset_pin(RG_GPIO_SND_HP_DETECT);
    #endif
    state.route = RG_AUDIO_ROUTE_UNKNOWN;
    return true;
}

// Retarget the analog output without touching the I2S peripheral. Only valid between the
// external-DAC devices, they share an identical bus configuration.
static bool driver_set_device(int device)
{
    if (device == state.device)
        return true;
    if (!DEVICE_IS_EXTERNAL(device) || !DEVICE_IS_EXTERNAL(state.device))
        return false;

    state.device = device;
    resolve_route();
    apply_route();
    return true;
}

static rg_audio_route_t driver_get_route(void)
{
    return state.route;
}

static bool write_frames(const rg_audio_frame_t *frames, size_t count)
{
    int volume = state.muted ? 0 : state.volume;
    rg_audio_frame_t buffer[DMA_BUFFER_LEN];
    size_t pos = 0;

    #if RG_AUDIO_USE_INT_DAC
    bool use_internal_dac = state.device == DEVICE_INT_DAC;
    #endif

    for (size_t i = 0; i < count; ++i)
    {
        int left = ((int32_t)frames[i].left * volume) / 100;
        int right = ((int32_t)frames[i].right * volume) / 100;

    #if RG_AUDIO_USE_INT_DAC
        if (use_internal_dac)
        {
            int mono = (left + right) >> 1;
        #if RG_AUDIO_USE_INT_DAC == 1
            left = mono + 0x8000; // the internal DAC expects unsigned data
            right = 0;
        #elif RG_AUDIO_USE_INT_DAC == 2
            left = 0;
            right = mono + 0x8000; // the internal DAC expects unsigned data
        #elif RG_AUDIO_USE_INT_DAC == 3
            // In two channel mode we use left and right as a differential mono output to increase resolution.
            int sample = mono;
            if (sample > 0x7F00)
            {
                left = 0x8000 + (sample - 0x7F00);
                right = -0x8000 + 0x7F00;
            }
            else if (sample < -0x7F00)
            {
                left = 0x8000 + (sample + 0x7F00);
                right = -0x8000 + -0x7F00;
            }
            else
            {
                left = 0x8000;
                right = -0x8000 + sample;
            }
        #endif
        }
    #endif

        // Clipping   (not necessary, we have (int16 * vol) and volume is never more than 1.0)
        // if (left > 32767) left = 32767; else if (left < -32768) left = -32767;
        // if (right > 32767) right = 32767; else if (right < -32768) right = -32767;

        // Queue
        buffer[pos].left = left;
        buffer[pos].right = right;
        pos++;

        if (pos == RG_COUNT(buffer) || i == count - 1)
        {
            const size_t requested = pos * sizeof(*buffer);
            size_t written = 0;
            esp_err_t result = i2s_write(I2S_NUM_0, buffer, requested, &written,
                                         pdMS_TO_TICKS(RG_AUDIO_I2S_WRITE_TIMEOUT_MS));
            if (result != ESP_OK || written != requested)
            {
                RG_LOGW("I2S submission error: %s, written=%d/%d\n",
                        esp_err_to_name(result), (int)written, (int)requested);
                return false;
            }
            pos = 0;
        }
    }
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    size_t submitted = 0;

    while (submitted < count)
    {
        size_t copied = 0;
        TaskHandle_t task;

        portENTER_CRITICAL(&queue_lock);
        task = state.task;
        if (state.running && state.queue_count < RG_AUDIO_QUEUE_LENGTH)
        {
            copied = RG_MIN(count - submitted, RG_AUDIO_QUEUE_LENGTH - state.queue_count);
            copied = RG_MIN(copied, RG_AUDIO_QUEUE_LENGTH - state.queue_write);
            memcpy(&state.queue[state.queue_write], &frames[submitted], copied * sizeof(*frames));
            state.queue_write = (state.queue_write + copied) % RG_AUDIO_QUEUE_LENGTH;
            state.queue_count += copied;
        }
        portEXIT_CRITICAL(&queue_lock);

        if (!task || !state.running)
            return false;

        if (copied)
        {
            submitted += copied;
            xTaskNotifyGive(task);
        }
        else
        {
            // The hardware sample clock is the ultimate pacing source. This
            // wait only happens when software is already a full queue ahead;
            // it prevents sample loss while normal submissions stay async.
            xSemaphoreTake(state.queue_space, portMAX_DELAY);
        }
    }

    return true;
}

static bool driver_set_mute(bool mute)
{
    i2s_zero_dma_buffer(I2S_NUM_0);
    state.muted = mute;
    apply_route();
    return true;
}

static bool driver_set_volume(int volume)
{
    state.volume = volume;
    return true;
}

static const char *driver_get_error(void)
{
    return state.last_error;
}

const rg_audio_driver_t rg_audio_driver_i2s = {
    .name = "i2s",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
    .set_sample_rate = driver_set_sample_rates,
    .set_device = driver_set_device,
    .get_route = driver_get_route,
    .get_error = driver_get_error,
};

#endif // RG_AUDIO_USE_INT_DAC || RG_AUDIO_USE_EXT_DAC
