#include "rg_system.h"
#include "rg_usb_hid.h"

#if defined(ESP_PLATFORM) && defined(RG_ENABLE_USB_HID_HOST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#define USB_HID_SETTING_ENABLE "USBHIDEnabled"
#define USB_HID_REPORT_MAX 64
#define USB_HID_MOUSE_HOLD_US 80000
#define USB_HID_AXIS_CAPTURE_THRESHOLD 16
#define USB_HID_AXIS_DEADZONE 8
#define USB_HID_CAPTURE_CONFIRM_REPORTS 3

#define SOURCE_TYPE(v) ((uint8_t)((v) >> 28))
#define SOURCE_KEYBOARD 1
#define SOURCE_MOUSE_BUTTON 2
#define SOURCE_MOUSE_MOTION 3
#define SOURCE_GENERIC_BIT 4
#define SOURCE_GENERIC_AXIS 5

typedef struct
{
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_event_t;

static QueueHandle_t hid_event_queue;
static volatile bool usb_ready;
static volatile bool usb_running;
static volatile bool hid_enabled;
static volatile uint32_t connected_mask;
static portMUX_TYPE hid_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t mappings[RG_USB_HID_DEVICE_COUNT][RG_KEY_COUNT];
static bool keyboard_keys[256];
static uint8_t mouse_buttons;
static int64_t mouse_motion_until[4];
static uint8_t generic_report[USB_HID_REPORT_MAX];
static size_t generic_report_len;

static volatile int capture_device = -1;
static volatile uint32_t capture_result;
static bool capture_keyboard_baseline[256];
static uint8_t capture_mouse_baseline;
static uint8_t capture_generic_baseline[USB_HID_REPORT_MAX];
static size_t capture_generic_len;
static bool capture_generic_valid;
static uint32_t capture_generic_candidate;
static uint8_t capture_generic_candidate_count;

static const uint16_t default_keyboard[RG_KEY_COUNT] = {
    [0] = HID_KEY_UP,
    [1] = HID_KEY_RIGHT,
    [2] = HID_KEY_DOWN,
    [3] = HID_KEY_LEFT,
    [4] = HID_KEY_TAB,
    [5] = HID_KEY_ENTER,
    [6] = HID_KEY_ESC,
    [7] = HID_KEY_F1,
    [8] = HID_KEY_Z,
    [9] = HID_KEY_X,
    [10] = HID_KEY_A,
    [11] = HID_KEY_S,
    [12] = HID_KEY_Q,
    [13] = HID_KEY_W,
};

static const uint8_t default_mouse[RG_KEY_COUNT] = {
    [0] = 3, /* up */
    [1] = 2, /* right */
    [2] = 4, /* down */
    [3] = 1, /* left */
    [6] = 7, /* middle button */
    [8] = 5, /* left button */
    [9] = 6, /* right button */
};

static void mapping_key(char *out, size_t out_size, rg_usb_hid_device_t device, int key_index)
{
    snprintf(out, out_size, "USBMap%d_%02d", (int)device, key_index);
}

static uint32_t default_mapping(rg_usb_hid_device_t device, int key_index)
{
    if (device == RG_USB_HID_KEYBOARD && default_keyboard[key_index])
        return ((uint32_t)SOURCE_KEYBOARD << 28) | default_keyboard[key_index];
    if (device == RG_USB_HID_MOUSE && default_mouse[key_index])
    {
        uint8_t value = default_mouse[key_index];
        if (value <= 4)
            return ((uint32_t)SOURCE_MOUSE_MOTION << 28) | (value - 1);
        return ((uint32_t)SOURCE_MOUSE_BUTTON << 28) | (value - 5);
    }
    return 0;
}

static void load_mappings(void)
{
    char key[16];
    for (int device = 0; device < RG_USB_HID_DEVICE_COUNT; ++device)
    {
        for (int i = 0; i < RG_KEY_COUNT; ++i)
        {
            mapping_key(key, sizeof(key), device, i);
            mappings[device][i] = (uint32_t)rg_settings_get_number(
                NS_GLOBAL, key, default_mapping((rg_usb_hid_device_t)device, i));
        }
    }
}

static bool key_in_boot_report(const hid_keyboard_input_report_boot_t *report, uint8_t usage)
{
    if (usage >= 0xE0 && usage <= 0xE7)
        return (report->modifier.val & (1U << (usage - 0xE0))) != 0;
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; ++i)
        if (report->key[i] == usage)
            return true;
    return false;
}

static void finish_capture(uint32_t source)
{
    if (source && capture_device >= 0 && capture_result == 0)
        capture_result = source;
}

static void process_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t))
        return;
    const hid_keyboard_input_report_boot_t *report = (const void *)data;
    bool next[256] = {0};
    for (int usage = 0xE0; usage <= 0xE7; ++usage)
        next[usage] = key_in_boot_report(report, usage);
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; ++i)
        next[report->key[i]] = report->key[i] != 0;

    portENTER_CRITICAL(&hid_lock);
    if (capture_device == RG_USB_HID_KEYBOARD)
        for (int usage = 1; usage < 256; ++usage)
            if (next[usage] && !capture_keyboard_baseline[usage])
            {
                finish_capture(((uint32_t)SOURCE_KEYBOARD << 28) | usage);
                break;
            }
    memcpy(keyboard_keys, next, sizeof(keyboard_keys));
    portEXIT_CRITICAL(&hid_lock);
}

static void process_mouse_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_mouse_input_report_boot_t))
        return;
    const hid_mouse_input_report_boot_t *report = (const void *)data;
    int64_t now = rg_system_timer();

    portENTER_CRITICAL(&hid_lock);
    mouse_buttons = report->buttons.val;
    if (report->x_displacement < -1) mouse_motion_until[0] = now + USB_HID_MOUSE_HOLD_US;
    if (report->x_displacement > 1)  mouse_motion_until[1] = now + USB_HID_MOUSE_HOLD_US;
    if (report->y_displacement < -1) mouse_motion_until[2] = now + USB_HID_MOUSE_HOLD_US;
    if (report->y_displacement > 1)  mouse_motion_until[3] = now + USB_HID_MOUSE_HOLD_US;

    if (capture_device == RG_USB_HID_MOUSE)
    {
        uint8_t pressed = report->buttons.val & ~capture_mouse_baseline;
        if (pressed)
            finish_capture(((uint32_t)SOURCE_MOUSE_BUTTON << 28) | __builtin_ctz(pressed));
        else if (report->x_displacement < -1) finish_capture(((uint32_t)SOURCE_MOUSE_MOTION << 28) | 0);
        else if (report->x_displacement > 1)  finish_capture(((uint32_t)SOURCE_MOUSE_MOTION << 28) | 1);
        else if (report->y_displacement < -1) finish_capture(((uint32_t)SOURCE_MOUSE_MOTION << 28) | 2);
        else if (report->y_displacement > 1)  finish_capture(((uint32_t)SOURCE_MOUSE_MOTION << 28) | 3);
    }
    portEXIT_CRITICAL(&hid_lock);
}

static void process_generic_report(const uint8_t *data, size_t length)
{
    length = RG_MIN(length, USB_HID_REPORT_MAX);
    portENTER_CRITICAL(&hid_lock);
    memcpy(generic_report, data, length);
    generic_report_len = length;

    if (capture_device == RG_USB_HID_GAMEPAD)
    {
        if (!capture_generic_valid)
        {
            memcpy(capture_generic_baseline, data, length);
            capture_generic_len = length;
            capture_generic_valid = true;
        }
        else
        {
            uint32_t candidate = 0;
            size_t count = RG_MIN(length, capture_generic_len);
            int best_axis = -1;
            int best_delta = 0;
            int changed_bits = 0;
            int changed_byte = -1;
            uint8_t changed_mask = 0;
            for (size_t i = 0; i < count; ++i)
            {
                int delta = (int)data[i] - capture_generic_baseline[i];
                int magnitude = abs(delta);
                uint8_t changed = data[i] ^ capture_generic_baseline[i];
                if (changed)
                {
                    changed_bits += __builtin_popcount(changed);
                    changed_byte = i;
                    changed_mask = changed & (uint8_t)(-changed);
                }
                if (magnitude > best_delta)
                {
                    best_delta = magnitude;
                    best_axis = i;
                }
            }

            /* Packed buttons normally rest at 0x00 (active high) or 0xFF
             * (active low). Do not interpret a one-bit change in a centered
             * analog byte as a button; values such as 0x80/0x81 commonly
             * alternate because of stick noise. */
            bool button_byte = changed_byte >= 0 &&
                               (capture_generic_baseline[changed_byte] == 0x00 ||
                                capture_generic_baseline[changed_byte] == 0xFF);
            if (changed_bits == 1 && button_byte)
            {
                uint8_t expected = (data[changed_byte] & changed_mask) != 0;
                candidate = ((uint32_t)SOURCE_GENERIC_BIT << 28) |
                            ((uint32_t)changed_byte << 16) | ((uint32_t)changed_mask << 8) | expected;
            }
            else if (best_axis >= 0 && best_delta >= USB_HID_AXIS_CAPTURE_THRESHOLD)
            {
                candidate = ((uint32_t)SOURCE_GENERIC_AXIS << 28) |
                            ((uint32_t)best_axis << 16) |
                            ((uint32_t)capture_generic_baseline[best_axis] << 8) | data[best_axis];
            }
            if (candidate == capture_generic_candidate)
            {
                if (++capture_generic_candidate_count >= USB_HID_CAPTURE_CONFIRM_REPORTS)
                    finish_capture(candidate);
            }
            else
            {
                capture_generic_candidate = candidate;
                capture_generic_candidate_count = candidate ? 1 : 0;
            }
        }
    }
    portEXIT_CRITICAL(&hid_lock);
}

static void interface_callback(hid_host_device_handle_t handle, hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK)
        return;

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT)
    {
        uint8_t data[USB_HID_REPORT_MAX];
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) != ESP_OK)
            return;
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && params.proto == HID_PROTOCOL_KEYBOARD)
            process_keyboard_report(data, length);
        else if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && params.proto == HID_PROTOCOL_MOUSE)
            process_mouse_report(data, length);
        else
            process_generic_report(data, length);
    }
    else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED)
    {
        uint32_t mask = params.proto == HID_PROTOCOL_KEYBOARD ? (1U << RG_USB_HID_KEYBOARD) :
                        params.proto == HID_PROTOCOL_MOUSE ? (1U << RG_USB_HID_MOUSE) :
                                                            (1U << RG_USB_HID_GAMEPAD);
        portENTER_CRITICAL(&hid_lock);
        connected_mask &= ~mask;
        memset(keyboard_keys, 0, sizeof(keyboard_keys));
        mouse_buttons = 0;
        generic_report_len = 0;
        portEXIT_CRITICAL(&hid_lock);
        hid_host_device_close(handle);
    }
    else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR)
        RG_LOGW("USB HID transfer error");
}

static void driver_callback(hid_host_device_handle_t handle, hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    hid_event_t queued = {.handle = handle, .event = event};
    if (hid_event_queue)
        xQueueSend(hid_event_queue, &queued, 0);
}

static void hid_device_task(void *arg)
{
    (void)arg;
    hid_event_t event;
    while (usb_running)
    {
        if (xQueueReceive(hid_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (event.event != HID_HOST_DRIVER_EVENT_CONNECTED)
            continue;

        hid_host_dev_params_t params;
        if (hid_host_device_get_params(event.handle, &params) != ESP_OK)
            continue;
        const hid_host_device_config_t config = {
            .callback = interface_callback,
            .callback_arg = NULL,
        };
        if (hid_host_device_open(event.handle, &config) != ESP_OK)
            continue;
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
        {
            hid_class_request_set_protocol(event.handle, HID_REPORT_PROTOCOL_BOOT);
            if (params.proto == HID_PROTOCOL_KEYBOARD)
                hid_class_request_set_idle(event.handle, 0, 0);
        }
        if (hid_host_device_start(event.handle) != ESP_OK)
        {
            hid_host_device_close(event.handle);
            continue;
        }
        uint32_t mask = params.proto == HID_PROTOCOL_KEYBOARD ? (1U << RG_USB_HID_KEYBOARD) :
                        params.proto == HID_PROTOCOL_MOUSE ? (1U << RG_USB_HID_MOUSE) :
                                                            (1U << RG_USB_HID_GAMEPAD);
        portENTER_CRITICAL(&hid_lock);
        connected_mask |= mask;
        portEXIT_CRITICAL(&hid_lock);
        RG_LOGI("USB HID interface connected (protocol=%d, interface=%d)", params.proto, params.iface_num);
    }
    vTaskDelete(NULL);
}

static void usb_event_task(void *arg)
{
    (void)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&config);
    if (err != ESP_OK)
    {
        RG_LOGE("USB host install failed: %s", esp_err_to_name(err));
        usb_ready = true;
        vTaskDelete(NULL);
        return;
    }
    usb_ready = true;
    while (usb_running)
    {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    }
    vTaskDelete(NULL);
}

void rg_usb_hid_init(void)
{
    if (usb_running)
        return;
    hid_event_queue = xQueueCreate(8, sizeof(hid_event_t));
    if (!hid_event_queue)
    {
        RG_LOGE("Unable to allocate USB HID event queue");
        return;
    }

    usb_running = true;
    usb_ready = false;
    xTaskCreatePinnedToCore(usb_event_task, "usb_events", 4096, NULL, 2, NULL, 0);
    while (!usb_ready)
        rg_task_delay(10);

    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_install(&config);
    if (err != ESP_OK)
    {
        RG_LOGE("USB HID host install failed: %s", esp_err_to_name(err));
        usb_running = false;
        return;
    }
    xTaskCreatePinnedToCore(hid_device_task, "usb_hid", 4096, NULL, 4, NULL, 0);
    RG_LOGI("USB HID host ready (input %s)", hid_enabled ? "enabled" : "disabled");
}

void rg_usb_hid_deinit(void)
{
    hid_enabled = false;
}

void rg_usb_hid_load_settings(void)
{
    load_mappings();
    hid_enabled = rg_settings_get_boolean(NS_GLOBAL, USB_HID_SETTING_ENABLE, false);
    RG_LOGI("USB HID settings loaded (input %s)", hid_enabled ? "enabled" : "disabled");
}

static bool source_active(uint32_t source, int64_t now)
{
    uint8_t type = SOURCE_TYPE(source);
    if (type == SOURCE_KEYBOARD)
        return keyboard_keys[source & 0xFF];
    if (type == SOURCE_MOUSE_BUTTON)
        return (mouse_buttons & (1U << (source & 7))) != 0;
    if (type == SOURCE_MOUSE_MOTION)
        return (source & 3) < 4 && mouse_motion_until[source & 3] > now;
    if (type == SOURCE_GENERIC_BIT)
    {
        uint8_t index = (source >> 16) & 0xFF;
        uint8_t mask = (source >> 8) & 0xFF;
        bool expected = source & 1;
        return index < generic_report_len && ((generic_report[index] & mask) != 0) == expected;
    }
    if (type == SOURCE_GENERIC_AXIS)
    {
        uint8_t index = (source >> 16) & 0xFF;
        uint8_t neutral = (source >> 8) & 0xFF;
        uint8_t target = source & 0xFF;
        if (index >= generic_report_len)
            return false;
        int target_delta = (int)target - (int)neutral;
        int value_delta = (int)generic_report[index] - (int)neutral;
        int threshold = RG_MAX(USB_HID_AXIS_DEADZONE, abs(target_delta) / 2);
        return target_delta < 0 ? value_delta < -threshold : value_delta > threshold;
    }
    return false;
}

uint32_t rg_usb_hid_get_gamepad_state(void)
{
    if (!hid_enabled)
        return 0;
    uint32_t state = 0;
    int64_t now = rg_system_timer();
    portENTER_CRITICAL(&hid_lock);
    for (int device = 0; device < RG_USB_HID_DEVICE_COUNT; ++device)
        for (int i = 0; i < RG_KEY_COUNT; ++i)
            if (source_active(mappings[device][i], now))
                state |= 1U << i;
    portEXIT_CRITICAL(&hid_lock);
    return state;
}

bool rg_usb_hid_get_enabled(void)
{
    return hid_enabled;
}

void rg_usb_hid_set_enabled(bool enabled)
{
    hid_enabled = enabled;
    rg_settings_set_boolean(NS_GLOBAL, USB_HID_SETTING_ENABLE, enabled);
    rg_settings_commit();
}

uint32_t rg_usb_hid_get_connected(void)
{
    return connected_mask;
}

uint32_t rg_usb_hid_get_mapping(rg_usb_hid_device_t device, int key_index)
{
    if (device < 0 || device >= RG_USB_HID_DEVICE_COUNT || key_index < 0 || key_index >= RG_KEY_COUNT)
        return 0;
    return mappings[device][key_index];
}

void rg_usb_hid_set_mapping(rg_usb_hid_device_t device, int key_index, uint32_t source)
{
    if (device < 0 || device >= RG_USB_HID_DEVICE_COUNT || key_index < 0 || key_index >= RG_KEY_COUNT)
        return;
    mappings[device][key_index] = source;
    char key[16];
    mapping_key(key, sizeof(key), device, key_index);
    rg_settings_set_number(NS_GLOBAL, key, source);
    rg_settings_commit();
}

void rg_usb_hid_reset_mappings(rg_usb_hid_device_t device)
{
    if (device < 0 || device >= RG_USB_HID_DEVICE_COUNT)
        return;
    for (int i = 0; i < RG_KEY_COUNT; ++i)
        rg_usb_hid_set_mapping(device, i, default_mapping(device, i));
}

bool rg_usb_hid_capture_source(rg_usb_hid_device_t device, uint32_t *source, int timeout_ms)
{
    if (device < 0 || device >= RG_USB_HID_DEVICE_COUNT || !source)
        return false;
    portENTER_CRITICAL(&hid_lock);
    capture_result = 0;
    capture_device = device;
    memcpy(capture_keyboard_baseline, keyboard_keys, sizeof(keyboard_keys));
    capture_mouse_baseline = mouse_buttons;
    memcpy(capture_generic_baseline, generic_report, generic_report_len);
    capture_generic_len = generic_report_len;
    capture_generic_valid = generic_report_len > 0;
    capture_generic_candidate = 0;
    capture_generic_candidate_count = 0;
    portEXIT_CRITICAL(&hid_lock);

    int64_t deadline = rg_system_timer() + (int64_t)timeout_ms * 1000;
    while (!capture_result && rg_system_timer() < deadline)
        rg_task_delay(10);

    portENTER_CRITICAL(&hid_lock);
    *source = capture_result;
    capture_device = -1;
    capture_result = 0;
    portEXIT_CRITICAL(&hid_lock);
    return *source != 0;
}

void rg_usb_hid_source_name(rg_usb_hid_device_t device, uint32_t source, char *out, size_t out_size)
{
    (void)device;
    if (!source)
    {
        snprintf(out, out_size, "None");
        return;
    }
    uint8_t type = SOURCE_TYPE(source);
    if (type == SOURCE_KEYBOARD)
    {
        uint8_t usage = source & 0xFF;
        const char *name = NULL;
        switch (usage)
        {
        case HID_KEY_UP: name = "Up arrow"; break;
        case HID_KEY_RIGHT: name = "Right arrow"; break;
        case HID_KEY_DOWN: name = "Down arrow"; break;
        case HID_KEY_LEFT: name = "Left arrow"; break;
        case HID_KEY_ENTER: name = "Enter"; break;
        case HID_KEY_ESC: name = "Escape"; break;
        case HID_KEY_TAB: name = "Tab"; break;
        case HID_KEY_SPACE: name = "Space"; break;
        case HID_KEY_F1: name = "F1"; break;
        default: break;
        }
        if (name) snprintf(out, out_size, "%s", name);
        else if (usage >= HID_KEY_A && usage <= HID_KEY_Z) snprintf(out, out_size, "%c", 'A' + usage - HID_KEY_A);
        else snprintf(out, out_size, "Key 0x%02X", usage);
    }
    else if (type == SOURCE_MOUSE_BUTTON)
        snprintf(out, out_size, "Mouse button %u", (unsigned)(source & 7) + 1);
    else if (type == SOURCE_MOUSE_MOTION)
    {
        static const char *names[] = {"Mouse left", "Mouse right", "Mouse up", "Mouse down"};
        snprintf(out, out_size, "%s", names[source & 3]);
    }
    else if (type == SOURCE_GENERIC_BIT)
        snprintf(out, out_size, "Byte %u bit %02X=%u", (unsigned)((source >> 16) & 0xFF),
                 (unsigned)((source >> 8) & 0xFF), (unsigned)(source & 1));
    else if (type == SOURCE_GENERIC_AXIS)
    {
        uint8_t neutral = (source >> 8) & 0xFF;
        uint8_t target = source & 0xFF;
        snprintf(out, out_size, "Axis %u %s", (unsigned)((source >> 16) & 0xFF),
                 (int)target - (int)neutral < 0 ? "negative" : "positive");
    }
    else
        snprintf(out, out_size, "Unknown");
}

#else

void rg_usb_hid_init(void) {}
void rg_usb_hid_deinit(void) {}
void rg_usb_hid_load_settings(void) {}
uint32_t rg_usb_hid_get_gamepad_state(void) { return 0; }
bool rg_usb_hid_get_enabled(void) { return false; }
void rg_usb_hid_set_enabled(bool enabled) { (void)enabled; }
uint32_t rg_usb_hid_get_connected(void) { return 0; }
uint32_t rg_usb_hid_get_mapping(rg_usb_hid_device_t device, int key_index) { (void)device; (void)key_index; return 0; }
void rg_usb_hid_set_mapping(rg_usb_hid_device_t device, int key_index, uint32_t source) { (void)device; (void)key_index; (void)source; }
void rg_usb_hid_reset_mappings(rg_usb_hid_device_t device) { (void)device; }
bool rg_usb_hid_capture_source(rg_usb_hid_device_t device, uint32_t *source, int timeout_ms) { (void)device; (void)source; (void)timeout_ms; return false; }
void rg_usb_hid_source_name(rg_usb_hid_device_t device, uint32_t source, char *out, size_t out_size) { (void)device; (void)source; if (out_size) *out = 0; }

#endif
