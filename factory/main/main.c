#include <rg_system.h>

#if defined(RG_UPDATER_DOWNLOAD_LOCATION)
#define MAX_CANDIDATES 8

typedef struct
{
    char path[RG_PATH_MAX + 1];
    time_t mtime;
} candidate_t;

typedef struct
{
    candidate_t items[MAX_CANDIDATES];
    size_t count;
    size_t skipped;
} candidates_t;

static int collect_image_cb(const rg_scandir_t *file, void *arg)
{
    candidates_t *list = (candidates_t *)arg;

    if (!file->is_file || !rg_extension_match(file->path, "img"))
        return RG_SCANDIR_CONTINUE;

    if (list->count >= MAX_CANDIDATES)
    {
        list->skipped++;
        return RG_SCANDIR_CONTINUE;
    }

    snprintf(list->items[list->count].path, sizeof(list->items[0].path), "%s", file->path);
    list->items[list->count].mtime = file->mtime;
    list->count++;

    return RG_SCANDIR_CONTINUE;
}

/**
 * Pick the image to apply out of whatever is sitting in the download folder.
 *
 * Newest first, because that is the one the user just downloaded: this used to take the first file
 * the directory happened to list, so a stale image left behind by an earlier update could win over
 * the new one. Checking an image means verifying its CRC and comparing every partition against
 * flash, which takes seconds per file, so the order matters for speed too - and the name is shown
 * while it works, because a silent minute looks like a hang.
 */
static bool find_pending_image(char *path)
{
    candidates_t list = {0};

    path[0] = 0;
    rg_storage_scandir(RG_UPDATER_DOWNLOAD_LOCATION, collect_image_cb, &list, RG_SCANDIR_FILES | RG_SCANDIR_STAT);

    if (list.skipped)
        RG_LOGW("More than %d images in the download folder, ignoring %d of them", MAX_CANDIDATES,
                (int)list.skipped);

    // Insertion sort, newest first. The list is at most MAX_CANDIDATES long.
    for (size_t i = 1; i < list.count; ++i)
    {
        candidate_t key = list.items[i];
        size_t j = i;
        while (j > 0 && list.items[j - 1].mtime < key.mtime)
        {
            list.items[j] = list.items[j - 1];
            j--;
        }
        list.items[j] = key;
    }

    for (size_t i = 0; i < list.count; ++i)
    {
        rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Looking for an update...\n%s",
                                  rg_basename(list.items[i].path));
        if (rg_firmware_image_pending(list.items[i].path, RG_FIRMWARE_STAGE_COMPLETE_UPDATE))
        {
            snprintf(path, RG_PATH_MAX + 1, "%s", list.items[i].path);
            return true;
        }
        RG_LOGI("'%s' is not applicable", list.items[i].path);
    }

    return false;
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
