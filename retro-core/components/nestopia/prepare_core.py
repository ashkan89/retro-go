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


def prepare_read_banks(source, changes):
    """Bypass only verified pure PRG readers; mapping writes revoke permission."""
    port = (source / 'NstIoPort.hpp').read_text()
    port = replace(port, '\t\t\tclass Port', '            typedef const byte* const* ReadBank;\n\n\t\t\tclass Port')
    port = replace(port, '\t\t\t\tWriter writer;', '\t\t\t\tWriter writer;\n                ReadBank* readBank;')
    port = replace(port, 'Port() {}', 'Port(): readBank(NULL) {}')
    port = replace(port, 'writer    ( w )', 'writer    ( w ), readBank(NULL)')
    port = replace(port, 'void Set(Component c,Reader r,Writer w)', '''typedef Reader ReadFunction;
                Port(const Port& p): component(p.component), reader(p.reader), writer(p.writer), readBank(NULL) {}
                Port& operator=(const Port& p)
                {
                    InvalidateReadBank();
                    component=p.component; reader=p.reader; writer=p.writer;
                    return *this;
                }
                void BindReadBank(ReadBank* p) { readBank=p; }
                void InvalidateReadBank() { if (readBank) *readBank=NULL; }
                bool IsReader(Component c,Reader r) const { return component==c && reader==r; }

                void Set(Component c,Reader r,Writer w)''')
    for signature in ['void Set(Component c,Reader r,Writer w)', 'void Set(Reader r)', 'void Set(Reader r,Writer w)']:
        anchor = signature + '\n\t\t\t\t{'
        port = replace(port, anchor, anchor + '\n                    InvalidateReadBank();')
    changes['NstIoPort.hpp'] = port
    cpu = changes['NstCpu.hpp']
    cpu = replace(cpu, '#define NST_CPU_H', '#define NST_CPU_H\n#define RG_NESTOPIA_READ_BANK_SUPPORTED 1')
    cpu = replace(cpu, 'std::unique_ptr<IoMap> mapStorage;',
                  'Io::ReadBank readBanks[128] = {};\n\t\t\tstd::unique_ptr<IoMap> mapStorage;')
    cpu = replace(cpu, 'uint PeekBus(uint address);', 'NST_FORCE_INLINE uint PeekMapped(uint) const;\n\t\t\tuint PeekBus(uint address);')
    cpu = replace(cpu, 'Io::Port& Map(Address address)', '''// Register only pure standard PRG readers. Each page is verified in full.
            void MapReadBank(uint,uint,void*,Io::Port::ReadFunction,Io::ReadBank);
            uint GetDirectReadPageCount() const
            {
                uint count=0;
                for (uint i=0; i<128; ++i) count += readBanks[i] != NULL;
                return count;
            }

            Io::Port& Map(Address address)''')
    changes['NstCpu.hpp'] = cpu
    cpu = changes['NstCpu.cpp']
    cpu = replace(cpu, ': Io::Map<SIZE_64K>( cpu, peek, poke ) {}', ''': Io::Map<SIZE_64K>( cpu, peek, poke )
        {
            for (uint i=0x8000; i<0x10000; ++i)
                ports[i].BindReadBank(&cpu->readBanks[(i-0x8000) >> 8]);
        }''')
    if cpu.count('map.Peek8( address )') != 2:
        raise ValueError('Direct PRG read anchors changed')
    cpu = cpu.replace('map.Peek8( address )', 'PeekMapped( address )')
    cpu = replace(cpu, 'const uint data = map.Peek16( address );',
                  'const uint lo = PeekMapped(address);\n\t\t\tconst uint data = lo | (PeekMapped(address+1) << 8);')
    anchor = 'RG_NES_CPU_HOT uint Cpu::PeekBus(const uint address)'
    cpu = replace(cpu, anchor, '''void Cpu::MapReadBank(uint first,uint last,void* component,Io::Port::ReadFunction reader,Io::ReadBank bank)
        {
            NST_ASSERT(first>=0x8000 && last<=0xFFFF && !(first&255) && (last&255)==255);
            for (uint page=first; page<=last; page+=256)
            {
                bool standard=true;
                for (uint i=0; i<256; ++i)
                    if (!map[page+i].IsReader(component,reader)) { standard=false; break; }
                readBanks[(page-0x8000) >> 8] = standard ? bank : NULL;
            }
        }

        NST_FORCE_INLINE uint Cpu::PeekMapped(uint address) const
        {
            if (uint(address-0x8000)<0x8000)
                if (Io::ReadBank bank=readBanks[(address-0x8000) >> 8])
                    return (*bank)[address & 0x1FFF];
            return map.Peek8(address);
        }

        ''' + anchor)
    changes['NstCpu.cpp'] = cpu
    memory = (source / 'NstMemory.hpp').read_text()
    memory = replace(memory, 'bool Readable(uint page) const',
                     'const byte* const* ReadPagePointer(uint page) const { return pages.mem+page; }\n\n\t\t\tbool Readable(uint page) const')
    changes['NstMemory.hpp'] = memory
    board = changes['board/NstBoard.cpp']
    calls = ''.join(f'\n                cpu.MapReadBank(0x{first:04X},0x{first+8191:04X},this,&Board::Peek_Prg_{name},prg.ReadPagePointer({index}));'
                    for index,(first,name) in enumerate([(0x8000,'8'),(0xA000,'A'),(0xC000,'C'),(0xE000,'E')]))
    board = replace(board, 'SubReset( hard );', 'SubReset( hard );' + calls)
    changes['board/NstBoard.cpp'] = board

    # FDS program RAM and BIOS are fixed arrays. Keep their page references
    # live and use the same reader verification/invalidation as cartridge PRG.
    fds_header = (source / 'NstFds.hpp').read_text()
    fds_header = replace(fds_header, '\t\t\tRam ram;', '\t\t\tRam ram;\n            const byte* directReadPages[4];')
    changes['NstFds.hpp'] = fds_header
    fds = changes['NstFds.cpp']
    fds = replace(fds, '\t\t\tbool Available() const', '            const byte* GetRomData() const { return rom; }\n\n\t\t\tbool Available() const')
    anchor = '\t\t\tcpu.Map( 0xE000, 0xFFFF ).Set( &bios, &Bios::Peek_Rom, &Bios::Poke_Nop );'
    fds = replace(fds, anchor, anchor + '''
            for (uint i=0; i<3; ++i)
            {
                directReadPages[i]=ram.mem+(i+1)*SIZE_8K;
                cpu.MapReadBank(0x8000+i*SIZE_8K,0x9FFF+i*SIZE_8K,&ram,&Ram::Peek_Ram,directReadPages+i);
            }
            directReadPages[3]=bios.GetRomData();
            cpu.MapReadBank(0xE000,0xFFFF,&bios,&Bios::Peek_Rom,directReadPages+3);''')
    fds = replace(fds, '#include "NstFds.hpp"', '''#include "NstFds.hpp"
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define RG_NES_FDS_HOT IRAM_ATTR
#else
#define RG_NES_FDS_HOT
#endif''')
    for accessor in ('NES_PEEK_A(Fds::Bios,Rom)', 'NES_PEEK_A(Fds::Ram,Ram)', 'NES_POKE_AD(Fds::Ram,Ram)'):
        fds = replace(fds, accessor, 'RG_NES_FDS_HOT ' + accessor)
    changes['NstFds.cpp'] = fds


def prepare_rgb_output(source, changes):
    """Optional RGB raster output; the original indexed mode remains the default."""
    screen = changes['NstVideoScreen.hpp']
    screen = replace(screen, 'Pixel pixels[uint(PIXELS)+PIXELS_PADDING];',
                     'Pixel pixels[uint(PIXELS)+PIXELS_PADDING];\n\t\t\t\tbool rgbPixels;')
    changes['NstVideoScreen.hpp'] = screen
    screen = (source / 'NstVideoScreen.cpp').read_text()
    screen = replace(screen, 'Screen::Screen()\n\t\t\t{', 'Screen::Screen()\n\t\t\t: rgbPixels(false)\n\t\t\t{')
    screen = replace(screen, 'Pixel(0)', 'Pixel(rgbPixels ? palette[0] : 0)')
    changes['NstVideoScreen.cpp'] = screen

    ppu = changes['NstPpu.hpp']
    ppu = replace(ppu, 'word palette[Palette::SIZE];',
                  'word palette[Palette::SIZE];\n\t\t\t\tword rgbPalette[Palette::SIZE];\n\t\t\t\tconst word* renderPalette;')
    ppu = replace(ppu, 'void UpdatePalette();', 'void UpdatePalette();\n\t\t\tNST_FORCE_INLINE void StorePalette(uint,word);')
    ppu = replace(ppu, 'void EnableCpuSynchronization();',
                  'void SetRgbOutput(bool);\n\t\t\tvoid EnableCpuSynchronization();')
    changes['NstPpu.hpp'] = ppu
    ppu = changes['NstPpu.cpp']
    ppu = replace(ppu, ': pixels(p) {}', ': pixels(p), renderPalette(palette) {}')
    # Keep the indexed palette for state/CPU semantics, mirror its writes to a
    # cached 32-entry output palette. Pixel loops need only one lookup/store.
    stores = [
        ('i', '((rgbMap ? rgbMap[palette.ram[i] & uint(Palette::COLOR)] : palette.ram[i]) & c) | e'),
        ('i', '(palette.ram[i] & ce[0]) | ce[1]'),
        ('i', '(map[palette.ram[i] & Palette::COLOR] & ce[0]) | ce[1]'),
        ('address', 'final'), ('address ^ 0x10', 'final'),
    ]
    for index, value in stores:
        ppu = replace(ppu, f'output.palette[{index}] = {value};', f'StorePalette({index}, {value});')
    ppu = replace(ppu, 'void Ppu::UpdatePalette()\n', '''NST_FORCE_INLINE void Ppu::StorePalette(uint index,word color)
        {
            output.palette[index] = color;
            if (screen.rgbPixels) output.rgbPalette[index] = word(screen.palette[color]);
        }

        void Ppu::SetRgbOutput(bool enable)
        {
            if (enable && !screen.rgbPixels)
                for (uint i=0; i<Video::Screen::PIXELS+Video::Screen::PIXELS_PADDING; ++i)
                    screen.pixels[i] = word(screen.palette[screen.pixels[i]]);
            else if (!enable && screen.rgbPixels)
            {
                // Mode changes are only prepared between frames. A raw-screen
                // observer starts from black until the next frame is rendered.
                screen.rgbPixels = false;
                screen.Clear();
            }
            screen.rgbPixels = enable;
            if (enable)
                for (uint i=0; i<Palette::SIZE; ++i)
                    output.rgbPalette[i] = word(screen.palette[output.palette[i]]);
            output.renderPalette = enable ? output.rgbPalette : output.palette;
        }

        void Ppu::UpdatePalette()
''')
    # Only the raster's reads change; raw palette RAM, register behavior and
    # all sprite/IRQ decisions continue using their original values.
    if ppu.count('*target = output.palette[pixel];') != 2:
        raise ValueError('Scalar raster color anchors changed')
    ppu = ppu.replace('*target = output.palette[pixel];', '*target = output.renderPalette[pixel];')
    ppu = replace(ppu, 'const uint pixel = output.palette[(scroll.address & 0x3F00)',
                  'const uint pixel = output.renderPalette[(scroll.address & 0x3F00)')
    changes['NstPpu.cpp'] = ppu

    renderer = (source / 'NstVideoRenderer.hpp').read_text()
    renderer = replace(renderer, 'void Blit(Output&,Input&,uint);',
                       'void Blit(Output&,Input&,uint);\n\t\t\t\tbool PrepareRgb(Input&);')
    changes['NstVideoRenderer.hpp'] = renderer
    renderer = (source / 'NstVideoRenderer.cpp').read_text()
    anchor = 'void Renderer::Blit(Output& output,Input& input,uint burstPhase)'
    renderer = replace(renderer, anchor, '''bool Renderer::PrepareRgb(Input& input)
            {
                if (!filter || state.filter != RenderState::FILTER_NONE || filter->format.bpp != 16)
                    return false;
                if (state.update) UpdateFilter(input);
                return true;
            }

            ''' + anchor)
    changes['NstVideoRenderer.cpp'] = renderer
    video = changes['NstVideoFilterNone.cpp']
    anchor = 'void Renderer::FilterNone::Blit(const Input& input,const Output& output,uint)\n\t\t\t{'
    video = replace(video, anchor, anchor + '''
                if (input.rgbPixels && format.bpp == 16)
                {
                    const byte* src = reinterpret_cast<const byte*>(input.pixels);
                    byte* dst = static_cast<byte*>(output.pixels);
                    if (output.pitch == WIDTH * sizeof(word))
                        std::memcpy(dst,src,PIXELS*sizeof(word));
                    else
                        for (uint y=0; y<HEIGHT; ++y, src+=WIDTH*sizeof(word), dst+=output.pitch)
                            std::memcpy(dst,src,WIDTH*sizeof(word));
                    return;
                }
''')
    changes['NstVideoFilterNone.cpp'] = video
    api = (source / 'api/NstApiVideo.hpp').read_text()
    api = replace(api, '#define NST_API_VIDEO_H', '#define NST_API_VIDEO_H\n#define RG_NESTOPIA_RGB_OUTPUT_SUPPORTED 1')
    api = replace(api, 'Result Blit(Output& output) throw();',
                  'Result PrepareRgbFrame(bool enable=true) throw();\n\t\t\tResult Blit(Output& output) throw();')
    changes['api/NstApiVideo.hpp'] = api
    api = (source / 'api/NstApiVideo.cpp').read_text()
    api = replace(api, '#include "NstApiVideo.hpp"', '#include "NstApiVideo.hpp"\n#include "NstApiInput.hpp"\n#include "NstApiCartridge.hpp"\n#include "NstApiMachine.hpp"')
    anchor = 'Result Video::Blit(Output& output) throw()'
    api = replace(api, anchor, '''Result Video::PrepareRgbFrame(bool enable) throw()
        {
            // Tested cartridge families, FDS and ordinary pads may opt in.
            // Light sensors and auxiliary screen observers keep indexed pixels.
            const Cartridge::Profile* profile = Cartridge(emulator).GetProfile();
            bool supported = Machine(emulator).Is(Machine::DISK);
            if (profile)
                switch (profile->board.mapper)
                {
                    case 0: case 2: case 3: case 4: case 5: case 9: case 10:
                    case 64: case 90: case 96: case 118: case 119: case 163:
                    case 165: case 209: supported = true; break;
                }
            Input input(emulator);
            for (uint p=0; p<Input::NUM_PORTS && supported; ++p)
            {
                const Input::Type type = input.GetConnectedController(p);
                if (type != Input::UNCONNECTED && (type < Input::PAD1 || type > Input::PAD4)) supported = false;
            }
            enable = enable && supported && emulator.renderer.PrepareRgb(emulator.ppu.GetScreen());
            emulator.ppu.SetRgbOutput(enable);
            return enable ? RESULT_OK : RESULT_NOP;
        }

        ''' + anchor)
    api = replace(api, 'if (emulator.renderer.IsReady())', '''if (emulator.ppu.GetScreen().rgbPixels)
            {
                RenderState state;
                if (GetRenderState(state) < RESULT_OK || state.filter != RenderState::FILTER_NONE || state.bits.count != 16)
                    return RESULT_ERR_INVALID_PARAM;
            }
            if (emulator.renderer.IsReady())''')
    changes['api/NstApiVideo.cpp'] = api


def prepare(source, destination, direct_prg=True):
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
    cpu = replace(cpu, '#include <cstring>', '''#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define RG_NES_CPU_HOT IRAM_ATTR
#else
#define RG_NES_CPU_HOT
#endif''')
    for signature in ['inline void Cpu::ExecuteOp()', 'uint Cpu::PeekBus(const uint address)', 'void Cpu::Clock()']:
        cpu = replace(cpu, signature, 'RG_NES_CPU_HOT ' + signature)
    addressing = list(re.finditer(r'(?:inline |NST_FORCE_INLINE )?uint Cpu::(?:Imm|Abs|AbsReg|AbsX|AbsY|Zpg|ZpgReg|ZpgX|ZpgY|IndX|IndY)_(?:R|RW|W)\([^)]*\)', cpu))
    if len(addressing) != 31:
        raise ValueError(f'CPU addressing helper anchors changed: {len(addressing)}')
    for match in addressing:
        cpu = replace(cpu, match.group(0), 'RG_NES_CPU_HOT ' + match.group(0))
    cpu = replace(cpu, 'NST_FORCE_INLINE void Cpu::Branch(uint tmp)', 'RG_NES_CPU_HOT NST_FORCE_INLINE void Cpu::Branch(uint tmp)')
    for operation in ['PEEK_A', 'POKE_AD']:
        for mirror in range(4):
            anchor = f'NES_{operation}(Cpu::Ram,Ram_{mirror})'
            cpu = replace(cpu, anchor, 'RG_NES_CPU_HOT ' + anchor)
    # The upstream groups common handlers before its rarely-executed block.
    # Keep those handlers with the dispatcher on P4; its later rare/unofficial
    # blocks retain their original placement. No instruction bodies change.
    if cpu.count('void Cpu::op##hex_()') != 9:
        raise ValueError('CPU opcode definition anchors changed')
    cpu = cpu.replace('void Cpu::op##hex_()', 'RG_NES_OPCODE_HOT void Cpu::op##hex_()')
    anchor = '\t\tNES_IR___( Adc, Imm,      0x69 )'
    cpu = replace(cpu, anchor, '\t\t#define RG_NES_OPCODE_HOT RG_NES_CPU_HOT\n\n' + anchor)
    anchor = '\t\t// rarely executed'
    cpu = replace(cpu, anchor, '\t\t#undef RG_NES_OPCODE_HOT\n\t\t#define RG_NES_OPCODE_HOT\n\n' + anchor)
    cpu = replace(cpu, '\t\t#undef StoreZpgX', '\t\t#undef RG_NES_OPCODE_HOT\n\t\t#undef StoreZpgX')
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
                  '\t\t\tNST_FORCE_INLINE void RenderTilePixels(const word*, uint);\n'
                  '\t\t\ttemplate<bool Direct> NST_NO_INLINE void RenderTile();\n'
                  '\t\t\tNST_NO_INLINE void Run();')
    ppu = replace(ppu, 'void EnableCpuSynchronization();',
                  'void EnableCpuSynchronization();\n'
                  '\t\t\tvoid EnableCpuSynchronization(const Hook&);\n'
                  '\t\t\tvoid SynchronizeCpu() { Hook_Sync(this); }\n'
                  '\t\t\tbool CanDeferCpuSynchronization(Cycle now) const { return uint(scanline) < 240 && cycles.hClock < 339 &&\n'
                  '\t\t\t    now < (cycles.vClock + HCLOCK_DUMMY) * cycles.one; }\n'
                  '\t\t\tbool CanDeferA12Synchronization(Cycle now, Cycle blockedUntil, uint hold) const\n'
                  '\t\t\t{\n'
                  '\t\t\t    if (cycles.hClock >= 256 || !(regs.ctrl[1] & Regs::CTRL1_BG_SP_ENABLED)) return false;\n'
                  '\t\t\t    if (!(regs.ctrl[0] & 0x10) && !(io.pattern & 0x1000))\n'
                  '\t\t\t        return now < (cycles.vClock + 260) * cycles.one;\n'
                  '\t\t\t    return hold > 8 && (regs.ctrl[0] & 0x10) && (io.pattern & 0x1000) &&\n'
                  '\t\t\t        blockedUntil > (cycles.vClock + cycles.hClock + 8) * cycles.one &&\n'
                  '\t\t\t        now < (cycles.vClock + 256) * cycles.one;\n'
                  '\t\t\t}')
    ppu = replace(ppu, 'Core::Io::Line line;', 'Core::Io::Line line;\n\t\t\t\tbool irqOnlyLine;')
    tile_fetches = ['UpdateAddressLine', 'OpenName', 'FetchName', 'OpenAttribute',
                    'FetchAttribute', 'OpenPattern', 'FetchBgPattern0', 'FetchBgPattern1']
    for method in tile_fetches:
        declaration = re.search(r'NST_FORCE_INLINE (?:void|uint) ' + method + r'\([^;]*\);', ppu).group(0)
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
    # Standard-memory tile clones keep the original bodies except dispatch.
    # They run only with verified accessors and either no address callback or
    # the known MMC3 IRQ timer, which cannot change graphics accessors.
    direct_fetches = []
    for method in tile_fetches:
        body = re.search(r'NST_FORCE_INLINE (?:void|uint) Ppu::' + method + r'\([^)]*\)\n\t\t\{.*?\n\t\t\}', ppu, re.S).group(0)
        body = body.replace('Ppu::' + method + '(', 'Ppu::' + method + 'Tile(')
        body = body.replace('UpdateAddressLine( ', 'UpdateAddressLineTile( ')
        body = body.replace('nmt.FetchName( address )', 'nmt[address >> 10 & 3][address & 0x3FF]')
        body = body.replace('nmt.FetchAttribute( address )', 'nmt[address >> 10 & 3][0x3C0 | (address & 0x3F)]')
        body = body.replace('chr.FetchPattern( (io.address & 0xFF00) | io.octalLatch )',
                            'chr.Peek( ((io.address & 0xFF00) | io.octalLatch) & 0x1FFF )')
        if method == 'UpdateAddressLine':
            body = replace(body, 'io.address = address;',
                           'const uint changed = io.address ^ address;\n\t\t\tio.address = address;')
            # Only the IRQ-only timer can be connected in this specialization.
            # Its sole input is A12, so unchanged A12 cannot change timer state.
            body = replace(body, 'if (io.line)', 'if (io.line && (changed & 0x1000))')
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
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
// Xtensa cannot put both template lambda COMDATs in one explicit section.
// Inlining keeps the inner loop in its parent's IRAM section instead.
#define RG_NES_TILE_INNER_HOT __attribute__((always_inline))
#else
#define RG_NES_TILE_INNER_HOT RG_NES_PPU_HOT
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define RG_NES_P4_MEMORY_HOT IRAM_ATTR
#else
#define RG_NES_P4_MEMORY_HOT
#endif''')
    for accessor in ['Pattern', 'Name_2000', 'Name_2400', 'Name_2800', 'Name_2C00']:
        owner = 'ChrMem' if accessor == 'Pattern' else 'NmtMem'
        anchor = f'NES_ACCESSOR(Ppu::{owner},{accessor})'
        ppu = replace(ppu, anchor, 'RG_NES_P4_MEMORY_HOT ' + anchor)
    ppu = replace(ppu, 'NES_HOOK(Ppu,Sync)', 'RG_NES_P4_MEMORY_HOT NES_HOOK(Ppu,Sync)')
    for phase in range(10):
        anchor = f'void Ppu::EvaluateSpritesPhase{phase}()'
        ppu = replace(ppu, anchor, 'RG_NES_PPU_HOT ' + anchor)
    tile_load = re.search(r'NST_SINGLE_CALL void Ppu::LoadTiles\(\)\n\t\t\{.*?\n\t\t\}', ppu, re.S).group(0)
    tile_load_fast = replace(tile_load, '\t\t\ttiles.fetched = 0;\n\n\t\t\tdst[0] = src[0][0];',
                  '\t\t\ttiles.fetched = 0;\n'
                  '\t\t\tif (!(tiles.pattern[0] | tiles.pattern[1]))\n'
                  '\t\t\t{\n\t\t\t\tstd::memset(dst,0,8);\n\t\t\t\treturn;\n\t\t\t}\n\n'
                  '\t\t\tdst[0] = src[0][0];')
    ppu = replace(ppu, tile_load, tile_load_fast)
    # Complete sprite-fetch spans need no per-dot scheduler return when no CPU
    # access intervenes. Retain both nametable opens and both pattern addresses
    # at their original dots, including distinct 8x16-sprite A12 transitions.
    anchor = '''\t\t\t\t\tcase 316:
\t\t\t\t\t{
\t\t\t\t\t\tconst uint slot = (cycles.hClock - 260) / 8;'''
    ppu = replace(ppu, anchor, '''\t\t\t\t\tcase 316:

                    {
                        if (directTiles)
                        {
                            while (cycles.hClock < 316 && cycles.count >= cycles.hClock + 8)
                            {
                                const uint clock = cycles.hClock;
                                const uint slot = (clock - 260) / 8;
                                byte object[4];
                                FetchOam2Object(object);
                                OpenPatternTile(slot * 4 >= oam.secondary ? OpenSprite() : OpenSprite(object));
                                cycles.hClock = clock + 1;
                                if (oam.buffer + slot * 4 < oam.buffered)
                                    io.pattern = io.busData = chr.Peek(((io.address & 0xFF00) | io.octalLatch) & 0x1FFF);
                                cycles.hClock = clock + 2;
                                OpenPatternTile(io.address | 8);
                                cycles.hClock = clock + 3;
                                if (slot * 4 < oam.secondary)
                                {
                                    io.busData = chr.Peek(((io.address & 0xFF00) | io.octalLatch) & 0x1FFF);
                                    LoadSprite(io.pattern,io.busData,object,slot);
                                }
                                else
                                    ClearSprite(slot);
                                cycles.hClock = clock + 4;
                                OpenNameTile(); StepOam2Address(2);
                                cycles.hClock = clock + 6;
                                OpenNameTile(); StepOam2Address(2);
                                cycles.hClock = clock + 8;
                            }
                            if (cycles.count <= cycles.hClock) break;
                        }
                        const uint slot = (cycles.hClock - 260) / 8;''')
    ppu = replace(ppu, 'NST_NO_INLINE void Ppu::Run()\n\t\t{',
                  '\n\n'.join(direct_fetches) + '\n\n#include "rg_nestopia_tiles.inc"\n\n'
                  '\t\tRG_NES_PPU_HOT NST_NO_INLINE void Ppu::Run()\n\t\t{\n\t\t\tRG_NES_SCOPE(ppuCycles);\n'
                  '\t\t\tconst bool directTiles = (!io.line || io.irqOnlyLine) && !hActiveHook && !hBlankHook &&\n'
                  '\t\t\t    chr.HasStandardAccessor() && nmt.HasStandardAccessors();')
    ppu = replace(ppu, '\t\t\t\t\tHActive:\n\n\t\t\t\t\t\tLoadTiles();',
                  '\t\t\t\t\tHActive:\n\n'
                  '\t\t\t\t\t\tif (cycles.hClock < 248 && cycles.count >= cycles.hClock + 8)\n'
                  '\t\t\t\t\t\t{\n\t\t\t\t\t\t\tif (directTiles) RenderTile<true>(); else RenderTile<false>();\n'
                  '\t\t\t\t\t\t\tif (cycles.count <= cycles.hClock) break;\n'
                  '\t\t\t\t\t\t\tif (cycles.hClock == 64) goto HActive64;\n'
                  '\t\t\t\t\t\t\tgoto HActive;\n\t\t\t\t\t\t}\n\n'
                  '\t\t\t\t\t\tLoadTiles();')
    ppu = replace(ppu, '\t\t\t\t\tcase 64:\n\n\t\t\t\t\t\tNST_VERIFY( regs.oam == 0 );',
                  '\t\t\t\t\tcase 64:\n\t\t\t\t\tHActive64:\n\n\t\t\t\t\t\tNST_VERIFY( regs.oam == 0 );')
    anchor = 'void Ppu::EnableCpuSynchronization()\n\t\t{\n\t\t\tcpu.AddHook( Hook(this,&Ppu::Hook_Sync) );\n\t\t}'
    ppu = replace(ppu, anchor, anchor + '\n\n\t\tvoid Ppu::EnableCpuSynchronization(const Hook& hook)\n\t\t{\n\t\t\tio.irqOnlyLine = true;\n\t\t\tcpu.AddHook(hook);\n\t\t}')
    ppu = replace(ppu, 'io.line.Unset();', 'io.line.Unset();\n\t\t\tio.irqOnlyLine = false;')
    ppu = replace(ppu, 'io.line = line;', 'io.irqOnlyLine = false;\n\t\t\tio.line = line;')
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

    # Optional IRQ bounds leave unknown timer units on the original hook.
    # For MMC3, N remaining valid A12 edges cannot assert IRQ sooner than
    # (N-1)*Hold PPU dots after the last synchronized CPU timestamp. The first
    # edge can occur immediately. CPU register handlers still call Update().
    timer = (source / 'NstTimer.hpp').read_text(encoding='utf-8')
    timer = replace(timer, 'bool Clock(Cycle cycle)', 'Cycle GetBlockedUntil() const { return clock; }\n\n\t\t\t\t\tbool Clock(Cycle cycle)')
    timer = replace(timer, 'bool Clock(Cycle) const', 'Cycle GetBlockedUntil() const { return 0; }\n\n\t\t\t\t\tbool Clock(Cycle) const')
    anchor = '\t\t\t\tFilter<Hold> filter;'
    timer = replace(timer, anchor, anchor + '''
                Cycle syncClock;

                template<typename T>
                auto EnableSync(const T& u,int) -> decltype(u.GetSyncDistance(),void())
                {
                    ppu.EnableCpuSynchronization(Hook(this,&A12::SyncBounded));
                }
                template<typename T>
                void EnableSync(const T&,long) { ppu.EnableCpuSynchronization(); }

                static void NST_REGCALL SyncBounded(void *p)
                {
                    A12& self = *static_cast<A12*>(p);
                    const Cycle now = self.cpu.GetCycles();
                    // The pre-render line can shorten an NTSC frame. Run it
                    // before the CPU reaches its initially scheduled end.
                    if (!self.ppu.CanDeferCpuSynchronization(now))
                    {
                        self.ppu.SynchronizeCpu();
                        self.syncClock = now;
                        return;
                    }
                    const uint distance = self.unit.GetSyncDistance();
                    if (!distance) return;
                    // Low-table background fetches cannot raise A12 before
                    // the first sprite pattern address at dot 260. Include
                    // the in-flight pattern in case $2000 changed mid-tile.
                    // CPU PPU accesses still update before touching registers.
                    // High-table fetches rise every eight dots. Once the next
                    // rise is blocked, a hold longer than eight rejects the
                    // whole uninterrupted background run, since every rejected
                    // rise also moves the filter's deadline forward.
                    if (self.ppu.CanDeferA12Synchronization(now,self.filter.GetBlockedUntil(),Hold)) return;
                    const Cycle base = self.syncClock <= now ? self.syncClock : 0;
                    if (!Hold || distance == 1 || now-base >= self.ppu.GetClock((distance-1)*Hold))
                    {
                        self.ppu.SynchronizeCpu();
                        self.syncClock = now;
                    }
                }''')
    if timer.count(': line(0), cpu(c), ppu(p)') != 2:
        raise ValueError('A12 constructor anchors changed')
    timer = timer.replace(': line(0), cpu(c), ppu(p)', ': line(0), cpu(c), ppu(p), syncClock(0)')
    timer = replace(timer, 'filter.VSync( cpu );',
                    'filter.VSync( cpu );\n\t\t\t\t\tsyncClock = syncClock > cpu.GetFrameCycles() ? syncClock-cpu.GetFrameCycles() : 0;')
    timer = replace(timer, 'ppu.EnableCpuSynchronization();\n\t\t\t}',
                    'syncClock = 0;\n\t\t\t\tEnableSync(unit,0);\n\t\t\t}')
    changes['NstTimer.hpp'] = timer
    mmc3 = (source / 'board/NstBoardMmc3.hpp').read_text(encoding='utf-8')
    mmc3 = replace(mmc3, '\t\t\t\t\tNST_FORCE_INLINE bool Clock()', '''                    // Zero means IRQ is disabled; one prevents deferral while
                    // the next edge may reload or assert the counter.
                    uint GetSyncDistance() const
                    {
                        return !enabled ? 0U : (!count || reload) ? 1U : count;
                    }

\t\t\t\t\tNST_FORCE_INLINE bool Clock()''')
    changes['board/NstBoardMmc3.hpp'] = mmc3

    # Cartridge detection/factory switches run only during image loading.
    # Keep them compact so the second tile specialization fits older devices'
    # installed emulator partitions, without size-optimizing mapper hot paths.
    board = (source / 'board/NstBoard.cpp').read_text(encoding='utf-8')
    board = replace(board, '#include "NstBoard.hpp"', '''#include "NstBoard.hpp"
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define RG_NES_BOARD_HOT IRAM_ATTR
#else
#define RG_NES_BOARD_HOT
#endif
#if defined(__GNUC__)
#define RG_NES_COLD __attribute__((cold))
#else
#define RG_NES_COLD
#endif''')
    for bank in ['8', 'A', 'C', 'E']:
        anchor = f'NES_PEEK_A(Board,Prg_{bank})'
        board = replace(board, anchor, 'RG_NES_BOARD_HOT ' + anchor)
    for signature in ['bool Board::Context::DetectBoard(wcstring string,const dword wram)',
                      'bool Board::Context::DetectBoard(const word mapper,const byte submapper,const dword chrRam,const dword wram,bool wramAuto)',
                      'Board* Board::Create(const Context& c)']:
        board = replace(board, signature, 'RG_NES_COLD ' + signature)
    changes['board/NstBoard.cpp'] = board

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

    prepare_rgb_output(source, changes)
    if direct_prg:
        prepare_read_banks(source, changes)

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
    parser.add_argument('--no-direct-prg', action='store_true')
    args = parser.parse_args()
    prepare(args.source, args.destination, not args.no_direct_prg)
