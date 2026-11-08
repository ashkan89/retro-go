# Game Boy Advance on ESP32-P4

This app integrates the RV32 JIT from [midworld/gpsp-esp32](https://github.com/midworld/gpsp-esp32),
commit `0015ff0eea6f8d21973beb7776682277e9ecefe5` (2026-11-08 checkout).
The vendored core is under `components/gpsp/core`; original copyright notices,
the GPL license, and the replacement BIOS source are preserved. The original
core is GPL-2.0-or-later; RV32 platform/frontend additions include
GPL-3.0-or-later code. This integration is distributed under GPL-3.0-or-later.

Build with ESP-IDF 5.5.5:

```
python rg_tool.py --target esp32-p4-n16r32-st7796 build gbsp
```

The standalone WT0132P4 target includes this eighth app by default. Other
targets retain their current app selection. This implementation requires P4.
Its 2 MiB partition follows retro-legacy, leaving existing app offsets intact.
Installing it for the first time also requires the updated partition table.

Place ROMs in `/roms/gba`. Raw `.gba` files are recommended: the core uses an
8 MiB ROM cache and pages larger cartridges from SD. ZIPs are unpacked to an
SD cache before the emulator starts; archives must fit available PSRAM during
unpacking. Saves and save states retain the original ROM name. Battery-backed
cartridge memory is saved on change every ten seconds, before opening a menu,
and on shutdown. State, reset, screenshot, and resume use Retro-Go's menus.
Retro-Go's manual/adaptive frame-skip selection is passed to gpSP's renderer.

An open replacement BIOS is included. An optional user-supplied official BIOS
can be placed at `/retro-go/bios/gba_bios.bin`. No Nintendo BIOS is bundled.
GPIO controls map A/B, D-pad, Start and Select as usual. GBA shoulder buttons
use Retro-Go L/R inputs (USB controllers, or additional GPIO buttons configured
by the user); this target's original ten-button wiring has no L/R buttons.

JIT code lives in aligned PSRAM with explicit data writeback and instruction
cache invalidation through IDF 5.5 APIs. The two code caches grow in 256 KiB
chunks to 2 MiB each. Core BSS is placed in PSRAM to preserve DMA/internal RAM.
The source renderer uses -O3; the P4 PPA handles compatible filtered RGB565
scaling, including the exact 240x160 to 480x320 GBA upscale. Unsupported geometry
or nearest-neighbour filtering uses the software renderer.

The hardware smoke test executed 121 native RV32 blocks and passed framebuffer rendering, PPA pixel readback,
and state restoration on the v1.3 board. To repeat it, explicitly configure
`idf.py -C gbsp -DRG_GBA_SELFTEST=ON app`; the test uses an original ARM7
program with no copyrighted cartridge header. Restore `RG_GBA_SELFTEST=OFF`
before building a release image. A small core status probe was added to verify
that native JIT blocks executed; other vendored core sources are unchanged.
The original v1.3 P4 does not run the upstream `bexti` stub instruction. This
port disables Zba/Zbb/Zbs/Zcb instruction selection and retains RV32 JIT/RVC;
do not enable those newer ISA options from another board's configuration.
Exactly 1 MiB mirrored mini-ROMs retain upstream's interpreter compatibility
path. The smoke-test cartridge is 256 KiB so it exercises the native JIT.

References reviewed:

- [gpSP RV32 JIT](https://github.com/midworld/gpsp-esp32)
- [mGBA port](https://github.com/SmartArduino/esp-mgba), commit
  `88c29e3e08256fba56f0c987edb672f9456b37f7`: ROM caching, PPA scaling;
  its current board/SDK integration targets S31 and is not copied here.
- [SNES P4 port](https://github.com/fcipaq/snes9x_esp32), commit
  `3bfb27927a20f5e877e9af47ed1670d737e9a358`: separate display/audio work,
  a related SNES9x core, and DSI display output. This SPI panel has a different
  bandwidth limit; replacing SNES9x alone cannot remove that limit.
