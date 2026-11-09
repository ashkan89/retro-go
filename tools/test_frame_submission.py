"""Exercise production frame submission ownership and busy-queue dropping."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'components/retro-go/rg_display.c').read_text()
functions = source[source.index('bool rg_display_uses_frame_copy('):source.index('bool rg_display_sync(bool block)')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define RG_DISPLAY_ASYNC_FRAMES 1
#define RG_PIXEL_PAL888 0x83
#define RG_PIXEL_PALETTE 0x80
#define MEM_SLOW 0
#define MEM_NOPANIC 0
typedef struct { int width,height,stride,offset,format; void *data; uint16_t *palette; } rg_surface_t;
typedef struct { const void *dataPtr; } rg_task_msg_t;
static struct { bool isLauncher; } app;
static struct { struct { int width,height; } source; bool changed; } display;
static struct { int64_t blockTime; int totalFrames; } counters;
static rg_surface_t *frame_copy;
static void *frame_copy_lock;
static bool locked;
static bool rg_mutex_take(void *lock,int timeout) { assert(timeout==0); if(locked)return false; locked=true;return true; }
static void rg_mutex_give(void *lock) { assert(locked);locked=false; }
static void *display_task_queue;
static bool busy;
static const rg_surface_t *queued;
static int sends, frees;
static void *rg_system_get_app_(void) { return &app; }
#define rg_system_get_app() (&app)
static int64_t rg_system_timer(void) { return 0; }
static bool rg_display_sync(bool block) { if (block) busy=false; return !busy; }
static void rg_surface_free(rg_surface_t *p) { if(p) { free(p); frees++; } }
static rg_surface_t *rg_surface_create(int w,int h,int f,int flags) {
    int stride=w*((f&0x80)?1:2);
    rg_surface_t *p=calloc(1,sizeof(*p)+stride*h+768);
    *p=(rg_surface_t){.width=w,.height=h,.stride=stride,.format=f,.data=p+1};
    p->palette=(uint16_t *)((uint8_t *)p->data+stride*h); return p;
}
static void rg_task_send(void *q,const rg_task_msg_t *msg) {
    assert(!busy); busy=true; queued=msg->dataPtr; sends++;
}
'''
code += functions
code += r'''
int main(void) {
    uint8_t pixels[32]; uint16_t palette[256];
    for(int i=0;i<32;i++) pixels[i]=i;
    for(int i=0;i<256;i++) palette[i]=i*17;
    rg_surface_t input={.width=3,.height=2,.stride=8,.offset=2,.format=0x81,.data=pixels,.palette=palette};
    locked=true;
    rg_display_submit(&input,0); // Another producer owns the snapshot.
    assert(sends==0);
    locked=false;
    rg_display_submit(&input,0);
    assert(sends==1 && queued!=&input && queued->offset==0 && queued->stride==3);
    assert(!memcmp(queued->data,pixels+2,3));
    assert(!memcmp((uint8_t *)queued->data+3,pixels+10,3));
    assert(!memcmp(queued->palette,palette,512));
    memset(pixels,0,sizeof(pixels)); memset(palette,0,sizeof(palette));
    assert(((uint8_t *)queued->data)[0]==2 && queued->palette[1]==17);
    rg_display_submit(&input,0);
    assert(sends==1 && ((uint8_t *)queued->data)[0]==2 && !frees);
    input.width=4;
    rg_display_submit(&input,0); // Geometry changes must also drop while busy.
    assert(sends==1 && !frees);
    busy=false;
    rg_display_submit(&input,0);
    assert(sends==2 && frees==1 && queued->width==4 && counters.totalFrames==2);
    busy=false;
    input.format=2; // Packed RGB565: exercise the full-frame copy with an offset.
    for(int i=0;i<32;i++) pixels[i]=i+13;
    rg_display_submit(&input,0);
    assert(sends==3 && queued->stride==8 && !memcmp(queued->data,pixels+2,16));
    memset(pixels,0,sizeof(pixels));
    assert(((uint8_t *)queued->data)[0]==15 && ((uint8_t *)queued->data)[15]==30);
    busy=false; app.isLauncher=true;
    rg_display_submit(&input,0);
    assert(sends==4 && queued==&input);
    rg_surface_free(frame_copy);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)
    (path / 'test.c').write_text(code)
    binary = path / 'test.exe'
    subprocess.run([args.cc, '-std=gnu99', '-O2', '-UNDEBUG', str(path / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Frame submission: owned pixels/palette, crop/stride, busy dropping, resize, launcher passed')
