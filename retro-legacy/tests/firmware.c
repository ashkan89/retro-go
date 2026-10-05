// Optional QEMU/device smoke test. Synthetic ROMs contain only code authored
// here. Build with -DRG_LEGACY_SELFTEST=ON; no SD card or display is required.
#include "legacy.h"
#include "esp_vfs.h"
#include "esp_heap_caps.h"
#include "Cartridge.h"
#include "ProSystem.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

bool RenderFlag = true;
static size_t audio_frames;
typedef struct { const char *name; uint8_t *data; size_t size, capacity; } file_t;
static file_t files[8];
static struct { file_t *file; size_t position; } handles[16];

void *rg_alloc(size_t size, uint32_t caps)
{ return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{ RG_ASSERT(frames && count > 0 && count <= 640, "Invalid audio block"); audio_frames += count; }
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
    RG_ASSERT(core->save(state), "State save failed");
    RG_ASSERT(core->reset(true), "Reset failed");
    RG_ASSERT(core->load(state), "State round trip failed");
    RG_ASSERT(!core->load("/test/bad"), "Malformed state accepted");
    core->step(0, &surface);
    printf("SELFTEST PASS: %s video=%dx%d audio=%u\n", rom, core->width, core->height, (unsigned)audio_frames);
    free(surface.data);
}

void app_main(void)
{
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
    // Regression: the original port aborted saving carts with 16K extra RAM.
    cartridge_type = CARTRIDGE_TYPE_SUPERCART_RAM; prosystem_Reset();
    extern uint8_t *cartRAM;
    cartRAM[123] = 0x5A;
    RG_ASSERT(prosystem_core.save("/test/a78.state"), "Cartridge RAM state save failed");
    cartRAM[123] = 0;
    RG_ASSERT(prosystem_core.load("/test/a78.state") && cartRAM[123] == 0x5A, "Cartridge RAM state lost");
    printf("SELFTEST PASS: cartridge RAM\nSELFTEST COMPLETE\n");
}
