#include <rg_system.h>
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
    uint8_t data[IMAGE_FOOTER_SIZE];

    if (file_size <= IMAGE_FOOTER_SIZE)
        return false;
    if (!read_exact(fp, file_size - IMAGE_FOOTER_SIZE, data, sizeof(data)))
        return false;
    if (memcmp(data, IMAGE_MAGIC, IMAGE_MAGIC_SIZE) != 0)
        return false;

    memcpy(footer->name, data + 8, 28);
    memcpy(footer->version, data + 36, 28);
    memcpy(footer->target, data + 64, 28);
    footer->name[28] = 0;
    footer->version[28] = 0;
    footer->target[28] = 0;
    footer->timestamp = read_le32(data + 92);
    footer->crc = read_le32(data + 96);
    return true;
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
        rg_gui_draw_message("Verifying image...\n%d%%", (int)((image_size - remaining) * 100 / image_size));
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
    uint8_t table[PARTITION_TABLE_SIZE];
    int count = 0;

    if (!read_exact(fp, PARTITION_TABLE_OFFSET, table, sizeof(table)))
        return -1;

    for (int offset = 0; offset < PARTITION_TABLE_DATA_SIZE; offset += PARTITION_ENTRY_SIZE)
    {
        const uint8_t *entry = table + offset;

        if (entry[0] == 0xEB && entry[1] == 0xEB)
            break;
        if (entry[0] == 0xFF && entry[1] == 0xFF)
            break;
        if (read_le16(entry) != 0x50AA)
            return -1;
        if (count >= max_partitions)
            return -1;

        partitions[count].type = entry[2];
        partitions[count].subtype = entry[3];
        partitions[count].offset = read_le32(entry + 4);
        partitions[count].size = read_le32(entry + 8);
        memcpy(partitions[count].label, entry + 12, 16);
        partitions[count].label[16] = 0;
        count++;
    }

    return count;
}

#ifdef ESP_PLATFORM
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

    rg_gui_draw_message("Erasing %s...", label);
    err = esp_flash_erase_region(esp_flash_default_chip, flash_offset, size);
    if (err != ESP_OK)
    {
        RG_LOGE("Erase failed for '%s': 0x%02X", label, err);
        free(buffer);
        return false;
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
        rg_gui_draw_message("Writing %s...\n%d%%", label, (int)(written * 100 / size));
    }

    free(buffer);
    return true;
}
#endif

static bool install_image(const char *path)
{
#ifndef ESP_PLATFORM
    (void)path;
    rg_gui_alert("Update failed!", "Factory updates are only supported on ESP targets.");
    return false;
#else
    image_footer_t footer = {0};
    image_partition_t partitions[32];
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

    partition_count = read_partition_table(fp, partitions, RG_COUNT(partitions));
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
    if (!has_factory || !has_launcher)
    {
        rg_gui_alert("Update failed!", "Image is missing required apps.");
        goto cleanup;
    }

    RG_LOGI("Installing %s %s for %s", footer.name, footer.version, footer.target);

    if (!write_flash_range(fp, BOOTLOADER_OFFSET, BOOTLOADER_OFFSET, BOOTLOADER_SIZE, "bootloader"))
        goto cleanup;
    if (!write_flash_range(fp, PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, "partition table"))
        goto cleanup;

    for (int i = 0; i < partition_count; i++)
    {
        image_partition_t *part = &partitions[i];

        if (part->type != ESP_PARTITION_TYPE_APP)
            continue;
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)
        {
            RG_LOGI("Skipping factory partition '%s'", part->label);
            continue;
        }
        if (running && part->offset < running->address + running->size && part->offset + part->size > running->address)
        {
            RG_LOGW("Skipping running partition overlap '%s'", part->label);
            continue;
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

    success = true;

cleanup:
    fclose(fp);
    return success;
#endif
}

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

    if (rg_gui_confirm("Firmware update", image_path, true) && install_image(image_path))
    {
        rg_gui_alert("Update complete", "Rebooting to launcher.");
        rg_storage_delete(image_path);
    }

    rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
