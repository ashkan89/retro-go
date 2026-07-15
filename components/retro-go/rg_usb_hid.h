#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    RG_USB_HID_GAMEPAD = 0,
    RG_USB_HID_KEYBOARD,
    RG_USB_HID_MOUSE,
    RG_USB_HID_DEVICE_COUNT,
} rg_usb_hid_device_t;

void rg_usb_hid_init(void);
void rg_usb_hid_deinit(void);
uint32_t rg_usb_hid_get_gamepad_state(void);

bool rg_usb_hid_get_enabled(void);
void rg_usb_hid_set_enabled(bool enabled);
uint32_t rg_usb_hid_get_connected(void);

uint32_t rg_usb_hid_get_mapping(rg_usb_hid_device_t device, int key_index);
void rg_usb_hid_set_mapping(rg_usb_hid_device_t device, int key_index, uint32_t source);
void rg_usb_hid_reset_mappings(rg_usb_hid_device_t device);
bool rg_usb_hid_capture_source(rg_usb_hid_device_t device, uint32_t *source, int timeout_ms);
void rg_usb_hid_source_name(rg_usb_hid_device_t device, uint32_t source, char *out, size_t out_size);

