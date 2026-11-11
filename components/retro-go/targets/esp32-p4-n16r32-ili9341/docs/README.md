# Wireless-Tag WT9932P4-TINY N16R32 + ILI9341

Target: `esp32-p4-n16r32-ili9341`, ESP-IDF **5.5.5**.
The board uses original P4 revision 1.x silicon, 16 MB QIO flash at 80 MHz,
32 MB HEX PSRAM at 200 MHz, and an external 320 x 240 ILI9341 display.
The CPU runs at **360 MHz**. ESP-IDF 5.5.5's pre-v3 runtime clock driver
rejects 400 MHz even with the experimental Kconfig override, causing an
assertion in `esp_clk_init` before application startup.
P4X / N16R32X revision 3.x needs another target.

## Board wiring and onboard storage

Use the onboard rear J5 microSD socket: **4-bit SDMMC slot 0**, GPIO39-44,
LDO4 at 3.3 V, up to 40 MHz. Remove the old external SPI SD wiring on
GPIO45/46/9/10. Wi-Fi moves to slot 1 on GPIO49/50/52/53/54, including reset.

The shared [board wiring guide](../../esp32-p4-n16r32-st7796/docs/BOARD.md)
contains the complete GPIO/header table, card contacts, migration instructions,
power connections, clock qualification requirement, and manufacturer sources.
LCD, audio, buttons, battery and motor assignments match that table.
The onboard GPIO51 WS2812 uses the manufacturer BSP's GRB byte order.
USB mass-storage mode uses the same onboard card and bus settings as FAT.

## Performance and capabilities

- Emulation runs on core 0; display, DMA completion and audio helpers use core 1.
  Owned PSRAM snapshots let games advance while LCD transfers finish. Busy
  submissions drop presentations without changing emulated game time.
- PSRAM XiP loads code and constant tables into 200 MHz HEX PSRAM at boot,
  reducing flash fetch traffic. It shares bandwidth with game data, so measure
  individual games before claiming a speedup. Cache lines remain 64 bytes.
- GBA retains the native RV32 JIT and original P4-safe ISA settings. ROM caching
  budgets up to **24 MiB** of free PSRAM, retaining **8 MiB** for JIT arenas,
  core/runtime state, savestates and UI. Smaller games release unused blocks;
  32 MiB games still page from SD. A 32 MiB ROM cannot coexist with the emulator
  entirely inside 32 MiB total PSRAM.
  GBA's app defaults disable `SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION`: IDF 5.5's
  XiP PSRAM split otherwise marks the JIT heap non-executable. The normal
  IRAM/DRAM PMP split is enabled. A build guard rejects the incompatible JIT/XiP
  configuration, which was reproduced as an instruction-access fault on COM3.
  Loading shows a message and temporarily extends the responsiveness timeout while
  reading/extracting a cartridge; the normal timeout resumes before gameplay.
- PPA scales RGB565 when ratios are exact multiples of 1/16 and bilinear
  filtering is selected. Other geometry, palettes, crops and nearest filtering
  keep the software renderer. PPA does not execute emulator instructions.
- Media artwork uses hardware baseline color JPEG decoding up to a 4 MiB DMA
  output buffer. MCU padding stays in the stride, outside visible dimensions.
  Hardware is used for 8-pixel-aligned dimensions of at least 64x64. Smaller
  thumbnails and other geometry use software: tiny padded inputs timed out on
  the connected rev 1.3. Compressed input is staged in DMA-readable PSRAM.
  Large images, grayscale, unavailable hardware and low memory also use software
  decoding with downscaling. Its explicit work pool fixes the component's
  insufficient default buffer with the selected fast decoder. PNG and
  MP3/FLAC/WAV paths remain supported.
- ILI9341 is **320 x 240**, RGB565, **40 MHz SPI2**, with 16-row DMA buffers.
  Payload-only full-screen throughput is **32.55 FPS**; unchanged regions can
  be omitted. This differs from 60 Hz game simulation/audio. 80 MHz requires
  separate panel/wiring validation and is not enabled here.
- H.264 is an encoder, not a hardware decoder for game logic or arbitrary video
  playback. ISP/MIPI CSI need a camera; MIPI DSI needs a compatible panel. This
  ILI9341 uses SPI. P4 AI instruction extensions and ESP-DL can run inference,
  but there is no general AI engine that accelerates these emulators. An AI
  workload would compete for CPU and memory.
- The LP CPU supports low-power control, not a third interchangeable game core.
  USB HS HID/XInput host and SD MSC device modes already exist and are exclusive.
  HUSB serves those functions; FUSB exposes the separate Serial/JTAG interface
  used by COM3. P4 has no native Wi-Fi/Bluetooth; neither is enabled.
- SPI/I2S already use DMA, LEDC drives the backlight, RMT drives the RGB LED,
  ADC1 monitors the battery divider, and hardware timers pace emulation.
  Cryptographic accelerators serve security workloads; Ethernet, TWAI/CAN,
  I3C and camera interfaces need their corresponding external hardware and
  provide no automatic acceleration of guest CPU instructions.

Espressif sources: [PSRAM/XiP](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-guides/external-ram.html),
[PPA](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/ppa.html),
[JPEG](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/jpeg.html),
[SDMMC](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/sdmmc_host.html),
[P4 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf).

## Build and verification

In an ESP-IDF 5.5.5 environment:

```powershell
python rg_tool.py --target esp32-p4-n16r32-ili9341 --release-build build-img
python rg_tool.py --target esp32-p4-n16r32-ili9341 --port COM3 --release-build install
python -m unittest discover -s tools/tests -v
python tools/test_p4_acceleration.py --cc cc
```

The full image includes factory, launcher, retro-core, prboom-go, gwenesis,
fmsx, retro-legacy and gbsp. Packed `.img` files are written at offset **0**;
the P4 bootloader starts at **0x2000**. Never use `--force` to bypass revision
compatibility.
`--release-build` keeps incremental builds and uses release logging; the `release`
command also cleans every selected application first. Unchanged placeholder
partition contents are no longer rewritten between apps.

Optional diagnostics: configure launcher with `-DRG_P4_SELFTEST=ON` to check
actual JPEG 420/422/444 colors, odd dimensions/strides and PPA PSRAM readback.
Configure gbsp with `-DRG_GBA_SELFTEST=ON` to check copyright-free ARM7/RV32
execution, a resident 16 MiB cartridge, rendering, frame skip and save-state
round trips. Disable both again
for production images. Host mocks check failure handling and ownership; they
do not establish accelerator timing or game FPS.

## Previous 360 MHz / external-SPI configuration: COM3, 2026-11-09

- All eight apps built with ESP-IDF 5.5.5, 360 MHz and PSRAM XiP. The complete
  image is 13,959,424 bytes, within 16 MB flash. CRC, partition bounds, exact app
  binary contents, P4 chip ID and revision range 1.0-1.99 passed validation.
  Both diagnostic options are disabled in that release-mode image. The full
  image was flashed with hash verification, and the final COM3 boot reached the
  launcher with SD mounted and no panic or development debug output.
- Hardware JPEG RGB565 readback passed for 128x128 YUV420/422/444 fixtures and
  a 128x120 image with MCU padding. Solid-color fixture decode/readback took
  about 1.4-1.9 ms; this is not a general-photo throughput benchmark. Software
  decoding of the unsupported 31x17 fixture and PPA PSRAM readback also passed.
- The 16 MiB generated GBA cartridge was fully resident, with a measured startup
  ROM-cache budget of 19 MiB. Native RV32 execution (121 blocks), framebuffer
  contents, PPA pixels, manual frame skip and save-state round trip passed after
  the executable-PSRAM configuration fix.
- Contra (Japan) then sustained 60 emulated FPS and about 30 presentations/sec
  over the 40 MHz ILI9341 link. Other games/systems need individual benchmarks;
  no universal FPS increase is claimed.
- Host checks passed for display rendering/ownership, frame-skip policy, audio
  FIFO/DMA ordering and gain ramps, NES timing/sprites, media playback paths,
  P4 JPEG buffer ownership/failures, SPI recovery, ROM-scan preservation and six
  release tests. The complete POSIX filesystem fault-injection test could not
  run on this Windows host; its isolated SPI/snapshot checks passed.

## External Wi-Fi

This target uses the same ESP32-WROOM-32D SDIO companion as the ST7796 target.
See the shared [Wi-Fi wiring, pull-ups and flash-voltage guide](../../esp32-p4-n16r32-st7796/docs/WIFI.md).
The onboard SD card and Wi-Fi companion use separate SDMMC slots and GPIOs.
Bluetooth remains disabled.
