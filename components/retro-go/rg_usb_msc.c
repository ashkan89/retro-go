#include "rg_system.h"
#include "rg_usb_msc.h"

#if defined(ESP_PLATFORM) && defined(RG_ENABLE_USB_MSC) && defined(RG_STORAGE_SDSPI_HOST)

#include <stdlib.h>
#include <string.h>

#include <driver/sdspi_host.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <sdmmc_cmd.h>
#include <class/msc/msc.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>

#define USB_MSC_BOOT_MAGIC        0x4D534355U
#define SDMMC_CMD_READ_SINGLE     17
#define SDMMC_CMD_READ_MULTIPLE   18
#define SDMMC_CMD_WRITE_SINGLE    24
#define SDMMC_CMD_WRITE_MULTIPLE  25
#define USB_ACTIVITY_HOLD_US      100000

RTC_NOINIT_ATTR static struct
{
    uint32_t magic;
    int screen_dim_timeout;
    int screen_off_timeout;
} usb_msc_boot;
static volatile bool usb_attached;
static bool usb_msc_mode;
static volatile rg_color_t usb_activity_color;
static volatile uint32_t usb_activity_sequence;

// esp_tinyusb's CONFIG_TINYUSB_DESC_MSC_STRING is only the interface label.
// Supply the SCSI inquiry identity that Windows uses for the disk device name.
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *inquiry, uint32_t size)
{
    (void)lun;
    if (!inquiry || size < sizeof(*inquiry))
        return 0;

    memcpy(inquiry->vendor_id, "Retro-Go", 8);
    memcpy(inquiry->product_id, "Retro-Go SD Card", 16);
    memcpy(inquiry->product_rev, "1.0 ", 4);
    return sizeof(*inquiry);
}

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
    if (usb_msc_boot.magic != USB_MSC_BOOT_MAGIC)
        return false;
    usb_msc_boot.magic = 0;
    usb_msc_mode = true;
    return true;
}

void rg_usb_msc_request(void)
{
    usb_msc_boot.screen_dim_timeout = rg_settings_get_number(NS_APP, "ScreenDimTimeout", 30);
    usb_msc_boot.screen_off_timeout = rg_settings_get_number(NS_APP, "ScreenOffTimeout", 10);
    usb_msc_boot.magic = USB_MSC_BOOT_MAGIC;
    esp_restart();
}

static esp_err_t usb_sdcard_transaction(int slot, sdmmc_command_t *cmd)
{
    if (cmd->opcode == SDMMC_CMD_READ_SINGLE || cmd->opcode == SDMMC_CMD_READ_MULTIPLE)
    {
        usb_activity_color = C_GREEN;
        usb_activity_sequence++;
    }
    else if (cmd->opcode == SDMMC_CMD_WRITE_SINGLE || cmd->opcode == SDMMC_CMD_WRITE_MULTIPLE)
    {
        usb_activity_color = C_RED;
        usb_activity_sequence++;
    }
    return sdspi_host_do_transaction(slot, cmd);
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
    host.do_transaction = usb_sdcard_transaction;
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

static bool update_screen_timeout(uint32_t joystick, int64_t now)
{
    enum { SCREEN_AWAKE, SCREEN_DIMMED, SCREEN_OFF };
    static int state = SCREEN_AWAKE;
    static int saved_backlight = -1;
    static int64_t last_activity;
    static bool wait_release;

    if (!last_activity)
        last_activity = now;

    if (wait_release)
    {
        if (joystick & RG_KEY_ANY)
        {
            last_activity = now;
            return false;
        }
        wait_release = false;
    }

    if (joystick & RG_KEY_ANY)
    {
        last_activity = now;
        if (state != SCREEN_AWAKE)
        {
            rg_display_set_backlight_raw(saved_backlight < 0 ? rg_display_get_backlight() : saved_backlight);
            saved_backlight = -1;
            state = SCREEN_AWAKE;
            wait_release = true;
            return false;
        }
        if (joystick & RG_KEY_MENU)
            return true;
    }

    const int dim_timeout = usb_msc_boot.screen_dim_timeout;
    const int off_timeout = usb_msc_boot.screen_off_timeout;
    if (dim_timeout <= 0)
        return false;

    const int64_t idle_ms = (now - last_activity) / 1000;
    if (state == SCREEN_AWAKE && idle_ms >= dim_timeout * 1000)
    {
        saved_backlight = rg_display_get_backlight();
        rg_display_set_backlight_raw(1);
        state = SCREEN_DIMMED;
    }
    if (state == SCREEN_DIMMED && off_timeout > 0 &&
        idle_ms >= (dim_timeout + off_timeout) * 1000)
    {
        rg_display_set_backlight_raw(0);
        state = SCREEN_OFF;
    }
    return false;
}

static void update_activity_led(int64_t now)
{
    static uint32_t seen_sequence;
    static int64_t activity_until;
    static rg_color_t shown = C_NONE;
    uint32_t sequence = usb_activity_sequence;
    if (sequence != seen_sequence)
    {
        seen_sequence = sequence;
        activity_until = now + USB_ACTIVITY_HOLD_US;
    }
    rg_color_t requested = now < activity_until ? usb_activity_color : C_NONE;
    if (requested != shown)
    {
        shown = requested;
        rg_system_set_led_color(shown);
    }
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
    while (true)
    {
        int64_t now = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad_unfiltered();
        if (update_screen_timeout(joystick, now))
            break;
        update_activity_led(now);
        if (!error && last_attached != usb_attached)
        {
            last_attached = usb_attached;
            draw_status(NULL);
        }
        rg_task_delay(20);
    }

    rg_system_set_led_color(C_NONE);
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
