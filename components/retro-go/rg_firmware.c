#include "rg_system.h"
#include "rg_firmware.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

#define IMAGE_FOOTER_SIZE 256
#define IMAGE_MAGIC "RG_IMG_0"
#define IMAGE_MAGIC_SIZE 8
#define PARTITION_TABLE_OFFSET 0x8000
#define PARTITION_TABLE_SIZE 0x1000
#define PARTITION_TABLE_DATA_SIZE 0xC00
#define PARTITION_ENTRY_SIZE 32
#define MAX_IMAGE_PARTITIONS 32
#define FLASH_SECTOR_SIZE 0x1000
#define FLASH_CHUNK_SIZE 0x4000

#if defined(CONFIG_IDF_TARGET_ESP32)
#define BOOTLOADER_OFFSET 0x1000
#define BOOTLOADER_SIZE   0x7000
#else
#define BOOTLOADER_OFFSET 0x0000
#define BOOTLOADER_SIZE   0x8000
#endif

typedef struct
{
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    char label[17];
} image_partition_t;

typedef struct
{
    char name[29];
    char version[29];
    char target[29];
    uint32_t timestamp;
    uint32_t crc;
} image_footer_t;

static uint16_t read_le16(const uint8_t *data)
{
    return data[0] | (data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

static bool read_exact(FILE *fp, long offset, void *buffer, size_t length)
{
    return fseek(fp, offset, SEEK_SET) == 0 && fread(buffer, 1, length, fp) == length;
}

static bool read_image_footer(FILE *fp, size_t file_size, image_footer_t *footer)
{
    uint8_t *data = rg_alloc(IMAGE_FOOTER_SIZE, MEM_FAST);
    bool success = false;

    if (!data)
        return false;
    if (file_size <= IMAGE_FOOTER_SIZE)
        goto cleanup;
    if (!read_exact(fp, file_size - IMAGE_FOOTER_SIZE, data, IMAGE_FOOTER_SIZE))
        goto cleanup;
    if (memcmp(data, IMAGE_MAGIC, IMAGE_MAGIC_SIZE) != 0)
        goto cleanup;

    memcpy(footer->name, data + 8, 28);
    memcpy(footer->version, data + 36, 28);
    memcpy(footer->target, data + 64, 28);
    footer->name[28] = 0;
    footer->version[28] = 0;
    footer->target[28] = 0;
    footer->timestamp = read_le32(data + 92);
    footer->crc = read_le32(data + 96);
    success = true;

cleanup:
    free(data);
    return success;
}

static bool verify_image_crc(FILE *fp, size_t image_size, uint32_t expected_crc)
{
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint32_t crc = 0;
    size_t remaining = image_size;
    bool success = false;

    if (!buffer)
        return false;
    if (fseek(fp, 0, SEEK_SET) != 0)
        goto cleanup;

    while (remaining > 0)
    {
        size_t chunk = RG_MIN(remaining, FLASH_CHUNK_SIZE);
        if (fread(buffer, 1, chunk, fp) != chunk)
            goto cleanup;

        crc = rg_crc32(crc, buffer, chunk);
        remaining -= chunk;
        rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Verifying image...\n%d%%",
                                  (int)((image_size - remaining) * 100 / image_size));
    }

    success = crc == expected_crc;
    if (!success)
        RG_LOGE("Image CRC mismatch: got %08X expected %08X", (int)crc, (int)expected_crc);

cleanup:
    free(buffer);
    return success;
}

static int read_partition_table(FILE *fp, image_partition_t *partitions, int max_partitions)
{
    uint8_t *table = rg_alloc(PARTITION_TABLE_SIZE, MEM_FAST);
    int count = 0;

    if (!table)
        return -1;
    if (!read_exact(fp, PARTITION_TABLE_OFFSET, table, PARTITION_TABLE_SIZE))
    {
        free(table);
        return -1;
    }

    for (int offset = 0; offset < PARTITION_TABLE_DATA_SIZE; offset += PARTITION_ENTRY_SIZE)
    {
        const uint8_t *entry = table + offset;

        if (entry[0] == 0xEB && entry[1] == 0xEB)
            break;
        if (entry[0] == 0xFF && entry[1] == 0xFF)
            break;
        if (read_le16(entry) != 0x50AA)
        {
            free(table);
            return -1;
        }
        if (count >= max_partitions)
        {
            free(table);
            return -1;
        }

        partitions[count].type = entry[2];
        partitions[count].subtype = entry[3];
        partitions[count].offset = read_le32(entry + 4);
        partitions[count].size = read_le32(entry + 8);
        memcpy(partitions[count].label, entry + 12, 16);
        partitions[count].label[16] = 0;
        count++;
    }

    free(table);
    return count;
}

#ifdef ESP_PLATFORM
static bool flash_range_matches(FILE *fp, uint32_t file_offset, uint32_t flash_offset, uint32_t size)
{
    uint8_t *file_buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint8_t *flash_buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint32_t checked = 0;
    bool matches = false;

    if (!file_buffer || !flash_buffer)
        goto cleanup;
    if (fseek(fp, file_offset, SEEK_SET) != 0)
        goto cleanup;

    while (checked < size)
    {
        size_t chunk = RG_MIN(size - checked, FLASH_CHUNK_SIZE);
        if (fread(file_buffer, 1, chunk, fp) != chunk)
            goto cleanup;
        if (esp_flash_read(esp_flash_default_chip, flash_buffer, flash_offset + checked, chunk) != ESP_OK)
            goto cleanup;
        if (memcmp(file_buffer, flash_buffer, chunk) != 0)
            goto cleanup;
        checked += chunk;
    }

    matches = true;

cleanup:
    free(file_buffer);
    free(flash_buffer);
    return matches;
}

static bool write_flash_range(FILE *fp, uint32_t file_offset, uint32_t flash_offset, uint32_t size, const char *label)
{
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint32_t written = 0;
    esp_err_t err;

    if (!buffer)
        return false;
    if ((flash_offset % FLASH_SECTOR_SIZE) != 0 || (size % FLASH_SECTOR_SIZE) != 0)
    {
        RG_LOGE("Unaligned flash range for '%s': offset=%08X size=%08X", label, (int)flash_offset, (int)size);
        free(buffer);
        return false;
    }
    if (flash_range_matches(fp, file_offset, flash_offset, size))
    {
        RG_LOGI("Skipping unchanged range '%s'", label);
        free(buffer);
        return true;
    }

    rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Flashing %s...\nPlease wait", label);

    for (uint32_t erased = 0; erased < size; erased += FLASH_SECTOR_SIZE)
    {
        rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Erasing %s...\n%d%%", label,
                                  (int)(erased * 100 / size));
        err = esp_flash_erase_region(esp_flash_default_chip, flash_offset + erased, FLASH_SECTOR_SIZE);
        if (err != ESP_OK)
        {
            RG_LOGE("Erase failed for '%s': 0x%02X", label, err);
            free(buffer);
            return false;
        }
        rg_task_delay(1);
    }

    if (fseek(fp, file_offset, SEEK_SET) != 0)
    {
        free(buffer);
        return false;
    }

    while (written < size)
    {
        size_t chunk = RG_MIN(size - written, FLASH_CHUNK_SIZE);
        if (fread(buffer, 1, chunk, fp) != chunk)
        {
            free(buffer);
            return false;
        }

        err = esp_flash_write(esp_flash_default_chip, buffer, flash_offset + written, chunk);
        if (err != ESP_OK)
        {
            RG_LOGE("Write failed for '%s': 0x%02X", label, err);
            free(buffer);
            return false;
        }

        written += chunk;
        rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Writing %s...\n%d%%", label,
                                  (int)(written * 100 / size));
        rg_task_delay(1);
    }

    rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "Flashed %s", label);
    free(buffer);
    return true;
}

static bool range_overlaps(const esp_partition_t *partition, uint32_t offset, uint32_t size)
{
    return partition && offset < partition->address + partition->size && offset + size > partition->address;
}
#endif

bool rg_firmware_install_image(const char *path, uint32_t flags)
{
#ifndef ESP_PLATFORM
    (void)path;
    (void)flags;
    rg_gui_alert("Update failed!", "Firmware updates are only supported on ESP targets.");
    return false;
#else
    image_footer_t footer = {0};
    image_partition_t *partitions = NULL;
    const esp_partition_t *running = esp_ota_get_running_partition();
    rg_stat_t stat = rg_storage_stat(path);
    FILE *fp;
    int partition_count;
    bool success = false;

    if (!stat.is_file)
    {
        rg_gui_alert("Update failed!", "Image file not found.");
        return false;
    }

    fp = fopen(path, "rb");
    if (!fp)
    {
        RG_LOGE("Failed to open '%s': errno=%d", path, errno);
        rg_gui_alert("Update failed!", "Could not open image file.");
        return false;
    }

    if (!read_image_footer(fp, stat.size, &footer))
    {
        rg_gui_alert("Update failed!", "Not a Retro-Go image.");
        goto cleanup;
    }

    size_t image_size = stat.size - IMAGE_FOOTER_SIZE;

    if (!verify_image_crc(fp, image_size, footer.crc))
    {
        rg_gui_alert("Update failed!", "Image checksum failed.");
        goto cleanup;
    }

    partitions = calloc(MAX_IMAGE_PARTITIONS, sizeof(*partitions));
    if (!partitions)
    {
        rg_gui_alert("Update failed!", "Out of memory.");
        goto cleanup;
    }

    partition_count = read_partition_table(fp, partitions, MAX_IMAGE_PARTITIONS);
    if (partition_count <= 0)
    {
        rg_gui_alert("Update failed!", "Invalid partition table.");
        goto cleanup;
    }

    bool has_factory = false;
    bool has_launcher = false;
    for (int i = 0; i < partition_count; i++)
    {
        image_partition_t *part = &partitions[i];
        if (part->type != ESP_PARTITION_TYPE_APP)
            continue;
        has_factory |= part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
        has_launcher |= strcmp(part->label, RG_APP_LAUNCHER) == 0;
    }
    if ((flags & RG_FIRMWARE_REQUIRE_FACTORY) && !has_factory)
    {
        rg_gui_alert("Update failed!", "Image is missing factory app.");
        goto cleanup;
    }
    if ((flags & RG_FIRMWARE_REQUIRE_LAUNCHER) && !has_launcher)
    {
        rg_gui_alert("Update failed!", "Image is missing launcher app.");
        goto cleanup;
    }

    RG_LOGI("Installing %s %s for %s", footer.name, footer.version, footer.target);

    for (int i = 0; i < partition_count; i++)
    {
        image_partition_t *part = &partitions[i];
        bool install = false;

        if (part->type != ESP_PARTITION_TYPE_APP)
            continue;
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)
            install = flags & RG_FIRMWARE_UPDATE_FACTORY;
        else
            install = flags & RG_FIRMWARE_UPDATE_APPS;
        if (!install)
            continue;
        if (range_overlaps(running, part->offset, part->size))
        {
            RG_LOGE("Refusing to write running partition overlap '%s'", part->label);
            rg_gui_alert("Update failed!", "Image layout overlaps running app.");
            goto cleanup;
        }
        if (part->offset + part->size > image_size)
        {
            RG_LOGE("Partition '%s' exceeds image size.", part->label);
            rg_gui_alert("Update failed!", "Image is truncated.");
            goto cleanup;
        }
        if (!write_flash_range(fp, part->offset, part->offset, part->size, part->label))
            goto cleanup;
    }

    if (flags & RG_FIRMWARE_UPDATE_PARTITION_TABLE)
    {
        if (!write_flash_range(fp, PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, "partition table"))
            goto cleanup;
    }

    if (flags & RG_FIRMWARE_UPDATE_BOOTLOADER)
    {
        if (!write_flash_range(fp, BOOTLOADER_OFFSET, BOOTLOADER_OFFSET, BOOTLOADER_SIZE, "bootloader"))
            goto cleanup;
    }

    success = true;

cleanup:
    free(partitions);
    fclose(fp);
    return success;
#endif
}
