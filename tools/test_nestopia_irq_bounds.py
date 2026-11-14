"""Compare the actual bounded A12 hook with the original instruction hook."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'retro-core/components/nestopia'

PRELUDE = r'''
#include "NstCore.hpp"
#include "NstAssert.hpp"
#include "NstHook.hpp"
#include "NstIoLine.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <utility>
#define NST_CPU_H
#define NST_PPU_H
namespace Nes { namespace Core {
struct Cpu {
    enum { IRQ_EXT=1 };
    Cycle now=0, frame; uint cpuClock;
    uint64_t frameBase=0;
    Hook hook;
    std::vector<std::pair<uint64_t,uint64_t>> events;
    Cpu(uint one):frame(341*262*one),cpuClock(one==4?12:16) { hook.Unset(); }
    Cycle GetCycles() const { return now; }
    Cycle GetClock(uint n=1) const { return n*cpuClock; }
    Cycle GetFrameCycles() const { return frame; }
    void AddHook(const Hook& h) { hook=h; }
    void DoIRQ(uint, Cycle cycle) { events.push_back({frameBase+cycle,frameBase+now}); }
    void ClearIRQ() {}
};
struct Ppu {
    Cpu &cpu; uint one; unsigned calls=0;
    uint64_t cursor=0; Address address=0;
    Io::Line line; bool maskNotifications=false;
    Ppu(Cpu& c,uint o):cpu(c),one(o) { line.Unset(); }
    Cycle GetClock(uint n=1) const { return n*one; }
    bool CanDeferCpuSynchronization(Cycle) const { return true; }
    bool CanDeferA12Synchronization(Cycle,Cycle blockedUntil,uint hold) const {
        const uint64_t end=(cpu.frameBase+cpu.now)/one;
        const uint dot=cursor%341;
        const uint mode=(cursor/8192)%3;
        const uint64_t limit=cursor-dot+(mode==1?260:256);
        return dot<256 && cursor/8192==limit/8192 && end<limit &&
            (mode==1 || (mode==2 && hold>8 &&
             blockedUntil+cpu.frameBase>(cursor+8)*one));
    }
    uint SetAddressLineHook(const Io::Line& l) { line=l; return address; }
    const Io::Line& GetAddressLineHook() const { return line; }
    static void NST_REGCALL sync(void *p) { static_cast<Ppu*>(p)->SynchronizeCpu(); }
    void EnableCpuSynchronization() { cpu.AddHook(Hook(this,&sync)); }
    void EnableCpuSynchronization(const Hook& h) { cpu.AddHook(h); }
    void Update() { SynchronizeCpu(); }
    void SynchronizeCpu() {
        ++calls;
        const uint64_t end=cpu.frameBase+cpu.now;
        while((cursor+1)*one<=end) {
            ++cursor;
            const uint dot=cursor%341;
            // Normal background/sprite fetches, interleaved with irregular
            // A12 bursts which exercise rejection by the hold filter.
            const uint scramble=uint(cursor)*1103515245U+12345U;
            const uint mode=(cursor/8192)%3;
            const bool high=mode==1 ? (dot>=260 && dot<320 && (dot&7)>=4)
                : mode==2 ? ((dot<256 || (dot>=320 && dot<336)) && (dot&7)>=4)
                          : ((scramble>>16)&1);
            const uint previous=address;
            address=(scramble&0x2fff)|(high?0x1000:0);
            if(line && (!maskNotifications || ((previous^address)&0x1000)))
                line.Toggle(address,Cycle(cursor*one-cpu.frameBase));
        }
    }
    void AddressWrite(Address a) {
        Update(); address=a;
        if(line) line.Toggle(address,cpu.now);
    }
};
}}
#include "NstTimer.hpp"
using namespace Nes::Core;
using namespace Nes;
struct LegacyIrq {
    uint count=0,latch=0; bool reload=false,enabled=false,persistent;
    LegacyIrq(bool p):persistent(p) {}
    void Reset(bool) { count=latch=0; reload=enabled=false; }
    // CLOCK_BODY
};
struct BoundIrq : LegacyIrq {
    BoundIrq(bool p):LegacyIrq(p) {}
    // DISTANCE_BODY
};
static uint randomWord() {
    static uint32_t state=0x168a12;
    state^=state<<13; state^=state>>17; state^=state<<5;
    return state;
}
template<uint Hold,uint Delay>
static void check(uint one,bool persistent,unsigned &steps,unsigned &original,unsigned &bounded) {
    Cpu refCpu(one),fastCpu(one);
    Ppu refPpu(refCpu,one),fastPpu(fastCpu,one);
    fastPpu.maskNotifications=true;
    Timer::A12<LegacyIrq,Hold,Delay> reference(refCpu,refPpu,persistent);
    Timer::A12<BoundIrq,Hold,Delay> candidate(fastCpu,fastPpu,persistent);
    reference.Reset(true); candidate.Reset(true);
    reference.unit.latch=candidate.unit.latch=17;
    reference.unit.enabled=candidate.unit.enabled=true;
    reference.unit.reload=candidate.unit.reload=true;
    for(unsigned step=0;step<50000;step++) {
        const Cycle end=refCpu.now+refCpu.GetClock(2+randomWord()%6);
        if(step%521==0 || step%73==0) {
            refCpu.now=fastCpu.now=end-refCpu.GetClock();
            reference.Update(); candidate.Update();
            assert(reference.unit.count==candidate.unit.count);
            assert(reference.unit.reload==candidate.unit.reload);
            if(step%521==0) {
                uint value=randomWord();
                reference.unit.latch=candidate.unit.latch=(value%4)?(value&31):(value&255);
                reference.unit.reload=candidate.unit.reload=true;
                reference.unit.enabled=candidate.unit.enabled=(value&3)!=0;
            } else {
                const Address a=randomWord()&0x3fff;
                refPpu.AddressWrite(a); fastPpu.AddressWrite(a);
            }
        }
        refCpu.now=fastCpu.now=end;
        refCpu.hook.Execute(); fastCpu.hook.Execute();
        assert(refCpu.events==fastCpu.events);
        if(end>=refCpu.frame) {
            reference.Update(); candidate.Update();
            assert(reference.unit.count==candidate.unit.count);
            assert(refCpu.events==fastCpu.events);
            refCpu.now-=refCpu.frame; fastCpu.now-=fastCpu.frame;
            reference.VSync(); candidate.VSync();
            refCpu.frameBase+=refCpu.frame; fastCpu.frameBase+=fastCpu.frame;
        }
        ++steps;
    }
    reference.Update(); candidate.Update();
    assert(reference.unit.count==candidate.unit.count && refCpu.events==fastCpu.events);
    assert(!refCpu.events.empty());
    if(Hold) assert(fastPpu.calls<refPpu.calls);
    else assert(fastPpu.calls<=refPpu.calls);
    original+=refPpu.calls; bounded+=fastPpu.calls;
}
int main() {
    unsigned steps=0,original=0,bounded=0;
    for(uint one:{4U,5U}) for(bool persistent:{false,true}) {
        check<16,0>(one,persistent,steps,original,bounded);
        check<8,2>(one,persistent,steps,original,bounded);
        check<0,0>(one,persistent,steps,original,bounded);
    }
    printf("PASS: %u CPU instruction boundaries, identical IRQ clocks and delivery boundaries; PPU syncs %u -> %u; NTSC/PAL, hold 0/8/16, delayed IRQ, reload/enable/disable, PPU writes and frame rebasing\n",steps,original,bounded);
}
'''


def method(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


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
        mmc3 = (core / 'board/NstBoardMmc3.hpp').read_text()
        source = PRELUDE.replace('// CLOCK_BODY', method(mmc3, 'NST_FORCE_INLINE bool Clock()'))
        source = source.replace('// DISTANCE_BODY', method(mmc3, 'uint GetSyncDistance() const'))
        path = temp / 'bounds.cpp'
        path.write_text(source)
        command = [args.cxx] + (['c++'] if Path(args.cxx).stem == 'zig' else [])
        exe = temp / 'bounds.exe'
        subprocess.run(command + ['-std=c++11', '-O2', '-UNDEBUG', '-Wno-narrowing',
                                  '-I'+str(core), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
