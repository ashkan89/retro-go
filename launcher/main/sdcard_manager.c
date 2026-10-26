#include <rg_system.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <cJSON.h>
#include <media.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include "gui.h"
#include "applications.h"
#include "webui.h"
#include "sdcard_manager.h"
#include "update_version.h"
#include "../../components/retro-go/libs/miniz/miniz.h"

#define ARCHIVE_PATH RG_STORAGE_ROOT "/.sdcard_structure.zip"
#define INSTALL_TEMP_PATH RG_STORAGE_ROOT "/.sdcard_install.tmp"
#define ARCHIVE_LIMIT INT_MAX // HTTP byte counts are signed 32-bit; ZIP contents stream to SD.

// All rendering stays on the UI task. Formatting runs on the I/O task so the
// modal keeps updating even while FatFs is blocked in f_mkfs.
static int format_done;
static bool format_ok;
static int64_t stage_started;
static int64_t last_draw;
static const char *stage;
static uint64_t installed_bytes, total_bytes;

static void progress(const char *title, int percent)
{
    int w = RG_MIN(rg_display_get_width() - 16, 300);
    int h = 96;
    int x = (rg_display_get_width() - w) / 2;
    int y = (rg_display_get_height() - h) / 2;
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    char text[80];
    if (percent >= 0)
        snprintf(text, sizeof(text), "%d%%", RG_MIN(percent, 100));
    else
        snprintf(text, sizeof(text), "Working... %ds", (int)((rg_system_timer() - stage_started) / 1000000));
    bool overlay = rg_gui_begin_overlay(x - 4, y - 4, w + 8, h + 8, C_NONE);
    rg_gui_draw_panel(x, y, w, h, 7, pal->background, pal->border, 255);
    rg_gui_draw_text(x + 8, y + 8, w - 16, _("SdCard Manager"), pal->accent, pal->background, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(x + 8, y + 30, w - 16, title, pal->text, pal->background, RG_TEXT_ALIGN_CENTER);
    if (percent >= 0)
        rg_gui_draw_progress_bar(x + 12, y + 56, w - 24, 6, percent, pal->accent, pal->divider);
    rg_gui_draw_text(x + 8, y + 70, w - 16, text, pal->text_dim, pal->background, RG_TEXT_ALIGN_CENTER);
    if (overlay) rg_gui_end_overlay();
    rg_input_read_gamepad();
    rg_system_tick(0);
    last_draw = rg_system_timer();
}

static void format_task(void *arg)
{
    (void)arg;
    format_ok = rg_storage_format();
    __atomic_store_n(&format_done, 1, __ATOMIC_RELEASE);
}

#ifdef RG_ENABLE_NETWORKING
// Resolve the exact installed version; never fall back to the latest release.
// Accept only stable firmware versions that can be represented by release tags.
typedef enum {
    ARCHIVE_FOUND, ARCHIVE_LOOKUP_FAILED, ARCHIVE_NOT_FOUND,
    ARCHIVE_TOO_LARGE, ARCHIVE_VERSION_UNSUPPORTED,
} archive_result_t;

static archive_result_t resolve_archive(const char *asset_name, char *url, size_t url_size, int *size)
{
    const char *version = rg_system_get_app()->version;
    uint32_t numbers[3];
    const char *suffix;
    if (!update_parse_version(version, numbers, &suffix) || *suffix) return ARCHIVE_VERSION_UNSUPPORTED;
    if (*version == 'v' || *version == 'V') ++version;
    const char *base = RG_UPDATER_GITHUB_RELEASES;
    const char *end = strstr(base, "/releases/");
    char api[512];
    if (!end || (size_t)snprintf(api, sizeof(api), "%.*s/releases/tags/v%s", (int)(end - base), base, version) >= sizeof(api)) return ARCHIVE_LOOKUP_FAILED;
    rg_http_header_t headers[] = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "Retro-Go-SD-Manager"}, {NULL, NULL}};
    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.verify_server = true;
    cfg.headers = headers;
    cfg.timeout_ms = 15000;
    rg_http_req_t *req = rg_network_http_open(api, &cfg);
    const int limit = 128 * 1024;
    char *buffer = NULL;
    cJSON *json = NULL;
    archive_result_t result = ARCHIVE_LOOKUP_FAILED;
    if (!req || req->status_code != 200 || req->content_length > limit) goto done;
    buffer = malloc(limit + 1);
    if (!buffer) goto done;
    int used = 0, count = 0;
    while (used < limit && (count = rg_network_http_read(req, buffer + used, RG_MIN(4096, limit - used))) > 0)
    {
        used += count;
        rg_system_tick(0);
    }
    if (count < 0 || used == limit || (req->content_length > 0 && used != req->content_length)) goto done;
    buffer[used] = 0;
    json = cJSON_Parse(buffer);
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(json, "tag_name"));
    if (!tag || tag[0] != 'v' || strcmp(tag + 1, version) || cJSON_IsTrue(cJSON_GetObjectItem(json, "draft"))) goto done;
    cJSON *asset;
    int matches = 0;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItem(json, "assets"))
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "name"));
        const char *link = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "browser_download_url"));
        cJSON *bytes = cJSON_GetObjectItem(asset, "size");
        if (!name || strcmp(name, asset_name)) continue;
        if (cJSON_IsNumber(bytes) && bytes->valuedouble > ARCHIVE_LIMIT)
        {
            result = ARCHIVE_TOO_LARGE;
            goto done;
        }
        if (!link || strncmp(link, "https://github.com/", 19) || strlen(link) >= url_size ||
            !cJSON_IsNumber(bytes) || bytes->valuedouble <= 0 || bytes->valuedouble > ARCHIVE_LIMIT) goto done;
        strcpy(url, link);
        *size = bytes->valueint;
        ++matches;
    }
    result = matches == 1 ? ARCHIVE_FOUND : matches == 0 ? ARCHIVE_NOT_FOUND : ARCHIVE_LOOKUP_FAILED;
done:
    cJSON_Delete(json);
    free(buffer);
    if (req) rg_network_http_close(req);
    return result;
}

static bool download_archive(const char *url, int expected)
{
    rg_http_cfg_t cfg = RG_HTTP_DEFAULT_CONFIG();
    cfg.verify_server = true;
    cfg.buffer_size = 16 * 1024;
    rg_http_req_t *req = rg_network_http_open(url, &cfg);
    FILE *fp = NULL;
    char *buffer = NULL;
    bool ok = false;
    if (!req || req->status_code != 200 || (req->content_length > 0 && req->content_length != expected)) goto done;
    if (rg_storage_get_free_space(RG_STORAGE_ROOT) < (int64_t)expected + 65536) goto done;
    buffer = malloc(16384);
    fp = fopen(ARCHIVE_PATH, "wb");
    if (!buffer || !fp) goto done;
    int used = 0, count;
    while ((count = rg_network_http_read(req, buffer, 16384)) > 0)
    {
        if (count > expected - used || fwrite(buffer, 1, count, fp) != (size_t)count) goto done;
        used += count;
        if (rg_system_timer() - last_draw > 200000) progress(_("Downloading SD files"), (int)((int64_t)used * 100 / expected));
        rg_system_tick(0);
    }
    ok = count == 0 && used == expected && !ferror(fp);
done:
    if (fp && fclose(fp)) ok = false;
    free(buffer);
    if (req) rg_network_http_close(req);
    if (!ok) rg_storage_delete(ARCHIVE_PATH);
    return ok;
}
#endif

// Reject traversal, absolute paths, backslash aliases, drive prefixes, and the
// download itself. Check the full name before copying into a fixed-size path.
static bool archive_path(mz_zip_archive *zip, mz_uint index, char *path, size_t size)
{
    char name[RG_PATH_MAX];
    mz_uint length = mz_zip_reader_get_filename(zip, index, NULL, 0);
    if (length <= 1 || length > sizeof(name) || mz_zip_reader_get_filename(zip, index, name, sizeof(name)) != length) return false;
    if (strlen(name) + 1 != length || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
        if (*c < 32 || *c == 127 || strchr("*?\"<>|", *c)) return false;
    const char *part = name;
    while (*part)
    {
        const char *slash = strchr(part, '/');
        size_t n = slash ? (size_t)(slash - part) : strlen(part);
        if (!n || part[n - 1] == '.' || part[n - 1] == ' ' || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        part += n;
        if (*part) ++part;
    }
    if ((size_t)snprintf(path, size, "%s/%s", RG_STORAGE_ROOT, name) >= size || !strcasecmp(path, ARCHIVE_PATH) || !strcasecmp(path, INSTALL_TEMP_PATH)) return false;
    return true;
}

static size_t install_chunk(void *opaque, mz_uint64 offset, const void *data, size_t size)
{
    (void)offset;
    size_t written = fwrite(data, 1, size, (FILE *)opaque);
    installed_bytes += written;
    if (rg_system_timer() - last_draw > 200000)
        progress(stage, total_bytes ? (int)(installed_bytes * 100 / total_bytes) : 0);
    rg_system_tick(0);
    return written;
}

// The iterator keeps the inflater on the heap. The callback extraction API
// puts ~10 KB of decoder state on the stack, exceeding nested launcher dialogs.
static bool extract_entry(mz_zip_archive *zip, mz_uint index, FILE *fp)
{
    mz_zip_reader_extract_iter_state *iter = mz_zip_reader_extract_iter_new(zip, index, 0);
    if (!iter) return false;
    void *buffer = malloc(16384);
    bool ok = buffer != NULL;
    size_t count;
    while (ok && (count = mz_zip_reader_extract_iter_read(iter, buffer, 16384)) > 0)
        ok = install_chunk(fp, 0, buffer, count) == count;
    // Releasing the iterator validates both uncompressed size and CRC.
    if (!mz_zip_reader_extract_iter_free(iter)) ok = false;
    free(buffer);
    return ok;
}

static bool install_archive(void)
{
    mz_zip_archive zip = {0};
    if (!mz_zip_reader_init_file(&zip, ARCHIVE_PATH, 0)) return false;
    bool ok = false;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    char path[RG_PATH_MAX];
    total_bytes = installed_bytes = 0;
    // Preflight every entry before installing anything. Bound expansion and file
    // count so a corrupt/hostile archive cannot exhaust the card or RAM.
    if (!count || count > 10000) goto done;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!archive_path(&zip, i, path, sizeof(path)) || !mz_zip_reader_file_stat(&zip, i, &st) ||
            !mz_zip_reader_is_file_supported(&zip, i) || mz_zip_reader_is_file_encrypted(&zip, i)) goto done;
        // FAT32 limits each file to 4 GiB minus one byte. Bound the sum
        // without imposing the old 512 MB limit on a whole data bundle.
        if (st.m_uncomp_size > UINT32_MAX ||
            st.m_uncomp_size > (uint64_t)INT64_MAX - (uint64_t)count * 32768 - total_bytes) goto done;
        total_bytes += st.m_uncomp_size;
    }
    if (rg_storage_get_free_space(RG_STORAGE_ROOT) < (int64_t)total_bytes + (int64_t)count * 32768) goto done;
    stage = _("Installing SD files");
    progress(stage, 0);
    for (mz_uint i = 0; i < count; ++i)
    {
        if (!archive_path(&zip, i, path, sizeof(path))) goto done;
        if (mz_zip_reader_is_file_a_directory(&zip, i))
        {
            if (!rg_storage_mkdir(path)) goto done;
        }
        else
        {
            char *slash = strrchr(path, '/');
            *slash = 0;
            bool parents = rg_storage_mkdir(path);
            *slash = '/';
            if (!parents) goto done;
            // Verify each replacement before removing the existing destination.
            FILE *fp = fopen(INSTALL_TEMP_PATH, "wb");
            if (!fp) goto done;
            // miniz verifies the uncompressed length and CRC before success.
            bool extracted = extract_entry(&zip, i, fp);
            bool failed = ferror(fp) != 0;
            if (fclose(fp)) failed = true;
            if (!extracted || failed)
            {
                goto done;
            }
            // FatFs rename cannot replace an existing file. Never recursively
            // delete a destination: a conflicting directory must fail safely.
            if (remove(path) != 0 && errno != ENOENT) goto done;
            if (rename(INSTALL_TEMP_PATH, path) != 0) goto done;
        }
        if (rg_system_timer() - last_draw > 200000)
            progress(stage, total_bytes ? (int)(installed_bytes * 100 / total_bytes) : (int)((i + 1) * 100 / count));
        rg_system_tick(0);
    }
    ok = true;
done:
    rg_storage_delete(INSTALL_TEMP_PATH);
    mz_zip_reader_end(&zip);
    return ok;
}

static void sdcard_manager_run(void)
{
    const rg_gui_option_t options[] = {
        {1, _("Format SD card"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Prepare SD card"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, _("Install data"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };
    int action = rg_gui_dialog(_("SdCard Manager"), options, 0);
    if (action < 1 || action > 3) return;
    bool prepare = action == 2;
    bool install_data = action == 3;
    const char *title = install_data ? _("Install data") : _("Prepare SD card");
#ifdef RG_ENABLE_NETWORKING
    char url[512];
    int archive_size = 0;
#endif
    if (prepare || install_data)
    {
#ifdef RG_ENABLE_NETWORKING
        rg_network_t info = rg_network_get_info();
        if (info.state != RG_NETWORK_CONNECTED || info.ap_mode)
        {
            rg_gui_alert(title, _("Connect to Wi-Fi with Internet access first."));
            return;
        }
        stage_started = rg_system_timer();
        progress(_("Finding matching release"), -1);
        const char *asset_name = install_data ? "sdcard_data.zip" : "sdcard_structure.zip";
        archive_result_t result = resolve_archive(asset_name, url, sizeof(url), &archive_size);
        if (result != ARCHIVE_FOUND)
        {
            char message[256];
            if (result == ARCHIVE_TOO_LARGE)
                snprintf(message, sizeof(message), _("%s exceeds the 2 GB download limit."), asset_name);
            else if (result == ARCHIVE_VERSION_UNSUPPORTED)
                snprintf(message, sizeof(message), _("Firmware version %s has no exact release tag. Use a release build."), rg_system_get_app()->version);
            else if (result == ARCHIVE_NOT_FOUND)
                snprintf(message, sizeof(message), _("%s is missing from firmware release %s. Card was not changed."), asset_name, rg_system_get_app()->version);
            else
                snprintf(message, sizeof(message), _("Could not read GitHub release %s. Check Internet access and try again. Card was not changed."), rg_system_get_app()->version);
            rg_gui_alert(title, message);
            return;
        }
#else
        rg_gui_alert(title, _("This build has no networking support."));
        return;
#endif
    }
    if (install_data)
    {
        if (!rg_storage_ready())
        {
            rg_gui_alert(title, _("Insert a formatted SD card and reboot first."));
            return;
        }
        if (!rg_gui_confirm(_("Overwrite existing data?"),
                            _("Files from sdcard_data.zip will replace existing files with the same paths. Other files are kept. Continue?"), false)) return;
    }
    else if (!rg_gui_confirm(_("Erase SD card?"), _("ALL files, games, saves and settings on the SD card will be erased. Continue?"), false)) return;
#ifdef RG_ENABLE_NETWORKING
    webui_stop();
#endif
    media_shutdown();
    gui_save_config();
    rg_settings_commit();
    gui_invalidate();
    applications_forget_lists();
    stage_started = rg_system_timer();
    bool ok = true;
    const char *error = NULL;
    if (!install_data)
    {
        progress(_("Quick formatting SD card"), -1);
        __atomic_store_n(&format_done, 0, __ATOMIC_RELEASE);
        bool started = rg_task_create("sd_format", format_task, NULL, 4096, RG_TASK_PRIORITY_1, RG_TASK_AFFINITY_IO) != NULL;
        if (started)
        {
            while (!__atomic_load_n(&format_done, __ATOMIC_ACQUIRE))
            {
                progress(_("Quick formatting SD card"), -1);
                rg_task_delay(100);
            }
        }
        ok = started && format_ok;
        error = started ? _("Formatting failed. The card may need formatting on a computer.") :
                          _("Could not start formatting. The card was not changed.");
    }
    if (ok && (prepare || install_data))
    {
#ifdef RG_ENABLE_NETWORKING
        progress(_("Downloading SD files"), 0);
        ok = download_archive(url, archive_size);
        error = install_data ? _("Download failed. No data files were installed. Retry Install data.") :
                               _("Download failed. The card was formatted but preparation is incomplete. Retry Prepare SD card.");
        if (ok)
        {
            stage_started = rg_system_timer();
            progress(_("Checking SD archive"), -1);
            ok = install_archive();
            error = install_data ? _("Installation failed. Some files may have been replaced. Retry Install data.") :
                                   _("Installation failed. Preparation is incomplete. Retry Prepare SD card.");
        }
        rg_storage_delete(ARCHIVE_PATH);
#endif
    }
    if (ok) progress(_("Complete"), 100);
    else rg_gui_alert(_("SdCard Manager"), error);
    rg_storage_commit();
    if (rg_gui_confirm(_("Reboot system?"), ok ? _("SD card is ready. Restart the console now?") : _("Restart the console after the failed SD operation?"), true))
        rg_system_restart();
    // Do not resume background card readers until a fresh boot. The old library
    // and ROM lists may refer to files removed or replaced by this operation.
}
void sdcard_manager_show_dialog(void)
{
    rg_system_set_screen_timeout_inhibit(true);
    sdcard_manager_run();
    rg_system_set_screen_timeout_inhibit(false);
}
#else
void sdcard_manager_show_dialog(void) {}
#endif
