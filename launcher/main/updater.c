#include <rg_system.h>
#include <malloc.h>
#include <string.h>
#include <cJSON.h>

#include "gui.h"

#if defined(RG_ENABLE_NETWORKING) && RG_UPDATER_ENABLE
typedef struct
{
    char name[64];
    char url[256];
    int size;
} asset_t;

typedef struct
{
    char name[64];
    char date[32];
    char url[256];
    asset_t *assets;
    size_t assets_count;
} release_t;

static void format_size(char *out, size_t out_len, int bytes, bool speed)
{
    if (bytes < 0)
    {
        snprintf(out, out_len, "-");
    }
    else if (bytes < 1024 * 1024)
    {
        snprintf(out, out_len, "%d KB%s", (bytes + 1023) / 1024, speed ? "/s" : "");
    }
    else
    {
        snprintf(out, out_len, "%.2fMB%s", bytes / (1024.f * 1024.f), speed ? "/s" : "");
    }
}

static void draw_download_progress(int received, int total, int speed)
{
    char received_str[16], total_str[16], speed_str[16], info[80];
    const int screen_w = rg_display_get_width();
    const int screen_h = rg_display_get_height();
    const int box_w = RG_MIN(screen_w - 24, 300);
    const int box_h = 82;
    const int box_x = (screen_w - box_w) / 2;
    const int box_y = (screen_h - box_h) / 2;
    const int bar_x = box_x + 12;
    const int bar_y = box_y + 42;
    const int bar_w = box_w - 24;
    const int bar_h = 26;
    const int inner_w = bar_w - 4;
    const int inner_h = bar_h - 4;
    int fill_w;

    if (total > 0)
    {
        fill_w = (int)(((int64_t)received * inner_w) / total);
    }
    else
    {
        int step = (received / (16 * 1024)) % (inner_w + 1);
        fill_w = step;
    }
    fill_w = RG_MIN(RG_MAX(fill_w, 0), inner_w);
    format_size(received_str, sizeof(received_str), received, false);
    format_size(total_str, sizeof(total_str), total, false);
    format_size(speed_str, sizeof(speed_str), speed, true);

    if (total > 0)
        snprintf(info, sizeof(info), "%s / %s  %s", received_str, total_str, speed_str);
    else
        snprintf(info, sizeof(info), "%s  %s", received_str, speed_str);

    rg_gui_draw_rect(box_x, box_y, box_w, box_h, 2, C_DIM_GRAY, C_NAVY);
    rg_gui_draw_text(box_x + 8, box_y + 12, box_w - 16, "Downloading update", C_WHITE, C_NAVY, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_rect(bar_x, bar_y, bar_w, bar_h, 1, C_WHITE, C_BLACK);
    rg_gui_draw_rect(bar_x + 2, bar_y + 2, inner_w, inner_h, 0, 0, C_DARK_GRAY);
    rg_gui_draw_rect(bar_x + 2, bar_y + 2, fill_w, inner_h, 0, 0, C_DODGER_BLUE);
    rg_gui_draw_text(bar_x + 3, bar_y + 6, bar_w - 6, info, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static bool download_file(const char *url, const char *filename, int expected_size)
{
    RG_ASSERT_ARG(url && filename);

    rg_http_req_t *req = NULL;
    FILE *fp = NULL;
    void *buffer = NULL;
    int received = 0;
    int written = 0;
    int len;
    int64_t start_time = 0;
    int64_t last_draw = 0;

    RG_LOGI("Downloading: '%s' to '%s'", url, filename);
    rg_gui_draw_message("Connecting...");

    if (!(req = rg_network_http_open(url, NULL)))
    {
        rg_gui_alert("Download failed!", "Connection failed!");
        return false;
    }

    if (!(buffer = malloc(16 * 1024)))
    {
        rg_network_http_close(req);
        rg_gui_alert("Download failed!", "Out of memory!");
        return false;
    }

    if (!(fp = fopen(filename, "wb")))
    {
        rg_network_http_close(req);
        free(buffer);
        rg_gui_alert("Download failed!", "File open failed!");
        return false;
    }

    int content_length = req->content_length > 0 ? req->content_length : expected_size;
    start_time = last_draw = rg_system_timer();
    draw_download_progress(0, content_length, 0);

    while ((len = rg_network_http_read(req, buffer, 16 * 1024)) > 0)
    {
        received += len;
        written += fwrite(buffer, 1, len, fp);
        int64_t now = rg_system_timer();
        if (now - last_draw > 200000)
        {
            int speed = (int)((int64_t)received * 1000000 / RG_MAX(1, now - start_time));
            draw_download_progress(received, content_length, speed);
            last_draw = now;
        }
        if (received != written)
            break; // No point in continuing
    }
    int64_t end_time = rg_system_timer();
    int speed = (int)((int64_t)received * 1000000 / RG_MAX(1, end_time - start_time));
    draw_download_progress(received, content_length, speed);

    rg_network_http_close(req);
    free(buffer);
    fclose(fp);

    if (received != written || (received != content_length && content_length != -1))
    {
        rg_storage_delete(filename);
        rg_gui_alert("Download failed!", "Read/write error!");
        return false;
    }

    gui_redraw();
    return true;
}

static cJSON *fetch_json(const char *url)
{
    RG_ASSERT_ARG(url);

    RG_LOGI("Fetching: '%s'", url);
    rg_gui_draw_hourglass();

    rg_http_req_t *req = NULL;
    char *buffer = NULL;
    cJSON *json = NULL;

    if (!(req = rg_network_http_open(url, NULL)))
    {
        RG_LOGW("Could not open releases URL '%s', aborting update check.", url);
        goto cleanup;
    }

    size_t buffer_length = RG_MAX(256 * 1024, req->content_length);

    if (!(buffer = calloc(1, buffer_length + 1)))
    {
        RG_LOGE("Out of memory, aborting update check!");
        goto cleanup;
    }

    if (rg_network_http_read(req, buffer, buffer_length) < 16)
    {
        RG_LOGW("Read from releases URL '%s' returned (almost) no bytes, aborting update check.", url);
        goto cleanup;
    }

    if (!(json = cJSON_Parse(buffer)))
        RG_LOGW("Could not parse JSON received from releases URL '%s'.", url);

cleanup:
    rg_network_http_close(req);
    free(buffer);
    gui_redraw();
    return json;
}

static rg_gui_event_t view_release_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const release_t *release = (release_t *)option->arg;

    if (event == RG_DIALOG_ENTER)
    {
    #if defined(RG_UPDATER_APPLICATION) && defined(RG_UPDATER_DOWNLOAD_LOCATION)
        rg_gui_option_t options[release->assets_count + 4];
        rg_gui_option_t *opt = options;

        *opt++ = (rg_gui_option_t){0, _("Date"), (char *)release->date, RG_DIALOG_FLAG_MESSAGE, NULL};
        *opt++ = (rg_gui_option_t){0, _("Files:"), NULL, RG_DIALOG_FLAG_MESSAGE, NULL};

        for (int i = 0; i < release->assets_count; i++)
            *opt++ = (rg_gui_option_t){i, release->assets[i].name, NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        *opt++ = (rg_gui_option_t)RG_DIALOG_END;

        int sel = rg_gui_dialog(release->name, options, 0);
        if (sel != RG_DIALOG_CANCELLED)
        {
            char dest_path[RG_PATH_MAX];
            snprintf(dest_path, RG_PATH_MAX, "%s/%s", RG_UPDATER_DOWNLOAD_LOCATION, release->assets[sel].name);
            if (!rg_storage_mkdir(RG_UPDATER_DOWNLOAD_LOCATION))
            {
                rg_gui_alert("Download failed!", "Could not create firmware folder!");
                return RG_DIALOG_REDRAW;
            }
            if (download_file(release->assets[sel].url, dest_path, release->assets[sel].size))
            {
                if (rg_gui_confirm(_("Download complete!"), _("Reboot to flash?"), true))
                {
                    if (rg_system_have_app(RG_UPDATER_APPLICATION))
                    {
                        if (rg_extension_match(dest_path, "img") &&
                            !rg_firmware_install_image(dest_path, RG_FIRMWARE_STAGE_PREPARE_UPDATE))
                        {
                            return RG_DIALOG_REDRAW;
                        }
                        rg_system_switch_app(RG_UPDATER_APPLICATION, NULL, dest_path, RG_BOOT_ONCE);
                    }
                    else
                        rg_gui_alert("Update failed!", "Firmware updater app not found!");
                }
            }
        }
    #else
        rg_gui_alert(release->name, release->url);
    #endif
        return RG_DIALOG_REDRAW;
    }

    return RG_DIALOG_VOID;
}

void updater_show_dialog(void)
{
    cJSON *releases_json = fetch_json(RG_UPDATER_GITHUB_RELEASES);
    size_t releases_count = RG_MIN(cJSON_GetArraySize(releases_json), 10);
    const char *dialog_title = _("Available Releases");
    rg_gui_option_t dialog_options[releases_count + 1];

    if (!releases_json)
    {
        rg_gui_alert(dialog_title, _("Connection failed!"));
        return;
    }
    if (!releases_count)
    {
        rg_gui_alert(dialog_title, _("Received empty list!"));
        cJSON_Delete(releases_json);
        return;
    }

    release_t *releases = calloc(releases_count, sizeof(release_t));
    rg_gui_option_t *opt = dialog_options;

    for (int i = 0; i < releases_count; ++i)
    {
        cJSON *release_json = cJSON_GetArrayItem(releases_json, i);
        cJSON *assets_json = cJSON_GetObjectItem(release_json, "assets");
        size_t assets_count = cJSON_GetArraySize(assets_json);
        char *name = cJSON_GetStringValue(cJSON_GetObjectItem(release_json, "name"));
        char *date = cJSON_GetStringValue(cJSON_GetObjectItem(release_json, "published_at"));
        char *url = cJSON_GetStringValue(cJSON_GetObjectItem(release_json, "html_url"));

        release_t *release = releases + i;
        snprintf(release->name, sizeof(release->name), "%s", name ?: "N/A");
        snprintf(release->date, sizeof(release->date), "%s", date ?: "N/A");
        snprintf(release->url, sizeof(release->url), "%s", url ?: "N/A");
        release->assets = calloc(assets_count, sizeof(asset_t));
        release->assets_count = 0;

        for (int j = 0; j < assets_count; ++j)
        {
            cJSON *asset_json = cJSON_GetArrayItem(assets_json, j);
            char *name = cJSON_GetStringValue(cJSON_GetObjectItem(asset_json, "name"));
            char *url = cJSON_GetStringValue(cJSON_GetObjectItem(asset_json, "browser_download_url"));
            cJSON *size = cJSON_GetObjectItem(asset_json, "size");
            if (name && url && rg_extension_match(name, "fw img"))
            {
                asset_t *asset = &release->assets[release->assets_count++];
                snprintf(asset->name, sizeof(asset->name), "%s", name);
                snprintf(asset->url, sizeof(asset->url), "%s", url);
                asset->size = cJSON_IsNumber(size) ? size->valueint : -1;
            }
        }
        *opt++ = (rg_gui_option_t){(intptr_t)release, release->name, NULL, RG_DIALOG_FLAG_NORMAL, &view_release_cb};
    }
    *opt++ = (rg_gui_option_t)RG_DIALOG_END;

    cJSON_Delete(releases_json);

    rg_gui_dialog(dialog_title, dialog_options, 0);

    for (int i = 0; i < releases_count; ++i)
        free(releases[i].assets);
    free(releases);
}
#endif
