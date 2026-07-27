#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void rg_usb_xinput_init(void);
void rg_usb_xinput_deinit(void);
void rg_usb_xinput_load_settings(void);
uint32_t rg_usb_xinput_get_gamepad_state(void);

bool rg_usb_xinput_get_enabled(void);
void rg_usb_xinput_set_enabled(bool enabled);
bool rg_usb_xinput_get_connected(void);

uint32_t rg_usb_xinput_get_mapping(int key_index);
void rg_usb_xinput_set_mapping(int key_index, uint32_t source);
void rg_usb_xinput_reset_mappings(void);
bool rg_usb_xinput_capture_source(uint32_t *source, int timeout_ms);
void rg_usb_xinput_source_name(uint32_t source, char *out, size_t out_size);
