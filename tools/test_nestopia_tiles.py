"""Compare the production sprite compositor with scalar 8-184-dot spans."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'retro-core/components/nestopia'

PRELUDE = r'''
#include <cstdint>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
using byte=uint8_t; using word=uint16_t; using uint=unsigned;
using dword=uint32_t; using idword=int32_t;
namespace Video { struct Screen { using Pixel=word; }; }
#define NST_FORCE_INLINE inline
#define NST_RESTRICT
#define NST_MIN(a,b) ((a)<(b)?(a):(b))
struct Ppu {
    struct Regs { enum { STATUS_SP_ZERO_HIT=0x40 }; uint status; } regs;
    struct { uint hClock, count; } cycles;
    struct { uint xFine; } scroll;
    struct { byte pixels[16]; uint mask; } tiles;
    struct Oam {
        struct Output { dword deadline; byte shift,behind,zero,palette,pixels[8]; } output[32];
        Output *visible; dword clock; uint mask;
    } oam;
    struct { word palette[32], *target; } output;
    word buffer[256];
    void RenderPixel(); void PrepareSpriteTile(word*); void RenderTilePixel(word);
    void RenderTilePixels(const word*, uint);
};
'''

RUNNER = r'''
static uint randomWord() {
    static uint state=0x941779ab;
    state^=state<<13; state^=state>>17; state^=state<<5; return state;
}
int main() {
    for(uint test=0;test<131072;test++) {
        Ppu scalar={},batch={};
        scalar.regs.status=randomWord()&255;
        scalar.cycles.hClock=(randomWord()%31)*8;
        uint limit=scalar.cycles.hClock<64?64:248;
        uint dots=(1+randomWord()%((limit-scalar.cycles.hClock)/8))*8;
        scalar.cycles.count=scalar.cycles.hClock+dots;
        scalar.scroll.xFine=randomWord()%8;
        scalar.tiles.mask=randomWord()&1?15:0;
        scalar.oam.mask=randomWord()&1?3:0;
        scalar.oam.clock=test%32?randomWord():0xfffffffcU;
        uint visible=randomWord()%33;
        scalar.oam.visible=scalar.oam.output+visible;
        for(uint i=0;i<16;i++) scalar.tiles.pixels[i]=randomWord()&15;
        for(uint i=0;i<256;i++) scalar.buffer[i]=0xbeef;
        for(uint i=0;i<32;i++) {
            scalar.output.palette[i]=randomWord()&511;
            auto &s=scalar.oam.output[i];
            int wait=(test&1)?int(randomWord()%24)-8:int(randomWord()%280)-20;
            s.deadline=scalar.oam.clock+wait;
            s.shift=randomWord()%9;
            s.behind=randomWord()&1?3:0;
            s.zero=i==0?3:0;
            s.palette=16+(randomWord()%4)*4;
            for(uint j=0;j<8;j++)s.pixels[j]=randomWord()&3;
        }
        batch=scalar;
        scalar.output.target=scalar.buffer+4;
        batch.output.target=batch.buffer+4;
        batch.oam.visible=batch.oam.output+visible;
        word sprites[248]; batch.PrepareSpriteTile(sprites);
        Ppu grouped=batch;
        grouped.output.target=grouped.buffer+4;
        grouped.oam.visible=grouped.oam.output+visible;
        for(uint dot=0;dot<dots;dot++) {
            scalar.RenderPixel(); batch.RenderTilePixel(sprites[dot]);
            if((dot&7)==7) {
                const uint clock=grouped.cycles.hClock;
                grouped.cycles.hClock+=8;
                grouped.RenderTilePixels(sprites+dot-7,clock);
            }
            if(dot==7) scalar.oam.mask=batch.oam.mask=grouped.oam.mask=randomWord()&1?3:0;
        }
        batch.oam.clock+=dots;
        grouped.oam.clock+=dots;
        assert(!memcmp(scalar.buffer,batch.buffer,sizeof(scalar.buffer)));
        assert(!memcmp(scalar.buffer,grouped.buffer,sizeof(scalar.buffer)));
        assert(scalar.regs.status==batch.regs.status);
        assert(scalar.regs.status==grouped.regs.status);
        assert(scalar.cycles.hClock==batch.cycles.hClock && scalar.oam.clock==batch.oam.clock);
        assert(scalar.cycles.hClock==grouped.cycles.hClock && scalar.oam.clock==grouped.oam.clock);
        assert(scalar.output.target==scalar.buffer+4+dots && batch.output.target==batch.buffer+4+dots);
        assert(grouped.output.target==grouped.buffer+4+dots);
        for(uint i=0;i<32;i++) {
            assert(scalar.oam.output[i].shift==batch.oam.output[i].shift);
            assert(scalar.oam.output[i].deadline==batch.oam.output[i].deadline);
        }
    }
    puts("PASS: 262144 scalar/span and eight-pixel comparisons (8-184 dots), 0-32 sprites, priority, changing clipping, transparency, sprite-zero and clock wrap");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('prepare_core', PORT / 'prepare_core.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder)
        core = module.prepare(PORT / 'upstream/source/core', temp / 'core')
        source = (core / 'NstPpu.cpp').read_text(encoding='utf-8')
        scalar = source[source.index('NST_FORCE_INLINE void Ppu::RenderPixel()'):
                        source.index('NST_SINGLE_CALL void Ppu::RenderPixel255()')]
        tile = (PORT / 'rg_nestopia_tiles.inc').read_text(encoding='utf-8')
        tile = tile[:tile.index('template<bool Direct>')]
        path = temp / 'tiles.cpp'
        path.write_text(PRELUDE + scalar + tile + RUNNER, encoding='utf-8')
        command = [args.cxx] + (['c++'] if Path(args.cxx).stem == 'zig' else [])
        executable = temp / 'tiles.exe'
        subprocess.run(command + ['-std=c++11', '-O2', '-UNDEBUG', str(path), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
