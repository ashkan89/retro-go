#include "legacy.h"
#include "../components/spectrum/config.h"
#include "z80.h"
#include "spperif.h"
#include "snapshot.h"

extern int snsh_load_checked(FILE *fp, int type);
static unsigned frame;
static int ticks;
static uint16_t palette[16];
static int controls;
static bool caps_shift, symbol_shift;
static int key_selection;
static int injected_key = -1, injected_frames;
static rg_audio_sample_t audio[TMNUM];
static int sound_dc = 128 * 256;

// Spectrum matrix order, five active-low keys on each row.
static const char *key_names[40] = {
    "Caps Shift", "Z", "X", "C", "V", "A", "S", "D", "F", "G",
    "Q", "W", "E", "R", "T", "1", "2", "3", "4", "5",
    "0", "9", "8", "7", "6", "P", "O", "I", "U", "Y",
    "Enter", "L", "K", "J", "H", "Space", "Symbol Shift", "M", "N", "B"
};

// spect.c keeps this hook for its original scanline frontend. This adapter
// renders from video RAM after the CPU frame and leaves sp_updating disabled.
byte *update_screen_line(byte *p, int coli, int scri, int border, qbyte *mark) { return p; }

static void render(rg_surface_t *surface)
{
    uint16_t *dest = surface->data;
    uint16_t border = palette[z80_proc.ula_outport & 7];
    for (int i = 0; i < 320 * 240; i++) dest[i] = border;
    for (int y = 0; y < 192; y++) {
        int address = 0x4000 | ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2);
        for (int x = 0; x < 32; x++) {
            uint8_t attr = z80_proc.mem[0x5800 + (y / 8) * 32 + x];
            int bright = (attr & 0x40) >> 3;
            int ink = (attr & 7) | bright, paper = ((attr >> 3) & 7) | bright;
            if ((attr & 0x80) && ((frame / 16) & 1)) { int swap = ink; ink = paper; paper = swap; }
            uint8_t pixels = z80_proc.mem[address + x];
            for (int bit = 0; bit < 8; bit++)
                dest[(y + 24) * 320 + 32 + x * 8 + bit] = palette[(pixels & (0x80 >> bit)) ? ink : paper];
        }
    }
}

static bool read_snapshot(const char *path, int type)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (type == SN_SNA) {
        if (fseek(fp, 0, SEEK_END) || ftell(fp) != 49179) { fclose(fp); return false; }
        rewind(fp);
    }
    Z80 saved_cpu = z80_proc;
    uint8_t *saved_ram = rg_alloc(0xC000, MEM_SLOW);
    if (!saved_ram) { fclose(fp); return false; }
    memcpy(saved_ram, z80_proc.mem + 0x4000, 0xC000);
    bool ok = snsh_load_checked(fp, type) && !ferror(fp);
    fclose(fp);
    if (!ok) {
        z80_proc = saved_cpu;
        memcpy(z80_proc.mem + 0x4000, saved_ram, 0xC000);
    }
    free(saved_ram);
    if (!ok) return false;
    ticks = 0; frame = 0; sp_scline = 0;
    sound_dc = 128 * 256;
    return ok;
}

static bool reset(bool hard)
{
    z80_reset();
    memset(z80_proc.mem + 0x4000, 0, 0xC000);
    ticks = 0; frame = 0; sp_scline = 0;
    sound_dc = 128 * 256;
    return true;
}

static void init(const char *path)
{
    sp_init();
    sp_updating = 0;
    for (int i = 0; i < PORT_TIME_NUM; i++) sp_scri[i] = -2;
    for (int i = 0; i < 16; i++) {
        int level = (i & 8) ? 255 : 205;
        int r = (i & 2) ? level : 0, g = (i & 4) ? level : 0, b = (i & 1) ? level : 0;
        palette[i] = ((r << 8) & 0xF800) | ((g << 3) & 0x7E0) | (b >> 3);
    }
    controls = (int)rg_settings_get_number(NS_APP, "Controls", 0) % 3;
    if (controls < 0) controls = 0;
    RG_ASSERT(read_snapshot(path, rg_extension_match(path, "sna") ? SN_SNA : SN_Z80),
              "Invalid or unsupported Spectrum 48K snapshot");
}

static void press(uint8_t rows[8], int index) { rows[index / 5] &= ~(1 << (index % 5)); }

static void step(uint32_t keys, rg_surface_t *surface)
{
    uint8_t rows[8]; memset(rows, 0xFF, sizeof(rows));
    unsigned joystick = (!!(keys & RG_KEY_RIGHT)) | (!!(keys & RG_KEY_LEFT) << 1)
        | (!!(keys & RG_KEY_DOWN) << 2) | (!!(keys & RG_KEY_UP) << 3) | (!!(keys & RG_KEY_A) << 4);
    z80_inports[0x1F] = controls == 0 ? joystick : 0;
    if (controls == 1) { // Sinclair 2: 6,7,8,9,0
        if (keys & RG_KEY_LEFT) press(rows, 24);
        if (keys & RG_KEY_RIGHT) press(rows, 23);
        if (keys & RG_KEY_DOWN) press(rows, 22);
        if (keys & RG_KEY_UP) press(rows, 21);
        if (keys & RG_KEY_A) press(rows, 20);
    } else if (controls == 2) { // QAOP + Space
        if (keys & RG_KEY_UP) press(rows, 10);
        if (keys & RG_KEY_DOWN) press(rows, 5);
        if (keys & RG_KEY_LEFT) press(rows, 26);
        if (keys & RG_KEY_RIGHT) press(rows, 25);
        if (keys & RG_KEY_A) press(rows, 35);
    }
    if (keys & RG_KEY_B) press(rows, 35);
    if (keys & RG_KEY_START) press(rows, 30);
    if (keys & RG_KEY_SELECT) press(rows, 20);
    if (caps_shift) press(rows, 0);
    if (symbol_shift) press(rows, 36);
    if (injected_frames > 0) { press(rows, injected_key); injected_frames--; }
    for (int port = 0; port < 256; port++) {
        uint8_t value = 0xFF;
        for (int row = 0; row < 8; row++) if (!(port & (1 << row))) value &= rows[row];
        sp_fe_inport_high[port] = value;
    }
    if (!(frame & 1)) sp_scline = 0;
    ticks = sp_halfframe(ticks + CHKTICK, EVENHF);
    z80_interrupt(0xFF);
    if (frame & 1) {
        for (int i = 0; i < TMNUM; i++) {
            int sample = sp_sound_buf[i] * 256;
            sound_dc += (sample - sound_dc) / 16;
            audio[i].left = audio[i].right = (sample - sound_dc) / 4;
        }
        rg_audio_submit(audio, TMNUM);
    }
    frame++;
    if (RenderFlag) render(surface);
}

static bool save(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    snsh_save(fp, SN_Z80);
    bool ok = !ferror(fp);
    return fclose(fp) == 0 && ok;
}
static bool load(const char *path) { return read_snapshot(path, SN_Z80); }

static rg_gui_event_t controls_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *names[] = {"Kempston", "Sinclair 2", "QAOP"};
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        controls = (controls + (event == RG_DIALOG_NEXT ? 1 : 2)) % 3;
        rg_settings_set_number(NS_APP, "Controls", controls);
    }
    strcpy(option->value, names[controls]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t keyboard_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) key_selection = (key_selection + 39) % 40;
    if (event == RG_DIALOG_NEXT) key_selection = (key_selection + 1) % 40;
    if (event == RG_DIALOG_ENTER) {
        injected_key = key_selection; injected_frames = 5;
        return RG_DIALOG_SELECT;
    }
    strcpy(option->value, key_names[key_selection]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t shift_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool *shift = option->arg == 0 ? &caps_shift : &symbol_shift;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER) *shift = !*shift;
    strcpy(option->value, *shift ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static void options(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Controls"), "", RG_DIALOG_FLAG_NORMAL, controls_cb};
    *dest++ = (rg_gui_option_t){1, _("Keyboard"), "", RG_DIALOG_FLAG_NORMAL, keyboard_cb};
    *dest++ = (rg_gui_option_t){0, _("Caps Shift"), "", RG_DIALOG_FLAG_NORMAL, shift_cb};
    *dest++ = (rg_gui_option_t){1, _("Symbol Shift"), "", RG_DIALOG_FLAG_NORMAL, shift_cb};
    *dest = (rg_gui_option_t)RG_DIALOG_END;
}

legacy_core_t spectrum_core = {init, step, save, load, reset, options, 320, 240, 15600, 50};
