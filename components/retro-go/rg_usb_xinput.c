#include "rg_system.h"
#include "rg_usb_xinput.h"
#include "rg_usb_host.h"

#if defined(ESP_PLATFORM) && defined(RG_ENABLE_USB_XINPUT)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define XINPUT_SETTING_ENABLE "USBXInputEnabled"
#define XINPUT_REPORT_MAX 20
#define XINPUT_AXIS_CAPTURE_THRESHOLD 24
#define XINPUT_AXIS_DEADZONE 16
#define XINPUT_CAPTURE_CONFIRM_REPORTS 3

// Xbox One/Series controllers expose a vendor-specific "GIP" interface, not a
// standard HID one. Class/subclass/protocol and the power-on handshake below
// are from the public GIP protocol (as used by the Linux xpad/xone drivers),
// not from Microsoft documentation, and were not verified against real
// hardware in this change.
#define XINPUT_IFACE_CLASS    0xFF
#define XINPUT_IFACE_SUBCLASS 0x47
#define XINPUT_IFACE_PROTOCOL 0xD0
#define GIP_CMD_POWER 0x05
#define GIP_CMD_INPUT 0x20

#define SOURCE_TYPE(v) ((uint8_t)((v) >> 28))
#define SOURCE_BUTTON 1
#define SOURCE_AXIS 2

typedef struct
{
    usb_device_handle_t dev_hdl;
    usb_transfer_t *in_xfer;
    usb_transfer_t *out_xfer;
    uint8_t iface_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t seq;
    bool connected;
} xinput_device_t;

static usb_host_client_handle_t client_handle;
static xinput_device_t devs[RG_USB_XINPUT_MAX_DEVICES];
static volatile bool task_running;
static volatile bool xinput_enabled;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t mapping[RG_KEY_COUNT];
static uint16_t buttons[RG_USB_XINPUT_MAX_DEVICES];
static uint8_t report[RG_USB_XINPUT_MAX_DEVICES][XINPUT_REPORT_MAX];
static size_t report_len[RG_USB_XINPUT_MAX_DEVICES];

static volatile bool capture_active;
static volatile uint32_t capture_result;
static bool capture_valid[RG_USB_XINPUT_MAX_DEVICES];
static uint16_t capture_baseline_buttons[RG_USB_XINPUT_MAX_DEVICES];
static uint8_t capture_baseline_report[RG_USB_XINPUT_MAX_DEVICES][XINPUT_REPORT_MAX];
static size_t capture_baseline_len[RG_USB_XINPUT_MAX_DEVICES];
static uint32_t capture_candidate[RG_USB_XINPUT_MAX_DEVICES];
static uint8_t capture_candidate_count[RG_USB_XINPUT_MAX_DEVICES];

// Bit index within the GIP button mask (see process_input_report) for each RG_KEY_*,
// in RG_KEY_UP..RG_KEY_R order. -1 means "no default, map it manually".
static const int8_t default_button_bits[RG_KEY_COUNT] = {
    8, 11, 9, 10, /* UP RIGHT DOWN LEFT -> D-pad */
    3, 2,         /* SELECT -> View, START -> Menu */
    -1, -1,       /* MENU, OPTION: unmapped by default */
    4, 5, 6, 7,   /* A B X Y */
    12, 13,       /* L -> LB, R -> RB */
};

static uint32_t default_mapping(int key_index)
{
    int bit = default_button_bits[key_index];
    if (bit < 0)
        return 0;
    return ((uint32_t)SOURCE_BUTTON << 28) | (uint32_t)bit;
}

static void load_mappings(void)
{
    char key[24];
    for (int i = 0; i < RG_KEY_COUNT; ++i)
    {
        snprintf(key, sizeof(key), "XInputMap_%02d", i);
        mapping[i] = (uint32_t)rg_settings_get_number(NS_GLOBAL, key, default_mapping(i));
    }
}

static void finish_capture(uint32_t source)
{
    if (source && capture_active && capture_result == 0)
        capture_result = source;
}

static void process_input_report(int instance, const uint8_t *data, size_t length)
{
    if (length < 4 || data[0] != GIP_CMD_INPUT)
        return;
    const uint8_t *payload = data + 4;
    size_t payload_len = RG_MIN(length - 4, XINPUT_REPORT_MAX);
    if (payload_len < 2)
        return;

    portENTER_CRITICAL(&lock);
    memcpy(report[instance], payload, payload_len);
    report_len[instance] = payload_len;
    buttons[instance] = payload[0] | ((uint16_t)payload[1] << 8);

    if (capture_active)
    {
        if (!capture_valid[instance])
        {
            capture_baseline_buttons[instance] = buttons[instance];
            memcpy(capture_baseline_report[instance], payload, payload_len);
            capture_baseline_len[instance] = payload_len;
            capture_valid[instance] = true;
        }
        else
        {
            uint16_t pressed = buttons[instance] & (uint16_t)~capture_baseline_buttons[instance];
            uint32_t candidate = 0;
            if (pressed)
            {
                candidate = ((uint32_t)SOURCE_BUTTON << 28) | (uint32_t)__builtin_ctz(pressed);
            }
            else
            {
                size_t count = RG_MIN(payload_len, capture_baseline_len[instance]);
                int best_axis = -1, best_delta = 0;
                for (size_t i = 2; i < count; ++i)
                {
                    int delta = (int)payload[i] - (int)capture_baseline_report[instance][i];
                    int magnitude = abs(delta);
                    if (magnitude > best_delta)
                    {
                        best_delta = magnitude;
                        best_axis = (int)i;
                    }
                }
                if (best_axis >= 0 && best_delta >= XINPUT_AXIS_CAPTURE_THRESHOLD)
                    candidate = ((uint32_t)SOURCE_AXIS << 28) | ((uint32_t)best_axis << 16) |
                                ((uint32_t)capture_baseline_report[instance][best_axis] << 8) | payload[best_axis];
            }
            if (candidate == capture_candidate[instance])
            {
                if (++capture_candidate_count[instance] >= XINPUT_CAPTURE_CONFIRM_REPORTS)
                    finish_capture(candidate);
            }
            else
            {
                capture_candidate[instance] = candidate;
                capture_candidate_count[instance] = candidate ? 1 : 0;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
}

static void in_xfer_done(usb_transfer_t *xfer)
{
    int instance = (int)(intptr_t)xfer->context;
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED)
    {
        process_input_report(instance, xfer->data_buffer, (size_t)xfer->actual_num_bytes);
        usb_host_transfer_submit(xfer);
        return;
    }
    if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE || xfer->status == USB_TRANSFER_STATUS_CANCELED)
        return;
    RG_LOGE("Xbox controller IN transfer failed, status=%d", xfer->status);
    usb_host_transfer_submit(xfer);
}

static void out_xfer_done(usb_transfer_t *xfer)
{
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
        RG_LOGW("Xbox controller command failed, status=%d", xfer->status);
}

static void send_power_on(int instance)
{
    xinput_device_t *dev = &devs[instance];
    uint8_t packet[] = {GIP_CMD_POWER, 0x20, dev->seq++, 0x01, 0x00};
    usb_transfer_t *xfer = dev->out_xfer;
    memcpy(xfer->data_buffer, packet, sizeof(packet));
    xfer->device_handle = dev->dev_hdl;
    xfer->bEndpointAddress = dev->ep_out;
    xfer->num_bytes = sizeof(packet);
    xfer->callback = out_xfer_done;
    xfer->context = (void *)(intptr_t)instance;
    usb_host_transfer_submit(xfer);
}

static const usb_intf_desc_t *next_interface(const void *cur, size_t total_len, int *offset)
{
    return (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
        (const usb_standard_desc_t *)cur, (uint16_t)total_len, USB_B_DESCRIPTOR_TYPE_INTERFACE, offset);
}

static const usb_intf_desc_t *find_xinput_interface(const usb_config_desc_t *config_desc)
{
    int offset = 0;
    const usb_intf_desc_t *iface = next_interface(config_desc, config_desc->wTotalLength, &offset);
    while (iface)
    {
        if (iface->bInterfaceClass == XINPUT_IFACE_CLASS && iface->bInterfaceSubClass == XINPUT_IFACE_SUBCLASS &&
            iface->bInterfaceProtocol == XINPUT_IFACE_PROTOCOL)
            return iface;
        iface = next_interface(iface, config_desc->wTotalLength, &offset);
    }
    return NULL;
}

static void try_open_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl;
    if (usb_host_device_open(client_handle, dev_addr, &dev_hdl) != ESP_OK)
        return;

    const usb_config_desc_t *config_desc = NULL;
    const usb_intf_desc_t *xinput_iface = NULL;
    if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) == ESP_OK)
        xinput_iface = find_xinput_interface(config_desc);

    int instance = -1;
    for (int i = 0; i < RG_USB_XINPUT_MAX_DEVICES; ++i)
    {
        if (!devs[i].connected)
        {
            instance = i;
            break;
        }
    }

    if (!xinput_iface || instance < 0)
    {
        usb_host_device_close(client_handle, dev_hdl);
        return;
    }

    uint8_t ep_in = 0, ep_out = 0;
    uint16_t ep_in_mps = 0;
    for (int i = 0; i < xinput_iface->bNumEndpoints; ++i)
    {
        int ep_offset = 0;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(xinput_iface, i, config_desc->wTotalLength, &ep_offset);
        if (!ep)
            continue;
        if (USB_EP_DESC_GET_EP_DIR(ep))
        {
            ep_in = ep->bEndpointAddress;
            ep_in_mps = USB_EP_DESC_GET_MPS(ep);
        }
        else
        {
            ep_out = ep->bEndpointAddress;
        }
    }

    if (!ep_in || !ep_out ||
        usb_host_interface_claim(client_handle, dev_hdl, xinput_iface->bInterfaceNumber, 0) != ESP_OK)
    {
        usb_host_device_close(client_handle, dev_hdl);
        return;
    }

    usb_transfer_t *in_xfer = NULL, *out_xfer = NULL;
    if (usb_host_transfer_alloc(RG_MAX(ep_in_mps, 32), 0, &in_xfer) != ESP_OK ||
        usb_host_transfer_alloc(32, 0, &out_xfer) != ESP_OK)
    {
        if (in_xfer)
            usb_host_transfer_free(in_xfer);
        if (out_xfer)
            usb_host_transfer_free(out_xfer);
        usb_host_interface_release(client_handle, dev_hdl, xinput_iface->bInterfaceNumber);
        usb_host_device_close(client_handle, dev_hdl);
        return;
    }

    xinput_device_t *dev = &devs[instance];
    portENTER_CRITICAL(&lock);
    dev->dev_hdl = dev_hdl;
    dev->iface_num = xinput_iface->bInterfaceNumber;
    dev->ep_in = ep_in;
    dev->ep_out = ep_out;
    dev->in_xfer = in_xfer;
    dev->out_xfer = out_xfer;
    dev->seq = 0;
    dev->connected = true;
    buttons[instance] = 0;
    report_len[instance] = 0;
    capture_valid[instance] = false;
    portEXIT_CRITICAL(&lock);

    in_xfer->device_handle = dev_hdl;
    in_xfer->bEndpointAddress = ep_in;
    in_xfer->callback = in_xfer_done;
    in_xfer->context = (void *)(intptr_t)instance;
    in_xfer->num_bytes = (int)in_xfer->data_buffer_size;
    usb_host_transfer_submit(in_xfer);

    send_power_on(instance);

    RG_LOGI("Xbox controller connected (addr=%d, iface=%d, instance=%d)", dev_addr, dev->iface_num, instance);
}

static void handle_device_gone(usb_device_handle_t dev_hdl)
{
    int instance = -1;
    for (int i = 0; i < RG_USB_XINPUT_MAX_DEVICES; ++i)
    {
        if (devs[i].connected && devs[i].dev_hdl == dev_hdl)
        {
            instance = i;
            break;
        }
    }
    if (instance < 0)
        return;

    xinput_device_t *dev = &devs[instance];

    portENTER_CRITICAL(&lock);
    buttons[instance] = 0;
    report_len[instance] = 0;
    portEXIT_CRITICAL(&lock);

    usb_host_transfer_free(dev->in_xfer);
    usb_host_transfer_free(dev->out_xfer);
    usb_host_interface_release(client_handle, dev->dev_hdl, dev->iface_num);
    usb_host_device_close(client_handle, dev->dev_hdl);
    memset(dev, 0, sizeof(*dev));
    RG_LOGI("Xbox controller disconnected (instance=%d)", instance);
}

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        try_open_device(event->new_dev.address);
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
        handle_device_gone(event->dev_gone.dev_hdl);
}

static void xinput_task(void *arg)
{
    (void)arg;
    while (task_running)
        usb_host_client_handle_events(client_handle, pdMS_TO_TICKS(100));
    vTaskDelete(NULL);
}

void rg_usb_xinput_init(void)
{
    if (task_running)
        return;

    if (!rg_usb_host_acquire())
    {
        RG_LOGE("USB host library unavailable");
        return;
    }

    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 10,
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = NULL,
    };
    if (usb_host_client_register(&config, &client_handle) != ESP_OK)
    {
        RG_LOGE("Unable to register Xbox controller USB client");
        rg_usb_host_release();
        return;
    }

    task_running = true;
    xTaskCreatePinnedToCore(xinput_task, "usb_xinput", 4096, NULL,
                            RG_TASK_PRIORITY_7, NULL, RG_TASK_AFFINITY_IO);
    RG_LOGI("USB Xbox controller host ready (input %s)", xinput_enabled ? "enabled" : "disabled");
}

void rg_usb_xinput_deinit(void)
{
    xinput_enabled = false;
}

void rg_usb_xinput_load_settings(void)
{
    load_mappings();
    xinput_enabled = rg_settings_get_boolean(NS_GLOBAL, XINPUT_SETTING_ENABLE, false);
    RG_LOGI("USB Xbox controller settings loaded (input %s)", xinput_enabled ? "enabled" : "disabled");
}

static bool source_active(int instance, uint32_t source)
{
    uint8_t type = SOURCE_TYPE(source);
    if (type == SOURCE_BUTTON)
        return (buttons[instance] & (1U << (source & 0xF))) != 0;
    if (type == SOURCE_AXIS)
    {
        uint8_t index = (source >> 16) & 0xFF;
        uint8_t neutral = (source >> 8) & 0xFF;
        uint8_t target = source & 0xFF;
        if (index >= report_len[instance])
            return false;
        int target_delta = (int)target - (int)neutral;
        int value_delta = (int)report[instance][index] - (int)neutral;
        int threshold = RG_MAX(XINPUT_AXIS_DEADZONE, abs(target_delta) / 2);
        return target_delta < 0 ? value_delta < -threshold : value_delta > threshold;
    }
    return false;
}

uint32_t rg_usb_xinput_get_gamepad_state(int instance)
{
    if (!xinput_enabled || instance < 0 || instance >= RG_USB_XINPUT_MAX_DEVICES)
        return 0;
    uint32_t state = 0;
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < RG_KEY_COUNT; ++i)
        if (source_active(instance, mapping[i]))
            state |= 1U << i;
    portEXIT_CRITICAL(&lock);
    return state;
}

bool rg_usb_xinput_get_enabled(void)
{
    return xinput_enabled;
}

void rg_usb_xinput_set_enabled(bool enabled)
{
    xinput_enabled = enabled;
    rg_settings_set_boolean(NS_GLOBAL, XINPUT_SETTING_ENABLE, enabled);
    rg_settings_commit();
}

bool rg_usb_xinput_get_connected(int instance)
{
    if (instance < 0 || instance >= RG_USB_XINPUT_MAX_DEVICES)
        return false;
    return devs[instance].connected;
}

uint32_t rg_usb_xinput_get_mapping(int key_index)
{
    if (key_index < 0 || key_index >= RG_KEY_COUNT)
        return 0;
    return mapping[key_index];
}

void rg_usb_xinput_set_mapping(int key_index, uint32_t source)
{
    if (key_index < 0 || key_index >= RG_KEY_COUNT)
        return;
    mapping[key_index] = source;
    char key[24];
    snprintf(key, sizeof(key), "XInputMap_%02d", key_index);
    rg_settings_set_number(NS_GLOBAL, key, source);
    rg_settings_commit();
}

void rg_usb_xinput_reset_mappings(void)
{
    for (int i = 0; i < RG_KEY_COUNT; ++i)
        rg_usb_xinput_set_mapping(i, default_mapping(i));
}

bool rg_usb_xinput_capture_source(uint32_t *source, int timeout_ms)
{
    if (!source)
        return false;
    portENTER_CRITICAL(&lock);
    capture_result = 0;
    for (int i = 0; i < RG_USB_XINPUT_MAX_DEVICES; ++i)
    {
        capture_valid[i] = report_len[i] > 0;
        capture_baseline_buttons[i] = buttons[i];
        memcpy(capture_baseline_report[i], report[i], report_len[i]);
        capture_baseline_len[i] = report_len[i];
        capture_candidate[i] = 0;
        capture_candidate_count[i] = 0;
    }
    capture_active = true;
    portEXIT_CRITICAL(&lock);

    int64_t deadline = rg_system_timer() + (int64_t)timeout_ms * 1000;
    while (!capture_result && rg_system_timer() < deadline)
        rg_task_delay(10);

    portENTER_CRITICAL(&lock);
    *source = capture_result;
    capture_active = false;
    capture_result = 0;
    portEXIT_CRITICAL(&lock);
    return *source != 0;
}

void rg_usb_xinput_source_name(uint32_t source, char *out, size_t out_size)
{
    if (!source)
    {
        snprintf(out, out_size, "None");
        return;
    }
    uint8_t type = SOURCE_TYPE(source);
    if (type == SOURCE_BUTTON)
    {
        static const char *names[16] = {
            "Sync", "Button 1", "Menu", "View", "A", "B", "X", "Y",
            "D-pad up", "D-pad down", "D-pad left", "D-pad right",
            "LB", "RB", "Left stick click", "Right stick click",
        };
        snprintf(out, out_size, "%s", names[source & 0xF]);
    }
    else if (type == SOURCE_AXIS)
    {
        uint8_t index = (source >> 16) & 0xFF;
        uint8_t neutral = (source >> 8) & 0xFF;
        uint8_t target = source & 0xFF;
        snprintf(out, out_size, "Axis byte %u %s", (unsigned)index,
                 (int)target - (int)neutral < 0 ? "negative" : "positive");
    }
    else
        snprintf(out, out_size, "Unknown");
}

#else

void rg_usb_xinput_init(void) {}
void rg_usb_xinput_deinit(void) {}
void rg_usb_xinput_load_settings(void) {}
uint32_t rg_usb_xinput_get_gamepad_state(int instance) { (void)instance; return 0; }
bool rg_usb_xinput_get_enabled(void) { return false; }
void rg_usb_xinput_set_enabled(bool enabled) { (void)enabled; }
bool rg_usb_xinput_get_connected(int instance) { (void)instance; return false; }
uint32_t rg_usb_xinput_get_mapping(int key_index) { (void)key_index; return 0; }
void rg_usb_xinput_set_mapping(int key_index, uint32_t source) { (void)key_index; (void)source; }
void rg_usb_xinput_reset_mappings(void) {}
bool rg_usb_xinput_capture_source(uint32_t *source, int timeout_ms) { (void)source; (void)timeout_ms; return false; }
void rg_usb_xinput_source_name(uint32_t source, char *out, size_t out_size) { (void)source; if (out_size) *out = 0; }

#endif
