# This file is injected late into rg_tool.py.

# Espressif chip in the device
IDF_TARGET = "esp32s3"
# .fw file format, if supported by the device
FW_FORMAT = "none"

# Include a factory flasher partition so the launcher can install downloaded .img updates.
DEFAULT_APPS = "factory launcher retro-core prboom-go gwenesis fmsx retro-legacy"

# Fit the factory updater and every app within 8 MB, including 64 KB metadata.
# App sizes are checked against the board's flash capacity when packing.
PROJECT_APPS["factory"][2] = 0x90000
PROJECT_APPS["launcher"][2] = 0x170000
PROJECT_APPS["retro-core"][2] = 0x120000
PROJECT_APPS["prboom-go"][2] = 0x100000
PROJECT_APPS["gwenesis"][2] = 0x120000
PROJECT_APPS["fmsx"][2] = 0xD0000
PROJECT_APPS["retro-legacy"][2] = 0x1C0000
