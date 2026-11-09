// Copyright-free ARM7 program: mode 3 red framebuffer, then an idle loop.
// Compiled only into explicit diagnostic builds, never a normal image.
#include "../../components/retro-go/drivers/display/p4_ppa.h"
#include "../components/gpsp/core/riscv/rv32_block_runtime.h"

static int find_contra(const rg_scandir_t *file, void *arg)
{
    if (file->is_file && (strstr(file->basename, "Contra") || strstr(file->basename, "contra")) &&
        rg_extension_match(file->basename, "nes zip"))
    {
        strcpy(arg, file->path);
        return RG_SCANDIR_STOP;
    }
    return RG_SCANDIR_CONTINUE;
}

static const char *prepare_selftest(void)
{
    const uint32_t program[] = {
        0xe3a00301, 0xe3a01b01, 0xe3811003, 0xe1c010b0,
        0xe3a00406, 0xe3a0101f, 0xe3a02c96, 0xe0c010b2,
        0xe2522001, 0x1afffffc, 0xeafffffe,
    };
    // Exactly 1 MiB selects the mirrored mini-ROM interpreter path. A 16 MiB
    // synthetic cartridge checks both native RV32 execution and the expanded
    // resident-ROM cache, without using a copyrighted game.
    const size_t rom_size = 16 * 1024 * 1024;
    uint8_t *rom = rg_alloc(rom_size, MEM_SLOW);
    memset(rom, 0, rom_size);
    *(uint32_t *)rom = 0xea00002e; // Branch over the cartridge header.
    memcpy(rom + 0xa0, "RETROGO TEST", 12);
    memcpy(rom + 0xc0, program, sizeof(program));
    const char *path = RG_BASE_PATH_CACHE "/gba-hardware-test.gba";
    // Preserve a completed fixture across diagnostic retries. Its code/header
    // are checked before reuse, so interrupted writes are rebuilt.
    uint8_t header[512];
    rg_stat_t info = rg_storage_stat(path);
    FILE *file = info.size == rom_size ? fopen(path, "rb") : NULL;
    bool reuse = file && fread(header, 1, sizeof(header), file) == sizeof(header) &&
                 memcmp(header, rom, sizeof(header)) == 0;
    if (file) fclose(file);
    RG_LOGI("GBA selftest: %s 16 MiB generated cartridge", reuse ? "reusing" : "writing");
    if (!reuse)
        RG_ASSERT(rg_storage_write_file(path, rom, rom_size, 0), "Selftest ROM write failed");
    RG_LOGI("GBA selftest: cartridge ready");
    free(rom);
    return path;
}

static void finish_selftest(void)
{
    extern bool gamepak_must_swap(void);
    RG_ASSERT(!gamepak_must_swap(), "16 MiB GBA cartridge is still paging from SD");
    uint64_t native_blocks = gpsp_rv32_block_execution_count();
    RG_ASSERT(native_blocks > 0, "GBA RV32 JIT did not execute native blocks");
    RG_LOGI("GBA native RV32 blocks: %llu", (unsigned long long)native_blocks);
    unsigned previous_frames = selftest_video_frames;
    app->frameskipManual = true;
    app->frameskipValue = 1;
    for (int i = 0; i < 12; ++i) retro_run();
    unsigned drawn = selftest_video_frames - previous_frames;
    RG_ASSERT(drawn >= 5 && drawn <= 8, "GBA manual frame-skip binding failed");
    app->frameskipManual = false;
    app->frameskip = 0;
    previous_frames = selftest_video_frames;
    for (int i = 0; i < 12; ++i) retro_run();
    RG_ASSERT(selftest_video_frames - previous_frames >= 10, "GBA frame-skip reset failed");
    // Exercise actual ARM-to-RV32 execution, RGB565 rendering and PSRAM caches.
    uint16_t *pixels = screen->data;
    for (int i = 0; i < 240 * 160; ++i)
        RG_ASSERT(pixels[i] == 0xf800, "GBA ARM7 framebuffer selftest failed");
    const rg_surface_t *scaled = p4_scale_frame(screen, 480, 320);
    RG_ASSERT(scaled, "PPA hardware scaling selftest failed");
    pixels = scaled->data;
    for (int i = 0; i < 480 * 320; ++i)
        RG_ASSERT(pixels[i] == 0xf800, "PPA pixel readback failed");
    p4_scaler_deinit();
    uint32_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    ram[0x1000] = 0x13579bdf;
    const char *state = RG_BASE_PATH_CACHE "/gba-hardware-test.state";
    RG_ASSERT(save_state(state), "GBA state save selftest failed");
    ram[0x1000] = 0;
    RG_ASSERT(load_state(state) && ram[0x1000] == 0x13579bdf, "GBA state restore selftest failed");
    retro_run();
    rg_display_sync(true);
    RG_LOGI("GBA SELFTEST PASS: resident 16 MiB ROM, ARM7/RV32, framebuffer, PPA pixels, frame skip, state round trip");
    retro_unload_game();
    retro_deinit();
    loaded = false;
    rg_storage_delete(state);
    rg_storage_delete(RG_BASE_PATH_CACHE "/gba-hardware-test.gba");
    rg_storage_delete(sram_path);
    char contra[RG_PATH_MAX + 1] = {0};
    rg_storage_scandir(RG_BASE_PATH_ROMS "/nes", find_contra, contra, RG_SCANDIR_FILES | RG_SCANDIR_RECURSIVE);
    if (contra[0])
    {
        RG_LOGI("Continuing NES audio benchmark: %s", contra);
        rg_system_switch_app("retro-core", "nes", contra, RG_BOOT_ONCE);
    }
    rg_system_switch_app("launcher", NULL, NULL, 0);
}
