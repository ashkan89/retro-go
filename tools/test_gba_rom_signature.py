"""Check production GBA backup signature search against a bytewise reference."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (ROOT / 'gbsp/components/gpsp/core/gba_memory.c').read_text()
search = source[source.index('static bool rom_has_signature('):source.index('static bool rom_is_pokemon_family(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
static u8 data[1024*1024];
static size_t comparisons, cases;
static u32 extent;
static int checked_compare(const void *a,const void *b,size_t n) {
    assert((const u8*)a>=data && (const u8*)a+n<=data+extent);
    comparisons++;
    return memcmp(a,b,n);
}
#define memcmp checked_compare
'''
code += search + r'''
#undef memcmp
static bool reference(const u8 *rom,u32 size,const char *sig) {
    size_t n=strlen(sig);
    if(n>size) return false;
    for(size_t i=0;i<=size-n;i++) if(!memcmp(rom+i,sig,n)) return true;
    return false;
}
static void check(u32 size,const char *sig) {
    extent=size;
    assert(rom_has_signature(data,size,sig)==reference(data,size,sig));
    cases++;
}
int main(void) {
    const char *signatures[]={"EEPROM_V","SRAM_V","FLASH1M_V","FLASH512_V","FLASH_V",""};
    for(unsigned s=0;s<sizeof(signatures)/sizeof(signatures[0]);s++) {
        const char *sig=signatures[s]; size_t len=strlen(sig);
        for(unsigned pos=0;pos<256;pos++) {
            memset(data,0,512); memcpy(data+pos,sig,len);
            for(unsigned size=0;size<=pos+len+1;size++) check(size,sig);
        }
        memset(data,sig[0],512); check(512,sig); // many overlapping candidates
        // A candidate at the final legal position, after a large bulk scan.
        memset(data,0,sizeof(data)); memcpy(data+sizeof(data)-len,sig,len);
        check(sizeof(data),sig);
    }
    uint32_t random=17;
    for(unsigned i=0;i<sizeof(data);i++) { random=random*1664525+1013904223; data[i]=random>>24; }
    for(unsigned s=0;s<5;s++) {
        check(sizeof(data),signatures[s]);
        for(unsigned offset=0;offset<16;offset++) {
            memcpy(data+10000+offset,signatures[s],strlen(signatures[s]));
            check(sizeof(data),signatures[s]);
        }
    }
    memset(data,0,sizeof(data)); comparisons=0;
    for(unsigned s=0;s<5;s++) check(sizeof(data),signatures[s]);
    assert(comparisons==0);
    printf("PASS: %zu signature/reference cases, unaligned matches, bounds, empty/truncated input; no memcmp calls for absent first bytes\n",cases);
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    harness, executable = folder / 'gba_signature.c', folder / 'gba_signature.exe'
    harness.write_text(code)
    subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', '-UNDEBUG',
                    str(harness), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
