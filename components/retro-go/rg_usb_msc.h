#pragma once

#include <stdbool.h>

void rg_usb_msc_request(void) __attribute__((noreturn));
bool rg_usb_msc_boot_requested(void);
void rg_usb_msc_run(void) __attribute__((noreturn));

