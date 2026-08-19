#include "rg_system.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RG_STORAGE_SDSPI_HOST)
#include <driver/sdspi_host.h>
#define SDCARD_DO_TRANSACTION sdspi_host_do_transaction
#elif defined(RG_STORAGE_SDMMC_HOST)
#include <driver/sdmmc_host.h>
#define SDCARD_DO_TRANSACTION sdmmc_host_do_transaction
#endif

#ifdef ESP_PLATFORM
#include <esp_vfs_fat.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <windows.h>
#define access _access
#define mkdir(A, B) mkdir(A)
#if defined(__MINGW32__)
#include <dirent.h>
#endif
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define SDMMC_CMD_STOP_TRANSMISSION     12
#define SDMMC_CMD_SEND_STATUS           13
#define SDMMC_CMD_READ_SINGLE_BLOCK     17
#define SDMMC_CMD_READ_MULTIPLE_BLOCK   18
#define SDMMC_CMD_WRITE_SINGLE_BLOCK    24
#define SDMMC_CMD_WRITE_MULTIPLE_BLOCK  25

#define SDCARD_MAX_RETRIES              3
#define SDCARD_RECOVERY_TIMEOUT_MS      1000

/**
 * File reads are split into chunks of this size, each retried on its own.
 *
 * One `fread` becomes one multi-block transfer, so an unbounded read hands the card a burst that
 * can span megabytes -- and a glitch anywhere in it loses the whole thing. Bounding the burst
 * bounds what a retry has to repeat. 16 KB is small enough for that and large enough that the
 * per-command overhead stays under the noise floor for ROM loading.
 */
#define FILE_IO_CHUNK_SIZE              0x4000
#define FILE_IO_RETRIES                 3

/* How many files may be open on the storage at once; see the note at the mount config. */
#define RG_STORAGE_MAX_OPEN_FILES       8

static bool disk_mounted = false;
#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static sdmmc_card_t *card_handle = NULL;
#endif
#if defined(RG_STORAGE_FLASH_PARTITION)
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
#endif

#define CHECK_PATH(path)          \
    if (!(path && path[0]))       \
    {                             \
        RG_LOGE("No path given"); \
        return false;             \
    }

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static bool sdcard_is_data_cmd(uint32_t opcode)
{
    return opcode == SDMMC_CMD_READ_SINGLE_BLOCK || opcode == SDMMC_CMD_READ_MULTIPLE_BLOCK ||
           opcode == SDMMC_CMD_WRITE_SINGLE_BLOCK || opcode == SDMMC_CMD_WRITE_MULTIPLE_BLOCK;
}

static bool sdcard_is_write_cmd(uint32_t opcode)
{
    return opcode == SDMMC_CMD_WRITE_SINGLE_BLOCK || opcode == SDMMC_CMD_WRITE_MULTIPLE_BLOCK;
}

static bool sdcard_is_multi_block_cmd(uint32_t opcode)
{
    return opcode == SDMMC_CMD_READ_MULTIPLE_BLOCK || opcode == SDMMC_CMD_WRITE_MULTIPLE_BLOCK;
}

static bool sdcard_is_transient_error(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC;
}

/**
 * Put the card back in a known state after a failed data transfer, so that retrying is worthwhile.
 *
 * CMD18/CMD25 are open ended: the card keeps streaming (or accepting) blocks until CMD12 tells it
 * to stop. Every error path inside the SPI host returns without sending that CMD12, so after a
 * glitch the card is still in the middle of the old transfer. Re-issuing the failed command then
 * reads the tail of that stream instead of a command response, which is why a single recoverable
 * glitch used to poison every following read and surface as a file that "stops" at a random offset.
 * Doing it in the right order -- stop, wait for ready, then retry -- is what every hardened SPI SD
 * driver does (see carlk3/no-OS-FatFS-SD, sd_read_blocks) and what the SD spec asks for.
 *
 * Only ever called for data commands, which means the card is initialized and in the transfer
 * state; running this during the mount handshake would confuse the initialization sequence.
 */
static void sdcard_recover(int slot, uint32_t timeout_ms, bool multi_block, bool is_write)
{
    // How long we are willing to wait for the card to say it is ready again. A read only has to
    // stop streaming, which is immediate; a write may still be programming a block, which the
    // spec allows to take a while. Keeping the read case short matters because a card that has
    // been physically removed fails this way on every transfer, and a full timeout per attempt
    // would turn "card pulled out" into seconds of stall per read.
    const uint32_t ready_budget_ms = is_write ? timeout_ms : RG_MIN(timeout_ms, 250u);

#if defined(RG_STORAGE_SDSPI_HOST)
    // Only the SPI host needs this. The SDMMC host tags multi block transfers with an "auto stop"
    // flag, so its hardware issues CMD12 itself whether the transfer succeeded or not and the card
    // is never left mid-stream.
    if (multi_block)
    {
        sdmmc_command_t stop = {
            .opcode = SDMMC_CMD_STOP_TRANSMISSION,
            .arg = 0,
            .flags = SCF_CMD_AC | SCF_RSP_R1B,
            .timeout_ms = timeout_ms,
        };
        // A card that had already finished rejects the command, which is harmless: all we need is
        // for it to be stopped by the time we return.
        esp_err_t err = SDCARD_DO_TRANSACTION(slot, &stop);
        RG_LOGD("SD Card CMD12 during recovery returned 0x%x", err);
        (void)err;
    }
#else
    (void)multi_block;
#endif

    // Then poll until the card answers a status request. This is both the "is it done yet" check
    // and a resync: the response to CMD13 is the first correctly framed byte after the abort.
    int64_t deadline = rg_system_timer() + (int64_t)ready_budget_ms * 1000;
    do
    {
        sdmmc_command_t status = {
            .opcode = SDMMC_CMD_SEND_STATUS,
            .arg = card_handle ? ((uint32_t)card_handle->rca << 16) : 0,
            .flags = SCF_CMD_AC | SCF_RSP_R1,
            .timeout_ms = timeout_ms,
        };
        if (SDCARD_DO_TRANSACTION(slot, &status) == ESP_OK)
            return;
        rg_task_delay(2);
    } while (rg_system_timer() < deadline);

    RG_LOGW("SD Card did not report ready during recovery");
}

static esp_err_t sdcard_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    rg_indicator_t indicator = RG_INDICATOR_ACTIVITY_DISK;
    if (cmdinfo->opcode == SDMMC_CMD_READ_SINGLE_BLOCK || cmdinfo->opcode == SDMMC_CMD_READ_MULTIPLE_BLOCK)
        indicator = RG_INDICATOR_ACTIVITY_DISK_READ;
    else if (cmdinfo->opcode == SDMMC_CMD_WRITE_SINGLE_BLOCK || cmdinfo->opcode == SDMMC_CMD_WRITE_MULTIPLE_BLOCK)
        indicator = RG_INDICATOR_ACTIVITY_DISK_WRITE;

    rg_system_set_indicator(indicator, 1);

    esp_err_t ret = SDCARD_DO_TRANSACTION(slot, cmdinfo);

    // The initial mount handshake already retries on these same transient errors
    // (see rg_storage_init below), but every transaction after that -- including the
    // ones FatFs issues internally while walking directory clusters during readdir(),
    // or while reading/writing file data -- previously went through unretried. A single
    // glitchy block read/write here used to surface as silent, undiagnosable truncation
    // or corruption further up the stack (e.g. a directory listing that stops early with
    // no error, or a short file read/write).
    if (sdcard_is_transient_error(ret))
    {
        const bool is_data = sdcard_is_data_cmd(cmdinfo->opcode);
        const bool is_multi = sdcard_is_multi_block_cmd(cmdinfo->opcode);
        const bool is_write = sdcard_is_write_cmd(cmdinfo->opcode);
        const uint32_t timeout_ms = cmdinfo->timeout_ms ?: SDCARD_RECOVERY_TIMEOUT_MS;

        for (int attempt = 1; attempt <= SDCARD_MAX_RETRIES && ret != ESP_OK; ++attempt)
        {
            RG_LOGW("SD Card transaction failed (op=%d, 0x%x), retry %d/%d...\n", (int)cmdinfo->opcode, ret, attempt,
                    SDCARD_MAX_RETRIES);
            // Without this the retry talks over a card that is still streaming the failed transfer
            // and is essentially guaranteed to fail too.
            if (is_data)
                sdcard_recover(slot, timeout_ms, is_multi, is_write);
            else
                rg_task_delay(2);
            ret = SDCARD_DO_TRANSACTION(slot, cmdinfo);
        }
        if (ret != ESP_OK)
            RG_LOGE("SD Card transaction failed after %d retries (op=%d, 0x%x)\n", SDCARD_MAX_RETRIES,
                    (int)cmdinfo->opcode, ret);
        else if (is_data)
            RG_LOGI("SD Card recovered after a failed transfer (op=%d)\n", (int)cmdinfo->opcode);
    }
    else if (ret != ESP_OK && sdcard_is_data_cmd(cmdinfo->opcode))
    {
        // Not retriable, but the card can still be left mid-stream by an aborted multi block
        // transfer. Stop it anyway so the *next* unrelated read isn't collateral damage.
        if (sdcard_is_multi_block_cmd(cmdinfo->opcode))
            sdcard_recover(slot, cmdinfo->timeout_ms ?: SDCARD_RECOVERY_TIMEOUT_MS, true,
                           sdcard_is_write_cmd(cmdinfo->opcode));
    }

    rg_system_set_indicator(indicator, 0);
    return ret;
}
#endif

void rg_storage_init(void)
{
    RG_ASSERT(!disk_mounted, "Storage already initialized!");
    int error_code = -1;

#if defined(RG_STORAGE_SDSPI_HOST)

    RG_LOGI("Looking for SD Card using SDSPI...");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_GPIO_SDSPI_MOSI,
        .miso_io_num = RG_GPIO_SDSPI_MISO,
        .sclk_io_num = RG_GPIO_SDSPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t err = spi_bus_initialize(RG_STORAGE_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) // check but do not abort, let esp_vfs_fat_sdspi_mount decide
        RG_LOGW("SPI bus init failed (0x%x)", err);

    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = RG_STORAGE_SDSPI_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDSPI_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = RG_STORAGE_SDSPI_HOST;
    slot_config.gpio_cs = RG_GPIO_SDSPI_CS;

    // If we're using esp-idf >= 5.0 and the SPI bus is not shared, we must keep the SD card selected
    // to work around slow accesses. (https://github.com/espressif/esp-idf/issues/10493)
    #ifdef RG_STORAGE_SDSPI_HOLD_CS
    gpio_set_direction(slot_config.gpio_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(slot_config.gpio_cs, 0);
    slot_config.gpio_cs = GPIO_NUM_NC;
    #endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        // The launcher alone can have several of these open at once (the media library scanner,
        // the artwork worker, the audio reader and whatever the UI is doing), and running out
        // surfaces to the caller as a plain "could not open". Each slot costs about half a KB.
        .max_files = RG_STORAGE_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
    };

    err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at probing speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        for (int attempt = 1; attempt <= 3 && err != ESP_OK; ++attempt)
        {
            rg_task_delay(100);
            err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
            if (err != ESP_OK)
                RG_LOGW("SD Card probing attempt %d failed (0x%x)", attempt, err);
        }
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_SDMMC_HOST)

    RG_LOGI("Looking for SD Card using SDMMC...");

    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.flags = SDMMC_HOST_FLAG_1BIT;
    host_config.slot = RG_STORAGE_SDMMC_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDMMC_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = RG_GPIO_SDSPI_CLK;
    slot_config.cmd = RG_GPIO_SDSPI_CMD;
    slot_config.d0 = RG_GPIO_SDSPI_D0;
    // d1 and d3 normally not used in width=1 but sdmmc_host_init_slot saves them, so just in case
    slot_config.d1 = slot_config.d3 = -1;
#endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        // The launcher alone can have several of these open at once (the media library scanner,
        // the artwork worker, the audio reader and whatever the UI is doing), and running out
        // surfaces to the caller as a plain "could not open". Each slot costs about half a KB.
        .max_files = RG_STORAGE_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at probing speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        for (int attempt = 1; attempt <= 3 && err != ESP_OK; ++attempt)
        {
            rg_task_delay(100);
            err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
            if (err != ESP_OK)
                RG_LOGW("SD Card probing attempt %d failed (0x%x)", attempt, err);
        }
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_USBOTG_HOST)

    #warning "USB OTG isn't available on your SOC"
    RG_LOGI("Looking for USB mass storage...");
    error_code = -1;

#elif !defined(RG_STORAGE_FLASH_PARTITION)

    RG_LOGI("Using host (stdlib) for storage.");
    // Maybe we should just check if RG_STORAGE_ROOT exists?
    error_code = 0;

#endif

#if defined(RG_STORAGE_FLASH_PARTITION)

    if (error_code) // only if no previous storage was successfully mounted already
    {
        RG_LOGI("Looking for an internal flash partition labelled '%s' to mount for storage...", RG_STORAGE_FLASH_PARTITION);

        esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = true, // if mount failed, it's probably because it's a clean install so the partition hasn't been formatted yet
            .max_files = RG_STORAGE_MAX_OPEN_FILES, // must be initialized, otherwise it will be 0, which doesn't make sense, and will trigger an ESP_ERR_NO_MEM error
        };

        esp_err_t err = esp_vfs_fat_spiflash_mount(RG_STORAGE_ROOT, RG_STORAGE_FLASH_PARTITION, &mount_config, &wl_handle);
        error_code = (int)err;
    }

#endif

    disk_mounted = !error_code;

    if (disk_mounted)
        RG_LOGI("Storage mounted at %s.", RG_STORAGE_ROOT);
    else
        RG_LOGE("Storage mounting failed! err=0x%x", error_code);
}

void rg_storage_deinit(void)
{
    if (!disk_mounted)
        return;

    rg_storage_commit();

    int error_code = 0;

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
    if (card_handle != NULL)
    {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(RG_STORAGE_ROOT, card_handle);
        card_handle = NULL; // NULL it regardless of success, nothing we can do on errors...
        error_code = (int)err;
    }
#endif

#if defined(RG_STORAGE_FLASH_PARTITION)
    if (wl_handle != WL_INVALID_HANDLE)
    {
        esp_err_t err = esp_vfs_fat_spiflash_unmount(RG_STORAGE_ROOT, wl_handle);
        wl_handle = WL_INVALID_HANDLE;
        error_code = (int)err;
    }
#endif

    if (error_code)
        RG_LOGE("Storage unmounting failed. err=0x%x", error_code);
    else
        RG_LOGI("Storage unmounted.");

    disk_mounted = false;
}

bool rg_storage_ready(void)
{
    return disk_mounted;
}

void rg_storage_commit(void)
{
    if (!disk_mounted)
        return;
    // flush buffers();
}

bool rg_storage_mkdir(const char *dir)
{
    CHECK_PATH(dir);

    if (mkdir(dir, 0777) == 0)
        return true;

    // FIXME: Might want to stat to see if it's a dir
    if (errno == EEXIST)
        return true;

    // Possibly missing some parents, try creating them
    char *temp = strdup(dir);
    for (char *p = temp + strlen(RG_STORAGE_ROOT) + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (strlen(temp) > 0)
            {
                mkdir(temp, 0777);
            }
            *p = '/';
            while (*(p + 1) == '/')
                p++;
        }
    }
    free(temp);

    // Finally try again
    if (mkdir(dir, 0777) == 0)
        return true;

    return false;
}

static int delete_cb(const rg_scandir_t *file, void *arg)
{
    rg_storage_delete(file->path);
    return RG_SCANDIR_CONTINUE;
}

bool rg_storage_delete(const char *path)
{
    CHECK_PATH(path);

    // Try the fast way first
    if (remove(path) == 0 || rmdir(path) == 0)
        return true;

    // If that fails, it's likely a non-empty directory and we go recursive
    // (errno could confirm but it has proven unreliable across platforms...)
    if (rg_storage_scandir(path, delete_cb, NULL, 0))
        return rmdir(path) == 0;

    return false;
}

rg_stat_t rg_storage_stat(const char *path)
{
    rg_stat_t ret = {0};
    struct stat statbuf;
    if (path && stat(path, &statbuf) == 0)
    {
        ret.basename = rg_basename(path);
        ret.extension = rg_extension(path);
        ret.size = statbuf.st_size;
        ret.mtime = statbuf.st_mtime;
        ret.is_file = S_ISREG(statbuf.st_mode);
        ret.is_dir = S_ISDIR(statbuf.st_mode);
        ret.exists = true;
    }
    return ret;
}

bool rg_storage_exists(const char *path)
{
    CHECK_PATH(path);
    return access(path, F_OK) == 0;
}

bool rg_storage_scandir(const char *path, rg_scandir_cb_t *callback, void *arg, uint32_t flags)
{
    CHECK_PATH(path);
    uint32_t types = flags & (RG_SCANDIR_FILES | RG_SCANDIR_DIRS);
    size_t path_len = strlen(path) + 1;
    struct stat statbuf;
    struct dirent *ent;

    if (path_len > RG_PATH_MAX - 5)
    {
        RG_LOGE("Folder path too long '%s'", path);
        return false;
    }

    DIR *dir = opendir(path);
    if (!dir)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Opendir failed (%d): '%s'", errno, path);
        return false;
    }

    // We allocate on heap in case we go recursive through rg_storage_delete
    rg_scandir_t *result = calloc(1, sizeof(rg_scandir_t));
    if (!result)
    {
        RG_LOGE("Memory allocation failed: '%s'", path);
        closedir(dir);
        return false;
    }

    strcat(strcpy(result->path, path), "/");
    result->basename = result->path + path_len;
    result->dirname = path;

    // readdir() returns NULL both at legitimate end-of-directory and on a real
    // read error (e.g. a transient SD card glitch mid-listing) -- errno is the only
    // way to tell those apart, and it must be cleared right before every call since
    // a successful call isn't guaranteed to reset it.
    for (;;)
    {
        errno = 0;
        ent = readdir(dir);
        if (!ent)
        {
            if (errno != 0)
                RG_LOGW("readdir() error (%d) while scanning '%s', listing may be incomplete\n", errno, path);
            break;
        }

        if (ent->d_name[0] == '.' && (!ent->d_name[1] || ent->d_name[1] == '.'))
        {
            // Skip self and parent
            continue;
        }

        if (path_len + strlen(ent->d_name) >= RG_PATH_MAX)
        {
            RG_LOGE("File path too long '%s/%s'", path, ent->d_name);
            continue;
        }

        strcpy((char *)result->basename, ent->d_name);
    #if defined(DT_REG) && defined(DT_DIR)
        result->is_file = ent->d_type == DT_REG;
        result->is_dir = ent->d_type == DT_DIR;
    #else
        result->is_file = 0;
        result->is_dir = 0;
        // We're forced to stat() if the OS doesn't provide type via dirent
        flags |= RG_SCANDIR_STAT;
    #endif

        if ((flags & RG_SCANDIR_STAT) && stat(result->path, &statbuf) == 0)
        {
            result->is_file = S_ISREG(statbuf.st_mode);
            result->is_dir = S_ISDIR(statbuf.st_mode);
            result->size = statbuf.st_size;
            result->mtime = statbuf.st_mtime;
        }

        if ((result->is_dir && types != RG_SCANDIR_FILES) || (result->is_file && types != RG_SCANDIR_DIRS))
        {
            int ret = (callback)(result, arg);

            if (ret == RG_SCANDIR_STOP)
                break;

            if (ret == RG_SCANDIR_SKIP)
                continue;
        }

        if ((flags & RG_SCANDIR_RECURSIVE) && result->is_dir)
        {
            rg_storage_scandir(result->path, callback, arg, flags);
        }
    }

    closedir(dir);
    free(result);

    return true;
}

int64_t rg_storage_get_free_space(const char *path)
{
    // Here we should translate the provided VFS path to the matching filesystem driver and drive
    // But we don't. Instead we just assume it's drive 0 of the fatfs driver. Yay laziness.
#ifdef ESP_PLATFORM
    DWORD nclst;
    FATFS *fatfs;
    if (f_getfree("0:", &nclst, &fatfs) == FR_OK)
    {
        return (int64_t)nclst * fatfs->csize * fatfs->ssize;
    }
#endif

    return -1;
}

/**
 * Read exactly `length` bytes starting at `offset`, in bounded chunks, retrying each one.
 *
 * Reads are addressed absolutely rather than sequentially because the stream position is not
 * defined after a failed read: continuing from wherever it ended up would silently return the
 * wrong bytes rather than an error. A file that is genuinely shorter than its directory entry
 * claims, or a card that is really gone, fails every attempt and is reported with the offset it
 * stopped at -- which used to be the one piece of information a caller never got.
 */
static bool file_read_at(FILE *fp, long offset, void *buffer, size_t length, const char *path)
{
    uint8_t *dest = buffer;
    size_t done = 0;

    while (done < length)
    {
        size_t chunk = RG_MIN(length - done, (size_t)FILE_IO_CHUNK_SIZE);
        bool ok = false;

        for (int attempt = 0; attempt <= FILE_IO_RETRIES && !ok; ++attempt)
        {
            if (attempt > 0)
            {
                RG_LOGW("Read at offset %d of '%s' failed, retry %d/%d", (int)(offset + done), path, attempt,
                        FILE_IO_RETRIES);
                clearerr(fp);
                rg_task_delay(10);
            }
            ok = fseek(fp, offset + (long)done, SEEK_SET) == 0 && fread(dest + done, 1, chunk, fp) == chunk;
        }

        if (!ok)
        {
            RG_LOGE("Read of '%s' stopped at %d of %d bytes (errno %d)", path, (int)(done), (int)length, errno);
            return false;
        }

        done += chunk;
    }

    return true;
}

bool rg_storage_read_file(const char *path, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(path);

    size_t output_buffer_alloc_size;
    size_t output_buffer_size;
    void *output_buffer;
    size_t file_size;

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Fopen failed (%d): '%s'", errno, path);
        return false;
    }

    long probed_size = (fseek(fp, 0, SEEK_END) == 0) ? ftell(fp) : -1;
    if (probed_size < 0)
    {
        RG_LOGE("Could not determine the size of '%s' (%d)", path, errno);
        fclose(fp);
        return false;
    }
    file_size = (size_t)probed_size;

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_alloc_size = *data_len;
        output_buffer_size = RG_MIN(*data_len, file_size);
        output_buffer = *data_out;
    }
    else
    {
        size_t blocksize = RG_MAX(0x400, (flags & 0xF) * 0x2000);
        output_buffer_alloc_size = (file_size + (blocksize - 1)) & ~(blocksize - 1);
        output_buffer_size = file_size;
        output_buffer = malloc(output_buffer_alloc_size);
    }

    if (!output_buffer)
    {
        RG_LOGE("Memory allocation failed: '%s'", path);
        fclose(fp);
        return false;
    }

    if (output_buffer_size && !file_read_at(fp, 0, output_buffer, output_buffer_size, path))
    {
        fclose(fp);
        if (!(flags & RG_FILE_USER_BUFFER))
            free(output_buffer);
        return false;
    }

    fclose(fp);

    // Wipe the extra allocated space, if any
    if (output_buffer_alloc_size > output_buffer_size)
    {
        memset(output_buffer + output_buffer_size, 0, output_buffer_alloc_size - output_buffer_size);
    }

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;
}

bool rg_storage_write_file(const char *path, const void *data_ptr, size_t data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_ptr || !data_len);
    CHECK_PATH(path);

    char temp_path[RG_PATH_MAX + 1];
    const char *target = path;

    // RG_FILE_ATOMIC_WRITE has been part of this function's documented contract, and half a dozen
    // callers pass it, but nothing implemented it: every write went straight at the target, so an
    // interrupted one (power loss, card pulled, card full) left the settings/playlist/cache file
    // truncated instead of leaving the previous version intact.
    bool atomic = (flags & RG_FILE_ATOMIC_WRITE) && (strlen(path) + 4 <= RG_PATH_MAX);

    if ((flags & RG_FILE_ATOMIC_WRITE) && !atomic)
        RG_LOGW("Path too long to write atomically, writing in place: '%s'", path);

    if (atomic)
    {
        snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
        target = temp_path;
    }

    FILE *fp = fopen(target, "wb");
    if (!fp)
    {
        RG_LOGE("Fopen failed (%d): '%s'", errno, target);
        return false;
    }

    bool success = !data_len || fwrite(data_ptr, 1, data_len, fp) == data_len;

    if (!success)
        RG_LOGE("Fwrite failed (%d): '%s'", errno, target);
    if (ferror(fp))
        success = false;

    // fclose is where buffered data finally reaches the card, so it is also where a full card or a
    // dying one shows up. Not checking it reported a successful write for a file that never landed.
    if (fclose(fp) != 0)
    {
        RG_LOGE("Fclose failed (%d): '%s'", errno, target);
        success = false;
    }

    if (atomic)
    {
        // FatFs' rename refuses to overwrite an existing name, so the old file has to go first.
        // Losing power in that window leaves the complete new data in the .tmp file, which is a
        // recoverable state; what must never happen is the target being left half written.
        if (success)
        {
            remove(path);
            if (rename(temp_path, path) != 0)
            {
                RG_LOGE("Rename failed (%d): '%s' -> '%s'", errno, temp_path, path);
                success = false;
            }
        }
        if (!success)
            remove(temp_path);
    }

    return success;
}

/**
 * This is a minimal UNZIP implementation that utilizes only the miniz primitives found in ESP32's ROM.
 * I think that we should use miniz' ZIP API instead and bundle miniz with retro-go. But first I need
 * to do some testing to determine if the increased executable size is acceptable...
 */
#if RG_ZIP_SUPPORT

#if defined(ESP_PLATFORM) && ESP_IDF_VERSION_MAJOR < 5
#include <rom/miniz.h>
#else
#include <miniz.h>
#endif

#define ZIP_MAGIC 0x04034b50
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t modified_time;
    uint16_t modified_date;
    uint32_t checksum;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_size;
    uint16_t extra_field_size;
    uint8_t filename[226];
    // uint8_t extra_field[];
    // uint8_t compressed_data[];
} zip_header_t;

bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(zip_path);

    zip_header_t header = {0};
    int header_pos = 0;

    FILE *fp = fopen(zip_path, "rb");
    if (!fp)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Fopen failed (%d): '%s'", errno, zip_path);
        return false;
    }

    // Very inefficient, we should read a block at a time and search it for a header. But I'm lazy.
    // Thankfully the header is usually found on the very first read :)
    for (header_pos = 0; !feof(fp) && header_pos < 0x10000; ++header_pos)
    {
        fseek(fp, header_pos, SEEK_SET);
        fread(&header, sizeof(header), 1, fp);
        if (header.magic == ZIP_MAGIC)
            break;
    }

    if (header.magic != ZIP_MAGIC)
    {
        RG_LOGE("No valid header found: '%s'", zip_path);
        fclose(fp);
        return false;
    }

    // Zero terminate or truncate filename just in case
    header.filename[RG_MIN(header.filename_size, 225)] = 0;

    RG_LOGI("Found file at %d, name: '%s', size: %d", header_pos, header.filename, (int)header.uncompressed_size);

    size_t stream_offset = header_pos + 30 + header.filename_size + header.extra_field_size;
    size_t stream_remaining = header.compressed_size;
    size_t output_buffer_align = RG_MAX(0x1000, (flags & 0xF) * 0x2000);
    size_t output_buffer_size;
    size_t output_buffer_pos = 0;
    uint8_t *output_buffer = NULL;

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_size = RG_MIN(*data_len, header.uncompressed_size);
        output_buffer = *data_out;
    }
    else
    {
        output_buffer_size = header.uncompressed_size;
        output_buffer = malloc((output_buffer_size + (output_buffer_align - 1)) & ~(output_buffer_align - 1));
    }

    size_t read_buffer_size = 0x8000;
    uint8_t *read_buffer = malloc(read_buffer_size);
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));

    if (!read_buffer || !output_buffer || !decomp)
    {
        RG_LOGE("Memory allocation failed: '%s'", zip_path);
        goto _fail;
    }

    tinfl_status status;
    tinfl_init(decomp);

    do
    {
        size_t input_size = RG_MIN(read_buffer_size, stream_remaining);
        size_t output_size = output_buffer_size - output_buffer_pos;
        if (!file_read_at(fp, stream_offset, read_buffer, input_size, zip_path))
            goto _fail;
        stream_offset += input_size;
        stream_remaining -= input_size;
        status = tinfl_decompress(
            decomp, read_buffer, &input_size, output_buffer, output_buffer + output_buffer_pos, &output_size,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (stream_remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        output_buffer_pos += output_size;
    } while (status == TINFL_STATUS_NEEDS_MORE_INPUT);

    // With user-provided buffer we might not reach TINFL_STATUS_DONE, but it doesn't mean we've failed
    if (status < TINFL_STATUS_DONE || output_buffer_pos != output_buffer_size) // (status != TINFL_STATUS_DONE)
    {
        RG_LOGE("Decompression failed (%d): %s", (int)status, zip_path);
        goto _fail;
    }

    free(read_buffer);
    free(decomp);
    fclose(fp);

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;

_fail:
    if (!(flags & RG_FILE_USER_BUFFER))
        free(output_buffer);
    free(read_buffer);
    free(decomp);
    fclose(fp);
    return false;
}
#else
bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_LOGE("ZIP support hasn't been enabled!");
    return false;
}
#endif
