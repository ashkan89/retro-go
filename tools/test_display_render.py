"""Run the production display renderer against known RGB565/palette surfaces."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'components/retro-go/rg_display.c').read_text()
start = source.index('IRAM_ATTR static inline void write_update(')
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
#define RG_PIXEL_GET_SIZE(f) ((f)&RG_PIXEL_PALETTE ? 1 : 2)
#define RG_SCREEN_PARTIAL_UPDATES partial_updates
#define LCD_BUFFER_LENGTH (480*4)
#define LINE_IS_REPEATED(y) (map_viewport_to_source_y[y] == map_viewport_to_source_y[(y)-1])
typedef struct { int format, stride, offset; void *data; uint16_t *palette; } rg_surface_t;
static struct {
    struct { bool filter_x, filter_y; int left, top, width, height; float step_x, step_y; } viewport;
    struct { struct { int left, top; } margins; } screen;
} display;
static struct { int fullFrames, partFrames; long busyTime; } counters;
static int map_viewport_to_source_x[481], map_viewport_to_source_y[321];
static uint32_t screen_line_checksum[321];
static bool partial_updates;
static uint16_t dma_buffer[LCD_BUFFER_LENGTH], result[480*320];
static int out_pos, send_count;
static long rg_system_timer(void) { return 0; }
static uint32_t rg_hash(void *data, int size) {
    uint8_t *p = data; uint32_t h = 2166136261u;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}
static unsigned blend_pixels(unsigned a, unsigned b) { assert(false); return 0; }
static uint16_t *lcd_get_buffer(int size) { return dma_buffer; }
static void lcd_set_window(int l, int t, int w, int h) { out_pos = t*w+l; }
static void lcd_send_buffer(uint16_t *data, size_t count) {
    if (count) { memcpy(result+out_pos, data, count*2); out_pos += count; ++send_count; }
}
static int draw_on_screen_display(int start, int end) { return 0; }
'''
code += source[start:end]
code += r'''
int main(void) {
    static uint8_t data[480*324*2];
    uint16_t palette[256];
    for (int i = 0; i < 256; ++i) palette[i] = i * 211;
    int widths[] = {160, 256, 300, 320, 480};
    for (int wi = 0; wi < 5; ++wi)
    for (int format = 1; format <= 3; ++format)
    for (int scale = 1; scale <= 2; ++scale)
    for (int partial = 0; partial <= 1; ++partial) {
        int w = widths[wi], h = 240, src_w = w / scale;
        int pixel_size = format == 3 ? 1 : 2;
        rg_surface_t surface = {.format = format == 3 ? RG_PIXEL_PALETTE|RG_PIXEL_565_BE : format,
            .stride = src_w*pixel_size + 8, .offset = 16, .data = data, .palette = palette};
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = i * 17 + i / 13;
        display.viewport.width = w; display.viewport.height = h;
        display.viewport.step_x = 1.f / scale; display.viewport.step_y = 0.5f;
        for (int x = 0; x <= w; ++x) map_viewport_to_source_x[x] = x/scale;
        for (int y = 0; y <= h; ++y) map_viewport_to_source_y[y] = y/2;
        partial_updates = partial;
        memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
        memset(result, 0, sizeof(result)); send_count = 0;
        write_update(&surface);
        for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = data + surface.offset + (y/2)*surface.stride + (x/scale)*pixel_size;
            uint16_t expected;
            if (format == 3) expected = palette[*p];
            else { memcpy(&expected, p, 2); if (format == 2) expected = (expected<<8)|(expected>>8); }
            assert(result[y*w+x] == expected);
        }
        if (partial) { send_count = 0; write_update(&surface); assert(send_count == 0); }
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'render.c'
    exe = Path(tmp) / 'render'
    src.write_text(code)
    subprocess.run([args.cc, '-std=gnu99', '-O2', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: 60 render cases, native/scaled rows, RGB565 BE/LE, palettes, padded stride, offset, repeated rows, unchanged frames')
