#include "legacy.h"

bool RenderFlag = true;
static legacy_core_t *core;
static rg_surface_t *surfaces[2], *current;

void *legacy_read_file(const char *path, size_t *size, size_t limit)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long length = ftell(fp);
    if (length <= 0 || (size_t)length > limit || fseek(fp, 0, SEEK_SET) != 0)
    { fclose(fp); return NULL; }
    void *data = rg_alloc(length, MEM_SLOW);
    bool ok = data && fread(data, 1, length, fp) == (size_t)length;
    fclose(fp);
    if (!ok) { free(data); return NULL; }
    *size = length;
    return data;
}

static bool screenshot(const char *path, int width, int height)
{
    return rg_surface_save_image_file(current, path, width, height);
}

static void event(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW) rg_display_submit(current, 0);
}

void app_main(void)
{
    rg_app_t *app = rg_system_init(32000, NULL, NULL);
    if (!strcmp(app->configNs, "spectrum")) core = &spectrum_core;
    else if (!strcmp(app->configNs, "a26")) core = &stella_core;
    else if (!strcmp(app->configNs, "a78")) core = &prosystem_core;
    else RG_PANIC("Unknown legacy emulator namespace");
    const rg_handlers_t handlers = {
        .loadState = core->load, .saveState = core->save, .reset = core->reset,
        .screenshot = screenshot, .event = event, .options = core->options,
    };
    app = rg_system_reinit(core->sample_rate, &handlers, NULL);
    core->init(app->romPath);
    surfaces[0] = rg_surface_create(core->width, core->height, RG_PIXEL_565_LE, MEM_SLOW);
    surfaces[1] = rg_surface_create(core->width, core->height, RG_PIXEL_565_LE, MEM_SLOW);
    RG_ASSERT(surfaces[0] && surfaces[1], "Video allocation failed");
    current = surfaces[0];
    rg_surface_fill(current, NULL, 0);
    rg_surface_fill(surfaces[1], NULL, 0);
    if (app->bootFlags & RG_BOOT_RESUME) rg_emu_load_state(app->saveSlot);
    // rg_system_apply_saved_overclock();
    rg_system_set_tick_rate(core->refresh_rate);
    app->frameskip = 0;
    unsigned frame = 0;
    while (true) {
        uint32_t keys = rg_input_read_gamepad();
        if (keys & RG_KEY_MENU) rg_gui_game_menu();
        else if (keys & RG_KEY_OPTION) rg_gui_options_menu();
        int64_t start = rg_system_timer();
        unsigned skip = rg_system_get_frameskip();
        RenderFlag = frame++ % (skip + 1) == 0;
        rg_surface_t *next = current == surfaces[0] ? surfaces[1] : surfaces[0];
        // Do not overwrite a surface still being transferred or wait behind a
        // slow display update before generating the next frame's audio.
        if (RenderFlag) RenderFlag = rg_display_sync(false);
        core->step(keys, next);
        if (RenderFlag) {
            current = next;
            rg_display_submit(current, 0);
        }
        rg_system_tick(rg_system_timer() - start);
    }
}
