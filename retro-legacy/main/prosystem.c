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
static unsigned audio_remainder;
static int audio_length, audio_done;
static int32_t audio_dc = -1;

void prosystem_AudioTick(uint16_t scanline)
{
    int target = scanline * audio_length / prosystem_scanlines;
    int pending = target - audio_done;
    if (pending < audio_length / 4 && scanline != prosystem_scanlines) return;

    for (int i = audio_done; i < target; i++) {
        int index = i * (prosystem_scanlines * 2) / audio_length;
        // TIA is unipolar (0..120), not unsigned PCM with silence at 128.
        // POKEY is also unipolar, with an idle value of 8. Remove their DC
        // continuously instead of driving the speaker with full-scale DC.
        int raw = tia_buffer[index];
        if (cartridge_pokey) raw = (raw + pokey_buffer[index]) / 2;
        int32_t level = raw * 65536;
        if (audio_dc < 0) audio_dc = level;
        audio_dc += (level - audio_dc) / 512;
        int sample = (level - audio_dc) / 256;
        sample = RG_MAX(-32768, RG_MIN(32767, sample));
        audio[i].left = audio[i].right = sample;
    }
    if (pending > 0) rg_audio_submit(audio + audio_done, pending);
    audio_done = target;
}

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
    audio_remainder = 0;
    audio_dc = -1;
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
    // Carry the fractional sample across NTSC frames: 533, 533, 534 at
    // 60 Hz produces exactly 32000 samples/sec rather than losing 20/sec.
    audio_remainder += 32000;
    audio_length = audio_remainder / prosystem_frequency;
    audio_remainder %= prosystem_frequency;
    audio_done = 0;
    prosystem_ExecuteFrame(input);
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
    if (ok) { audio_remainder = 0; audio_dc = -1; }
    return ok;
}

static bool reset(bool hard)
{ prosystem_Reset(); audio_remainder = 0; audio_dc = -1; return true; }

legacy_core_t prosystem_core = {init, step, save, load, reset, NULL, 320, 240, 32000, 60};
