"""Run the production display renderer against known RGB565/palette surfaces."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'components/retro-go/rg_display.c').read_text()
start = source.index('IRAM_ATTR static inline bool write_update(')
end = source.index('static void update_viewport_scaling(', start)
code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define IRAM_ATTR
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
#define RG_PIXEL_PALETTE 0x80
#define RG_PIXEL_565_BE 1
#define RG_PIXEL_565_LE 2
#define RG_PIXEL_PAL565_LE (RG_PIXEL_PALETTE|RG_PIXEL_565_LE)
#define RG_DISPLAY_FILTER_BOTH 3
#define RG_PIXEL_GET_SIZE(f) ((f)&RG_PIXEL_PALETTE ? 1 : 2)
#define RG_SCREEN_PARTIAL_UPDATES partial_updates
#define LCD_BUFFER_LENGTH (480*4)
static int lcd_buffer_pixels = LCD_BUFFER_LENGTH;
#define LINE_IS_REPEATED(y) (map_viewport_to_source_y[y] == map_viewport_to_source_y[(y)-1])
typedef struct { int width,height,format,stride,offset; void *data; uint16_t *palette; } rg_surface_t;
static struct { int filter; } config = {3};
static bool hardware_enabled;
#if CONFIG_IDF_TARGET_ESP32P4
// PPA's real pixel math/endian handling is checked on the device. This stub
// isolates the renderer's hardware-surface row selection and SPI byte order.
static const rg_surface_t *p4_scale_frame(const rg_surface_t *s,int w,int h) {
    if (!hardware_enabled) return NULL;
    static uint16_t pixels[480*320];
    static rg_surface_t out;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const uint8_t *p=(const uint8_t*)s->data+s->offset+(y*s->height/h)*s->stride;
        uint16_t v;
        if(s->format&RG_PIXEL_PALETTE) v=s->palette[p[x*s->width/w]];
        else memcpy(&v,p+(x*s->width/w)*2,2);
        if(s->format==RG_PIXEL_565_BE || s->format==(RG_PIXEL_PALETTE|RG_PIXEL_565_BE)) v=(v<<8)|(v>>8);
        pixels[y*w+x]=v;
    }
    out=(rg_surface_t){.width=w,.height=h,.format=2,.stride=w*2,.data=pixels}; return &out;
}
static void p4_copy_rgb565_be(uint16_t *d,const uint16_t *s,size_t n) {
    while(n--) { uint16_t p=*s++; *d++=(p<<8)|(p>>8); }
}
#endif
static struct {
    struct { bool filter_x, filter_y; int left, top, width, height; float step_x, step_y; } viewport;
    struct { struct { int left, top; } margins; } screen;
} display;
static struct { int fullFrames, partFrames; long busyTime; } counters;
static struct { uint32_t ppaTimeUs; } transfer_stats;
static int map_viewport_to_source_x[481], map_viewport_to_source_y[321];
static uint32_t screen_line_checksum[321];
static uint16_t horizontal_blend_columns[480];
static int horizontal_blend_count;
static bool partial_updates;
static uint16_t dma_buffer[LCD_BUFFER_LENGTH], result[480*320];
static int out_pos, send_count, pixels_sent, window_count;
static long rg_system_timer(void) { return 0; }
static uint32_t rg_hash(void *data, int size) {
    uint8_t *p = data; uint32_t h = 2166136261u;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}
static unsigned blend_pixels(unsigned a, unsigned b) { assert(false); return 0; }
static uint16_t *lcd_get_buffer(int size) { return dma_buffer; }
static void lcd_set_window(int l, int t, int w, int h) { out_pos = t*w+l; ++window_count; }
static void lcd_send_buffer(uint16_t *data, size_t count) {
    if (count) { memcpy(result+out_pos, data, count*2); out_pos += count; ++send_count; pixels_sent += count; }
}
static int draw_on_screen_display(int end, bool *wrote_to_panel) { return 0; }
'''
code += source[start:end]
code += r'''
int main(void) {
    static uint8_t data[480*324*2];
    uint16_t palette[256];
    for (int i = 0; i < 256; ++i) palette[i] = i * 211;
    int widths[] = {160, 256, 300, 320, 480};
    for (int accelerated = 0; accelerated <= CONFIG_IDF_TARGET_ESP32P4; ++accelerated)
    for (int wi = 0; wi < 5; ++wi)
    for (int format = 1; format <= 4; ++format)
    for (int scale = 1; scale <= 2; ++scale)
    for (int partial = 0; partial <= 1; ++partial) {
        int w = widths[wi], h = 240, src_w = w / scale;
        hardware_enabled = accelerated;
        int pixel_size = format >= 3 ? 1 : 2;
        rg_surface_t surface = {.width=src_w,.height=h/2,
            .format = format >= 3 ? RG_PIXEL_PALETTE|(format-2) : format,
            .stride = src_w*pixel_size + 8, .offset = 16, .data = data, .palette = palette};
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = i * 17 + i / 13;
        display.viewport.width = w; display.viewport.height = h;
        display.viewport.step_x = 1.f / scale; display.viewport.step_y = 0.5f;
        for (int x = 0; x <= w; ++x) map_viewport_to_source_x[x] = x/scale;
        for (int y = 0; y <= h; ++y) map_viewport_to_source_y[y] = y/2;
        partial_updates = partial;
        memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
        memset(result, 0, sizeof(result)); send_count = window_count = 0;
        assert(write_update(&surface));
        assert(window_count == 1);
        for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = data + surface.offset + (y/2)*surface.stride + (x/scale)*pixel_size;
            uint16_t expected;
            if (format >= 3) { expected = palette[*p]; if(format==4) expected=(expected<<8)|(expected>>8); }
            else { memcpy(&expected, p, 2); if (format == 2) expected = (expected<<8)|(expected>>8); }
            assert(result[y*w+x] == expected);
        }
        if (partial) {
            send_count = 0;
            assert(!write_update(&surface));
            assert(send_count == 0);
            pixels_sent = 0;
            memset(data + surface.offset + 7 * surface.stride, 0, src_w * pixel_size);
            assert(write_update(&surface));
            assert(pixels_sent == 2 * w);
            for (int y = 14; y < 16; ++y)
                for (int x = 0; x < w; ++x)
                    assert(result[y * w + x] == 0);
        }
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'render.c'
    exe = Path(tmp) / ('render.exe' if os.name == 'nt' else 'render')
    src.write_text(code)
    for hardware in (0, 1):
        subprocess.run([args.cc, '-std=gnu99', '-O2', '-UNDEBUG', f'-DCONFIG_IDF_TARGET_ESP32P4={hardware}',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('PASS: 240 software/hardware/fallback render cases, RGB565/palettes BE/LE, padded stride, offset, repeated and dirty rows')

# Run the production SPI ring and window serializer with a FIFO that leaves
# descriptors and buffers in flight until a producer reclaims them.
spi_source = (root / 'components/retro-go/drivers/display/ili9341.h').read_text()
spi_code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define DRAM_ATTR
#define IRAM_ATTR
#define LCD_BUFFER_LENGTH 64
static int lcd_buffer_pixels = LCD_BUFFER_LENGTH;
#define pdMS_TO_TICKS(t) (t)
#define portMAX_DELAY 0
#define ESP_OK 0
#define SPI_TRANS_VARIABLE_ADDR 1
#define RG_PANIC(message) abort()
#define RG_LOGW(...) abort()
#define RG_ASSERT_ARG(x) assert(x)
#define RG_GPIO_LCD_DC 0
typedef int spi_device_handle_t;
typedef int SemaphoreHandle_t;
typedef struct { void *user; const void *tx_buffer; size_t length; uint64_t addr; int flags; } spi_transaction_t;
typedef struct { spi_transaction_t base; int address_bits; } spi_transaction_ext_t;
static struct { struct { int real_width, real_height; } screen; } display = {{480, 320}};
static spi_transaction_t *in_flight[8];
static unsigned read_index, pending, submitted, dc, peak_pending;
static unsigned expected_count, expected[1024];
static void expect(unsigned value) { expected[expected_count++] = value; }
static void xSemaphoreTakeRecursive(int m, int ticks) {}
static void xSemaphoreGiveRecursive(int m) {}
static void gpio_set_level(int pin, int value) { dc = value; }
static void spi_pre_transfer_cb(spi_transaction_t *t);
static int spi_device_queue_trans(int dev, spi_transaction_t *t, int ticks)
{
    assert(pending < 8);
    for (unsigned i = 0; i < pending; ++i)
        assert(in_flight[(read_index + i) % 8] != t);
    in_flight[(read_index + pending++) % 8] = t;
    if (peak_pending < pending) peak_pending = pending;
    return ESP_OK;
}
static int spi_device_get_trans_result(int dev, spi_transaction_t **result, int ticks)
{
    assert(pending);
    spi_transaction_t *t = in_flight[read_index++ % 8];
    --pending;
    spi_pre_transfer_cb(t);
    if (t->flags & SPI_TRANS_VARIABLE_ADDR) {
        assert(!t->length && !t->tx_buffer);
        int bits = ((spi_transaction_ext_t *)t)->address_bits;
        assert(bits > 0 && bits <= 32 && bits % 8 == 0);
        for (int shift = bits - 8; shift >= 0; shift -= 8) {
            assert(submitted < expected_count);
            assert(((dc << 8) | ((t->addr >> shift) & 255)) == expected[submitted++]);
        }
    } else {
        assert(t->length && t->tx_buffer);
        const uint8_t *data = t->tx_buffer;
        for (size_t i = 0; i < t->length / 8; ++i) {
            assert(submitted < expected_count);
            assert(((dc << 8) | data[i]) == expected[submitted++]);
        }
    }
    *result = t;
    return ESP_OK;
}
'''
spi_code += spi_source[spi_source.index('#define SPI_TRANSACTION_COUNT'):spi_source.index('static void spi_init(')]
spi_code += spi_source[spi_source.index('static void lcd_set_window('):spi_source.index('static void lcd_sync(')]
spi_code += r'''
int main(void)
{
    static uint16_t buffers[SPI_BUFFER_COUNT][LCD_BUFFER_LENGTH];
    for (unsigned i = 0; i < SPI_BUFFER_COUNT; ++i)
        spi_buffers[spi_free_buffers++] = buffers[i];
    lcd_left = lcd_right = lcd_top = lcd_bottom = -1;
    lcd_set_window(257, 258, 2, 2);
    expect(0x2a); expect(0x101); expect(0x101); expect(0x101); expect(0x102);
    expect(0x2b); expect(0x101); expect(0x102); expect(0x101); expect(0x103);
    expect(0x2c);
    lcd_set_window(257, 258, 2, 2);
    expect(0x2c);
    lcd_set_window(257, 259, 2, 1);
    expect(0x2b); expect(0x101); expect(0x103); expect(0x101); expect(0x103);
    expect(0x2c);
    lcd_set_window(256, 259, 3, 1);
    expect(0x2a); expect(0x101); expect(0x100); expect(0x101); expect(0x102);
    expect(0x2c);

    for (unsigned i = 0; i < 40; ++i) {
        ILI9341_CMD(0x36, i);
        expect(0x36); expect(0x100 | i);
        uint16_t *pixels = lcd_get_buffer(2);
        ((uint8_t *)pixels)[0] = i;
        ((uint8_t *)pixels)[1] = i + 1;
        lcd_send_buffer(pixels, 1);
        expect(0x100 | i); expect(0x100 | (i + 1));

        uint16_t *unused = lcd_get_buffer(2);
        lcd_send_buffer(unused, 0);
    }
    ILI9341_CMD(0xE0, 1, 2, 3, 4, 5);
    expect(0xE0);
    for (unsigned i = 1; i <= 5; ++i) expect(0x100 | i);
    spi_drain();
    assert(!pending && !spi_pending && spi_free_buffers == SPI_BUFFER_COUNT);
    assert(submitted == expected_count && peak_pending == SPI_TRANSACTION_COUNT);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'spi.c'
    exe = Path(tmp) / ('spi.exe' if os.name == 'nt' else 'spi')
    src.write_text(spi_code)
    subprocess.run([args.cc, '-std=gnu99', '-O2', '-UNDEBUG', '-DCONFIG_IDF_TARGET_ESP32S3=1',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: SPI command byte order, window reuse, FIFO wraparound, buffer reuse, and drain')
