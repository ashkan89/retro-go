#include "rg_system.h"
#include "rg_usb_msc.h"

#if defined(ESP_PLATFORM) && defined(RG_ENABLE_USB_MSC) && defined(RG_STORAGE_SDSPI_HOST)

#include <stdlib.h>

#include <driver/sdspi_host.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <sdmmc_cmd.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>

#define USB_MSC_BOOT_MAGIC 0x4D534355U

RTC_NOINIT_ATTR static uint32_t usb_msc_boot_magic;
static volatile bool usb_attached;
static bool usb_msc_mode;

static void usb_device_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED)
        usb_attached = true;
    else if (event->id == TINYUSB_EVENT_DETACHED)
        usb_attached = false;
}

bool rg_usb_msc_boot_requested(void)
{
    if (usb_msc_mode)
        return true;
    if (usb_msc_boot_magic != USB_MSC_BOOT_MAGIC)
        return false;
    usb_msc_boot_magic = 0;
    usb_msc_mode = true;
    return true;
}

void rg_usb_msc_request(void)
{
    usb_msc_boot_magic = USB_MSC_BOOT_MAGIC;
    esp_restart();
}

static esp_err_t init_raw_sdcard(sdmmc_card_t **out_card, int *out_slot)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_GPIO_SDSPI_MOSI,
        .miso_io_num = RG_GPIO_SDSPI_MISO,
        .sclk_io_num = RG_GPIO_SDSPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(RG_STORAGE_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        return err;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = RG_STORAGE_SDSPI_HOST;
    slot_cfg.gpio_cs = RG_GPIO_SDSPI_CS;
    err = sdspi_host_init_device(&slot_cfg, out_slot);
    if (err != ESP_OK)
    {
        spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return err;
    }

    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (!card)
    {
        sdspi_host_remove_device(*out_slot);
        spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return ESP_ERR_NO_MEM;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = *out_slot;
    host.max_freq_khz = RG_STORAGE_SDSPI_SPEED;
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK)
    {
        free(card);
        sdspi_host_remove_device(*out_slot);
        spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return err;
    }

    *out_card = card;
    return ESP_OK;
}

static void draw_status(const char *error)
{
    if (error)
        rg_gui_draw_message("USB SD card\n\n%s\n\nPress MENU to restart.", error);
    else
        rg_gui_draw_message("USB SD card\n\n%s\nManage files in Explorer.\nSafely eject and unplug USB,\nthen press MENU to restart.",
                            usb_attached ? "Connected to computer." : "Connect the Type-C port to a computer.");
}

void rg_usb_msc_run(void)
{
    RG_LOGI("Starting dedicated USB mass-storage mode...");

    // Keep the normal FAT mount and USB HID host stopped. The PC is the SD
    // card's only owner for the whole session.
    rg_settings_init(true);
    rg_input_init();
    rg_display_init();
    rg_gui_init();

    sdmmc_card_t *card = NULL;
    int sd_slot = -1;
    tinyusb_msc_storage_handle_t storage = NULL;
    bool msc_installed = false;
    bool tusb_installed = false;
    const char *error = NULL;

    esp_err_t err = init_raw_sdcard(&card, &sd_slot);
    if (err != ESP_OK)
    {
        RG_LOGE("USB MSC SD initialization failed: %s", esp_err_to_name(err));
        error = "SD card initialization failed.";
    }

    if (!error)
    {
        tinyusb_msc_driver_config_t msc_cfg = {
            .user_flags.auto_mount_off = 1,
        };
        err = tinyusb_msc_install_driver(&msc_cfg);
        if (err == ESP_OK)
            msc_installed = true;
        else
            error = "USB mass-storage driver failed.";
    }

    if (!error)
    {
        tinyusb_msc_storage_config_t storage_cfg = {
            .medium.card = card,
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
            .fat_fs = {
                .base_path = RG_STORAGE_ROOT,
                .config = {
                    .format_if_mount_failed = false,
                    .max_files = 4,
                    .allocation_unit_size = 0,
                },
                .do_not_format = true,
            },
        };
        err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &storage);
        if (err != ESP_OK)
            error = "SD card could not be shared.";
    }

    if (!error)
    {
        tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_device_event);
        err = tinyusb_driver_install(&tusb_cfg);
        if (err == ESP_OK)
            tusb_installed = true;
        else
            error = "USB device initialization failed.";
    }

    bool last_attached = usb_attached;
    draw_status(error);
    rg_input_wait_for_key(RG_KEY_MENU, false, 500);
    while (!(rg_input_read_gamepad() & RG_KEY_MENU))
    {
        if (!error && last_attached != usb_attached)
        {
            last_attached = usb_attached;
            draw_status(NULL);
        }
        rg_task_delay(20);
    }

    rg_gui_draw_message("Restarting...\n\nKeep USB unplugged for controller mode.");
    if (tusb_installed)
        tinyusb_driver_uninstall();
    if (storage)
        tinyusb_msc_delete_storage(storage);
    if (msc_installed)
        tinyusb_msc_uninstall_driver();
    if (card)
    {
        sdspi_host_remove_device(sd_slot);
        free(card);
        spi_bus_free(RG_STORAGE_SDSPI_HOST);
    }
    rg_task_delay(100);
    esp_restart();
}

#else

bool rg_usb_msc_boot_requested(void) { return false; }
void rg_usb_msc_request(void) { rg_system_restart(); }
void rg_usb_msc_run(void) { rg_system_restart(); }

#endif

