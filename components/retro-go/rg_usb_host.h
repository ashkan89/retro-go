#pragma once

#include <stdbool.h>

// Shared owner of the ESP-IDF USB host library (usb_host_install/usb_host_lib_handle_events).
// Multiple USB host class-driver clients (HID, XInput, ...) coexist on the single USB-OTG
// peripheral by acquiring/releasing a reference here instead of each calling usb_host_install()
// themselves, which would fail the second time it's called.
bool rg_usb_host_acquire(void);
void rg_usb_host_release(void);
