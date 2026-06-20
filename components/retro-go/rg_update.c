#include "rg_system.h"
#include "rg_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <strings.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

#define IMG_FOOTER_MAGIC "RG_IMG_0"
#define IMG_FOOTER_SIZE 256
#define IMG_PART_TABLE_OFFSET 0x8000
#define IMG_PART_TABLE_SIZE 0xC00
#define IMG_PART_ENTRY_SIZE 32
#define IMG_MAX_PARTITIONS 32
#define IMG_BUFFER_SIZE (16 * 1024)

typedef struct
{
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    char label[17];
} image_part_t;

typedef struct
{
    size_t file_size;
    size_t image_size;
    char name[29];
    char version[29];
    char target[29];
    uint32_t timestamp;
    uint32_t crc;
    image_part_t parts[IMG_MAX_PARTITIONS];
    size_t parts_count;
} image_info_t;

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

#ifdef ESP_PLATFORM
static bool read_image_info(const char *path, image_info_t *info, bool verify_crc)
{
    FILE *fp = fopen(path, "rb");
    uint8_t footer[IMG_FOOTER_SIZE];
    uint8_t table[IMG_PART_TABLE_SIZE];
    bool ok = false;
    bool alerted = false;

    RG_ASSERT_ARG(path && info);
    memset(info, 0, sizeof(*info));

    if (!fp)
    {
        rg_gui_alert(_("OTA update failed!"), _("File open failed!"));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
        goto cleanup;

    long file_size = ftell(fp);
    if (file_size <= IMG_FOOTER_SIZE)
        goto cleanup;

    info->file_size = (size_t)file_size;
    info->image_size = info->file_size - IMG_FOOTER_SIZE;

    if (fseek(fp, (long)info->image_size, SEEK_SET) != 0 || fread(footer, 1, sizeof(footer), fp) != sizeof(footer))
        goto cleanup;

    if (memcmp(footer, IMG_FOOTER_MAGIC, 8) != 0)
    {
        rg_gui_alert(_("OTA update failed!"), _("Invalid image file!"));
        alerted = true;
        goto cleanup;
    }

    memcpy(info->name, footer + 8, 28);
    memcpy(info->version, footer + 36, 28);
    memcpy(info->target, footer + 64, 28);
    info->timestamp = read_le32(footer + 92);
    info->crc = read_le32(footer + 96);

    if (strcasecmp(info->target, RG_TARGET_NAME) != 0)
    {
        rg_gui_alert(_("OTA update failed!"), _("Image target does not match this device!"));
        alerted = true;
        goto cleanup;
    }

    if (verify_crc)
    {
        uint8_t *buffer = malloc(IMG_BUFFER_SIZE);
        size_t remaining = info->image_size;
        uint32_t crc = 0;

        if (!buffer)
        {
            rg_gui_alert(_("OTA update failed!"), _("Out of memory!"));
            alerted = true;
            goto cleanup;
        }

        rg_gui_draw_message(_("Verifying image..."));
        fseek(fp, 0, SEEK_SET);

        while (remaining > 0)
        {
            size_t chunk = RG_MIN(remaining, IMG_BUFFER_SIZE);
            if (fread(buffer, 1, chunk, fp) != chunk)
            {
                free(buffer);
                goto cleanup;
            }
            crc = rg_crc32(crc, buffer, chunk);
            remaining -= chunk;
        }
        free(buffer);

        if (crc != info->crc)
        {
            rg_gui_alert(_("OTA update failed!"), _("Image checksum failed!"));
            alerted = true;
            goto cleanup;
        }
    }

    if (fseek(fp, IMG_PART_TABLE_OFFSET, SEEK_SET) != 0 || fread(table, 1, sizeof(table), fp) != sizeof(table))
        goto cleanup;

    for (size_t i = 0; i + IMG_PART_ENTRY_SIZE <= sizeof(table); i += IMG_PART_ENTRY_SIZE)
    {
        const uint8_t *entry = table + i;

        if (entry[0] != 0xAA || entry[1] != 0x50)
            break;

        if (info->parts_count >= IMG_MAX_PARTITIONS)
            break;

        image_part_t *part = &info->parts[info->parts_count++];
        part->type = entry[2];
        part->subtype = entry[3];
        part->offset = read_le32(entry + 4);
        part->size = read_le32(entry + 8);
        memcpy(part->label, entry + 12, 16);
        part->label[16] = 0;
    }

    ok = true;

cleanup:
    fclose(fp);
    if (!ok && !alerted)
        rg_gui_alert(_("OTA update failed!"), _("Could not read image metadata!"));
    return ok;
}

static const image_part_t *find_image_app(const image_info_t *info, const char *label)
{
    for (size_t i = 0; i < info->parts_count; ++i)
    {
        const image_part_t *part = &info->parts[i];
        if (part->type == ESP_PARTITION_TYPE_APP && strcmp(part->label, label) == 0)
            return part;
    }
    return NULL;
}

static bool flash_partition(FILE *fp, const image_part_t *src, const esp_partition_t *dest, int index, int total)
{
    uint8_t *buffer = malloc(IMG_BUFFER_SIZE);
    size_t remaining = src->size;
    size_t written = 0;

    if (!buffer)
    {
        rg_gui_alert(_("OTA update failed!"), _("Out of memory!"));
        return false;
    }

    RG_LOGI("Flashing '%s' from image offset 0x%08X (%u bytes)", dest->label, (unsigned)src->offset, (unsigned)src->size);
    rg_gui_draw_message(_("Erasing %s (%d/%d)..."), dest->label, index, total);

    esp_err_t err = esp_partition_erase_range(dest, 0, src->size);
    if (err != ESP_OK)
    {
        RG_LOGE("esp_partition_erase_range('%s') failed: 0x%X", dest->label, err);
        free(buffer);
        rg_gui_alert(_("OTA update failed!"), _("Flash erase failed!"));
        return false;
    }

    if (fseek(fp, src->offset, SEEK_SET) != 0)
    {
        free(buffer);
        rg_gui_alert(_("OTA update failed!"), _("File read failed!"));
        return false;
    }

    while (remaining > 0)
    {
        size_t chunk = RG_MIN(remaining, IMG_BUFFER_SIZE);

        if (fread(buffer, 1, chunk, fp) != chunk)
        {
            free(buffer);
            rg_gui_alert(_("OTA update failed!"), _("File read failed!"));
            return false;
        }

        err = esp_partition_write(dest, written, buffer, chunk);
        if (err != ESP_OK)
        {
            RG_LOGE("esp_partition_write('%s') failed: 0x%X", dest->label, err);
            free(buffer);
            rg_gui_alert(_("OTA update failed!"), _("Flash write failed!"));
            return false;
        }

        written += chunk;
        remaining -= chunk;

        if ((written & 0xFFFF) == 0 || remaining == 0)
            rg_gui_draw_message(_("Flashing %s (%d/%d)\n%u / %u"),
                    dest->label, index, total, (unsigned)written, (unsigned)src->size);
    }

    free(buffer);
    return true;
}

static bool flash_image_apps(const char *path, const char *only_label, const char *skip_label)
{
    image_info_t info;
    FILE *fp = NULL;
    int todo = 0;
    int index = 0;
    bool ok = false;

    if (!read_image_info(path, &info, true))
        return false;

    for (size_t i = 0; i < info.parts_count; ++i)
    {
        image_part_t *src = &info.parts[i];

        if (src->type != ESP_PARTITION_TYPE_APP)
            continue;
        if (only_label && strcmp(src->label, only_label) != 0)
            continue;
        if (skip_label && strcmp(src->label, skip_label) == 0)
            continue;

        const esp_partition_t *dest = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, src->label);
        if (!dest || src->size > dest->size || src->offset + src->size > info.image_size)
        {
            rg_gui_alert(_("OTA update failed!"), _("Partition table is not compatible!"));
            return false;
        }
        todo++;
    }

    if (todo == 0)
    {
        rg_gui_alert(_("OTA update failed!"), _("No app partition to flash!"));
        return false;
    }

    fp = fopen(path, "rb");
    if (!fp)
    {
        rg_gui_alert(_("OTA update failed!"), _("File open failed!"));
        return false;
    }

    for (size_t i = 0; i < info.parts_count; ++i)
    {
        image_part_t *src = &info.parts[i];

        if (src->type != ESP_PARTITION_TYPE_APP)
            continue;
        if (only_label && strcmp(src->label, only_label) != 0)
            continue;
        if (skip_label && strcmp(src->label, skip_label) == 0)
            continue;

        const esp_partition_t *dest = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, src->label);
        if (!flash_partition(fp, src, dest, ++index, todo))
            goto cleanup;
    }

    ok = true;

cleanup:
    fclose(fp);
    return ok;
}

bool rg_update_start_image(const char *path)
{
    image_info_t info;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *candidate = NULL;
    esp_partition_iterator_t it;

    if (!path || !path[0])
        return false;

    if (!read_image_info(path, &info, true))
        return false;

    if (!running)
    {
        rg_gui_alert(_("OTA update failed!"), _("Could not find running partition!"));
        return false;
    }

    it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it)
    {
        const esp_partition_t *part = esp_partition_get(it);
        if (part && strcmp(part->label, running->label) != 0 && find_image_app(&info, part->label))
        {
            candidate = part;
            break;
        }
        it = esp_partition_next(it);
    }
    if (it)
        esp_partition_iterator_release(it);

    if (!candidate)
    {
        rg_gui_alert(_("OTA update failed!"), _("No alternate app partition found!"));
        return false;
    }

    char args[RG_PATH_MAX + 32];
    snprintf(args, sizeof(args), RG_UPDATE_IMG_ARG_PREFIX "1:%s", path);
    rg_gui_draw_message(_("Rebooting to update..."));
    rg_system_switch_app(candidate->label, "rg_update", args, RG_BOOT_ONCE);
}

bool rg_update_handle_pending(const char *args)
{
    const char *payload;
    const esp_partition_t *running;
    char next_args[RG_PATH_MAX + 48];
    char current_label[17];

    if (!args || strncmp(args, RG_UPDATE_IMG_ARG_PREFIX, strlen(RG_UPDATE_IMG_ARG_PREFIX)) != 0)
        return false;

    payload = args + strlen(RG_UPDATE_IMG_ARG_PREFIX);
    running = esp_ota_get_running_partition();
    snprintf(current_label, sizeof(current_label), "%s", running ? running->label : "");

    if (!running)
    {
        rg_gui_alert(_("OTA update failed!"), _("Could not find running partition!"));
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
    }

    if (payload[0] == '1' && payload[1] == ':' && payload[2])
    {
        const char *path = payload + 2;

        if (!flash_image_apps(path, NULL, current_label))
            rg_system_switch_app(current_label, NULL, NULL, 0);

        snprintf(next_args, sizeof(next_args), RG_UPDATE_IMG_ARG_PREFIX "2:%s:%s", current_label, path);
        rg_gui_draw_message(_("Rebooting to finish update..."));
        rg_system_switch_app(RG_APP_LAUNCHER, "rg_update", next_args, RG_BOOT_ONCE);
    }

    if (payload[0] == '2' && payload[1] == ':')
    {
        char label[17];
        const char *path;
        const char *sep = strchr(payload + 2, ':');

        if (!sep || sep == payload + 2)
        {
            rg_gui_alert(_("OTA update failed!"), _("Invalid update state!"));
            rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
        }

        size_t label_len = RG_MIN((size_t)(sep - (payload + 2)), sizeof(label) - 1);
        memcpy(label, payload + 2, label_len);
        label[label_len] = 0;
        path = sep + 1;

        if (strcmp(label, current_label) == 0)
        {
            rg_gui_alert(_("OTA update failed!"), _("Invalid update state!"));
            rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
        }

        if (!flash_image_apps(path, label, NULL))
            rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);

        rg_storage_delete(path);
        rg_gui_alert(_("Update complete!"), _("The system will now restart."));
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
    }

    rg_gui_alert(_("OTA update failed!"), _("Invalid update state!"));
    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}

#else
bool rg_update_start_image(const char *path)
{
    (void)path;
    rg_gui_alert(_("OTA update failed!"), _("OTA flashing is not supported on this platform!"));
    return false;
}

bool rg_update_handle_pending(const char *args)
{
    (void)args;
    return false;
}
#endif
