# ESP32-S3-N16R8 + ST7796
- Status: DIY / hand-wired build, no official device or prebuilt release
- Module: ESP32-S3-N16R8 (16MB flash, 8MB Octal PSRAM)
- Display: ST7796, 480x320, SPI

# Hardware info
This target is for a self-wired handheld built around a bare ESP32-S3-N16R8 module or dev board. There is no fixed
enclosure/PCB, so full features and the wiring diagram/table are documented once for all 6 N16R8/N8R2 variants in the
[root README](../../../../README.md#esp32-s3-n16r8--n8r2-diy-builds).

Pin assignments are defined in [`config.h`](../config.h).

The optional 3.5mm headphone jack (PCM5102A sharing the I2S bus with the speaker amp, plus jack detection on
GPIO1) is wired up in the [headphone jack section](../../../../README.md#headphone-jack-optional) of the root
README. It is enabled in `config.h` by default and is inert on boards that don't have it wired.

# Known issues:

# Images
No photos available yet.
