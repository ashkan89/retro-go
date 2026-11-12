// Retro-Go frontend for gpSP: P4 RV32 JIT / S3 interpreter (GPL-3.0-or-later).
#include "rg_system.h"
#include "libretro.h"
#include "gpsp_memory.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "gpsp_rv32_platform.h"
#endif
#include "esp_heap_caps.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef RG_GBA_SELFTEST
#define RG_GBA_SELFTEST 0
#endif

#if CONFIG_IDF_TARGET_ESP32P4
extern const struct gpsp_rv32_platform_ops gpsp_platform;
#endif
static rg_app_t *app;
static rg_surface_t *screen;
static char *sram_path;
static uint32_t keys, sram_hash;
static int64_t last_save;
static bool loaded;
#if RG_GBA_SELFTEST
static unsigned selftest_video_frames;
#endif

static void core_log(enum retro_log_level level, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    RG_LOGI("gpSP: %s", message);
}

static bool environment(unsigned cmd, void *data)
{
    static int last_frameskip = -1;
    static char frameskip_interval[12];
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = RG_BASE_PATH_BIOS; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = RG_BASE_PATH_SAVES; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = core_log; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        {
            int skip = rg_system_get_frameskip();
            *(bool *)data = skip != last_frameskip;
            last_frameskip = skip;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true; return true;
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            struct retro_variable *var = data;
            var->value = NULL;
            if (!strcmp(var->key, "gpsp_sound_rate")) var->value = "32768";
            else if (!strcmp(var->key, "gpsp_bios")) var->value = "auto";
            else if (!strcmp(var->key, "gpsp_boot_mode")) var->value = "game";
            else if (!strcmp(var->key, "gpsp_serial")) var->value = "disabled";
            else if (!strcmp(var->key, "gpsp_rtc_time_source")) var->value = "system";
            else if (!strcmp(var->key, "gpsp_frameskip"))
                var->value = rg_system_get_frameskip() ? "fixed_interval" : "disabled";
            else if (!strcmp(var->key, "gpsp_frameskip_interval"))
            {
                snprintf(frameskip_interval, sizeof(frameskip_interval), "%d", rg_system_get_frameskip());
                var->value = frameskip_interval;
            }
            return var->value != NULL;
        }
        default: return false;
    }
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width != 240 || height != 160)
        return;
#if RG_GBA_SELFTEST
    selftest_video_frames++;
#endif
    for (unsigned y = 0; y < height; ++y)
        memcpy((uint8_t *)screen->data + y * screen->stride, (const uint8_t *)data + y * pitch, width * 2);
    rg_display_submit(screen, 0);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    rg_audio_submit((const rg_audio_frame_t *)data, frames);
    return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
    const rg_audio_frame_t frame = {left, right};
    rg_audio_submit(&frame, 1);
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    static const uint32_t map[] = {
        RG_KEY_B, RG_KEY_Y, RG_KEY_SELECT, RG_KEY_START, RG_KEY_UP, RG_KEY_DOWN,
        RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_A, RG_KEY_X, RG_KEY_L, RG_KEY_R,
    };
    // One emulated GBA has one keypad. P2 is a separate logical controller,
    // not a second handheld or a link-cable connection.
    return port == 0 && index == 0 && device == RETRO_DEVICE_JOYPAD &&
           id < RG_COUNT(map) && (keys & map[id]) ? 1 : 0;
}

static bool save_sram(void)
{
    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!loaded || !data || !size)
        return true;
    uint32_t hash = rg_crc32(0, data, size);
    if (hash == sram_hash)
        return true;
    if (!rg_storage_write_file(sram_path, data, size, RG_FILE_ATOMIC_WRITE))
        return false;
    sram_hash = hash;
    return true;
}

static bool save_state(const char *path)
{
    size_t size = retro_serialize_size();
    void *data = rg_alloc(size, MEM_SLOW | MEM_NOPANIC);
    bool ok = data && retro_serialize(data, size) && rg_storage_write_file(path, data, size, RG_FILE_ATOMIC_WRITE);
    free(data);
    return save_sram() && ok;
}

static bool load_state(const char *path)
{
    void *data = NULL;
    size_t size = 0;
    bool ok = rg_storage_read_file(path, &data, &size, 0) && size == retro_serialize_size() && retro_unserialize(data, size);
    free(data);
    return ok;
}

static bool reset(bool hard) { retro_reset(); return true; }
static bool screenshot(const char *path, int width, int height)
{
    return rg_surface_save_image_file(screen, path, width, height);
}

static void event(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW && screen)
        rg_display_submit(screen, 0);
    if (event == RG_EVENT_SHUTDOWN)
        save_sram();
}

#if RG_GBA_SELFTEST
#include "selftest.h"
#endif

void app_main(void)
{
    const rg_handlers_t handlers = {
        .saveState = save_state, .loadState = load_state, .reset = reset,
        .screenshot = screenshot, .event = event,
    };
    app = rg_system_init(32768, &handlers, NULL);
    const int runtime_tick_timeout = app->tickTimeout;
    // Large SPI cartridges and ZIP extraction legitimately exceed the normal
    // 3-second responsiveness timeout. Restore it after refreshing the tick.
    app->tickTimeout = 300000000;
#if RG_GBA_SELFTEST
    app->frameskip = 0;
    app->frameskipManual = false;
#endif
    RG_ASSERT(RG_GBA_SELFTEST || (app->romPath && app->romPath[0]), "Select a GBA ROM from the launcher");
    rg_gui_draw_message(_("Loading game..."));
    screen = rg_surface_create(240, 160, RG_PIXEL_565_LE, MEM_SLOW);
    RG_ASSERT(screen, "Unable to allocate GBA display");
    memset(screen->data, 0, screen->stride * screen->height);
    sram_path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    char *save_dir = strdup(sram_path);
    if (save_dir)
    {
        char *slash = strrchr(save_dir, '/');
        if (slash) *slash = 0;
        if (!rg_storage_mkdir(save_dir))
            RG_LOGW("Unable to create GBA save directory: %s", save_dir);
        free(save_dir);
    }
    const char *rom_path = app->romPath;
#if RG_GBA_SELFTEST
    rom_path = prepare_selftest();
    free(sram_path);
    sram_path = strdup(RG_BASE_PATH_CACHE "/gba-hardware-test.sram");
#endif
    if (rg_extension_match(rom_path, "zip"))
    {
        void *rom = NULL;
        size_t size = 0;
        RG_ASSERT(rg_storage_unzip_file(rom_path, NULL, &rom, &size, 0),
                  "Cannot unpack GBA ZIP; extract large ROMs to a .gba file first");
        bool ok = rg_storage_write_file(RG_BASE_PATH_CACHE "/gba-unpacked.gba", rom, size, RG_FILE_ATOMIC_WRITE);
        free(rom);
        RG_ASSERT(ok, "Unable to cache unpacked GBA ROM");
        rom_path = RG_BASE_PATH_CACHE "/gba-unpacked.gba";
    }
#if CONFIG_IDF_TARGET_ESP32P4
    gpsp_rv32_set_platform(&gpsp_platform);
#endif
    // Budget after static core storage and the screen, using free PSRAM rather
    // than internal RAM. Release unused ROM blocks and page larger games.
    const size_t mib = 1024 * 1024;
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#if CONFIG_IDF_TARGET_ESP32P4
    // Leave room for both 2 MiB JIT arenas, core state, savestates and UI.
    const unsigned reserve_mib = 8, max_rom_mib = 24;
#else
    // No JIT arenas on S3; leave room for core state, savestates and UI.
    const unsigned reserve_mib = 3, max_rom_mib = 4;
    RG_ASSERT(free_psram >= (reserve_mib + 1) * mib,
              "GBA needs at least 4 MiB free PSRAM");
#endif
    unsigned rom_mib = free_psram > reserve_mib * mib
        ? RG_MIN(max_rom_mib, (free_psram - reserve_mib * mib) / mib) : 1;
    RG_ASSERT(gpsp_set_rom_buffer_limit(rom_mib), "Invalid GBA ROM memory budget");
    RG_LOGI("GBA ROM cache budget: %u MiB, reserving %u MiB for runtime", rom_mib, reserve_mib);
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();
    const struct retro_game_info game = {.path = rom_path};
    RG_LOGI("Loading GBA cartridge: %s", rom_path);
    RG_ASSERT(retro_load_game(&game), "Unable to load GBA ROM");
    RG_LOGI("GBA cartridge loaded");
    loaded = true;
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    rg_audio_set_sample_rate(av.timing.sample_rate);
    rg_system_set_tick_rate((int)(av.timing.fps + 0.5));
    void *data = NULL;
    size_t size = 0, capacity = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (sram && rg_storage_read_file(sram_path, &data, &size, 0))
    {
        if (size <= capacity)
            memcpy(sram, data, size);
        free(data);
    }
    sram_hash = sram ? rg_crc32(0, sram, capacity) : 0;
    if (!RG_GBA_SELFTEST && (app->bootFlags & RG_BOOT_RESUME))
        rg_emu_load_state(app->saveSlot);
    last_save = rg_system_timer();
    rg_system_tick(0);
    app->tickTimeout = RG_GBA_SELFTEST ? 300000000 : runtime_tick_timeout;
#if RG_GBA_SELFTEST
    RG_LOGI("GBA selftest: running 120 ARM7/RV32 frames");
    for (int i = 0; i < 120; ++i)
    {
        retro_run();
        rg_system_tick(0);
    }
    finish_selftest();
#endif
    while (true)
    {
        keys = rg_input_read_gamepad_player(RG_PLAYER_1);
        if (keys & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            save_sram();
            if (keys & RG_KEY_MENU) rg_gui_game_menu();
            else rg_gui_options_menu();
            keys = 0;
        }
        int64_t start = rg_system_timer();
        retro_run();
        rg_system_tick(rg_system_timer() - start);
        if (rg_system_timer() - last_save > 10000000)
        {
            save_sram();
            last_save = rg_system_timer();
        }
    }
}
