#include "rg_system.h"
#include "rg_usb_host.h"

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32P4)
#include "hal/usb_dwc_hal.h"

void __real_usb_dwc_hal_core_soft_reset(usb_dwc_hal_context_t *hal);

// IDF resets the controller during root-port disconnect/error recovery.
// Restore the speed restriction before it powers the root port again.
void __wrap_usb_dwc_hal_core_soft_reset(usb_dwc_hal_context_t *hal)
{
    __real_usb_dwc_hal_core_soft_reset(hal);
#if RG_USB_HOST_FULL_SPEED
    if (hal->dev == USB_DWC_LL_GET_HW(0))
        usb_dwc_ll_hcfg_set_fsls_supp_only(hal->dev);
#endif
}
#endif

#if defined(ESP_PLATFORM) && (defined(RG_ENABLE_USB_HID_HOST) || defined(RG_ENABLE_USB_XINPUT))

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static volatile int ref_count;
static volatile bool task_running;
static volatile bool init_done;
static volatile bool init_ok;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void usb_host_task(void *arg)
{
    (void)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if defined(CONFIG_IDF_TARGET_ESP32P4) && RG_USB_HOST_FULL_SPEED
        // Keep HUSB (controller 0), but configure its speed before attachment.
        .peripheral_map = BIT0,
        .root_port_unpowered = true,
#endif
    };
    esp_err_t err = usb_host_install(&config);
#if defined(CONFIG_IDF_TARGET_ESP32P4) && RG_USB_HOST_FULL_SPEED
    if (err == ESP_OK)
    {
        // IDF 5.5 has no transaction translator for FS/LS devices behind
        // a high-speed hub. Make the hub negotiate full speed on HUSB.
        // Setting this before root-port power also covers boot-attached hubs.
        usb_dwc_ll_hcfg_set_fsls_supp_only(USB_DWC_LL_GET_HW(0));
        err = usb_host_lib_set_root_port_power(true);
        if (err != ESP_OK)
            usb_host_uninstall();
        else
            RG_LOGI("USB HUSB host ready (full speed for HID/XInput hubs)");
    }
#endif
    if (err != ESP_OK)
    {
        RG_LOGE("USB host install failed: %s", esp_err_to_name(err));
        init_ok = false;
        init_done = true;
        task_running = false;
        vTaskDelete(NULL);
        return;
    }
    init_ok = true;
    init_done = true;
    while (task_running)
    {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    }
    usb_host_uninstall();
    vTaskDelete(NULL);
}

bool rg_usb_host_acquire(void)
{
    portENTER_CRITICAL(&lock);
    int count = ++ref_count;
    portEXIT_CRITICAL(&lock);

    if (count == 1)
    {
        init_done = false;
        task_running = true;
        xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, NULL,
                                RG_TASK_PRIORITY_7, NULL, RG_TASK_AFFINITY_IO);
    }
    while (!init_done)
        rg_task_delay(10);
    return init_ok;
}

void rg_usb_host_release(void)
{
    portENTER_CRITICAL(&lock);
    int count = --ref_count;
    portEXIT_CRITICAL(&lock);
    if (count <= 0)
        task_running = false;
}

#endif
