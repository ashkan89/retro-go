"""Prepare the pinned core for embedded builds without editing the submodule.

Each substitution is checked: an upstream change must be reviewed instead of
silently losing an optimization. Write only changed files for incremental builds.
The same prepared sources are used by the native differential tests.
"""
from pathlib import Path
import argparse
import re


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Nestopia port anchor changed: {old[:100]!r}")
    return source.replace(old, new, 1)


def prepare(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError('Prepared core must be outside the pinned source tree')
    changes = {}

    # The 64K per-address callback table and 256x240 pixel buffer alone are
    # ~900 KiB. Keeping them inline forced all CPU/APU/PPU registers into PSRAM.
    # Owners precede references so constructor failures clean up automatically.
    cpu = (source / "NstCpu.hpp").read_text(encoding='utf-8')
    cpu = replace(cpu, '#include "NstAssert.hpp"', '#include <memory>\n#include "NstAssert.hpp"')
    cpu = replace(cpu, 'IoMap map;', 'std::unique_ptr<IoMap> mapStorage;\n\t\t\tIoMap& map;')

    apu = (source / "NstApu.cpp").read_text(encoding='utf-8')
    dma = '''\t\tvoid Apu::ClockDMA(uint readAddress)
\t\t{
\t\t\tClockPendingLoad( cpu.GetCycles(), readAddress );

\t\t\tif (cycles.dmcClock + cpu.GetClock(2) <= cpu.GetCycles())
\t\t\t\tClockDmc( cpu.GetCycles(), readAddress );
\t\t}'''
    # Every PPU synchronization polls DMA. Inline the usual no-event path;
    # keep actual DMA and its bus/interrupt side effects in the original code.
    apu = replace(apu, dma, '')
    apu = replace(apu, '#include <cstring>', '#include <cstring>\n#include "rg_nestopia_profile.hpp"')
    for declaration, counter in [('NST_NO_INLINE void NST_FASTCALL Apu::WalkSpan(dword span)', 'apuWalkCycles'),
                                 ('NST_NO_INLINE Apu::Channel::Sample Apu::GetSample()', 'apuSampleCycles')]:
        anchor = declaration + '\n\t\t{'
        apu = replace(apu, anchor, anchor + '\n\t\t\tRG_NES_SCOPE(' + counter + ');')
    dma = dma.replace('void Apu::ClockDMA', 'inline void Apu::ClockDMA')
    dma = dma.replace('ClockPendingLoad( cpu.GetCycles(), readAddress );',
                      'if (dmc.HasPendingLoad() && dmc.GetLoadClock() + cpu.GetClock(2) <= cpu.GetCycles())\n'
                      '\t\t\t\tClockPendingLoad( cpu.GetCycles(), readAddress );')
    cpu = replace(cpu, '\t\t};\n\t}\n}\n\n#endif', '\t\t};\n' + dma + '\n\t}\n}\n\n#endif')
    changes['NstCpu.hpp'] = cpu
    changes['NstApu.cpp'] = apu

    cpu = (source / 'NstCpu.cpp').read_text(encoding='utf-8')
    cpu = replace(cpu, 'map   ( this, &Cpu::Peek_Overflow, &Cpu::Poke_Overflow )',
                  'mapStorage ( new IoMap(this, &Cpu::Peek_Overflow, &Cpu::Poke_Overflow) ),\n'
                  '\t\tmap ( *mapStorage )')
    # A switch lets GCC directly branch to/integrate the existing opcode bodies
    # instead of decoding a C++ member-function pointer on every instruction.
    dispatch = 'switch (opcode=FetchPc8())\n\t\t\t{\n'
    dispatch += ''.join(f'\t\t\t\tcase 0x{i:02X}: op0x{i:02X}(); break;\n' for i in range(256))
    dispatch += '\t\t\t}'
    cpu = replace(cpu, '(*this.*opcodes[opcode=FetchPc8()])();', dispatch)
    changes['NstCpu.cpp'] = cpu

    accessor = (source / 'NstIoAccessor.hpp').read_text(encoding='utf-8')
    accessor = replace(accessor, '\t\t\t\tData Fetch(Address address) const',
                       '\t\t\t\tbool Is(const void* c,Function f) const { return component == c && function == f; }\n\n'
                       '\t\t\t\tData Fetch(Address address) const')
    changes['NstIoAccessor.hpp'] = accessor

    ppu = (source / 'NstPpu.hpp').read_text(encoding='utf-8')
    ppu = replace(ppu, '#include "NstMemory.hpp"', '#include <memory>\n#include "NstMemory.hpp"')
    ppu = replace(ppu, 'Video::Screen screen;',
                  'std::unique_ptr<Video::Screen> screenStorage;\n\t\t\tVideo::Screen& screen;')
    ppu = replace(ppu, '\t\t\t\tvoid ResetAccessor();',
                  '\t\t\t\tvoid ResetAccessor();\n'
                  '\t\t\t\tbool HasStandardAccessor() const { return accessor.Is(this, &ChrMem::Access_Pattern); }')
    ppu = replace(ppu, '\t\t\t\tvoid ResetAccessors();',
                  '\t\t\t\tvoid ResetAccessors();\n'
                  '\t\t\t\tbool HasStandardAccessors() const { return\n'
                  '\t\t\t\t    accessors[0].Is(this,&NmtMem::Access_Name_2000) && accessors[1].Is(this,&NmtMem::Access_Name_2400) &&\n'
                  '\t\t\t\t    accessors[2].Is(this,&NmtMem::Access_Name_2800) && accessors[3].Is(this,&NmtMem::Access_Name_2C00); }')
    ppu = replace(ppu, '\t\t\tNST_NO_INLINE void Run();',
                  '\t\t\tNST_FORCE_INLINE void PrepareSpriteTile(word*);\n'
                  '\t\t\tNST_FORCE_INLINE void RenderTilePixel(word);\n'
                  '\t\t\tNST_NO_INLINE void RenderTile();\n'
                  '\t\t\tNST_NO_INLINE void Run();')
    tile_fetches = ['UpdateAddressLine', 'OpenName', 'FetchName', 'OpenAttribute',
                    'FetchAttribute', 'OpenPattern', 'FetchBgPattern0', 'FetchBgPattern1']
    for method in tile_fetches:
        declaration = re.search(r'NST_FORCE_INLINE void ' + method + r'\([^;]*\);', ppu).group(0)
        ppu = replace(ppu, declaration, declaration + '\n\t\t\t' + declaration.replace(method, method + 'Tile'))
    # Store the dot at which a sprite becomes active. The original decrements
    # every sprite's X counter on every pixel, even hundreds of dots before it
    # can draw. A shared visible-dot clock removes those writes. Keep the shift
    # register separate: rendering-off advances X counters but freezes shifters.
    ppu = replace(ppu, '\t\t\t\t\tbyte counter;', '\t\t\t\t\tdword deadline;')
    ppu = replace(ppu, '\t\t\t\tconst byte* limit;', '\t\t\t\tdword clock;\n\t\t\t\tconst byte* limit;')
    changes['NstPpu.hpp'] = ppu
    ppu = (source / 'NstPpu.cpp').read_text(encoding='utf-8')
    ppu = replace(ppu, '#include <cstring>', '#include <cstring>\n#include "rg_nestopia_profile.hpp"')
    # These clones are used only after all accessors and the absence of an
    # address hook have been checked. Keep the original operation bodies and
    # replace only the dispatch to the standard memory implementation.
    direct_fetches = []
    for method in tile_fetches:
        body = re.search(r'NST_FORCE_INLINE void Ppu::' + method + r'\([^)]*\)\n\t\t\{.*?\n\t\t\}', ppu, re.S).group(0)
        body = body.replace('Ppu::' + method + '(', 'Ppu::' + method + 'Tile(')
        body = body.replace('UpdateAddressLine( ', 'UpdateAddressLineTile( ')
        body = body.replace('nmt.FetchName( address )', 'nmt[address >> 10 & 3][address & 0x3FF]')
        body = body.replace('nmt.FetchAttribute( address )', 'nmt[address >> 10 & 3][0x3C0 | (address & 0x3F)]')
        body = body.replace('chr.FetchPattern( (io.address & 0xFF00) | io.octalLatch )',
                            'chr.Peek( ((io.address & 0xFF00) | io.octalLatch) & 0x1FFF )')
        body = body.replace('\n\t\t\tif (io.line)\n\t\t\t\tio.line.Toggle( io.address, GetCycles() );', '')
        direct_fetches.append(body)
    ppu = replace(ppu, '#include "NstState.hpp"', '''#include "NstState.hpp"
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#endif
#if (defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3) || \
    (defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
#define RG_NES_PPU_HOT IRAM_ATTR
#else
#define RG_NES_PPU_HOT
#endif''')
    for phase in range(10):
        anchor = f'void Ppu::EvaluateSpritesPhase{phase}()'
        ppu = replace(ppu, anchor, 'RG_NES_PPU_HOT ' + anchor)
    ppu = replace(ppu, 'NST_NO_INLINE void Ppu::Run()\n\t\t{',
                  '\n\n'.join(direct_fetches) + '\n\n#include "rg_nestopia_tiles.inc"\n\n'
                  '\t\tRG_NES_PPU_HOT NST_NO_INLINE void Ppu::Run()\n\t\t{\n\t\t\tRG_NES_SCOPE(ppuCycles);\n'
                  '\t\t\tconst bool batchSprites = !io.line && !hActiveHook && !hBlankHook &&\n'
                  '\t\t\t    chr.HasStandardAccessor() && nmt.HasStandardAccessors();')
    ppu = replace(ppu, '\t\t\t\t\tHActive:\n\n\t\t\t\t\t\tLoadTiles();',
                  '\t\t\t\t\tHActive:\n\n'
                  '\t\t\t\t\t\tif (batchSprites && cycles.hClock < 248 && cycles.count >= cycles.hClock + 8)\n'
                  '\t\t\t\t\t\t{\n\t\t\t\t\t\t\tRenderTile();\n'
                  '\t\t\t\t\t\t\tif (cycles.count <= cycles.hClock) break;\n'
                  '\t\t\t\t\t\t\tif (cycles.hClock == 64) goto HActive64;\n'
                  '\t\t\t\t\t\t\tgoto HActive;\n\t\t\t\t\t\t}\n\n'
                  '\t\t\t\t\t\tLoadTiles();')
    ppu = replace(ppu, '\t\t\t\t\tcase 64:\n\n\t\t\t\t\t\tNST_VERIFY( regs.oam == 0 );',
                  '\t\t\t\t\tcase 64:\n\t\t\t\t\tHActive64:\n\n\t\t\t\t\t\tNST_VERIFY( regs.oam == 0 );')
    ppu = replace(ppu, ': limit(buffer + STD_LINE_SPRITES*4)', ': clock(0), limit(buffer + STD_LINE_SPRITES*4)')
    ppu = replace(ppu, 'entry->counter = buffer[3];', 'entry->deadline = oam.clock + buffer[3];')
    if ppu.count('entry->counter = 0;') != 2:
        raise ValueError('Sprite empty-fetch anchors changed')
    ppu = ppu.replace('entry->counter = 0;', 'entry->deadline = oam.clock;')
    for name in ['RenderPixel', 'RenderPixel255']:
        anchor = 'void Ppu::' + name + '()\n\t\t{'
        ppu = replace(ppu, anchor, anchor + '\n\t\t\tconst dword spriteClock = oam.clock++;')
    old_counter = '''\t\t\t\tif (sprite->counter)
\t\t\t\t{
\t\t\t\t\t--sprite->counter;
\t\t\t\t\tcontinue;
\t\t\t\t}

\t\t\t\tif (sprite->shift == 8)
\t\t\t\t\tcontinue;'''
    if ppu.count(old_counter) != 2:
        raise ValueError('Sprite dot-loop anchors changed')
    ppu = ppu.replace(old_counter, '''\t\t\t\tif (sprite->shift == 8 || idword(spriteClock - sprite->deadline) < 0)
\t\t\t\t\tcontinue;''')
    ppu = replace(ppu, '!oam.visible[-1].counter', 'idword(oam.clock - oam.visible[-1].deadline) >= 0')
    ppu = replace(ppu, '''for (Oam::Output* NST_RESTRICT s=oam.output; s != oam.visible; ++s)
\t\t\t\t\t\t\ts->counter = (s->counter > hClock - i) ? (s->counter - (hClock - i)) : 0;''',
                  'oam.clock += hClock - i;')
    ppu = replace(ppu, 's->counter = 0;', 's->deadline = oam.clock;')
    ppu = replace(ppu, 'output (screen.pixels)',
                  'screenStorage (new Video::Screen),\n\t\tscreen (*screenStorage),\n\t\toutput (screen.pixels)')
    changes['NstPpu.cpp'] = ppu

    # P4 rev1 has only a small executable SRAM region shared with static data.
    # An always-resident 8 KiB FDS BIOS in that region prevented the hot PPU
    # loop from fitting. The BIOS is ordinary cached ROM data, never ISR data.
    fds = (source / 'NstFds.cpp').read_text(encoding='utf-8')
    fds = replace(fds, '#include "NstFds.hpp"', '#include "NstFds.hpp"\n#ifdef ESP_PLATFORM\n#include <esp_attr.h>\n#endif')
    fds = replace(fds, 'Fds::Bios Fds::bios;',
                  '#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4\n'
                  '\t\tEXT_RAM_BSS_ATTR\n#endif\n\t\tFds::Bios Fds::bios;')
    changes['NstFds.cpp'] = fds

    machine = (source / 'NstMachine.hpp').read_text(encoding='utf-8')
    machine = replace(machine, '#include <iosfwd>', '#include <iosfwd>\n#include "rg_nestopia_memory.hpp"')
    machine = replace(machine, 'class Machine\n', 'class Machine : public RetroGo::HotAllocator\n')
    changes['NstMachine.hpp'] = machine

    screen = (source / 'NstVideoScreen.hpp').read_text(encoding='utf-8')
    screen = replace(screen, '#define NST_VIDEO_SCREEN_H', '#define NST_VIDEO_SCREEN_H\n#include "rg_nestopia_memory.hpp"')
    screen = replace(screen, 'struct Screen\n', 'struct Screen : public RetroGo::HotAllocator\n')
    changes['NstVideoScreen.hpp'] = screen

    # RGB output commonly resides in PSRAM. Convert four adjacent pixels per
    # group so aligned destinations can use full-width stores instead of one
    # halfword store per pixel. memcpy preserves native output byte order and
    # avoids type-punning the caller's word buffer.
    video = (source / 'NstVideoFilterNone.cpp').read_text(encoding='utf-8')
    video = replace(video, '#include "NstCore.hpp"', '#include <cstring>\n#include <cstdint>\n#include "NstCore.hpp"')
    anchor = '\t\t\t\tfor (uint prefetched=*src++, i=PIXELS; i; --i)'
    video = replace(video, anchor, '''\t\t\t\tif ((reinterpret_cast<std::uintptr_t>(dst) & 3) == 0)
\t\t\t\t{
\t\t\t\t\tfor (uint i=0; i<PIXELS; i+=4)
\t\t\t\t\t{
\t\t\t\t\t\tconst T pixels[4] = { T(input.palette[src[0]]), T(input.palette[src[1]]),
\t\t\t\t\t\t                      T(input.palette[src[2]]), T(input.palette[src[3]]) };
\t\t\t\t\t\tstd::memcpy(__builtin_assume_aligned(dst,4), pixels, sizeof(pixels));
\t\t\t\t\t\tsrc += 4; dst += 4;
\t\t\t\t\t}
\t\t\t\t\treturn;
\t\t\t\t}

''' + anchor)
    changes['NstVideoFilterNone.cpp'] = video

    changes['rg_nestopia_memory.hpp'] = (Path(__file__).parent / 'rg_nestopia_memory.hpp').read_text(encoding='utf-8')
    for name in ['rg_nestopia_profile.hpp', 'rg_nestopia_profile.cpp', 'rg_nestopia_tiles.inc']:
        changes[name] = (Path(__file__).parent / name).read_text(encoding='utf-8')
    for path in source.rglob('*'):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        target = destination / relative
        data = changes.pop(relative.as_posix(), None)
        data = data.encode() if data is not None else path.read_bytes()
        if not target.exists() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    for name, data in changes.items():
        target = destination / name
        if not target.exists() or target.read_text(encoding='utf-8') != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(data, encoding='utf-8')
    # The NTSC renderer includes its companion library relative to core/.
    for path in (source.parent / 'nes_ntsc').rglob('*'):
        if path.is_file():
            target = destination.parent / 'nes_ntsc' / path.relative_to(source.parent / 'nes_ntsc')
            data = path.read_bytes()
            if not target.exists() or target.read_bytes() != data:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    prepare(args.source, args.destination)
