# Experimental Nestopia backend

This component builds the native C++ API from the pinned
[ashkan89/nestopia-liberto fork](https://github.com/ashkan89/nestopia-liberto),
not its libretro frontend. The reviewed revision is
`f1e02d12664834536edbf7352995a3d0cd1caef4`.
Retro-Go's adapter is `../../main/main_nestopia.cpp`.

Initialize sources with `git submodule update --init --recursive`.
Select **Retro-Go > NES emulator** in menuconfig. Only the selected core and
its adapter compile; Nofrendo builds do not require the Nestopia checkout.
Nestopia uses C++ exceptions and disables its optional zlib dependency.

## Review of the P4 integration

The review covers `87e6f384` and `45f4f33a` (the two experimental P4 commits)
and the subsequent build fixes through `23c05ca1`.

Fixed integration defects:

- ZIP data previously used a nonseekable stream buffer. Nestopia queries stream
  lengths and seeks while loading images; the adapter now implements bounded
  absolute, relative and end-relative seeks.
- Image loading does not apply the desired machine region automatically. The
  adapter now selects `GetDesiredMode()` before powering on, then chooses its
  frame pacing and overscan from that region.
- Audio requests now use the allocated buffer capacity and submit only the
  sample count returned by the fork's patched sound API. This drains fractional
  sample carry without a hardcoded per-frame sample count or stale audio tail.
- Battery RAM, EEPROM and FDS patch callbacks were absent. The adapter now
  restores these files on image loading and powers off the machine during
  Retro-Go's shutdown event to trigger the save callbacks before SD unmount.
- FDS images now insert disk 1 side A after power-on. A **Disk side** menu
  option allows changing media sides. The BIOS path is
  `/retro-go/bios/disksys.rom` on the storage device, with `fds_bios.bin` as a
  legacy fallback.
- Backend selection now excludes the unused adapter and core from compilation.
  Missing Nestopia sources produce an actionable configuration error.
- Firmware CI initializes the submodule and runs a native core regression test.

## Saves and validation

Nestopia states and Nofrendo states are incompatible. They use Retro-Go's shared
state slot paths: use different slots when switching backends. Persistent native
data is stored separately as `<SRAM path>.nestopia.battery`,
`.nestopia.eeprom`, and `.nestopia.ips`. Saves are flushed on normal exit;
abrupt power loss does not run the shutdown callback.

Run `python tools/test_nestopia.py --cxx c++` with GCC or Clang, or pass the
absolute path to `zig.exe`. This compiles the pinned core and extracts the
adapter's actual stream, image-loading and persistence functions. Generated
mapper-0 ROMs exercise plain/ZIP loading, PAL/NTSC selection, 2,400 frames of
audio with buffer guards, state round trips and battery write/reload. The ZIP
container extraction is mocked; its returned ROM bytes pass through the real
adapter and core. No commercial ROMs are needed.

These checks do not establish hardware frame rate, I2S behavior, compatibility
across all mappers, or FDS/EEPROM gameplay persistence. Those require device and
game-specific testing. An ESP-IDF build verifies firmware linking and partition
fit separately from the host test.
