#include <rg_system.h>

#if defined(RG_UPDATER_DOWNLOAD_LOCATION)
static int pending_image_cb(const rg_scandir_t *file, void *arg)
{
    char *path = (char *)arg;

    if (!file->is_file || !rg_extension_match(file->path, "img"))
        return RG_SCANDIR_CONTINUE;
    if (!rg_firmware_image_pending(file->path, RG_FIRMWARE_STAGE_COMPLETE_UPDATE))
        return RG_SCANDIR_CONTINUE;

    snprintf(path, RG_PATH_MAX + 1, "%s", file->path);
    return RG_SCANDIR_STOP;
}

static bool find_pending_image(char *path)
{
    path[0] = 0;
    rg_storage_scandir(RG_UPDATER_DOWNLOAD_LOCATION, pending_image_cb, path, RG_SCANDIR_FILES | RG_SCANDIR_STAT);
    return path[0] != 0;
}
#endif

static void show_factory_menu(const char *message)
{
    const rg_gui_option_t options[] = {
        {0, message, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, "Continue to launcher", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, "Reboot to recovery", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    if (rg_gui_dialog("Factory", options, 1) == 2)
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, RG_BOOT_RECOVERY);
    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}

void app_main(void)
{
    rg_app_t *app = rg_system_init(32000, NULL, NULL);
    const char *image_path = app->bootArgs;
#if defined(RG_UPDATER_DOWNLOAD_LOCATION)
    char pending_image[RG_PATH_MAX + 1];
#endif

    app->configNs = "factory";
    app->isLauncher = true;

    if (!rg_storage_ready())
    {
        rg_gui_alert("Update failed!", "Storage mount failed.");
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
    }

    if (!image_path || !image_path[0])
    {
    #if defined(RG_UPDATER_DOWNLOAD_LOCATION)
        if (find_pending_image(pending_image))
        {
            char message[RG_PATH_MAX + 64];
            snprintf(message, sizeof(message), "Apply pending update?\n%s", rg_basename(pending_image));
            if (!rg_gui_confirm("Firmware update", message, true))
                show_factory_menu("Update was not applied.");
            image_path = pending_image;
        }
        else
    #endif
        {
            show_factory_menu("No update available.");
        }
    }

    rg_display_clear(C_BLACK);
    rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Firmware update\nVerifying image...");

    if (rg_firmware_install_image(image_path, RG_FIRMWARE_STAGE_COMPLETE_UPDATE))
    {
        rg_gui_alert("Update complete", "Rebooting to launcher.");
        rg_storage_delete(image_path);
        rg_system_restart();
    }

    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
