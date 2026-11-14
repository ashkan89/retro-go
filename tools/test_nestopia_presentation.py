"""Exercise Nestopia's production presentation path with a slow borrowed display."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
source = (ROOT / 'retro-core/main/main_nestopia.cpp').read_text()
functions = source[source.index('static void submit_video('):source.index('static void event_handler(')]
draw = source[source.index('        const bool drawFrame = wantFrame && acquire_video_surface();'):]
draw = draw[:draw.index('        if (nsfPlayer &&')]

code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct rg_surface_t { int width, height, stride, offset; void *data; };
struct App { bool frameskipManual; } appValue, *app=&appValue;
static rg_surface_t surfaces[2], *updates[2], *currentUpdate;
static int overscan=true, refreshRate=60, autocrop;
static bool frameCopy=true;
static int64_t now, releaseAt, duration;
static unsigned submitted, blocked, emulated;
static rg_surface_t *borrowed;
static rg_surface_t metadata;
static uint32_t pixels[2], snapshot;
static void consume() {
    if (!borrowed || now<releaseAt) return;
    assert(std::memcmp(borrowed,&metadata,sizeof(metadata))==0);
    assert(*static_cast<uint32_t*>(borrowed->data)==snapshot);
    borrowed=nullptr;
}
static bool rg_display_uses_frame_copy() { return frameCopy; }
static bool rg_display_sync(bool wait) {
    consume();
    if (borrowed && wait) { ++blocked; now=releaseAt; consume(); }
    return !borrowed;
}
enum { RG_DISPLAY_SUBMIT_BORROW=1 };
static void rg_display_submit(rg_surface_t *s,unsigned flags) {
    assert(flags==RG_DISPLAY_SUBMIT_BORROW && !borrowed);
    borrowed=s; metadata=*s; snapshot=*static_cast<uint32_t*>(s->data);
    releaseAt=now+duration; ++submitted;
}
namespace Nes {
enum { RESULT_OK=0 };
namespace Core { namespace Video {
struct Output { void *data; int stride; Output(void *d,int s):data(d),stride(s) {} };
} }
}
static struct {
    int Blit(Nes::Core::Video::Output &out) {
        assert(!borrowed || out.data!=borrowed->data);
        *static_cast<uint32_t*>(out.data)=emulated;
        return Nes::RESULT_OK;
    }
} video;
#define RG_PANIC(message) assert(false)
'''
code += functions
code += '\nstatic bool step(bool wantFrame) {\n' + draw + '\nreturn drawFrame;\n}\n'
code += r'''
static void reset(bool manual,int64_t renderTime,bool copy=true) {
    now=releaseAt=0; duration=renderTime; borrowed=nullptr;
    submitted=blocked=emulated=0; frameCopy=copy; app->frameskipManual=manual;
    for(int i=0;i<2;++i) surfaces[i]={256,240,512,0,pixels+i};
    updates[0]=surfaces; updates[1]=copy?surfaces:surfaces+1;
    currentUpdate=updates[0];
}
static void run(unsigned frames,unsigned skip=0) {
    for(unsigned i=0;i<frames;++i) {
        now+=16667; consume(); ++emulated;
        step(i%(skip+1)==0);
    }
    rg_display_sync(true);
}
int main() {
    reset(false,8000); run(600);
    assert(emulated==600 && submitted==600 && blocked==1 && now<10020000);
    reset(false,35000); run(600);
    assert(emulated==600 && submitted==200 && blocked==1 && now<10040000);
    // Manual zero keeps every presentation, including its unavoidable wait.
    reset(true,35000); run(600);
    assert(emulated==600 && submitted==600 && blocked==600 && now>21000000);
    reset(true,35000); run(600,2);
    assert(emulated==600 && submitted==200);
    // A menu redraw must release the borrow before changing crop metadata.
    reset(false,35000); ++emulated; assert(step(true));
    autocrop=1; overscan=0; submit_video();
    assert(blocked==1 && borrowed->width==240 && borrowed->height==240);
    assert(borrowed->offset==16); rg_display_sync(true);
    // Switching from Manual to Auto must immediately stop waiting on the LCD.
    reset(true,35000); ++emulated; assert(step(true));
    app->frameskipManual=false; assert(!step(true)); assert(blocked==0);
    now=releaseAt; ++emulated; assert(step(true)); rg_display_sync(true);
    // Legacy double surfaces retain the original blocking submission behavior.
    reset(false,35000,false); run(12); assert(submitted==12);
    puts("PASS: native every-frame output, slow-LCD Auto pacing, immutable borrows, Manual 0/2, redraws and mode changes");
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    src, executable = temp / 'presentation.cpp', temp / 'presentation.exe'
    src.write_text(code)
    command = [args.cxx] + (['c++'] if Path(args.cxx).stem == 'zig' else [])
    subprocess.run(command + ['-std=c++11', '-O2', '-UNDEBUG', str(src), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
