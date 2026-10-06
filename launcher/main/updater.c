#include <rg_system.h>
#include <malloc.h>
#include <string.h>
#include <cJSON.h>

#include "gui.h"
#include "rg_update.h"
#include "updater.h"
#include <limits.h>

#if defined(RG_ENABLE_NETWORKING) && RG_UPDATER_ENABLE
#include "update_version.h"
typedef struct
{
    char name[160];
    char url[512];
    int size;
} asset_t;

typedef struct
{
    char version[29];
    asset_t asset;
} release_t;

static bool check_busy;
static bool check_succeeded;
static unsigned startup_attempts;
static int64_t next_startup_check;

/**
 * How much is read from the socket and written to the card at a time.
 *
 * Bigger means fewer FatFs transactions and fewer trips through the HTTP client per megabyte, but
 * the read only returns once the whole chunk has arrived, and the cancel button is polled between
 * chunks - so this is also the worst-case delay before B is noticed. 32 KB is a tenth of a second at
 * a healthy rate and just over a second on a bad link.
 */
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)

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
    const int box_h = 96;
    const int box_x = (screen_w - box_w) / 2;
    const int box_y = (screen_h - box_h) / 2;
    const int bar_x = box_x + 12;
    const int bar_y = box_y + 42;
    const int bar_w = box_w - 24;
    const int bar_h = 26;
    const int inner_w = bar_w - 4;
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

    // Composited like every other overlay, over whatever the launcher last drew: one transfer per
    // update instead of a card built piece by piece on screen. This used to keep a full-screen
    // scratch surface of its own (~150 KB) purely to avoid that flicker.
    bool overlay = rg_gui_begin_overlay(box_x - 5, box_y - 5, box_w + 10, box_h + 12, C_NONE);

    // Same card, header chip and bar as the rest of the UI, so an update looks like part of the
    // firmware rather than like a different program that took the screen.
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    rg_color_t box_bg = rg_gui_get_theme_color("dialog", "background", pal->background);
    int text_h = rg_gui_get_font_height() + 2;
    int chip_h = text_h + 6;

    rg_gui_draw_shadow(box_x, box_y, box_w, box_h, 7, 3);
    rg_gui_draw_panel(box_x, box_y, box_w, box_h, 7, box_bg, pal->border, 255);
    rg_gui_draw_panel(box_x + 7, box_y + 7, box_w - 14, chip_h, 4, pal->surface_alt, C_NONE, 255);
    rg_gui_draw_panel(box_x + 7, box_y + 9, 3, chip_h - 4, 1, pal->accent, C_NONE, 255);
    rg_gui_draw_text(box_x + 14, box_y + 7 + (chip_h - text_h) / 2, box_w - 28, _("Downloading update"), pal->text,
                     pal->surface_alt, RG_TEXT_ALIGN_CENTER);

    // The fill keeps its own accent so a download always looks the same, whatever the theme
    int bar_thickness = RG_MAX(bar_h / 4, 4);
    rg_gui_draw_progress_bar(bar_x, bar_y + 2, bar_w, bar_thickness, (fill_w * 100) / RG_MAX(inner_w, 1), pal->accent,
                             rg_gui_scale_color(pal->divider, 200));
    rg_gui_draw_text(bar_x, bar_y + bar_thickness + 6, bar_w, info, pal->text_dim, box_bg, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(bar_x, bar_y + bar_thickness + 6 + text_h, bar_w, _("B  Cancel"), pal->accent, box_bg,
                     RG_TEXT_ALIGN_CENTER);

    if (overlay)
        rg_gui_end_overlay();
}

typedef struct
{
    const char *keep;
    int removed;
} purge_state_t;

static int purge_image_cb(const rg_scandir_t *file, void *arg)
{
    purge_state_t *state = (purge_state_t *)arg;

    if (!file->is_file || !rg_extension_match(file->path, "img"))
        return RG_SCANDIR_CONTINUE;
    if (state->keep && strcmp(file->path, state->keep) == 0)
        return RG_SCANDIR_CONTINUE;

    RG_LOGI("Removing stale firmware image '%s'", file->path);
    if (rg_storage_delete(file->path))
        state->removed++;

    return RG_SCANDIR_CONTINUE;
}

/**
 * Delete firmware images already sitting in the download folder.
 *
 * Nothing used to clean these up, so every update left another few megabytes behind. That is not
 * only wasted space (which a 4 MB image can easily run out of, and a truncated download then fails
 * its checksum): the factory app looks in this folder for the image to apply, so an old one being
 * there at all is a hazard.
 */
static int purge_stale_images(const char *keep)
{
    purge_state_t state = {.keep = keep};
    rg_storage_scandir(RG_UPDATER_DOWNLOAD_LOCATION, purge_image_cb, &state, RG_SCANDIR_FILES);
    if (state.removed)
        RG_LOGI("Removed %d stale firmware image(s)", state.removed);
    return state.removed;
}

static bool download_file(const char *url, const char *filename, int expected_size)
{
    RG_ASSERT_ARG(url && filename);

    rg_http_req_t *req = NULL;
    FILE *fp = NULL;
    void *buffer = NULL;
    int received = 0;
    int written = 0;
    int len = 0;
    int64_t start_time = 0;
    int64_t last_draw = 0;

    RG_LOGI("Downloading: '%s' to '%s'", url, filename);
    rg_gui_draw_message("Connecting...");

    // A firmware image is megabytes, so this asks for a transfer sized for that: a 16 KB socket
    // buffer inside the HTTP client and 64 KB reads out of it. The old 1 KB/16 KB pair meant
    // thousands of small reads (and, over TLS, thousands of record boundaries) per megabyte.
    rg_http_cfg_t http_cfg = RG_HTTP_DEFAULT_CONFIG();
    http_cfg.buffer_size = 16 * 1024;
    http_cfg.verify_server = true;

    if (!(req = rg_network_http_open(url, &http_cfg)))
    {
        rg_gui_alert("Download failed!", "Connection failed!");
        return false;
    }

    if (req->status_code != 200 || expected_size <= 0 ||
        (req->content_length > 0 && req->content_length != expected_size))
    {
        rg_network_http_close(req);
        rg_gui_alert(_("Download failed!"), _("Unexpected server response!"));
        return false;
    }

    if (!(buffer = malloc(DOWNLOAD_CHUNK_SIZE)))
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

    // A firmware image is megabytes and the card may not have room for it. Finding that out now
    // beats finding out later: a write that runs out of space leaves a file whose size looks
    // right but whose contents stop early, which then surfaces as a verification failure with no
    // obvious cause.
    int64_t free_space = rg_storage_get_free_space(filename);
    int needed = req->content_length > 0 ? req->content_length : expected_size;

    if (free_space >= 0 && needed > 0 && free_space < (int64_t)needed + 64 * 1024)
    {
        char message[128];
        snprintf(message, sizeof(message), "Needs %d KB, only %d KB free on the card.", needed / 1024,
                 (int)(free_space / 1024));
        fclose(fp);
        rg_storage_delete(filename);
        rg_network_http_close(req);
        free(buffer);
        rg_gui_alert("Download failed!", message);
        return false;
    }

    int content_length = req->content_length > 0 ? req->content_length : expected_size;
    start_time = last_draw = rg_system_timer();
    draw_download_progress(0, content_length, 0);

    // Input is polled on every chunk. Before, the download owned the device until it finished: no
    // button did anything, and once the screen had dimmed or switched off there was no way to bring
    // it back either, because nothing was reading the gamepad (which is what wakes it).
    bool cancelled = false;
    uint32_t prev_keys = rg_input_read_gamepad();

    while ((len = rg_network_http_read(req, buffer, DOWNLOAD_CHUNK_SIZE)) > 0)
    {
        rg_system_tick(0);
        if (len > expected_size - received)
            break;
        received += len;
        written += fwrite(buffer, 1, len, fp);
        int64_t now = rg_system_timer();
        int speed = (int)((int64_t)received * 1000000 / RG_MAX(1, now - start_time));

        if (now - last_draw > 200000)
        {
            draw_download_progress(received, content_length, speed);
            last_draw = now;
        }

        uint32_t keys = rg_input_read_gamepad();

        // Edge triggered, so holding the button does not ask again and again
        if ((keys & RG_KEY_B) && !(prev_keys & RG_KEY_B))
        {
            // The transfer just pauses while the question is on screen; the server may drop a
            // connection left idle for a long time, in which case this fails like any other read
            // error and the partial file is removed.
            if (rg_gui_confirm(_("Cancel update?"), _("Stop the download and delete the partial file?"), false))
            {
                cancelled = true;
                break;
            }
            draw_download_progress(received, content_length, speed);
            last_draw = rg_system_timer();
            keys = rg_input_read_gamepad();
        }

        prev_keys = keys;

        if (received != written)
            break; // No point in continuing
    }
    if (!cancelled)
    {
        int64_t end_time = rg_system_timer();
        int speed = (int)((int64_t)received * 1000000 / RG_MAX(1, end_time - start_time));
        draw_download_progress(received, content_length, speed);
    }

    rg_network_http_close(req);
    free(buffer);

    // fclose is where a buffered write finally reaches the card, so it is also where running out
    // of space shows up. Checking it (and the size that ended up on the card) is the difference
    // between reporting a bad download now and a mysterious bad image later.
    bool write_error = ferror(fp) != 0;
    if (fclose(fp) != 0)
        write_error = true;

    rg_stat_t written_stat = rg_storage_stat(filename);

    if (!cancelled && (write_error || (int)written_stat.size != written))
    {
        char message[128];
        snprintf(message, sizeof(message), "The card stored %d of %d bytes.", (int)written_stat.size,
                 written);
        rg_storage_delete(filename);
        rg_gui_alert("Download failed!", message);
        return false;
    }

    if (cancelled)
    {
        rg_storage_delete(filename);
        rg_gui_draw_message(_("Update cancelled"));
        rg_task_delay(700);
        gui_redraw();
        return false;
    }

    if (len != 0 || received != written || received != expected_size || received != content_length)
    {
        rg_storage_delete(filename);
        rg_gui_alert("Download failed!", "Read/write error!");
        return false;
    }

    gui_redraw();
    return true;
}

// Bounded, complete response reads also support chunked GitHub responses.
// No drawing or system ticks here: this runs on the background task too.
static cJSON *fetch_json(const char *url, int *status)
{
    const size_t limit = 128 * 1024;
    const rg_http_header_t headers[] = {
        {"Accept", "application/vnd.github+json"},
        {"User-Agent", "Retro-Go-Updater"},
        {NULL, NULL},
    };
    rg_http_cfg_t config = RG_HTTP_DEFAULT_CONFIG();
    config.headers = headers;
    config.verify_server = true;
    config.timeout_ms = 15000;
    rg_http_req_t *req = rg_network_http_open(url, &config);
    char *buffer = NULL;
    cJSON *json = NULL;
    *status = req ? req->status_code : 0;
    if (!req || req->status_code != 200 || req->content_length > (int)limit)
        goto cleanup;
    buffer = malloc(limit + 1);
    if (!buffer) goto cleanup;
    size_t used = 0;
    int len = 0;
    while (used < limit && (len = rg_network_http_read(req, buffer + used, RG_MIN(4096, limit - used))) > 0)
        used += len;
    // Reject truncated/oversized JSON rather than parsing a partial response.
    if (used == limit || len < 0 ||
        (req->content_length > 0 && used != (size_t)req->content_length)) goto cleanup;
    buffer[used] = 0;
    json = cJSON_Parse(buffer);
cleanup:
    rg_network_http_close(req);
    free(buffer);
    return json;
}

typedef enum { CHECK_FAILED, CHECK_CURRENT, CHECK_UNSUPPORTED, CHECK_AVAILABLE } check_result_t;

static check_result_t check_release(release_t *release)
{
    int status;
    cJSON *json = fetch_json(RG_UPDATER_GITHUB_RELEASES, &status);
    check_result_t result = CHECK_FAILED;
    if (!json)
        return status == 404 ? CHECK_UNSUPPORTED : CHECK_FAILED;
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(json, "tag_name"));
    uint32_t numbers[3];
    const char *suffix;
    if (!cJSON_IsObject(json) || cJSON_IsTrue(cJSON_GetObjectItem(json, "draft")) ||
        cJSON_IsTrue(cJSON_GetObjectItem(json, "prerelease")) ||
        !update_parse_version(tag, numbers, &suffix) || *suffix) goto cleanup;
    if (*tag == 'v' || *tag == 'V') ++tag;
    if (strlen(tag) >= sizeof(release->version)) goto cleanup;
    uint32_t installed[3];
    const char *installed_suffix;
    if (!update_parse_version(rg_system_get_app()->version, installed, &installed_suffix)) goto cleanup;
    if (!update_version_is_newer(tag, rg_system_get_app()->version))
    {
        result = CHECK_CURRENT;
        goto cleanup;
    }
    snprintf(release->version, sizeof(release->version), "%s", tag);
    cJSON *asset;
    int matches = 0;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItem(json, "assets"))
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "name"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "browser_download_url"));
        cJSON *size = cJSON_GetObjectItem(asset, "size");
        if (!update_image_asset_matches(name, tag, RG_TARGET_NAME)) continue;
        if (strlen(name) >= sizeof(release->asset.name)) goto cleanup;
        if (!url || strncmp(url, "https://github.com/", 19) != 0 ||
            strlen(url) >= sizeof(release->asset.url) || !cJSON_IsNumber(size) ||
            size->valuedouble <= 256 || size->valuedouble > INT_MAX) goto cleanup;
        snprintf(release->asset.name, sizeof(release->asset.name), "%s", name);
        snprintf(release->asset.url, sizeof(release->asset.url), "%s", url);
        release->asset.size = size->valueint;
        ++matches;
    }
    result = matches == 1 ? CHECK_AVAILABLE : CHECK_UNSUPPORTED;
cleanup:
    cJSON_Delete(json);
    return result;
}

static void startup_check_task(void *arg)
{
    (void)arg;
    release_t release = {0};
    check_result_t result = check_release(&release);
    if (result != CHECK_FAILED) rg_gui_set_update_available(result == CHECK_AVAILABLE);
    __atomic_store_n(&check_succeeded, result != CHECK_FAILED, __ATOMIC_RELEASE);
    __atomic_store_n(&check_busy, false, __ATOMIC_RELEASE);
}

void updater_poll(void)
{
    if (__atomic_load_n(&check_busy, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&check_succeeded, __ATOMIC_ACQUIRE) || startup_attempts >= 3 ||
        rg_system_timer() < next_startup_check ||
        rg_network_get_info().state != RG_NETWORK_CONNECTED) return;
    // AP mode has no upstream Internet even though its state is connected.
    if (rg_network_get_info().ap_mode) return;
    __atomic_store_n(&check_busy, true, __ATOMIC_RELEASE);
    ++startup_attempts;
    next_startup_check = rg_system_timer() + 60000000;
    if (!rg_task_create("rg_update_check", startup_check_task, NULL, 8 * 1024,
                        RG_TASK_PRIORITY_1, RG_TASK_AFFINITY_IO))
        __atomic_store_n(&check_busy, false, __ATOMIC_RELEASE);
}

void updater_show_dialog(void)
{
    if (rg_network_get_info().state != RG_NETWORK_CONNECTED)
    {
        rg_gui_alert(_("Check for updates"), _("Wi-Fi is not connected!"));
        return;
    }
    rg_gui_draw_message(_("Checking for updates..."));
    while (__atomic_load_n(&check_busy, __ATOMIC_ACQUIRE)) rg_task_delay(50);
    release_t release = {0};
    check_result_t result = check_release(&release);
    if (result != CHECK_FAILED) rg_gui_set_update_available(result == CHECK_AVAILABLE);
    __atomic_store_n(&check_succeeded, result != CHECK_FAILED, __ATOMIC_RELEASE);
    if (result != CHECK_AVAILABLE)
    {
        const char *message = result == CHECK_CURRENT ? _("Your firmware is up to date.") :
            result == CHECK_UNSUPPORTED ? _("No compatible image in the latest release.") :
            _("Could not check for updates. Please try again later.");
        rg_gui_alert(_("Check for updates"), message);
        return;
    }
#if defined(RG_UPDATER_APPLICATION) && defined(RG_UPDATER_DOWNLOAD_LOCATION)
    char prompt[128];
    snprintf(prompt, sizeof(prompt), _("Version %s is available. Download update?"), release.version);
    if (!rg_gui_confirm(_("Update available"), prompt, true)) return;
    char dest_path[RG_PATH_MAX];
    if (snprintf(dest_path, sizeof(dest_path), "%s/%s", RG_UPDATER_DOWNLOAD_LOCATION,
                 release.asset.name) >= sizeof(dest_path))
    {
        rg_gui_alert(_("Download failed!"), _("Firmware path is too long!"));
        return;
    }
    if (!rg_storage_mkdir(RG_UPDATER_DOWNLOAD_LOCATION))
    {
        rg_gui_alert(_("Download failed!"), _("Could not create firmware folder!"));
        return;
    }
    purge_stale_images(dest_path);
    if (!download_file(release.asset.url, dest_path, release.asset.size)) return;
    if (!rg_firmware_image_matches_version(dest_path, release.version))
    {
        rg_storage_delete(dest_path);
        rg_gui_alert(_("Download failed!"), _("Image version does not match the release!"));
        return;
    }
    if (!rg_system_have_app(RG_UPDATER_APPLICATION))
    {
        rg_gui_alert(_("Cannot install update"),
                     _("This firmware has no factory partition. Flash the downloaded image over USB once."));
        return;
    }
    if (!rg_gui_confirm(_("Download complete!"), _("Verify and reboot to install update?"), true)) return;
    // Verify target and image CRC before preparing the factory and handing off.
    if (!rg_firmware_install_image(dest_path, RG_FIRMWARE_STAGE_PREPARE_UPDATE))
    {
        rg_storage_delete(dest_path);
        return;
    }
    rg_system_switch_app(RG_UPDATER_APPLICATION, NULL, dest_path, RG_BOOT_ONCE);
#else
    rg_gui_alert(_("Update available"), release.version);
#endif
}
#else
void updater_poll(void) {}
void updater_show_dialog(void) {}
#endif
