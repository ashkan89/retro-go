#include <rg_system.h>

void app_main(void)
{
    rg_app_t *app = rg_system_init(32000, NULL, NULL);
    const char *image_path = app->bootArgs;

    app->configNs = "factory";
    app->isLauncher = true;

    if (!image_path || !image_path[0])
    {
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
    }

    if (!rg_storage_ready())
    {
        rg_gui_alert("Update failed!", "Storage mount failed.");
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
    }

    rg_gui_draw_message("Firmware update\nVerifying image...");

    if (rg_firmware_install_image(image_path, RG_FIRMWARE_STAGE_COMPLETE_UPDATE))
    {
        rg_gui_alert("Update complete", "Rebooting to launcher.");
        rg_storage_delete(image_path);
        rg_system_restart();
    }

    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
