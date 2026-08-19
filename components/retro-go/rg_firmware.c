#include "rg_system.h"
#include "rg_firmware.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <strings.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

#define IMAGE_FOOTER_SIZE         256
#define IMAGE_MAGIC               "RG_IMG_0"
#define IMAGE_MAGIC_SIZE          8
#define PARTITION_TABLE_OFFSET    0x8000
#define PARTITION_TABLE_SIZE      0x1000
#define PARTITION_TABLE_DATA_SIZE 0xC00
#define PARTITION_ENTRY_SIZE      32
#define MAX_IMAGE_PARTITIONS      32
#define FLASH_SECTOR_SIZE         0x1000
#define FLASH_CHUNK_SIZE          0x4000

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

static rg_rect_t draw_firmware_message(const char *format, ...)
{
    char buffer[256];
    va_list va;

    va_start(va, format);
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);

    return rg_gui_draw_message_flags(RG_DIALOG_FLAG_ALIGN_CENTER, "%s", buffer);
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

/**
 * CRC the image and compare it against its footer. Returns NULL on success, or the reason it failed.
 *
 * A short read used to be reported as a checksum mismatch, which sends anyone debugging a bad update
 * after the wrong problem; and the two numbers that identify the problem in one step were only ever
 * written to the log. `computed_out` is optional.
 */
static const char *verify_image_crc(FILE *fp, size_t image_size, uint32_t expected_crc, bool show_progress,
                                    uint32_t *computed_out)
{
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint32_t crc = 0;
    size_t remaining = image_size;
    static char read_error[80]; // The message carries the offset, which is the useful part
    const char *error = "Out of memory while verifying.";

    if (!buffer)
        return error;
    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        error = "Could not seek in the image file.";
        goto cleanup;
    }

    if (show_progress)
        rg_display_clear(C_BLACK);

    while (remaining > 0)
    {
        size_t chunk = RG_MIN(remaining, FLASH_CHUNK_SIZE);
        size_t offset = image_size - remaining;

        // One retry, because a single failed read over SPI is usually a glitch rather than a
        // damaged file, and losing a whole update to it would be a poor trade. A file that is
        // genuinely shorter than its directory entry claims fails both times.
        if (fread(buffer, 1, chunk, fp) != chunk)
        {
            RG_LOGW("Read failed at offset %d, retrying", (int)offset);
            clearerr(fp);
            if (fseek(fp, offset, SEEK_SET) != 0 || fread(buffer, 1, chunk, fp) != chunk)
            {
                RG_LOGE("Short read at offset %d of %d", (int)offset, (int)image_size);
                snprintf(read_error, sizeof(read_error),
                         "Could not read the image file\npast %d of %d bytes.", (int)offset,
                         (int)image_size);
                error = read_error;
                goto cleanup;
            }
        }

        crc = rg_crc32(crc, buffer, chunk);
        remaining -= chunk;
        if (show_progress)
            draw_firmware_message("Verifying image...\n%d%%", (int)((image_size - remaining) * 100 / image_size));
    }

    if (crc == expected_crc)
        error = NULL;
    else
        RG_LOGE("Image CRC mismatch: got %08X expected %08X over %d bytes", (int)crc, (int)expected_crc,
                (int)image_size);

cleanup:
    if (computed_out)
        *computed_out = crc;
    free(buffer);
    return error;
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

/**
 * Write one range of the image to flash. Returns NULL on success, or the reason it failed.
 *
 * The reason is returned rather than logged because every one of these used to be a silent
 * `return false`: the install aborted, the caller had nothing to report, and the user was left
 * looking at a screen that said only that the update had failed.
 */
static const char *write_flash_range(FILE *fp, uint32_t file_offset, uint32_t flash_offset, uint32_t size,
                                     const char *label)
{
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST);
    uint32_t written = 0;
    esp_err_t err;

    if (!buffer)
        return "Out of memory while flashing.";
    if ((flash_offset % FLASH_SECTOR_SIZE) != 0 || (size % FLASH_SECTOR_SIZE) != 0)
    {
        RG_LOGE("Unaligned flash range for '%s': offset=%08X size=%08X", label, (int)flash_offset, (int)size);
        free(buffer);
        return "Image layout is not sector aligned.";
    }
    if (flash_range_matches(fp, file_offset, flash_offset, size))
    {
        RG_LOGI("Skipping unchanged range '%s'", label);
        free(buffer);
        return NULL;
    }
    rg_display_clear(C_BLACK);
    // draw_firmware_message("Flashing %s...\nPlease wait", label);

    for (uint32_t erased = 0; erased < size; erased += FLASH_SECTOR_SIZE)
    {
        draw_firmware_message("Erasing %s...\n%d%%", label, (int)(erased * 100 / size));
        err = esp_flash_erase_region(esp_flash_default_chip, flash_offset + erased, FLASH_SECTOR_SIZE);
        if (err != ESP_OK)
        {
            RG_LOGE("Erase failed for '%s': 0x%02X", label, err);
            free(buffer);
            return "Flash erase failed.";
        }
        rg_task_delay(1);
    }

    if (fseek(fp, file_offset, SEEK_SET) != 0)
    {
        free(buffer);
        return "Could not seek in the image file.";
    }

    rg_display_clear(C_BLACK);

    while (written < size)
    {
        size_t chunk = RG_MIN(size - written, FLASH_CHUNK_SIZE);
        if (fread(buffer, 1, chunk, fp) != chunk)
        {
            RG_LOGE("Short read from the image at offset %d", (int)(file_offset + written));
            free(buffer);
            return "Could not read the image file.";
        }

        err = esp_flash_write(esp_flash_default_chip, buffer, flash_offset + written, chunk);
        if (err != ESP_OK)
        {
            RG_LOGE("Write failed for '%s': 0x%02X", label, err);
            free(buffer);
            return "Flash write failed.";
        }

        written += chunk;
        draw_firmware_message("Writing %s...\n%d%%", label, (int)(written * 100 / size));
        rg_task_delay(1);
    }

    rg_display_clear(C_BLACK);
    draw_firmware_message("Flashed %s", label);
    free(buffer);
    return NULL;
}

static bool range_overlaps(const esp_partition_t *partition, uint32_t offset, uint32_t size)
{
    return partition && offset < partition->address + partition->size && offset + size > partition->address;
}

bool rg_firmware_image_pending(const char *path, uint32_t flags)
{
    image_footer_t footer = {0};
    image_partition_t *partitions = NULL;
    const esp_partition_t *running = esp_ota_get_running_partition();
    rg_stat_t stat = rg_storage_stat(path);
    FILE *fp = NULL;
    bool has_factory = false;
    bool has_launcher = false;
    bool pending = false;
    int partition_count;
    size_t image_size;

    if (!stat.is_file || !running)
        return false;

    fp = fopen(path, "rb");
    if (!fp)
        return false;
    if (!read_image_footer(fp, stat.size, &footer))
        goto cleanup;
    if (strcasecmp(footer.target, RG_TARGET_NAME) != 0)
        goto cleanup;

    image_size = stat.size - IMAGE_FOOTER_SIZE;
    if (verify_image_crc(fp, image_size, footer.crc, false, NULL) != NULL)
        goto cleanup;

    partitions = calloc(MAX_IMAGE_PARTITIONS, sizeof(*partitions));
    if (!partitions)
        goto cleanup;

    partition_count = read_partition_table(fp, partitions, MAX_IMAGE_PARTITIONS);
    if (partition_count <= 0)
        goto cleanup;

    for (int i = 0; i < partition_count; i++)
    {
        image_partition_t *part = &partitions[i];
        if (part->type != ESP_PARTITION_TYPE_APP)
            continue;
        has_factory |= part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
        has_launcher |= strcmp(part->label, RG_APP_LAUNCHER) == 0;
    }
    if ((flags & RG_FIRMWARE_REQUIRE_FACTORY) && !has_factory)
        goto cleanup;
    if ((flags & RG_FIRMWARE_REQUIRE_LAUNCHER) && !has_launcher)
        goto cleanup;

    for (int i = 0; i < partition_count && !pending; i++)
    {
        image_partition_t *part = &partitions[i];
        bool inspect = false;

        if (part->type != ESP_PARTITION_TYPE_APP)
            continue;
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)
            inspect = flags & RG_FIRMWARE_UPDATE_FACTORY;
        else
            inspect = flags & RG_FIRMWARE_UPDATE_APPS;
        if (!inspect)
            continue;
        if (range_overlaps(running, part->offset, part->size))
            goto cleanup;
        if (part->offset + part->size > image_size)
            goto cleanup;
        pending = !flash_range_matches(fp, part->offset, part->offset, part->size);
    }

    if (!pending && (flags & RG_FIRMWARE_UPDATE_PARTITION_TABLE))
    {
        if (PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE > image_size)
            goto cleanup;
        pending = !flash_range_matches(fp, PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
    }
    if (!pending && (flags & RG_FIRMWARE_UPDATE_BOOTLOADER))
    {
        if (BOOTLOADER_OFFSET + BOOTLOADER_SIZE > image_size)
            goto cleanup;
        pending = !flash_range_matches(fp, BOOTLOADER_OFFSET, BOOTLOADER_OFFSET, BOOTLOADER_SIZE);
    }

cleanup:
    free(partitions);
    if (fp)
        fclose(fp);
    return pending;
}
#endif

/**
 * Read an image's footer into a one-line description: "name version (target)".
 *
 * Worth showing before an update is applied: a release built for an older partition layout looks
 * exactly like the right file otherwise, and the only visible difference is its version.
 */
bool rg_firmware_image_describe(const char *path, char *out, size_t out_len)
{
    image_footer_t footer = {0};
    rg_stat_t stat = rg_storage_stat(path);
    FILE *fp;

    if (!out || out_len < 8)
        return false;

    out[0] = 0;

    if (!stat.is_file || !(fp = fopen(path, "rb")))
        return false;

    bool success = read_image_footer(fp, stat.size, &footer);
    fclose(fp);

    if (success)
        snprintf(out, out_len, "%s %s (%s)", footer.name, footer.version, footer.target);

    return success;
}

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
    const char *error;
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
    if (strcasecmp(footer.target, RG_TARGET_NAME) != 0)
    {
        rg_gui_alert("Update failed!", "Image target does not match this device.");
        goto cleanup;
    }

    size_t image_size = stat.size - IMAGE_FOOTER_SIZE;

    uint32_t computed_crc = 0;
    if ((error = verify_image_crc(fp, image_size, footer.crc, true, &computed_crc)))
    {
        // The numbers go on screen because they separate the two possible causes in one step: a
        // transfer that corrupted the file gives a different value on every attempt, while a file
        // that matches on a PC but not here points at the image's own footer.
        char message[192];
        snprintf(message, sizeof(message), "%s\n\nComputed %08X\nExpected %08X\nOver %d bytes", error,
                 (int)computed_crc, (int)footer.crc, (int)image_size);
        rg_gui_alert("Update failed!", message);
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
        if ((error = write_flash_range(fp, part->offset, part->offset, part->size, part->label)))
        {
            rg_gui_alert("Update failed!", error);
            goto cleanup;
        }
    }

    if (flags & RG_FIRMWARE_UPDATE_PARTITION_TABLE)
    {
        if ((error = write_flash_range(fp, PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE,
                                       "partition table")))
        {
            rg_gui_alert("Update failed!", error);
            goto cleanup;
        }
    }

    if (flags & RG_FIRMWARE_UPDATE_BOOTLOADER)
    {
        if ((error = write_flash_range(fp, BOOTLOADER_OFFSET, BOOTLOADER_OFFSET, BOOTLOADER_SIZE, "bootloader")))
        {
            rg_gui_alert("Update failed!", error);
            goto cleanup;
        }
    }

    success = true;

cleanup:
    free(partitions);
    fclose(fp);
    return success;
#endif
}

#ifndef ESP_PLATFORM
bool rg_firmware_image_pending(const char *path, uint32_t flags)
{
    (void)path;
    (void)flags;
    return false;
}
#endif
