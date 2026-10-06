// Optional QEMU/device smoke test. Synthetic ROMs contain only code authored
// here. Build with -DRG_LEGACY_SELFTEST=ON; no SD card or display is required.
#include "legacy.h"
#include "esp_vfs.h"
#include "esp_heap_caps.h"
#include "Cartridge.h"
#include "ProSystem.h"
#include "Memory.h"
#include "Maria.h"
#include "Equates.h"
#include "Tia.h"
#include "Pokey.h"
#include "Region.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

bool RenderFlag = true;
static size_t audio_frames;
static size_t audio_blocks, audio_max_block, audio_nonzero;
typedef struct { const char *name; uint8_t *data; size_t size, capacity; } file_t;
static file_t files[8];
static struct { file_t *file; size_t position; } handles[16];

void *rg_alloc(size_t size, uint32_t caps)
{ return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    RG_ASSERT(frames && count > 0 && count <= 640, "Invalid audio block");
    audio_frames += count;
    audio_blocks++;
    if (count > audio_max_block) audio_max_block = count;
    for (size_t i = 0; i < count; i++)
        if (frames[i].left || frames[i].right) audio_nonzero++;
}
const char *rg_gettext(const char *text) { return text; }
double rg_settings_get_number(const char *ns, const char *key, double fallback) { return fallback; }
void rg_settings_set_number(const char *ns, const char *key, double value) {}
bool rg_extension_match(const char *path, const char *ext)
{ const char *dot = strrchr(path, '.'); return dot && !strcmp(dot + 1, ext); }
void rg_system_panic(const char *context, const char *message)
{ printf("SELFTEST FAIL: %s: %s\n", context, message); abort(); }

static file_t *make_file(const char *name, size_t capacity)
{
    for (int i = 0; i < 8; i++) if (!files[i].name) {
        files[i] = (file_t){name, rg_alloc(capacity, 0), 0, capacity};
        RG_ASSERT(files[i].data, "Fixture allocation failed");
        memset(files[i].data, 0, capacity);
        return &files[i];
    }
    RG_PANIC("Too many fixtures");
}

static int mem_open(const char *path, int flags, int mode)
{
    file_t *file = NULL;
    for (int i = 0; i < 8; i++) if (files[i].name && !strcmp(files[i].name, path)) file = &files[i];
    if (!file) { errno = ENOENT; return -1; }
    for (int i = 0; i < 16; i++) if (!handles[i].file) {
        if (flags & O_TRUNC) file->size = 0;
        handles[i].file = file; handles[i].position = (flags & O_APPEND) ? file->size : 0;
        return i;
    }
    errno = EMFILE; return -1;
}
static int mem_close(int fd) { handles[fd].file = NULL; return 0; }
static ssize_t mem_read(int fd, void *data, size_t size)
{
    file_t *file = handles[fd].file;
    size_t left = handles[fd].position < file->size ? file->size - handles[fd].position : 0;
    if (size > left) size = left;
    memcpy(data, file->data + handles[fd].position, size); handles[fd].position += size;
    return size;
}
static ssize_t mem_write(int fd, const void *data, size_t size)
{
    file_t *file = handles[fd].file;
    if (handles[fd].position + size > file->capacity) { errno = ENOSPC; return -1; }
    memcpy(file->data + handles[fd].position, data, size); handles[fd].position += size;
    if (handles[fd].position > file->size) file->size = handles[fd].position;
    return size;
}
static off_t mem_seek(int fd, off_t offset, int whence)
{
    off_t position = offset + (whence == SEEK_END ? handles[fd].file->size :
                               whence == SEEK_CUR ? handles[fd].position : 0);
    if (position < 0 || position > handles[fd].file->capacity) { errno = EINVAL; return -1; }
    handles[fd].position = position; return position;
}
static int mem_fstat(int fd, struct stat *st)
{ memset(st, 0, sizeof(*st)); st->st_mode = S_IFREG | 0666; st->st_size = handles[fd].file->size; return 0; }

void *legacy_read_file(const char *path, size_t *size, size_t limit)
{
    FILE *fp = fopen(path, "rb"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long length = ftell(fp); rewind(fp);
    if (length <= 0 || length > limit) { fclose(fp); return NULL; }
    void *data = rg_alloc(length, 0);
    bool ok = data && fread(data, 1, length, fp) == length;
    fclose(fp); if (!ok) { free(data); return NULL; }
    *size = length; return data;
}

static void exercise_maria_clipping(void)
{
    // Long sprites can cross the right edge, start off-screen, or wrap the
    // 8-bit horizontal counter back onto the left edge. Exercise both cell
    // writers, including transparent cells with kangaroo mode enabled.
    prosystem_Reset();
    for (int wide = 0; wide < 2; wide++) {
        for (int transparent = 0; transparent < 2; transparent++) {
            for (int horizontal = 0; horizontal < 256; horizontal++) {
                memory_Write(CTRL, 0x40 | (transparent ? 4 : 0));
                memory_Write(DPPH, 0x18); memory_Write(DPPL, 0);
                memory_Write(0x1800, 0); // DLL offset, no NMI or holey DMA
                memory_Write(0x1801, 0x18); memory_Write(0x1802, 0x20);
                // Extended, direct, 32-byte sprite at $1900.
                memory_Write(0x1820, 0); memory_Write(0x1821, 0x40 | (wide ? 0x80 : 0));
                memory_Write(0x1822, 0x19); memory_Write(0x1823, 0);
                memory_Write(0x1824, horizontal); memory_Write(0x1826, 0);
                memory_Write(BACKGRND + (wide ? 15 : 3), 0x46);
                for (int i = 0; i < 32; i++) memory_Write(0x1900 + i, transparent ? 0 : 0xFF);
                maria_scanline = maria_displayArea.top;
                RG_ASSERT(maria_RenderScanline() == 144, "Off-screen sprite corrupted DMA cycles");
                maria_scanline = maria_visibleArea.top;
                maria_RenderScanline();
                const uint8_t *line = maria_surface + (maria_scanline - maria_displayArea.top) * 320;
                bool drawn[160] = {0};
                for (int i = 0; i < 32 * (wide ? 2 : 4); i++) {
                    uint8_t cell = horizontal + i;
                    if (cell < 160 && !transparent) drawn[cell] = true;
                }
                for (int i = 0; i < 160; i++) {
                    uint8_t expected = drawn[i] ? 0x46 : 0;
                    RG_ASSERT(line[i * 2] == expected && line[i * 2 + 1] == expected,
                        "Off-screen sprite clipping or horizontal wrap failed");
                }
            }
        }
    }
    printf("SELFTEST PASS: Atari 7800 off-screen sprites and horizontal wrap\n");
}

static void exercise(legacy_core_t *core, const char *rom, const char *state)
{
    core->init(rom);
    rg_surface_t surface = {.width = core->width, .height = core->height,
        .stride = core->width * 2, .format = RG_PIXEL_565_LE};
    surface.data = rg_alloc(surface.stride * surface.height, 0);
    RG_ASSERT(surface.data, "Surface allocation failed");
    audio_frames = 0;
    for (int i = 0; i < 6; i++) core->step(i & 1 ? RG_KEY_A | RG_KEY_RIGHT : 0, &surface);
    if (core == &spectrum_core || core == &stella_core) {
        bool colored = false;
        for (int i = 0; i < surface.width * surface.height; i++) if (((uint16_t *)surface.data)[i]) colored = true;
        RG_ASSERT(colored, "Synthetic ROM did not render color");
    }
    RG_ASSERT(audio_frames > 0, "Audio was not submitted");
    if (core == &stella_core)
        RG_ASSERT(audio_frames == (31400 * 6) / core->refresh_rate, "Atari 2600 sample clock drifted");
    RG_ASSERT(core->save(state), "State save failed");
    RG_ASSERT(core->reset(true), "Reset failed");
    RG_ASSERT(core->load(state), "State round trip failed");
    RG_ASSERT(!core->load("/test/bad"), "Malformed state accepted");
    core->step(0, &surface);
    printf("SELFTEST PASS: %s video=%dx%d audio=%u\n", rom, core->width, core->height, (unsigned)audio_frames);
    free(surface.data);
}

static void exercise_prosystem_audio(void)
{
    // The synthetic ROM never enables sound. Silence must remain zero, even
    // with POKEY's nonzero idle bias, and frame skip must not suppress audio.
    rg_surface_t unused = {0};
    RenderFlag = false;
    for (int pal = 0; pal < 2; pal++) {
        region_type = pal ? REGION_PAL : REGION_NTSC;
        for (int pokey = 0; pokey < 2; pokey++) {
            cartridge_pokey = pokey;
            prosystem_core.reset(true);
            audio_frames = audio_blocks = audio_max_block = audio_nonzero = 0;
            int frames = pal ? 50 : 60;
            for (int i = 0; i < frames; i++) prosystem_core.step(0, &unused);
            RG_ASSERT(audio_frames == 32000, "Atari 7800 sample clock drifted");
            RG_ASSERT(audio_nonzero == 0, "Atari 7800 silence contains DC");
            RG_ASSERT(audio_blocks >= frames * 4 && audio_max_block <= 160,
                "Atari 7800 audio was delayed until frame end");
        }
    }
    // A real TIA square wave must survive DC removal.
    region_type = REGION_NTSC;
    cartridge_pokey = false;
    prosystem_core.reset(true);
    tia_SetRegister(AUDC0, 4); tia_SetRegister(AUDF0, 4); tia_SetRegister(AUDV0, 15);
    audio_nonzero = 0;
    for (int i = 0; i < 3; i++) prosystem_core.step(0, &unused);
    RG_ASSERT(audio_nonzero > 0, "Atari 7800 tone was filtered out");
    region_type = REGION_AUTO;
    prosystem_core.reset(true);
    RenderFlag = true;
    printf("SELFTEST PASS: Atari 7800 TIA/POKEY silence, PAL/NTSC sample clock and streamed audio\n");
}

void app_main(void)
{
    extern int audio_dma_selftest(void);
    RG_ASSERT(audio_dma_selftest() == 0, "I2S batching or FIFO order failed");
    printf("SELFTEST PASS: complete I2S DMA blocks and ring wrap (75 producer scenarios)\n");
    extern int snes_audio_selftest(void);
    RG_ASSERT(snes_audio_selftest() == 0, "SNES PAL/NTSC audio clock failed");
    printf("SELFTEST PASS: SNES PAL/NTSC sample totals and mixer bounds, both filters\n");
    extern int msx_audio_selftest(void);
    RG_ASSERT(msx_audio_selftest() == 0, "MSX fractional stereo sample clock failed");
    printf("SELFTEST PASS: MSX fractional sample totals and whole stereo frames\n");
    const esp_vfs_t vfs = {.flags = ESP_VFS_FLAG_DEFAULT, .open = mem_open,
        .close = mem_close, .read = mem_read, .write = mem_write, .lseek = mem_seek, .fstat = mem_fstat};
    ESP_ERROR_CHECK(esp_vfs_register("/test", &vfs, NULL));
    file_t *sna = make_file("/spectrum.sna", 49179); sna->size = 49179;
    sna->data[23] = 0; sna->data[24] = 0x80;
    uint8_t spectrum_program[] = {0x02, 0x80, 0x3E, 0x02, 0xD3, 0xFE, 0xC3, 0x02, 0x80};
    memcpy(sna->data + 27 + 0x4000, spectrum_program, sizeof(spectrum_program));
    file_t *a26 = make_file("/game.a26", 4096); a26->size = 4096; memset(a26->data, 0xEA, 4096);
    // 262-scanline NTSC frame, red background, looping forever.
    const uint8_t vcs[] = {0x78,0xD8,0xA2,0xFF,0x9A,0xA9,2,0x85,0,0x85,2,0x85,2,0x85,2,
        0xA9,0,0x85,0,0xA2,37,0x85,2,0xCA,0xD0,0xFB,0xA9,0,0x85,1,0xA9,0x46,0x85,9,
        0xA2,192,0x85,2,0xCA,0xD0,0xFB,0xA9,2,0x85,1,0xA2,30,0x85,2,0xCA,0xD0,0xFB,0x4C,5,0xF0};
    memcpy(a26->data, vcs, sizeof(vcs)); a26->data[4092] = 0; a26->data[4093] = 0xF0;
    file_t *a78 = make_file("/game.a78", 16512); a78->size = 16512;
    memcpy(a78->data + 1, "ATARI7800", 9); a78->data[50] = 0; a78->data[51] = 0x40;
    const uint8_t pro[] = {0x78,0xA9,0x46,0x85,0x20,0x4C,0,0xC0};
    memcpy(a78->data + 128, pro, sizeof(pro)); a78->data[128 + 16380] = 0; a78->data[128 + 16381] = 0xC0;
    make_file("/spectrum.state", 65536); make_file("/a26.state", 131072); make_file("/a78.state", 32829);
    file_t *bad = make_file("/bad", 1); bad->size = 1;
    exercise(&spectrum_core, "/test/spectrum.sna", "/test/spectrum.state");
    // Z80 v2 with three raw 16K pages (0xffff block length).
    file_t *extended = make_file("/extended.z80", 49216); extended->size = 49216;
    extended->data[30] = 23; extended->data[32] = 2; extended->data[33] = 0x80;
    const uint8_t pages[] = {8, 4, 5};
    for (int i = 0; i < 3; i++) {
        size_t offset = 55 + i * 16387;
        extended->data[offset] = 0xFF; extended->data[offset + 1] = 0xFF;
        extended->data[offset + 2] = pages[i];
        if (i == 1) memcpy(extended->data + offset + 3, spectrum_program, sizeof(spectrum_program));
    }
    RG_ASSERT(spectrum_core.load("/test/extended.z80"), "Raw Z80 pages rejected");
    extended->data[34] = 4;
    RG_ASSERT(!spectrum_core.load("/test/extended.z80"), "128K snapshot accepted");
    printf("SELFTEST PASS: extended Z80 pages and 128K rejection\n");
    exercise(&stella_core, "/test/game.a26", "/test/a26.state");
    file_t *stella_state = &files[4];
    size_t capacity = stella_state->capacity;
    stella_state->capacity = 1;
    RG_ASSERT(!stella_core.save("/test/a26.state"), "Failed state write reported success");
    stella_state->capacity = capacity;
    printf("SELFTEST PASS: failed Stella write\n");
    exercise(&prosystem_core, "/test/game.a78", "/test/a78.state");
    exercise_prosystem_audio();
    exercise_maria_clipping();
    // Regression: the original port aborted saving carts with 16K extra RAM.
    uint8_t *ram_rom = rg_alloc(65536 + 128, 0);
    RG_ASSERT(ram_rom, "RAM cartridge allocation failed");
    memset(ram_rom, 0, 65536 + 128);
    memcpy(ram_rom + 1, "ATARI7800", 9);
    ram_rom[50] = 1; ram_rom[54] = 6;
    memcpy(ram_rom + 128 + 49152, pro, sizeof(pro));
    ram_rom[128 + 65533] = 0xC0;
    RG_ASSERT(cartridge_Load(ram_rom, 65536 + 128), "RAM cartridge load failed");
    free(ram_rom); prosystem_Reset();
    for (int i = 0; i < 16384; i++) memory_Write(0x4000 + i, (i ^ (i >> 8)) & 0xFF);
    for (int i = 0; i < 16384; i++)
        RG_ASSERT(memory_Read(0x4000 + i) == ((i ^ (i >> 8)) & 0xFF), "Cartridge RAM write lost");
    RG_ASSERT(prosystem_core.save("/test/a78.state"), "Cartridge RAM state save failed");
    memory_Write(0x407B, 0);
    RG_ASSERT(prosystem_core.load("/test/a78.state") && memory_Read(0x407B) == 0x7B, "Cartridge RAM state lost");
    printf("SELFTEST PASS: cartridge RAM\nSELFTEST COMPLETE\n");
}
