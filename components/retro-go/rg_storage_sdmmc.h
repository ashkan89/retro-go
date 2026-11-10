#pragma once

// Internal configuration shared by the filesystem and exclusive USB MSC mode.
#include <driver/sdmmc_host.h>

#ifndef RG_STORAGE_SDMMC_WIDTH
#define RG_STORAGE_SDMMC_WIDTH 1
#endif
#if RG_STORAGE_SDMMC_WIDTH != 1 && RG_STORAGE_SDMMC_WIDTH != 4
#error "SD card bus width must be 1 or 4"
#endif

static inline void rg_storage_configure_sdmmc(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    // Preserve DEINIT_ARG and the slot-scoped deinit callback from IDF. Tearing
    // down the whole controller would also disconnect the SDIO Wi-Fi companion.
    host->flags &= ~(SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DDR);
    host->flags |= SDMMC_HOST_FLAG_1BIT;
#if RG_STORAGE_SDMMC_WIDTH == 4
    host->flags |= SDMMC_HOST_FLAG_4BIT;
#endif
    host->slot = RG_STORAGE_SDMMC_HOST;
    host->max_freq_khz = RG_STORAGE_SDMMC_SPEED;

    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = RG_STORAGE_SDMMC_WIDTH;
#if SOC_SDMMC_USE_GPIO_MATRIX
#ifdef RG_GPIO_SDMMC_CLK
    slot->clk = RG_GPIO_SDMMC_CLK;
    slot->cmd = RG_GPIO_SDMMC_CMD;
    slot->d0 = RG_GPIO_SDMMC_D0;
#if RG_STORAGE_SDMMC_WIDTH == 4
    slot->d1 = RG_GPIO_SDMMC_D1;
    slot->d2 = RG_GPIO_SDMMC_D2;
    slot->d3 = RG_GPIO_SDMMC_D3;
#else
    slot->d1 = slot->d2 = slot->d3 = GPIO_NUM_NC;
#endif
    slot->d4 = slot->d5 = slot->d6 = slot->d7 = GPIO_NUM_NC;
#elif defined(RG_GPIO_SDSPI_CMD)
    // Compatibility with older 1-bit targets using the original pin names.
    slot->clk = RG_GPIO_SDSPI_CLK;
    slot->cmd = RG_GPIO_SDSPI_CMD;
    slot->d0 = RG_GPIO_SDSPI_D0;
    slot->d1 = slot->d2 = slot->d3 = GPIO_NUM_NC;
#endif
#endif
}
