"""Compare the production grouped OAM evaluator with upstream's per-dot phases."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'retro-core/components/nestopia'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
upstream = (PORT / 'upstream/source/core/NstPpu.cpp').read_text()
tiles = (PORT / 'rg_nestopia_tiles.inc').read_text()
start = tiles.index('{', tiles.index('if (combinedEvaluation)'))
depth, end = 1, start + 1
while depth:
    depth += (tiles[end] == '{') - (tiles[end] == '}')
    end += 1
span = tiles[start+1:end-1]
names = ['StepOam2Address', 'CorruptOam', 'ReadSecondaryOam', 'EvaluateSpritesEven', 'EvaluateSpritesOdd']
names += [f'EvaluateSpritesPhase{i}' for i in range(10)]
bodies = []
for name in names:
    body = re.search(r'(?:NST_FORCE_INLINE )?void Ppu::' + name + r'\([^)]*\)\n\t\t\{.*?\n\t\t\}', upstream, re.S)
    assert body, name
    bodies.append(body.group(0))
code = r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using uint=unsigned; using byte=uint8_t;
#define NST_FORCE_INLINE inline
struct Ppu {
    struct Regs { enum { STATUS_SP_OVERFLOW=0x20 }; uint status; } regs;
    struct { uint hClock; } cycles;
    int scanline;
    struct Oam {
        enum { MAX_LINE_SPRITES=32, NO_CORRUPTION=255 };
        byte ram[256],buffer[128];
        byte* buffered; const byte* limit;
        void (Ppu::*phase)();
        uint address,index,latch,height,secondary,corrupt,oam2Address;
        bool oam2Full,spriteZeroInLine;
    } oam;
    void StepOam2Address(uint); void CorruptOam(); void ReadSecondaryOam();
    void EvaluateSpritesEven(); void EvaluateSpritesOdd();
'''
code += ''.join(f'void EvaluateSpritesPhase{i}();\n' for i in range(10))
code += 'void Span(uint dots) {' + span + '}\n};\n' + '\n'.join(bodies)
code += r'''
static uint randomWord() {
    static uint state=0x81ac9b13;
    state^=state<<13; state^=state>>17; state^=state<<5; return state;
}
int main() {
    for(uint test=0;test<200000;test++) {
        Ppu ref={},batch={};
        for(auto& x:ref.oam.ram) x=randomWord();
        for(auto& x:ref.oam.buffer) x=randomWord();
        ref.scanline=randomWord()%240; ref.cycles.hClock=64;
        ref.regs.status=randomWord()&255;
        ref.oam.height=randomWord()&1?8:16;
        ref.oam.limit=ref.oam.buffer+(randomWord()&1?32:128);
        ref.oam.buffered=ref.oam.buffer;
        ref.oam.phase=(test&15)?&Ppu::EvaluateSpritesPhase1:&Ppu::EvaluateSpritesPhase0;
        ref.oam.address=randomWord()&255; ref.oam.secondary=(randomWord()%33)*4;
        ref.oam.oam2Address=randomWord()%32; ref.oam.oam2Full=randomWord()&1;
        ref.oam.corrupt=Ppu::Oam::NO_CORRUPTION;
        const uint before=(randomWord()%23)*4;
        for(uint i=0;i<before;i++) {
            ref.EvaluateSpritesEven(); ++ref.cycles.hClock;
            ref.EvaluateSpritesOdd(); ++ref.cycles.hClock;
        }
        // Resuming rendering can corrupt the row containing the next read.
        if(test&1) ref.oam.corrupt=ref.oam.address/8;
        const uint dots=(1+randomWord()%((92-before)/4))*8;
        batch=ref;
        batch.oam.buffered=batch.oam.buffer+(ref.oam.buffered-ref.oam.buffer);
        batch.oam.limit=batch.oam.buffer+(ref.oam.limit-ref.oam.buffer);
        batch.Span(dots);
        for(uint i=0;i<dots/2;i++) {
            ref.EvaluateSpritesEven(); ++ref.cycles.hClock;
            ref.EvaluateSpritesOdd(); ++ref.cycles.hClock;
        }
        bool equal=!memcmp(ref.oam.ram,batch.oam.ram,256) && !memcmp(ref.oam.buffer,batch.oam.buffer,128);
        equal=equal && ref.oam.buffered-ref.oam.buffer==batch.oam.buffered-batch.oam.buffer;
        equal=equal && ref.oam.phase==batch.oam.phase && ref.regs.status==batch.regs.status;
        const uint a[]={ref.oam.address,ref.oam.index,ref.oam.latch,ref.oam.height,ref.oam.secondary,ref.oam.corrupt,
                        ref.oam.oam2Address,ref.oam.oam2Full,ref.oam.spriteZeroInLine};
        const uint b[]={batch.oam.address,batch.oam.index,batch.oam.latch,batch.oam.height,batch.oam.secondary,batch.oam.corrupt,
                        batch.oam.oam2Address,batch.oam.oam2Full,batch.oam.spriteZeroInLine};
        equal=equal && !memcmp(a,b,sizeof(a));
        if(!equal) { fprintf(stderr,"OAM state differs: case %u, before %u, span %u\n",test,before,dots); return 1; }
    }
    puts("PASS: 200000 grouped/scalar OAM spans, misaligned addresses, corruption, overflow and both sprite limits");
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder=Path(directory); source=folder/'oam.cpp'; exe=folder/'oam.exe'
    source.write_text(code)
    command=[args.cxx] + (['c++'] if Path(args.cxx).stem=='zig' else [])
    subprocess.run(command+['-std=c++11','-O2','-UNDEBUG',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
