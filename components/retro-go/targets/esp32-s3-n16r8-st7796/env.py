# This file is injected late into rg_tool.py.

# Espressif chip in the device
IDF_TARGET = "esp32s3"
# .fw file format, if supported by the device
FW_FORMAT = "none"

# Include a factory flasher partition so the launcher can install downloaded .img updates.
DEFAULT_APPS = "factory launcher retro-core prboom-go gwenesis fmsx"
