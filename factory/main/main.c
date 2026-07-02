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

    if (rg_gui_confirm("Firmware update", image_path, true) &&
        rg_firmware_install_image(image_path,
            RG_FIRMWARE_UPDATE_APPS |
            RG_FIRMWARE_UPDATE_PARTITION_TABLE |
            RG_FIRMWARE_REQUIRE_FACTORY |
            RG_FIRMWARE_REQUIRE_LAUNCHER))
    {
        rg_gui_alert("Update complete", "Rebooting to launcher.");
        rg_storage_delete(image_path);
    }

    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
