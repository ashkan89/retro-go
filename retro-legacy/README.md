# Spectrum 48K and Atari integration

This app ports three cores from the revisions pinned by RetroESP32. It uses the
workspace's Retro-Go services for display scaling, audio routing, input, menus,
screenshots, settings, reset, and save/resume.
The app's `sdkconfig.defaults` enables C++ exceptions for Stella's serializers.
The Atari 7800 framebuffer uses PSRAM to leave internal RAM available for
the display driver and system services.

| System | Launcher namespace and SD ROM folder | Files | Source revision |
| --- | --- | --- | --- |
| ZX Spectrum 48K | `spectrum`, `/roms/spectrum` | `.sna`, `.z80` | `pelle7/odroid-go-spectrum-emulator` at `22801bc0536fa91b6f0e833eef8ec2552ddc66f7` |
| Atari 2600 | `a26`, `/roms/a26` | `.a26`, `.bin` | `OtherCrashOverride/stella-odroid-go` at `2221f5ef161d787b1dffd884d52cc4bedf1963e0` |
| Atari 7800 | `a78`, `/roms/a78` | `.a78` | `OtherCrashOverride/prosystem-odroid-go` at `5bfd4113b0e03de86b78ab4e771f7d5f88891d1f` |

Sources are vendored under `components`; building does not require RetroESP32
or its submodules. Original copyright notices are retained, and GPL license
texts accompany the cores. The Spectrum ROM image is retained from that port.

## Build and install

In your ESP-IDF environment, from the workspace root:

```sh
python rg_tool.py --target esp32-s3-devkit build launcher retro-legacy
python rg_tool.py --target esp32-s3-devkit build-img
```

Replace the target with your board. `retro-legacy` has a 2 MiB app partition in
`rg_tool.py` and is included in default firmware builds. Install a newly generated
complete image to add that partition to an existing installation; flashing only
the app cannot create the new partition. The cores require PSRAM.

All eight ESP32-S3 N16R8/N8R2 targets with ST7796, ST7789v2, ST7789, or ILI9341
include this app in their default full builds. N8R2 uses a 1.75 MiB legacy
partition and smaller reservations for the other apps to fit the factory updater
and all apps within 8 MiB. Packaging checks actual binary sizes and rejects
images exceeding the board's flash capacity.

The launcher adds separate Spectrum, Atari 2600, and Atari 7800 tabs when the
`retro-legacy` partition is installed. Open Options → Hide tabs to choose Show
or Hide for each system. Those choices are saved when leaving the options menu
and restored after reboot; ROM folders may be empty without hiding the tabs.

## Controls

MENU opens the game menu; OPTION opens Retro-Go options.

| System | D-pad | A / B | START | SELECT |
| --- | --- | --- | --- | --- |
| Spectrum | Selected joystick / QAOP | Fire / Space | Enter | 0 |
| Atari 2600 | Player 1 joystick | Fire / Fire | Reset | Select |
| Atari 7800 | Player 1 joystick | Button 2 / Button 1 | Pause | Select |

Atari 7800 START+SELECT holds the console Reset switch. Spectrum's emulator
options select Kempston, Sinclair 2, or QAOP controls; the Keyboard option sends
the selected Spectrum key when confirmed. Caps Shift and Symbol Shift can be
held using their options to enter shifted characters. Atari controller mappings currently
cover player 1 joysticks, rather than paddle or keypad games.

## How this workspace wires emulators

1. `launcher/main/applications.c` registers a tab with a namespace, extensions,
   ROM directory, and application partition label. `application_start()` passes
   the namespace, path, save slot, and boot flags to `rg_system_switch_app()`.
2. `rg_tool.py` builds separate ESP-IDF apps and packages their binaries in OTA
   partitions. `retro-core/main/main.c` dispatches its bundled emulators by
   namespace; this app follows the same approach in `main/main.c`.
3. Each adapter implements `legacy_core_t` and registers Retro-Go handlers.
   The common loop processes menus, respects frame skip, synchronizes two video
   surfaces, submits audio, and reports frame timing to the system.
4. Save paths, slot selection, screenshots, and return-to-launcher behavior are
   managed by Retro-Go. Spectrum states use the Z80 snapshot codec regardless
   of the original ROM extension; Stella states include the cartridge digest;
   ProSystem states validate length and digest before loading.

The ODROID file selectors, display SPI drivers, audio drivers, and task loops
are replaced by the adapters. PAL/NTSC timing and visible video dimensions come
from the Atari cores. Spectrum renders its 256x192 bitmap, attributes, bright
colors, flashing attributes, and border into a 320x240 surface.

Port fixes include Spectrum's aligned 64K RAM allocation, loading uncompressed
extended Z80 pages, snapshot error reporting, Stella's aliased framebuffer
destructor, and ProSystem's cartridge-RAM writes and save/load path. ProSystem's
MARIA renderer clips off-screen sprite cells to its 160-byte line RAM while
preserving horizontal wrap; unchecked writes here could corrupt renderer state
and panic when a game moved from its menu into gameplay. ProSystem's original
state format preserves CPU, banking, and RAM; it does not capture every audio
or video device register. Spectrum supports 48K snapshots; tape loading and
128K models are outside this adapter.
The default Atari launcher icons reuse the original ports' `tile.png` assets.

## Smoke tests

After a normal build, run these commands from `retro-legacy` in the ESP-IDF
environment. They temporarily replace the app entry point with the test harness.

```sh
python tests/configure_qemu.py
idf.py -DRG_LEGACY_SELFTEST=ON app bootloader
python tests/run_qemu.py /path/to/qemu-system-xtensa
python tests/configure_qemu.py --restore
idf.py -DRG_LEGACY_SELFTEST=OFF app bootloader
```

Use `--psram-mb 2` with `run_qemu.py` to check the N8R2 memory budget.
`python tests/test_targets.py` checks all eight default app lists, full-image
capacity, and rejection of oversized apps and storage partitions without IDF.

Validated with ESP-IDF 5.5.5: launcher and production app builds pass for all
eight S3 targets listed above. The full N8R2 ILI9341 image, including all apps,
the factory updater, and the metadata footer, is 8,257,792 bytes and fits 8 MiB.
The ESP32-S3 QEMU harness passes frame execution, audio, reset, save/load, malformed
state rejection, failed Stella writes, extended Z80 pages, and Atari 7800
cartridge-RAM persistence with 2 MiB PSRAM. QEMU's temporary configuration uses
quad PSRAM and DIO flash; the restore step returns the saved board settings.
Physical display, audio, and controller behavior
still need testing on the target board.

Use Espressif QEMU with ESP32-S3 support and quad PSRAM emulation. The test firmware uses a small memory
filesystem, generated Spectrum/Atari ROMs, and an audio capture stub. It checks
frame execution, colored Spectrum/2600 output, audio delivery, reset, state
round trips, malformed states, extended Z80 pages, 128K rejection, and Atari
cartridge RAM persistence. These tests do not exercise physical display, audio,
SD card, or controller hardware.
The Atari 7800 regression checks also cover all 256 horizontal sprite positions
in both cell modes, transparent kangaroo-mode cells, DMA cycle counts, horizontal
wrap, and CPU writes across the full 16K cartridge RAM window.
