#pragma once

#include <stdbool.h>
#include <stdint.h>

enum
{
    RG_FIRMWARE_UPDATE_FACTORY         = (1 << 0),
    RG_FIRMWARE_UPDATE_APPS            = (1 << 1),
    RG_FIRMWARE_UPDATE_PARTITION_TABLE = (1 << 2),
    RG_FIRMWARE_UPDATE_BOOTLOADER      = (1 << 3),
    RG_FIRMWARE_REQUIRE_FACTORY        = (1 << 5),
    RG_FIRMWARE_REQUIRE_LAUNCHER       = (1 << 6),
};

bool rg_firmware_install_image(const char *path, uint32_t flags);
