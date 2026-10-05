#include "legacy.h"
#include "Cartridge.h"
#include "Database.h"
#include "ProSystem.h"
#include "Maria.h"
#include "Palette.h"
#include "Tia.h"
#include "Pokey.h"

static uint8_t input[17] = {[15] = 1};
static uint16_t palette[256];
static rg_audio_sample_t audio[640];

static void init(const char *path)
{
    size_t size;
    void *data = legacy_read_file(path, &size, 1024 * 1024);
    RG_ASSERT(data, "Unable to load Atari 7800 ROM");
    bool ok = size >= 128 && cartridge_Load(data, size);
    free(data);
    RG_ASSERT(ok, "Invalid Atari 7800 cartridge");
    database_Load(cartridge_digest);
    prosystem_Reset();
    prosystem_core.refresh_rate = prosystem_frequency;
    prosystem_core.height = maria_visibleArea.bottom - maria_visibleArea.top + 1;
    RG_ASSERT(prosystem_core.height > 0 && prosystem_core.height <= 292, "Invalid Atari 7800 display area");
    for (int i = 0; i < 256; i++) {
        const uint8_t *rgb = palette_data + i * 3;
        palette[i] = ((rgb[0] << 8) & 0xF800) | ((rgb[1] << 3) & 0x7E0) | (rgb[2] >> 3);
    }
}

static void step(uint32_t keys, rg_surface_t *surface)
{
    input[0] = !!(keys & RG_KEY_RIGHT); input[1] = !!(keys & RG_KEY_LEFT);
    input[2] = !!(keys & RG_KEY_DOWN); input[3] = !!(keys & RG_KEY_UP);
    input[4] = !!(keys & RG_KEY_B); input[5] = !!(keys & RG_KEY_A);
    input[12] = !!((keys & RG_KEY_START) && (keys & RG_KEY_SELECT));
    input[13] = !!(keys & RG_KEY_SELECT); input[14] = !!(keys & RG_KEY_START);
    prosystem_ExecuteFrame(input);
    int length = 32000 / prosystem_frequency;
    int source_length = prosystem_scanlines * 2;
    for (int i = 0; i < length; i++) {
        int index = i * source_length / length;
        int sample = tia_buffer[index] - 128;
        if (cartridge_pokey) sample = (sample + pokey_buffer[index] - 128) / 2;
        audio[i].left = audio[i].right = sample * 256;
    }
    rg_audio_submit(audio, length);
    if (RenderFlag) {
        int offset = (maria_visibleArea.top - maria_displayArea.top) * 320;
        uint16_t *dest = surface->data;
        for (int i = 0; i < surface->width * surface->height; i++)
            dest[i] = palette[maria_surface[offset + i]];
    }
}

static size_t state_size(void)
{
    return cartridge_type == CARTRIDGE_TYPE_SUPERCART_RAM ? 32829 : 16445;
}

static bool save(const char *path)
{
    char *data = rg_alloc(state_size(), MEM_SLOW);
    if (!data) return false;
    bool ok = prosystem_Save(data, false);
    FILE *fp = ok ? fopen(path, "wb") : NULL;
    ok = fp && fwrite(data, 1, state_size(), fp) == state_size();
    if (fp) ok = fclose(fp) == 0 && ok;
    free(data);
    return ok;
}

static bool load(const char *path)
{
    size_t size;
    char *data = legacy_read_file(path, &size, 32829);
    bool ok = data && size == state_size() && prosystem_Load(data);
    free(data);
    return ok;
}

static bool reset(bool hard) { prosystem_Reset(); return true; }

legacy_core_t prosystem_core = {init, step, save, load, reset, NULL, 320, 240, 32000, 60};
