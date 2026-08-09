# This file is injected late into rg_tool.py.

# Espressif chip in the device
IDF_TARGET = "esp32s3"
# .fw file format, if supported by the device
FW_FORMAT = "none"

# Include a factory flasher partition so the launcher can install downloaded .img updates.
DEFAULT_APPS = "factory launcher retro-core prboom-go gwenesis fmsx"

# This target has 8 MB flash, so keep enough app-partition headroom for OTA images.
# The launcher now also carries the media player (decoders, DSP and UI).
PROJECT_APPS["launcher"][2] = 0x1C0000
PROJECT_APPS["retro-core"][2] = 0x140000
PROJECT_APPS["prboom-go"][2] = 0x100000
PROJECT_APPS["gwenesis"][2] = 0x140000
PROJECT_APPS["fmsx"][2] = 0x100000
