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
