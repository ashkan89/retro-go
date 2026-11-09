"""Exercise production P4 JPEG DMA ownership, padding and resource-failure paths."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
helper = (root / 'components/media/p4_jpeg.h').read_text()
helper = '\n'.join(line for line in helper.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define ESP_OK 0
#define MALLOC_CAP_SPIRAM 1
#define MEDIA_MAX_ARTWORK_PIXELS (4096 * 4096)
#define MEDIA_MAX_ARTWORK_BYTES (768 * 1024)
#define RG_PIXEL_565_LE 2
#define RG_MAX(a,b) ((a)>(b)?(a):(b))
#define JPEG_DOWN_SAMPLING_YUV444 0
#define JPEG_DOWN_SAMPLING_YUV422 1
#define JPEG_DOWN_SAMPLING_YUV420 2
#define JPEG_DEC_ALLOC_OUTPUT_BUFFER 1
#define JPEG_DEC_ALLOC_INPUT_BUFFER 2
#define JPEG_DECODE_OUT_FORMAT_RGB565 1
#define JPEG_DEC_RGB_ELEMENT_ORDER_BGR 1
#define JPEG_YUV_RGB_CONV_STD_BT601 1
typedef int esp_err_t;
typedef void *jpeg_decoder_handle_t;
typedef struct { unsigned width,height; int sample_method; } jpeg_decode_picture_info_t;
typedef struct { int buffer_direction; } jpeg_decode_memory_alloc_cfg_t;
typedef struct { int timeout_ms; } jpeg_decode_engine_cfg_t;
typedef struct { int output_format,rgb_order,conv_std; } jpeg_decode_cfg_t;
typedef struct { int width,height,stride,format; void *data; bool free_data; } rg_image_t;
static jpeg_decode_picture_info_t header;
static int fail_info,fail_engine,fail_process,fail_alloc,fail_input,fail_resize,live_engines,live_buffers;
static const uint8_t *original_input;
static size_t free_psram,expected_size;
static esp_err_t jpeg_decoder_get_info(const uint8_t *d,size_t n,jpeg_decode_picture_info_t *i)
{ (void)d;(void)n; *i=header; return fail_info; }
static size_t heap_caps_get_free_size(int caps) { (void)caps; return free_psram; }
static void *jpeg_alloc_decoder_mem(size_t n,const jpeg_decode_memory_alloc_cfg_t *c,size_t *capacity) {
    if(c->buffer_direction==JPEG_DEC_ALLOC_OUTPUT_BUFFER) assert(n==expected_size);
    else assert(c->buffer_direction==JPEG_DEC_ALLOC_INPUT_BUFFER && n==16);
    *capacity=(n+63)&~(size_t)63;
    if(fail_alloc || (fail_input && c->buffer_direction==JPEG_DEC_ALLOC_INPUT_BUFFER)) return NULL;
    ++live_buffers; return calloc(1,*capacity);
}
static esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *c,jpeg_decoder_handle_t *h)
{ assert(c->timeout_ms>0); if(fail_engine) return 1; ++live_engines; *h=(void*)1; return 0; }
static esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t h) { assert(h); --live_engines; return 0; }
static esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t h,const jpeg_decode_cfg_t *c,
    const uint8_t *d,size_t n,uint8_t *pixels,size_t capacity,uint32_t *written) {
    assert(n==16 && d!=original_input && !memcmp(d,original_input,n));
    assert(h && capacity>=expected_size);
    assert(c->output_format==JPEG_DECODE_OUT_FORMAT_RGB565 && c->rgb_order==JPEG_DEC_RGB_ELEMENT_ORDER_BGR);
    *written=expected_size; memset(pixels,0x55,expected_size); return fail_process;
}
static void tracked_free(void *p) { if(p) --live_buffers; free(p); }
static void rg_surface_free(rg_image_t *s) { if(s) { if(s->free_data) tracked_free(s->data); free(s); } }
static rg_image_t *rg_surface_resize(rg_image_t *s,int w,int h) {
    assert(s->width==72 && s->height==72 && s->stride==160);
    assert(w==15 && h==15); if(fail_resize) return NULL;
    rg_image_t *r=calloc(1,sizeof(*r)); *r=(rg_image_t){.width=w,.height=h}; return r;
}
#define free tracked_free
'''
# Only the DMA pixels are tracked in the production helper's direct free calls.
code += helper
code += r'''
#undef free
int main(void) {
    const uint8_t data[16]={0}; original_input=data; free_psram=32*1024*1024;
    for(int sampling=0;sampling<3;++sampling) {
        header=(jpeg_decode_picture_info_t){72,72,sampling};
        expected_size=(sampling==0?144:160)*(sampling==2?80:72);
        rg_image_t *img=p4_jpeg_decode(data,sizeof(data),0);
        assert(img && img->width==72 && img->height==72 && img->stride==(sampling==0?144:160));
        assert(img->format==RG_PIXEL_565_LE && img->free_data);
        rg_surface_free(img); assert(!live_engines && !live_buffers);
    }
    header=(jpeg_decode_picture_info_t){72,72,2}; expected_size=160*80;
    rg_image_t *img=p4_jpeg_decode(data,sizeof(data),15);
    assert(img && img->width==15 && img->height==15); rg_surface_free(img);
    fail_resize=1; assert(!p4_jpeg_decode(data,sizeof(data),15)); fail_resize=0;
    for(int i=0;i<5;++i) {
        int *failure=i==0?&fail_info:i==1?&fail_alloc:i==2?&fail_engine:i==3?&fail_process:&fail_input;
        *failure=1; assert(!p4_jpeg_decode(data,sizeof(data),0)); *failure=0;
        assert(!live_buffers && !live_engines);
    }
    free_psram=1024; assert(!p4_jpeg_decode(data,sizeof(data),0));
    free_psram=32*1024*1024;
    header.sample_method=3; assert(!p4_jpeg_decode(data,sizeof(data),0));
    header=(jpeg_decode_picture_info_t){31,17,2}; assert(!p4_jpeg_decode(data,sizeof(data),0));
    header=(jpeg_decode_picture_info_t){31,24,2}; assert(!p4_jpeg_decode(data,sizeof(data),0));
    header=(jpeg_decode_picture_info_t){40,40,2}; assert(!p4_jpeg_decode(data,sizeof(data),0));
    header=(jpeg_decode_picture_info_t){4096,4096,2}; assert(!p4_jpeg_decode(data,sizeof(data),0));
    header.width=0; assert(!p4_jpeg_decode(data,sizeof(data),0));
    assert(!live_engines && !live_buffers);
    puts("P4 JPEG: MCU padding, RGB565 order, aspect ratio, limits, failures and ownership passed");
}
'''
with tempfile.TemporaryDirectory() as folder:
    source = Path(folder) / 'p4-jpeg.c'
    executable = Path(folder) / 'p4-jpeg'
    source.write_text(code)
    subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
