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
/**
 * How much is read from the card (and written to flash) at a time.
 *
 * One `fread` of this size becomes one CMD18 multi-block read covering that many sectors, and the
 * whole burst is lost if the card glitches anywhere inside it. 4 KB is one flash sector, which is
 * also the erase granularity, so nothing downstream wants a bigger unit. This used to be 16 KB,
 * which made each read four times as long a window to survive and four times as expensive to retry.
 */
#define FLASH_CHUNK_SIZE          0x1000
/* A failed read is retried this many times; see the comment in verify_image_crc(). */
#define READ_RETRY_COUNT          3

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

/**
 * Read exactly `length` bytes from `offset`, retrying a short read.
 *
 * Every read here is by absolute offset rather than sequential, because after a failed read the
 * stream position is not defined and continuing from it would silently CRC the wrong bytes.
 *
 * A single failed block read over SPI is usually a recoverable glitch rather than a damaged file,
 * and losing a whole update to one would be a poor trade. The storage layer already stops the
 * card's open-ended transfer and waits for it to report ready before retrying (see
 * sdcard_recover() in rg_storage.c), so a failure that reaches this function has genuinely failed
 * several times over -- but re-seeking and asking once more is nearly free and covers the case
 * where the card just needed a moment. A file that is genuinely shorter than its directory entry
 * claims fails every attempt.
 */
static bool read_exact(FILE *fp, long offset, void *buffer, size_t length)
{
    for (int attempt = 0; attempt <= READ_RETRY_COUNT; ++attempt)
    {
        if (attempt > 0)
        {
            RG_LOGW("Read of %d bytes at offset %d failed, retry %d/%d", (int)length, (int)offset, attempt,
                    READ_RETRY_COUNT);
            clearerr(fp);
            rg_task_delay(10);
        }
        if (fseek(fp, offset, SEEK_SET) == 0 && fread(buffer, 1, length, fp) == length)
            return true;
    }
    RG_LOGE("Could not read %d bytes at offset %d", (int)length, (int)offset);
    return false;
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
    uint8_t *data = rg_alloc(IMAGE_FOOTER_SIZE, MEM_FAST | MEM_NOPANIC);
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
 * Remembers the last image that passed verification.
 *
 * An update reads the image several times over: the factory app asks whether an update is pending
 * (which CRCs the whole file), then installs it (which CRCs it again), and each pass is another
 * few megabytes of card reads and another chance to hit a transfer glitch. Identity is the path
 * plus the size and mtime, so replacing the file invalidates the entry.
 */
static struct
{
    char path[RG_PATH_MAX + 1];
    size_t size;
    time_t mtime;
    bool valid;
} verified_image;

static bool image_already_verified(const char *path, const rg_stat_t *stat)
{
    return verified_image.valid && verified_image.size == stat->size && verified_image.mtime == stat->mtime &&
           strcmp(verified_image.path, path) == 0;
}

static void remember_verified_image(const char *path, const rg_stat_t *stat)
{
    snprintf(verified_image.path, sizeof(verified_image.path), "%s", path);
    verified_image.size = stat->size;
    verified_image.mtime = stat->mtime;
    verified_image.valid = true;
}

static void forget_verified_image(void)
{
    verified_image.valid = false;
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
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST | MEM_NOPANIC);
    uint32_t crc = 0;
    size_t remaining = image_size;
    static char read_error[80]; // The message carries the offset, which is the useful part
    const char *error;
    int last_percent = -1;

    if (!buffer)
        return "Out of memory while verifying.";

    if (show_progress)
        rg_display_clear(C_BLACK);

    while (remaining > 0)
    {
        size_t chunk = RG_MIN(remaining, FLASH_CHUNK_SIZE);
        size_t offset = image_size - remaining;

        if (!read_exact(fp, offset, buffer, chunk))
        {
            RG_LOGE("Short read at offset %d of %d", (int)offset, (int)image_size);
            snprintf(read_error, sizeof(read_error), "Could not read the image file\npast %d of %d bytes.",
                     (int)offset, (int)image_size);
            error = read_error;
            goto cleanup;
        }

        crc = rg_crc32(crc, buffer, chunk);
        remaining -= chunk;

        // Tells the system monitor we are alive. Without it the watchdog decides the app has hung
        // three seconds in and starts drawing "App unresponsive" over the progress dialog -- and
        // holding MENU at that point kills the update.
        rg_system_tick(0);

        // Only when the number on screen would actually change. At 4 KB a chunk this loop runs
        // ~2000 times for a 8 MB image, and redrawing the dialog every time costs more than the
        // verification itself (and on the boards that share one SPI bus between the card and the
        // screen, it is contending with the very reads it is reporting on).
        if (show_progress)
        {
            int percent = (int)((image_size - remaining) * 100 / image_size);
            if (percent != last_percent)
            {
                draw_firmware_message("Verifying image...\n%d%%", percent);
                last_percent = percent;
            }
        }
    }

    if (crc == expected_crc)
    {
        error = NULL;
    }
    else
    {
        // Note this was the bug that made every genuine checksum mismatch report itself as an
        // out-of-memory failure: `error` was pre-loaded with that string and never reassigned here.
        RG_LOGE("Image CRC mismatch: got %08X expected %08X over %d bytes", (int)crc, (int)expected_crc,
                (int)image_size);
        error = "The image failed its checksum.";
    }

cleanup:
    if (computed_out)
        *computed_out = crc;
    free(buffer);
    return error;
}

static int read_partition_table(FILE *fp, image_partition_t *partitions, int max_partitions)
{
    uint8_t *table = rg_alloc(PARTITION_TABLE_SIZE, MEM_FAST | MEM_NOPANIC);
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
    uint8_t *file_buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST | MEM_NOPANIC);
    uint8_t *flash_buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST | MEM_NOPANIC);
    uint32_t checked = 0;
    bool matches = false;

    if (!file_buffer || !flash_buffer)
        goto cleanup;

    while (checked < size)
    {
        size_t chunk = RG_MIN(size - checked, FLASH_CHUNK_SIZE);
        if (!read_exact(fp, file_offset + checked, file_buffer, chunk))
            goto cleanup;
        if (esp_flash_read(esp_flash_default_chip, flash_buffer, flash_offset + checked, chunk) != ESP_OK)
            goto cleanup;
        if (memcmp(file_buffer, flash_buffer, chunk) != 0)
            goto cleanup;
        checked += chunk;
        rg_system_tick(0);
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
    uint8_t *buffer = rg_alloc(FLASH_CHUNK_SIZE, MEM_FAST | MEM_NOPANIC);
    uint32_t written = 0;
    int last_percent = -1;
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
        int percent = (int)(erased * 100 / size);
        if (percent != last_percent)
        {
            draw_firmware_message("Erasing %s...\n%d%%", label, percent);
            last_percent = percent;
        }
        err = esp_flash_erase_region(esp_flash_default_chip, flash_offset + erased, FLASH_SECTOR_SIZE);
        if (err != ESP_OK)
        {
            RG_LOGE("Erase failed for '%s': 0x%02X", label, err);
            free(buffer);
            return "Flash erase failed.";
        }
        rg_system_tick(0);
        rg_task_delay(1);
    }

    rg_display_clear(C_BLACK);
    last_percent = -1;

    while (written < size)
    {
        size_t chunk = RG_MIN(size - written, FLASH_CHUNK_SIZE);
        if (!read_exact(fp, file_offset + written, buffer, chunk))
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
        int percent = (int)(written * 100 / size);
        if (percent != last_percent)
        {
            draw_firmware_message("Writing %s...\n%d%%", label, percent);
            last_percent = percent;
        }
        rg_system_tick(0);
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
    const char *error;
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

    // Every read below is on the card, and the whole check takes seconds per image. Nothing in
    // here polls the gamepad, so without this the screen dims and then blanks partway through
    // with no way for the user to bring it back.
    rg_system_set_screen_timeout_inhibit(true);

    if (!read_image_footer(fp, stat.size, &footer))
        goto cleanup;
    if (strcasecmp(footer.target, RG_TARGET_NAME) != 0)
        goto cleanup;

    image_size = stat.size - IMAGE_FOOTER_SIZE;
    if (image_already_verified(path, &stat))
    {
        RG_LOGI("'%s' already passed verification, not re-reading it", path);
    }
    else if ((error = verify_image_crc(fp, image_size, footer.crc, false, NULL)) != NULL)
    {
        // Say why. This used to fail silently, so an image that was present but corrupt was
        // indistinguishable from no image at all ("No update available") to anyone looking at
        // the screen.
        RG_LOGE("'%s' failed verification: %s", path, error);
        forget_verified_image();
        goto cleanup;
    }
    else
    {
        remember_verified_image(path, &stat);
    }

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
    rg_system_set_screen_timeout_inhibit(false);
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

    // Held for the whole install. Erasing and writing several megabytes of flash takes minutes,
    // and none of it reads the gamepad, so the screen would otherwise dim after 30 seconds and
    // switch off 10 seconds later -- in the middle of a firmware write, looking exactly like a
    // device that has died.
    rg_system_set_screen_timeout_inhibit(true);

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

    // The caller (the factory app) usually just verified this exact file to decide it had an
    // update to apply. Reading all of it a second time buys nothing and is another few megabytes
    // of card transfers to survive.
    if (image_already_verified(path, &stat))
    {
        RG_LOGI("'%s' already passed verification, skipping the second pass", path);
    }
    else
    {
        uint32_t computed_crc = 0;
        if ((error = verify_image_crc(fp, image_size, footer.crc, true, &computed_crc)))
        {
            // The numbers go on screen because they separate the two possible causes in one step: a
            // transfer that corrupted the file gives a different value on every attempt, while a file
            // that matches on a PC but not here points at the image's own footer.
            char message[192];
            snprintf(message, sizeof(message), "%s\n\nComputed %08X\nExpected %08X\nOver %d bytes", error,
                     (int)computed_crc, (int)footer.crc, (int)image_size);
            forget_verified_image();
            rg_gui_alert("Update failed!", message);
            goto cleanup;
        }
        remember_verified_image(path, &stat);
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
    rg_system_set_screen_timeout_inhibit(false);
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
