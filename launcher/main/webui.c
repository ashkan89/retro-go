#include "rg_system.h"
#include "gui.h"
#include "webui.h"

#ifdef RG_ENABLE_NETWORKING
#include <esp_http_server.h>
#include <cJSON.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const unsigned char webui_gzip[];
extern const size_t webui_gzip_size;
static httpd_handle_t server;
static char *http_buffer;
static rg_mutex_t *action_mutex;
static struct { int action, value; int64_t at; char path[RG_PATH_MAX + 1]; } pending;
enum { ACTION_NONE, ACTION_VOLUME, ACTION_BACKLIGHT, ACTION_LOG, ACTION_RESTART, ACTION_INSTALL };
#define TRANSFER_SIZE 8192
#define LIST_LIMIT 128

static esp_err_t error_response(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

// Non-simple headers protect mutations from cross-origin forms/fetches.
// No CORS or OPTIONS handler is provided, so browser preflights are refused.
static bool trusted_request(httpd_req_t *req)
{
    char value[8];
    return httpd_req_get_hdr_value_str(req, "X-RetroGo", value, sizeof(value)) == ESP_OK
        && strcmp(value, "1") == 0;
}

static bool valid_path(const char *path, bool allow_root)
{
    const size_t root = strlen(RG_STORAGE_ROOT);
    size_t length = strlen(path);
    if (length > RG_PATH_MAX - 12 || strncmp(path, RG_STORAGE_ROOT, root)) return false;
    if (length == root) return allow_root;
    if (path[root] != '/' || path[length - 1] == '/') return false;
    for (const char *part = path + root + 1; *part;)
    {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || part[0] == '.' || part[size - 1] == '.' || part[size - 1] == ' ') return false;
        for (size_t i = 0; i < size; ++i)
            if ((unsigned char)part[i] < 32 || strchr("\\:*?\"<>|", part[i])) return false;
        if (size >= 10 && (!strncasecmp(part + size - 10, ".rg-upload", 10)
            || !strncasecmp(part + size - 10, ".rg-backup", 10))) return false;
        if (!end) break;
        part = end + 1;
    }
    return true;
}

static bool request_path(const char *uri, char *path)
{
    size_t n = 0;
    while (*uri && *uri != '?')
    {
        unsigned char value = *uri++;
        if (value == '%')
        {
            if (!uri[0] || !uri[1] || !isxdigit((unsigned char)uri[0]) || !isxdigit((unsigned char)uri[1])) return false;
            char hex[] = {uri[0], uri[1], 0};
            value = strtoul(hex, NULL, 16);
            uri += 2;
        }
        if (!value || n >= RG_PATH_MAX) return false;
        path[n++] = value;
    }
    path[n] = 0;
    return valid_path(path, false);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *text = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!text) return error_response(req, "500 Internal Server Error", "Not enough memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(req, text);
    free(text);
    return result;
}

typedef struct { cJSON *array; int count, offset, visited; bool truncated, failed; } listing_t;
static int add_file(const rg_scandir_t *entry, void *arg)
{
    listing_t *list = arg;
    if (entry->basename[0] == '.' || !valid_path(entry->path, false)) return RG_SCANDIR_CONTINUE;
    if (list->visited++ < list->offset) return RG_SCANDIR_CONTINUE;
    if (list->count == LIST_LIMIT) { list->truncated = true; return RG_SCANDIR_STOP; }
    cJSON *item = cJSON_CreateObject();
    if (!item || !cJSON_AddStringToObject(item, "name", entry->basename)
        || !cJSON_AddNumberToObject(item, "size", entry->size)
        || !cJSON_AddNumberToObject(item, "mtime", entry->mtime)
        || !cJSON_AddBoolToObject(item, "is_dir", entry->is_dir)
        || !cJSON_AddItemToArray(list->array, item))
    {
        cJSON_Delete(item);
        list->failed = true;
        return RG_SCANDIR_ERROR;
    }
    ++list->count;
    return RG_SCANDIR_CONTINUE;
}

static esp_err_t http_status_handler(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return send_json(req, NULL);
    rg_app_t *app = rg_system_get_app();
    rg_stats_t stats = rg_system_get_stats();
    rg_network_t network = rg_network_get_info();
    rg_battery_t battery = rg_input_read_battery();
    cJSON_AddBoolToObject(obj, "success", true);
    cJSON_AddStringToObject(obj, "name", RG_PROJECT_NAME);
    cJSON_AddStringToObject(obj, "target", RG_TARGET_NAME);
    cJSON_AddStringToObject(obj, "version", app->version);
    cJSON_AddStringToObject(obj, "build", app->buildDate);
    cJSON_AddStringToObject(obj, "root", RG_STORAGE_ROOT);
    cJSON_AddNumberToObject(obj, "uptime", stats.uptime);
    cJSON_AddNumberToObject(obj, "heap_free", stats.freeMemoryInt);
    cJSON_AddNumberToObject(obj, "heap_total", stats.totalMemoryInt);
    cJSON_AddNumberToObject(obj, "psram_free", stats.freeMemoryExt);
    cJSON_AddNumberToObject(obj, "psram_total", stats.totalMemoryExt);
    cJSON_AddNumberToObject(obj, "cpu_mhz", rg_system_get_cpu_speed());
    cJSON_AddBoolToObject(obj, "storage_ready", rg_storage_ready());
    cJSON_AddNumberToObject(obj, "storage_free", rg_storage_ready() ? rg_storage_get_free_space(RG_STORAGE_ROOT) : -1);
    cJSON_AddNumberToObject(obj, "volume", rg_audio_get_volume());
    cJSON_AddNumberToObject(obj, "brightness", rg_display_get_backlight());
    cJSON_AddNumberToObject(obj, "log_level", rg_system_get_log_level());
    cJSON_AddStringToObject(obj, "theme", rg_gui_get_theme_name() ?: "default");
    cJSON_AddStringToObject(obj, "ssid", network.name);
    cJSON_AddStringToObject(obj, "ip", network.ip_addr);
    cJSON_AddNumberToObject(obj, "rssi", network.rssi);
    cJSON_AddNumberToObject(obj, "channel", network.channel);
    cJSON_AddNumberToObject(obj, "network_state", network.state);
    cJSON_AddBoolToObject(obj, "ap_mode", network.ap_mode);
    cJSON_AddBoolToObject(obj, "battery_present", battery.present);
    cJSON_AddNumberToObject(obj, "battery", battery.level);
    cJSON_AddNumberToObject(obj, "battery_volts", battery.volts);
    cJSON_AddBoolToObject(obj, "charging", battery.charging);
#ifdef RG_UPDATER_APPLICATION
    cJSON_AddBoolToObject(obj, "can_install", rg_system_have_app(RG_UPDATER_APPLICATION));
#else
    cJSON_AddBoolToObject(obj, "can_install", false);
#endif
    cJSON *apps = cJSON_AddArrayToObject(obj, "apps");
    const char *names[] = {"factory", "launcher", "retro-core", "prboom-go", "gwenesis", "fmsx", "retro-legacy", "gbsp"};
    for (size_t i = 0; apps && i < sizeof(names) / sizeof(names[0]); ++i)
        if (rg_system_have_app(names[i])) cJSON_AddItemToArray(apps, cJSON_CreateString(names[i]));
    return send_json(req, obj);
}

static esp_err_t http_api_handler(httpd_req_t *req)
{
    if (!trusted_request(req)) return error_response(req, "403 Forbidden", "Missing X-RetroGo header");
    if (req->content_len < 2 || req->content_len > 1024)
        return error_response(req, "413 Payload Too Large", "API body must be 2-1024 bytes");
    char body[1025];
    size_t received = 0;
    while (received < req->content_len)
    {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return ESP_FAIL;
        received += count;
    }
    body[received] = 0;
    cJSON *content = cJSON_ParseWithLength(body, received + 1);
    if (!content) return error_response(req, "400 Bad Request", "Invalid JSON");
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "cmd")) ?: "";
    const char *arg1 = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "arg1")) ?: "";
    const char *arg2 = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "arg2")) ?: "";
    cJSON *response = cJSON_CreateObject();
    if (!response) { cJSON_Delete(content); return send_json(req, NULL); }
    bool success = false;
    const char *error = "Operation failed; check the SD card and available space";
    if (!strcmp(cmd, "install"))
    {
#ifdef RG_UPDATER_APPLICATION
        const char *folder = RG_BASE_PATH "/firmware/";
        rg_stat_t image = {0};
        if (valid_path(arg1, false) && !strncmp(arg1, folder, strlen(folder))) image = rg_storage_stat(arg1);
        if (image.is_file && rg_extension_match(arg1, "img") && rg_system_have_app(RG_UPDATER_APPLICATION))
        {
            rg_mutex_take(action_mutex, -1);
            if (!pending.action)
            {
                pending.action = ACTION_INSTALL;
                snprintf(pending.path, sizeof(pending.path), "%s", arg1);
                pending.at = rg_system_timer() + 750000;
                success = true;
            }
            rg_mutex_give(action_mutex);
            error = "Another device action is pending";
        }
        else error = "Select a staged .img in retro-go/firmware; a factory updater partition is required";
#else
        error = "This target does not support on-device firmware installation";
#endif
    }
    else if (!strcmp(cmd, "settings") || !strcmp(cmd, "restart"))
    {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(content, "value");
        int action = !strcmp(cmd, "restart") ? ACTION_RESTART :
            !strcmp(arg1, "volume") ? ACTION_VOLUME : !strcmp(arg1, "brightness") ? ACTION_BACKLIGHT :
            !strcmp(arg1, "log_level") ? ACTION_LOG : ACTION_NONE;
        int minimum = action == ACTION_BACKLIGHT ? 1 : 0;
        int maximum = action == ACTION_LOG ? RG_LOG_MAX - 1 : 100;
        if (action && (action == ACTION_RESTART || (cJSON_IsNumber(value) && isfinite(value->valuedouble)
            && value->valuedouble == value->valueint && value->valueint >= minimum && value->valueint <= maximum)))
        {
            rg_mutex_take(action_mutex, -1);
            if (!pending.action)
            {
                pending.action = action;
                pending.value = value ? value->valueint : 0;
                pending.at = rg_system_timer() + 750000;
                success = true;
            }
            rg_mutex_give(action_mutex);
            error = "Another device action is pending; retry in a moment";
        }
        else error = "Unsupported setting or value";
    }
    else if (!valid_path(arg1, !strcmp(cmd, "list") || !strcmp(cmd, "mkdir")) || (!strcmp(cmd, "rename") && !valid_path(arg2, false)))
        error = "Path must stay inside the SD card; hidden and reserved paths are unavailable";
    else if (!rg_storage_ready()) error = "SD card is not mounted";
    else
    {
        gui.http_lock = true;
        if (!strcmp(cmd, "list"))
        {
            cJSON *offset = cJSON_GetObjectItemCaseSensitive(content, "offset");
            listing_t list = {.array = cJSON_AddArrayToObject(response, "files")};
            bool valid_offset = !offset || (cJSON_IsNumber(offset) && isfinite(offset->valuedouble)
                && offset->valuedouble == offset->valueint && offset->valueint >= 0 && offset->valueint <= 1000000);
            if (valid_offset && offset) list.offset = offset->valueint;
            success = valid_offset && list.array && rg_storage_scandir(arg1, add_file, &list, RG_SCANDIR_STAT) && !list.failed;
            cJSON_AddBoolToObject(response, "truncated", list.truncated);
            cJSON_AddNumberToObject(response, "limit", LIST_LIMIT);
            cJSON_AddNumberToObject(response, "offset", list.offset);
            cJSON_AddNumberToObject(response, "next_offset", list.offset + list.count);
        }
        else if (!strcmp(cmd, "rename"))
        {
            if (strncasecmp(arg1, arg2, strlen(arg1)) == 0 && arg2[strlen(arg1)] == '/') error = "Cannot move a folder inside itself";
            else if (rg_storage_exists(arg2)) error = "Destination already exists";
            else success = rename(arg1, arg2) == 0;
        }
        else if (!strcmp(cmd, "delete")) success = rg_storage_delete(arg1);
        else if (!strcmp(cmd, "mkdir")) success = rg_storage_mkdir(arg1);
        else if (!strcmp(cmd, "touch"))
        {
            if (rg_storage_exists(arg1)) error = "File already exists";
            else { FILE *fp = fopen(arg1, "wb"); success = fp && fclose(fp) == 0; }
        }
        else error = "Unknown command";
        if (success && strcmp(cmd, "list")) gui_invalidate();
        gui.http_lock = false;
    }
    cJSON_AddBoolToObject(response, "success", success);
    if (!success) cJSON_AddStringToObject(response, "error", error);
    cJSON_Delete(content);
    return send_json(req, response);
}

static esp_err_t http_upload_handler(httpd_req_t *req)
{
    char filename[RG_PATH_MAX + 1], temporary[RG_PATH_MAX + 12], backup[RG_PATH_MAX + 12];
    if (!trusted_request(req)) return error_response(req, "403 Forbidden", "Missing X-RetroGo header");
    if (!request_path(req->uri, filename)) return error_response(req, "400 Bad Request", "Invalid SD path");
    if (!rg_storage_ready()) return error_response(req, "503 Service Unavailable", "SD card is not mounted");
    rg_stat_t existing = rg_storage_stat(filename);
    if (existing.exists && !existing.is_file)
        return error_response(req, "409 Conflict", "The destination is a folder, not a file. Choose another filename.");
    bool had_target = rg_storage_exists(filename);
    char overwrite[8];
    bool replace = httpd_req_get_hdr_value_str(req, "X-RetroGo-Overwrite", overwrite, sizeof(overwrite)) == ESP_OK
        && !strcmp(overwrite, "1");
    if (had_target && !replace) return error_response(req, "409 Conflict", "File exists. Choose Replace to confirm, or rename the incoming file.");
    snprintf(backup, sizeof(backup), "%s.rg-backup", filename);
    if (rg_storage_exists(backup)) return error_response(req, "409 Conflict", "A recovery copy exists (.rg-backup). Recover or remove it directly on the SD card before replacing this file.");
    int64_t available = rg_storage_get_free_space(RG_STORAGE_ROOT);
    if (available >= 0 && (uint64_t)req->content_len > (uint64_t)available)
        return error_response(req, "507 Insufficient Storage", "Not enough free space on SD card");
    snprintf(temporary, sizeof(temporary), "%s.rg-upload", filename);
    gui.http_lock = true;
    rg_task_delay(100);
    FILE *fp = fopen(temporary, "wb");
    size_t received = 0;
    bool success = fp != NULL;
    while (success && received < req->content_len)
    {
        size_t remaining = req->content_len - received;
        int count = httpd_req_recv(req, http_buffer, remaining < TRANSFER_SIZE ? remaining : TRANSFER_SIZE);
        if (count <= 0 || fwrite(http_buffer, 1, count, fp) != (size_t)count) { success = false; break; }
        received += count;
        rg_task_yield();
    }
    if (fp && fclose(fp) != 0) success = false;
    bool backed_up = false;
    if (success && had_target) success = backed_up = rename(filename, backup) == 0;
    if (success && rename(temporary, filename) != 0)
    {
        if (backed_up && rename(backup, filename) != 0)
            RG_LOGE("Upload rollback failed; original retained at '%s'", backup);
        success = false;
    }
    if (success && backed_up) remove(backup);
    if (!success) remove(temporary);
    gui.http_lock = false;
    gui_invalidate();
    if (!success) return error_response(req, "500 Internal Server Error", "Upload incomplete; original files were preserved");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t http_download_handler(httpd_req_t *req)
{
    char filename[RG_PATH_MAX + 1];
    if (!request_path(req->uri, filename)) return error_response(req, "400 Bad Request", "Invalid SD path");
    gui.http_lock = true;
    rg_stat_t entry = rg_storage_stat(filename);
    FILE *fp = entry.is_file ? fopen(filename, "rb") : NULL;
    if (!fp) { gui.http_lock = false; return httpd_resp_send_404(req); }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t result = ESP_OK;
    for (size_t count; (count = fread(http_buffer, 1, TRANSFER_SIZE, fp));)
    {
        result = httpd_resp_send_chunk(req, http_buffer, count);
        if (result != ESP_OK) break;
        rg_task_yield();
    }
    if (ferror(fp)) result = ESP_FAIL;
    fclose(fp);
    gui.http_lock = false;
    return result == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : result;
}

static esp_err_t http_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    return httpd_resp_send(req, (const char *)webui_gzip, webui_gzip_size);
}

// Apply controls on the launcher thread after the HTTP response is sent.
void webui_poll(void)
{
    if (!server || gui.http_lock || !rg_mutex_take(action_mutex, 0)) return;
    int action = ACTION_NONE, value = pending.value;
    char image_path[RG_PATH_MAX + 1];
    if (pending.action && rg_system_timer() >= pending.at)
    {
        action = pending.action;
        memcpy(image_path, pending.path, sizeof(image_path));
        pending.action = ACTION_NONE;
    }
    rg_mutex_give(action_mutex);
    switch (action)
    {
        case ACTION_VOLUME: rg_audio_set_volume(value); break;
        case ACTION_BACKLIGHT: rg_display_set_backlight(value); break;
        case ACTION_LOG: rg_system_set_log_level(value); break;
        case ACTION_RESTART: rg_settings_commit(); rg_storage_commit(); rg_system_restart(); break;
#ifdef RG_UPDATER_APPLICATION
        case ACTION_INSTALL:
        {
            // Stop all HTTP storage users before device-side confirmation and verification.
            webui_stop();
            char prompt[RG_PATH_MAX + 160];
            snprintf(prompt, sizeof(prompt), "%s\nKeep external power connected. Confirm to validate and install this image.", image_path);
            if (rg_gui_confirm("Web firmware request", prompt, false)
                && rg_firmware_install_image(image_path, RG_FIRMWARE_STAGE_PREPARE_UPDATE))
                rg_system_switch_app(RG_UPDATER_APPLICATION, NULL, image_path, RG_BOOT_ONCE);
            webui_start();
            gui_invalidate();
            return;
        }
#endif
        default: return;
    }
    rg_settings_commit();
    gui_invalidate();
}

void webui_stop(void)
{
    if (!server) return;
    httpd_stop(server);
    server = NULL;
    free(http_buffer); http_buffer = NULL;
    rg_mutex_free(action_mutex); action_mutex = NULL;
    pending.action = ACTION_NONE;
}

void webui_start(void)
{
    if (server) return;
    http_buffer = malloc(TRANSFER_SIZE);
    action_mutex = rg_mutex_create();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    if (!http_buffer || !action_mutex || httpd_start(&server, &config) != ESP_OK)
    {
        free(http_buffer); http_buffer = NULL;
        if (action_mutex) rg_mutex_free(action_mutex);
        action_mutex = NULL; server = NULL;
        RG_LOGE("Unable to start web manager");
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = http_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = http_status_handler},
        {.uri = "/api", .method = HTTP_POST, .handler = http_api_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = http_download_handler},
        {.uri = "/*", .method = HTTP_PUT, .handler = http_upload_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) { webui_stop(); return; }
    RG_LOGI("Flash-resident web manager started");
}
#endif
