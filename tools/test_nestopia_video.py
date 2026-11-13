"""Compare production unfiltered RGB conversion with the pinned core."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'retro-core/components/nestopia'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('prepare', PORT / 'prepare_core.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    prepared = module.prepare(PORT / 'upstream/source/core', temp / 'core')
    code = r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using uint=unsigned; using word=uint16_t; using dword=uint32_t; using byte=uint8_t;
#define NST_RESTRICT
struct Input { using Pixel=word; word pixels[256*240+1]; dword palette[512]; };
struct Output { void *pixels; long pitch; };
'''
    for name, core in [('Reference', PORT / 'upstream/source/core'), ('Prepared', prepared)]:
        text = (core / 'NstVideoFilterNone.cpp').read_text()
        functions = []
        for method in ('BlitAligned', 'BlitUnaligned'):
            body = re.search(r'template<typename T>\s+void Renderer::FilterNone::' + method +
                             r'\(.*?\n\t\t\t\}', text, re.S).group(0)
            functions.append(body.replace('void Renderer::FilterNone::', 'static void '))
        code += 'struct ' + name + ' { enum { WIDTH=256, HEIGHT=240, PIXELS=256*240 };\n'
        code += '\n'.join(functions) + '\n};\n'
    code += r'''
static Input input;
static uint32_t randomState=0x9e3779b9;
static uint32_t randomValue() { randomState^=randomState<<13; randomState^=randomState>>17; return randomState^=randomState<<5; }
template<class T> void check() {
    alignas(16) byte expected[256*240*4+240*16+64];
    alignas(16) byte actual[sizeof(expected)];
    for(unsigned seed=0;seed<32;seed++) {
        for(auto &p: input.pixels) p=randomValue()&511;
        for(auto &p: input.palette) p=randomValue();
        for(unsigned padding: {0U,8U}) for(unsigned offset: {0U,unsigned(sizeof(T))}) {
            memset(expected,0xa5,sizeof(expected)); memset(actual,0xa5,sizeof(actual));
            Output reference={expected+16+offset,256*long(sizeof(T))+long(padding)};
            Output prepared={actual+16+offset,reference.pitch};
            if(padding) { Reference::BlitUnaligned<T>(input,reference); Prepared::BlitUnaligned<T>(input,prepared); }
            else { Reference::BlitAligned<T>(input,reference); Prepared::BlitAligned<T>(input,prepared); }
            assert(memcmp(actual,expected,sizeof(actual))==0);
        }
    }
}
int main() { check<word>(); check<dword>(); puts("PASS: 256 upstream/port RGB conversions, 16/32-bit output, aligned/unaligned buffers, padded rows and buffer guards"); }
'''
    code = '#include <initializer_list>\n' + code
    source = temp / 'video.cpp'
    source.write_text(code)
    compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
    command = [compiler] + (['c++'] if Path(compiler).stem == 'zig' else [])
    executable = temp / 'video.exe'
    subprocess.run(command + ['-std=c++11', '-O2', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
