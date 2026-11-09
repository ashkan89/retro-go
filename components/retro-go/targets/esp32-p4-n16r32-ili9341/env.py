# This file is injected late into rg_tool.py.

# Espressif chip in the device
IDF_TARGET = "esp32p4"
# .fw file format, if supported by the device
FW_FORMAT = "none"

# Include a factory flasher partition so the launcher can install downloaded .img updates.
DEFAULT_APPS = "factory launcher retro-core prboom-go gwenesis fmsx retro-legacy gbsp"

# This target has 16 MB flash, so keep enough app-partition headroom for OTA images.
# The launcher now also carries the media player (decoders, DSP and UI).
PROJECT_APPS["launcher"][2] = 0x1C0000
PROJECT_APPS["retro-core"][2] = 0x180000
PROJECT_APPS["prboom-go"][2] = 0x180000
PROJECT_APPS["gwenesis"][2] = 0x180000
PROJECT_APPS["fmsx"][2] = 0x180000
PROJECT_APPS["retro-legacy"][2] = 0x280000
PROJECT_APPS["gbsp"][2] = 0x200000

# ESP32-WROOM-32D companion running ESP-Hosted over SDIO; see ../esp32-p4-n16r32-st7796/docs/WIFI.md.
NETWORKING_SUPPORTED = True
