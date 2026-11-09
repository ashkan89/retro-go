"""Exercise production PPA geometry, palette expansion, DMA bounds and fallbacks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
directory = root / 'components/retro-go/drivers/display'
geometry = (directory / 'p4_scale_geometry.h').read_text().replace('#pragma once', '')
helper = '\n'.join(line for line in (directory / 'p4_ppa.h').read_text().splitlines()
                   if not line.startswith('#include'))
pixels = (directory / 'p4_pixels.h').read_text().replace('#pragma once', '')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define ESP_OK 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define PPA_OPERATION_SRM 1
#define PPA_SRM_COLOR_MODE_RGB565 1
#define PPA_TRANS_MODE_BLOCKING 1
#define RG_LOGI(...) ((void)0)
#define RG_PIXEL_565_BE 1
#define RG_PIXEL_565_LE 2
#define RG_PIXEL_PAL565_BE 129
#define RG_PIXEL_PAL565_LE 130
typedef void *ppa_client_handle_t;
typedef struct { int oper_type, max_pending_trans_num; } ppa_client_config_t;
typedef struct { const void *buffer; size_t buffer_size; int pic_w,pic_h,block_w,block_h,srm_cm; } picture_t;
typedef struct { picture_t in,out; float scale_x,scale_y; bool byte_swap; int mode; } ppa_srm_oper_config_t;
typedef struct { int width,height,stride,offset,format; void *data; uint16_t *palette; } rg_surface_t;
static bool fail_register,fail_alloc,fail_operation;
static int live_clients,calls,vector_calls;
static uint16_t expected[256];
static int ppa_register_client(const ppa_client_config_t *c,ppa_client_handle_t *h) {
    assert(c->oper_type==1 && c->max_pending_trans_num==1);
    if(fail_register) return 1;
    *h=(void*)1; ++live_clients; return 0;
}
static int ppa_unregister_client(ppa_client_handle_t h) { assert(h); --live_clients; return 0; }
static void *heap_caps_aligned_alloc(size_t a,size_t n,int caps) {
    assert(a==64 && n%64==0 && caps==3); return fail_alloc ? NULL : malloc(n);
}
static int ppa_do_scale_rotate_mirror(ppa_client_handle_t h,const ppa_srm_oper_config_t *o) {
    assert(h && o->mode==1 && o->in.srm_cm==1 && o->out.srm_cm==1);
    assert((int)(o->in.block_w*o->scale_x)==o->out.pic_w);
    assert((int)(o->in.block_h*o->scale_y)==o->out.pic_h);
    assert(o->out.buffer_size%64==0 && o->out.buffer_size>=(size_t)o->out.pic_w*o->out.pic_h*2);
    for(int y=0;y<o->in.block_h;++y) for(int x=0;x<o->in.block_w;++x) {
        uint16_t p;
        memcpy(&p,(const uint8_t*)o->in.buffer+(y*o->in.pic_w+x)*2,2);
        if(o->byte_swap) p=(p<<8)|(p>>8);
        assert(p==expected[x%256]);
    }
    ++calls; return fail_operation ? 1 : ESP_OK;
}
void p4_swap_pixels(uint16_t *d,const uint16_t *s,size_t n) {
    assert(n && n%8==0); ++vector_calls;
    while(n--) { uint16_t p=*s++; *d++=(p<<8)|(p>>8); }
}
'''
code += geometry + helper + pixels
code += r'''
int main(void) {
    const int cases[][4]={{240,160,480,320},{256,240,336,315},{256,192,416,312},
        {160,144,350,315},{256,224,352,308},{320,224,440,308},{320,200,480,300}};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        int w=480,h=320; p4_fit_viewport(cases[i][0],cases[i][1],480,320,true,&w,&h);
        assert(w==cases[i][2] && h==cases[i][3]);
        assert(p4_scale_factor(cases[i][0],w)==p4_scale_factor(cases[i][1],h));
    }
    int w=480,h=320; p4_fit_viewport(256,240,480,320,false,&w,&h);
    assert(w==480 && h==315);
    assert(!p4_scale_factor(256,341) && !p4_scale_factor(0,320) && !p4_scale_factor(256,4096));
    uint8_t data[4*520+4]; uint16_t palette[256];
    for(int i=0;i<256;++i) expected[i]=(i*239)^0xace1;
    const int formats[]={1,2,129,130};
    for(int f=0;f<4;++f) {
        int indexed=f>=2,be=(f%2)==0;
        rg_surface_t source={.width=256,.height=4,.stride=indexed?260:520,.offset=4,
            .format=formats[f],.data=data,.palette=palette};
        memset(data,0xee,sizeof(data));
        for(int x=0;x<256;++x) palette[x]=be?(expected[x]<<8)|(expected[x]>>8):expected[x];
        for(int y=0;y<4;++y) for(int x=0;x<256;++x) {
            uint8_t *p=data+4+y*source.stride+x*(indexed?1:2);
            if(indexed) *p=x; else memcpy(p,&palette[x],2);
        }
        const rg_surface_t *out=p4_scale_frame(&source,336,5);
        assert(out && out->width==336 && out->height==5 && out->stride==672 && out->format==2);
        assert(!p4_scale_frame(&source,341,5));
        fail_operation=true; assert(!p4_scale_frame(&source,336,5)); fail_operation=false;
        p4_scaler_deinit(); assert(!live_clients);
        fail_alloc=true; assert(!p4_scale_frame(&source,336,5)); fail_alloc=false;
        p4_scaler_deinit(); assert(!live_clients);
        fail_register=true; assert(!p4_scale_frame(&source,336,5)); fail_register=false;
        assert(!p4_scale_frame(&source,336,5)); p4_scaler_deinit();
        assert(p4_scale_frame(&source,336,5)); p4_scaler_deinit();
    }
    // Odd source heights must use the truncated output size, not exact ratios.
    assert(p4_scale_factor(241,316)==21);
    // Exercise every 16-byte alignment pair and short/tail lengths. The real
    // assembly is additionally tested on COM3; the host checks dispatch/bounds.
    _Alignas(16) uint16_t src[512],dst[512];
    for(int i=0;i<512;++i) src[i]=(i*317)^0x1234;
    for(int a=0;a<8;++a) for(int b=0;b<8;++b) for(int n=0;n<481;n+=7) {
        memset(dst,0xa5,sizeof(dst)); p4_copy_rgb565_be(dst+b,src+a,n);
        for(int i=0;i<n;++i) assert(dst[b+i]==(uint16_t)((src[a+i]<<8)|(src[a+i]>>8)));
        assert(dst[b+n]==0xa5a5); if(b) assert(dst[b-1]==0xa5a5);
    }
    assert(vector_calls && calls); puts("PASS: PPA viewports, RGB/palette endian and stride, DMA bounds, failures, SIMD dispatch/tails");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp) / 'scaler.c', Path(tmp) / 'scaler.exe'
    src.write_text(code)
    subprocess.run([args.cc, '-std=gnu11', '-O2', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
